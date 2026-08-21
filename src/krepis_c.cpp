#include "krepis/krepis_c.h"

#include "krepis/display_list.hpp"
#include "krepis/font_registry.hpp"
#include "krepis/glyph_outline.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

struct KrepisDisplayEngineOpaque {
	KrepisDisplayEngineOpaque() : outlines(fonts) {}

	std::thread::id owner_thread = std::this_thread::get_id();
	krepis::DisplayListPublisher publisher;
	krepis::FontRegistry fonts;
	krepis::GlyphOutlineCache outlines;
	std::vector<krepis::Glyph> glyph_scratch;
	std::uint64_t next_lease_handle = 1;
	std::unordered_map<KrepisDisplayLease, std::uint64_t> display_leases;
	std::unordered_map<KrepisGlyphPathLease, krepis::SharedGlyphPath> path_leases;
};

static_assert(sizeof(KrepisGlyph) == 24);
static_assert(offsetof(KrepisGlyph, glyph_id) == 0);
static_assert(offsetof(KrepisGlyph, cluster_byte_offset) == 20);
static_assert(sizeof(KrepisGlyphPathCommand) == sizeof(krepis::GlyphPathCommand));
static_assert(offsetof(KrepisGlyphPathCommand, values) == 4);

namespace {

constexpr std::uint16_t abi_major = 1;
constexpr std::uint16_t abi_minor = 0;
constexpr KrepisStatus ok_status = KREPIS_STATUS_OK;

KrepisStatus status(krepis::Error error) noexcept {
	return static_cast<KrepisStatus>(error.code());
}

bool correct_thread(KrepisDisplayEngine engine) noexcept {
	return engine != nullptr && engine->owner_thread == std::this_thread::get_id();
}

std::uint64_t next_lease_handle(KrepisDisplayEngine engine) noexcept {
	const auto handle = engine->next_lease_handle++;
	if (handle == 0 || engine->next_lease_handle == 0) std::terminate();
	return handle;
}

template <typename Function>
KrepisStatus guard(Function&& function) noexcept {
	try {
		return function();
	} catch (const std::bad_alloc&) {
		std::terminate();
	} catch (...) {
		return KREPIS_STATUS_PANIC;
	}
}

}  // namespace

extern "C" {

KrepisStatus krepis_display_engine_create(
	std::uint16_t requested_major,
	std::uint16_t requested_minor,
	KrepisDisplayEngine* out_engine
) {
	if (out_engine == nullptr) return KREPIS_STATUS_INVALID_ARGUMENT;
	*out_engine = nullptr;
	if (requested_major != abi_major || requested_minor > abi_minor) {
		return KREPIS_STATUS_VERSION_MISMATCH;
	}
	return guard([&]() -> KrepisStatus {
		*out_engine = new KrepisDisplayEngineOpaque();
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_engine_destroy(KrepisDisplayEngine engine) {
	if (!correct_thread(engine)) return KREPIS_STATUS_INVALID_ARGUMENT;
	if (!engine->display_leases.empty() || !engine->path_leases.empty() ||
	    engine->publisher.stats().outstanding_leases != 0) {
		return KREPIS_STATUS_INVALID_STATE;
	}
	return guard([&]() -> KrepisStatus {
		delete engine;
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_register_font(
	KrepisDisplayEngine engine,
	std::uint64_t font_id,
	const std::uint8_t* bytes,
	std::uint64_t byte_size,
	std::uint32_t face_index
) {
	if (!correct_thread(engine) || bytes == nullptr || byte_size == 0 ||
	    byte_size > std::numeric_limits<std::size_t>::max()) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	return guard([&]() -> KrepisStatus {
		auto result = engine->fonts.register_font(
			font_id,
			std::span<const std::byte>(
				reinterpret_cast<const std::byte*>(bytes),
				static_cast<std::size_t>(byte_size)
			),
			face_index
		);
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_acquire_glyph_path(
	KrepisDisplayEngine engine,
	std::uint64_t font_id,
	std::uint32_t glyph_id,
	std::int32_t font_size_26_6,
	KrepisGlyphPathSpan* out_span
) {
	if (!correct_thread(engine) || out_span == nullptr ||
	    out_span->struct_size != sizeof(KrepisGlyphPathSpan)) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	out_span->abi_major = 0;
	out_span->abi_minor = 0;
	out_span->commands = nullptr;
	out_span->command_count = 0;
	out_span->lease = 0;
	return guard([&]() -> KrepisStatus {
		auto result = engine->outlines.outline(font_id, glyph_id, font_size_26_6);
		if (!result.is_ok()) return status(result.error());
		const auto handle = next_lease_handle(engine);
		auto inserted = engine->path_leases.emplace(handle, result.value());
		if (!inserted.second) std::terminate();
		out_span->abi_major = abi_major;
		out_span->abi_minor = abi_minor;
		out_span->commands = reinterpret_cast<const KrepisGlyphPathCommand*>(
			inserted.first->second->data()
		);
		out_span->command_count = inserted.first->second->size();
		out_span->lease = handle;
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_release_glyph_path(
	KrepisDisplayEngine engine,
	KrepisGlyphPathLease lease
) {
	if (!correct_thread(engine) || lease == 0) return KREPIS_STATUS_INVALID_ARGUMENT;
	const auto found = engine->path_leases.find(lease);
	if (found == engine->path_leases.end()) return KREPIS_STATUS_INVALID_ARGUMENT;
	engine->path_leases.erase(found);
	return KREPIS_STATUS_OK;
}

KrepisStatus krepis_display_builder_reset(KrepisDisplayEngine engine) {
	if (!correct_thread(engine)) return KREPIS_STATUS_INVALID_ARGUMENT;
	return guard([&]() -> KrepisStatus {
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		builder.value()->reset();
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_add_rect(
	KrepisDisplayEngine engine,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t color_rgba
) {
	if (!correct_thread(engine)) return KREPIS_STATUS_INVALID_ARGUMENT;
	return guard([&]() -> KrepisStatus {
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		auto result = builder.value()->add_rect(x, y, width, height, color_rgba);
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_add_glyph_run(
	KrepisDisplayEngine engine,
	std::int32_t baseline_x_26_6,
	std::int32_t baseline_y_26_6,
	std::int32_t font_size_26_6,
	std::uint32_t color_rgba,
	std::uint64_t font_id,
	std::uint32_t direction,
	const KrepisGlyph* glyphs,
	std::uint32_t glyph_count
) {
	if (!correct_thread(engine) || glyphs == nullptr || glyph_count == 0 || direction > 1) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	return guard([&]() -> KrepisStatus {
		engine->glyph_scratch.clear();
		engine->glyph_scratch.reserve(glyph_count);
		for (std::uint32_t i = 0; i < glyph_count; ++i) {
			engine->glyph_scratch.push_back(krepis::Glyph{
				glyphs[i].glyph_id,
				glyphs[i].cluster_byte_offset,
				glyphs[i].x_advance_26_6,
				glyphs[i].y_advance_26_6,
				glyphs[i].x_offset_26_6,
				glyphs[i].y_offset_26_6,
			});
		}
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		auto result = builder.value()->add_glyph_run(
			baseline_x_26_6,
			baseline_y_26_6,
			font_size_26_6,
			color_rgba,
			font_id,
			direction == 0 ? krepis::GlyphDirection::ltr : krepis::GlyphDirection::rtl,
			engine->glyph_scratch
		);
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_push_clip(
	KrepisDisplayEngine engine,
	float x,
	float y,
	float width,
	float height
) {
	if (!correct_thread(engine)) return KREPIS_STATUS_INVALID_ARGUMENT;
	return guard([&]() -> KrepisStatus {
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		auto result = builder.value()->push_clip(x, y, width, height);
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_pop_clip(KrepisDisplayEngine engine) {
	if (!correct_thread(engine)) return KREPIS_STATUS_INVALID_ARGUMENT;
	return guard([&]() -> KrepisStatus {
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		auto result = builder.value()->pop_clip();
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_push_transform(
	KrepisDisplayEngine engine,
	const float affine_2x3[6]
) {
	if (!correct_thread(engine) || affine_2x3 == nullptr) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	return guard([&]() -> KrepisStatus {
		std::array<float, 6> affine{};
		for (std::size_t i = 0; i < affine.size(); ++i) affine[i] = affine_2x3[i];
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		auto result = builder.value()->push_transform(affine);
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_pop_transform(KrepisDisplayEngine engine) {
	if (!correct_thread(engine)) return KREPIS_STATUS_INVALID_ARGUMENT;
	return guard([&]() -> KrepisStatus {
		auto builder = engine->publisher.begin_frame();
		if (!builder.is_ok()) return status(builder.error());
		auto result = builder.value()->pop_transform();
		return result.is_ok() ? ok_status : status(result.error());
	});
}

KrepisStatus krepis_display_publish(
	KrepisDisplayEngine engine,
	std::uint64_t* out_frame_token
) {
	if (!correct_thread(engine) || out_frame_token == nullptr) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	*out_frame_token = 0;
	return guard([&]() -> KrepisStatus {
		auto result = engine->publisher.publish();
		if (!result.is_ok()) return status(result.error());
		*out_frame_token = result.value();
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_acquire(
	KrepisDisplayEngine engine,
	KrepisDisplaySpan* out_span
) {
	if (!correct_thread(engine) || out_span == nullptr ||
	    out_span->struct_size != sizeof(KrepisDisplaySpan)) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	out_span->abi_major = 0;
	out_span->abi_minor = 0;
	out_span->data = nullptr;
	out_span->byte_size = 0;
	out_span->frame_token = 0;
	out_span->lease = 0;
	return guard([&]() -> KrepisStatus {
		auto result = engine->publisher.acquire_front();
		if (!result.is_ok()) return status(result.error());
		const auto lease = next_lease_handle(engine);
		auto inserted = engine->display_leases.emplace(lease, result.value().lease_id);
		if (!inserted.second) std::terminate();
		out_span->abi_major = abi_major;
		out_span->abi_minor = abi_minor;
		out_span->data = reinterpret_cast<const std::uint8_t*>(result.value().data);
		out_span->byte_size = result.value().size;
		out_span->frame_token = result.value().frame_token;
		out_span->lease = lease;
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_release(
	KrepisDisplayEngine engine,
	KrepisDisplayLease lease
) {
	if (!correct_thread(engine) || lease == 0) return KREPIS_STATUS_INVALID_ARGUMENT;
	const auto found = engine->display_leases.find(lease);
	if (found == engine->display_leases.end()) return KREPIS_STATUS_INVALID_ARGUMENT;
	return guard([&]() -> KrepisStatus {
		auto result = engine->publisher.release(found->second);
		if (!result.is_ok()) return status(result.error());
		engine->display_leases.erase(found);
		return KREPIS_STATUS_OK;
	});
}

KrepisStatus krepis_display_get_stats(
	KrepisDisplayEngine engine,
	KrepisDisplayStats* out_stats
) {
	if (!correct_thread(engine) || out_stats == nullptr ||
	    out_stats->struct_size != sizeof(KrepisDisplayStats)) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	const auto& stats = engine->publisher.stats();
	out_stats->reserved = 0;
	out_stats->published_frames = stats.published_frames;
	out_stats->acquired_leases = stats.acquired_leases;
	out_stats->released_leases = stats.released_leases;
	out_stats->outstanding_leases = stats.outstanding_leases;
	return KREPIS_STATUS_OK;
}

KrepisStatus krepis_display_validate(
	const std::uint8_t* data,
	std::uint64_t byte_size,
	std::uint16_t supported_major,
	std::uint16_t supported_minor
) {
	if (data == nullptr || byte_size > std::numeric_limits<std::size_t>::max()) {
		return KREPIS_STATUS_INVALID_ARGUMENT;
	}
	return guard([&]() -> KrepisStatus {
		auto result = krepis::validate_display_list(
			std::span<const std::byte>(
				reinterpret_cast<const std::byte*>(data),
				static_cast<std::size_t>(byte_size)
			),
			supported_major,
			supported_minor
		);
		return result.is_ok() ? ok_status : status(result.error());
	});
}

}  // extern "C"
