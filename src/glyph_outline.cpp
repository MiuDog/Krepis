#include "krepis/glyph_outline.hpp"

#include <hb.h>
#include <hb-ot.h>

#include <cmath>
#include <climits>
#include <exception>
#include <unordered_map>
#include <utility>

namespace krepis {
namespace {

struct OutlineKey {
	FontId font_id;
	std::uint32_t glyph_id;
	std::int32_t font_size_26_6;

	bool operator==(const OutlineKey&) const = default;
};

struct OutlineKeyHash {
	std::size_t operator()(const OutlineKey& value) const noexcept {
		auto hash = std::hash<FontId>{}(value.font_id);
		hash ^= std::hash<std::uint32_t>{}(value.glyph_id) + 0x9E3779B9u +
		        (hash << 6u) + (hash >> 2u);
		hash ^= std::hash<std::int32_t>{}(value.font_size_26_6) + 0x9E3779B9u +
		        (hash << 6u) + (hash >> 2u);
		return hash;
	}
};

void append_command(
	void* draw_data,
	GlyphPathOpcode opcode,
	std::array<float, 6> values = {}
) noexcept {
	try {
		auto& commands = *static_cast<std::vector<GlyphPathCommand>*>(draw_data);
		commands.push_back(GlyphPathCommand{opcode, values});
	} catch (...) {
		std::terminate();
	}
}

void move_to(
	hb_draw_funcs_t*,
	void* draw_data,
	hb_draw_state_t*,
	float x,
	float y,
	void*
) {
	append_command(draw_data, GlyphPathOpcode::move_to, {x, y, 0, 0, 0, 0});
}

void line_to(
	hb_draw_funcs_t*,
	void* draw_data,
	hb_draw_state_t*,
	float x,
	float y,
	void*
) {
	append_command(draw_data, GlyphPathOpcode::line_to, {x, y, 0, 0, 0, 0});
}

void quadratic_to(
	hb_draw_funcs_t*,
	void* draw_data,
	hb_draw_state_t*,
	float control_x,
	float control_y,
	float x,
	float y,
	void*
) {
	append_command(
		draw_data,
		GlyphPathOpcode::quadratic_to,
		{control_x, control_y, x, y, 0, 0}
	);
}

void cubic_to(
	hb_draw_funcs_t*,
	void* draw_data,
	hb_draw_state_t*,
	float control1_x,
	float control1_y,
	float control2_x,
	float control2_y,
	float x,
	float y,
	void*
) {
	append_command(
		draw_data,
		GlyphPathOpcode::cubic_to,
		{control1_x, control1_y, control2_x, control2_y, x, y}
	);
}

void close_path(
	hb_draw_funcs_t*,
	void* draw_data,
	hb_draw_state_t*,
	void*
) {
	append_command(draw_data, GlyphPathOpcode::close_path);
}

bool all_finite(const std::vector<GlyphPathCommand>& commands) noexcept {
	for (const auto& command : commands) {
		for (const auto value : command.values) {
			if (!std::isfinite(value)) return false;
		}
	}
	return true;
}

}  // namespace

class GlyphOutlineCache::Impl {
public:
	explicit Impl(const FontProvider& provider) : provider_(provider) {
		draw_funcs_ = hb_draw_funcs_create();
		if (draw_funcs_ == hb_draw_funcs_get_empty()) std::terminate();
		hb_draw_funcs_set_move_to_func(draw_funcs_, move_to, nullptr, nullptr);
		hb_draw_funcs_set_line_to_func(draw_funcs_, line_to, nullptr, nullptr);
		hb_draw_funcs_set_quadratic_to_func(draw_funcs_, quadratic_to, nullptr, nullptr);
		hb_draw_funcs_set_cubic_to_func(draw_funcs_, cubic_to, nullptr, nullptr);
		hb_draw_funcs_set_close_path_func(draw_funcs_, close_path, nullptr, nullptr);
		hb_draw_funcs_make_immutable(draw_funcs_);
	}

	~Impl() {
		faces_.clear();
		hb_draw_funcs_destroy(draw_funcs_);
	}

	[[nodiscard]] Result<SharedGlyphPath> outline(
		FontId font_id,
		std::uint32_t glyph_id,
		std::int32_t font_size_26_6
	) {
		if (font_id == 0 || glyph_id == 0 || font_size_26_6 <= 0) {
			return Error{ErrorCode::invalid_argument, "glyph outline 參數不合法"};
		}
		refresh_revision();
		const OutlineKey key{font_id, glyph_id, font_size_26_6};
		if (const auto found = outlines_.find(key); found != outlines_.end()) {
			++hits_;
			return found->second;
		}
		auto opened = face(font_id);
		if (!opened.is_ok()) return opened.error();
		hb_font_set_scale(opened.value()->font, font_size_26_6, font_size_26_6);
		auto commands = std::make_shared<std::vector<GlyphPathCommand>>();
		hb_font_draw_glyph(
			opened.value()->font,
			glyph_id,
			draw_funcs_,
			commands.get()
		);
		if (!all_finite(*commands)) {
			return Error{ErrorCode::corrupt_data, "HarfBuzz 產生非有限字形座標"};
		}
		SharedGlyphPath immutable = commands;
		outlines_.emplace(key, immutable);
		++misses_;
		return immutable;
	}

	[[nodiscard]] GlyphOutlineCacheStats stats() const noexcept {
		return GlyphOutlineCacheStats{hits_, misses_, outlines_.size()};
	}

private:
	struct Face {
		hb_blob_t* blob = nullptr;
		hb_face_t* face = nullptr;
		hb_font_t* font = nullptr;

		~Face() {
			if (font != nullptr) hb_font_destroy(font);
			if (face != nullptr) hb_face_destroy(face);
			if (blob != nullptr) hb_blob_destroy(blob);
		}
	};

	void refresh_revision() {
		const auto current = provider_.font_set_revision();
		if (has_revision_ && current == revision_) return;
		faces_.clear();
		outlines_.clear();
		revision_ = current;
		has_revision_ = true;
	}

	[[nodiscard]] Result<Face*> face(FontId font_id) {
		if (const auto found = faces_.find(font_id); found != faces_.end()) {
			return found->second.get();
		}
		auto opened = provider_.open(font_id);
		if (!opened.is_ok()) return opened.error();
		const auto data = opened.value();
		if (data.bytes.empty() || data.bytes.size() > UINT_MAX) {
			return Error{ErrorCode::invalid_argument, "glyph outline 字型 bytes 不合法"};
		}
		auto owner = std::make_unique<Face>();
		owner->blob = hb_blob_create(
			reinterpret_cast<const char*>(data.bytes.data()),
			static_cast<unsigned int>(data.bytes.size()),
			HB_MEMORY_MODE_READONLY,
			nullptr,
			nullptr
		);
		owner->face = hb_face_create(owner->blob, data.face_index);
		owner->font = hb_font_create(owner->face);
		if (owner->blob == hb_blob_get_empty() || owner->face == hb_face_get_empty() ||
		    owner->font == hb_font_get_empty()) {
			return Error{ErrorCode::invalid_argument, "無法建立 glyph outline HarfBuzz 字型"};
		}
		hb_ot_font_set_funcs(owner->font);
		auto* result = owner.get();
		faces_.emplace(font_id, std::move(owner));
		return result;
	}

	const FontProvider& provider_;
	hb_draw_funcs_t* draw_funcs_ = nullptr;
	std::uint64_t revision_ = 0;
	bool has_revision_ = false;
	std::unordered_map<FontId, std::unique_ptr<Face>> faces_;
	std::unordered_map<OutlineKey, SharedGlyphPath, OutlineKeyHash> outlines_;
	std::size_t hits_ = 0;
	std::size_t misses_ = 0;
};

GlyphOutlineCache::GlyphOutlineCache(const FontProvider& provider)
	: impl_(std::make_unique<Impl>(provider)) {}

GlyphOutlineCache::~GlyphOutlineCache() = default;

Result<SharedGlyphPath> GlyphOutlineCache::outline(
	FontId font_id,
	std::uint32_t glyph_id,
	std::int32_t font_size_26_6
) {
	return impl_->outline(font_id, glyph_id, font_size_26_6);
}

GlyphOutlineCacheStats GlyphOutlineCache::stats() const noexcept {
	return impl_->stats();
}

}  // namespace krepis
