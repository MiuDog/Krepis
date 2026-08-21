#include "krepis/display_list.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>

namespace krepis {
namespace {

constexpr std::uint32_t display_magic = 0x4C44524B;
constexpr std::uint16_t display_major = 1;
constexpr std::uint16_t display_minor = 0;
constexpr std::size_t frame_header_size = 32;
constexpr std::size_t command_header_size = 8;
constexpr std::size_t maximum_frame_bytes = 256 * 1024 * 1024;
constexpr std::uint32_t maximum_commands = 10'000'000;
constexpr std::uint32_t maximum_glyphs = 10'000'000;

template <typename Integer>
void append_little(std::vector<std::byte>& bytes, Integer value) {
	static_assert(std::is_integral_v<Integer> && sizeof(Integer) <= sizeof(std::uint64_t));
	using Unsigned = std::make_unsigned_t<Integer>;
	const auto bits = static_cast<Unsigned>(value);
	for (std::size_t i = 0; i < sizeof(Integer); ++i) {
		bytes.push_back(static_cast<std::byte>((bits >> (i * 8)) & 0xFF));
	}
}

template <typename Integer>
void patch_little(std::vector<std::byte>& bytes, std::size_t offset, Integer value) {
	using Unsigned = std::make_unsigned_t<Integer>;
	const auto bits = static_cast<Unsigned>(value);
	for (std::size_t i = 0; i < sizeof(Integer); ++i) {
		bytes[offset + i] = static_cast<std::byte>((bits >> (i * 8)) & 0xFF);
	}
}

void patch_float(std::vector<std::byte>& bytes, std::size_t offset, float value) {
	patch_little(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

class Reader {
public:
	explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

	template <typename Integer>
	[[nodiscard]] bool little(std::size_t offset, Integer& out) const noexcept {
		static_assert(std::is_integral_v<Integer>);
		if (offset > bytes_.size() || bytes_.size() - offset < sizeof(Integer)) return false;
		using Unsigned = std::make_unsigned_t<Integer>;
		Unsigned bits = 0;
		for (std::size_t i = 0; i < sizeof(Integer); ++i) {
			bits |= static_cast<Unsigned>(std::to_integer<unsigned int>(bytes_[offset + i]))
			        << (i * 8);
		}
		out = static_cast<Integer>(bits);
		return true;
	}

	[[nodiscard]] bool floating(std::size_t offset, float& out) const noexcept {
		std::uint32_t bits = 0;
		if (!little(offset, bits)) return false;
		out = std::bit_cast<float>(bits);
		return true;
	}

private:
	std::span<const std::byte> bytes_;
};

bool finite_rect(float x, float y, float width, float height) noexcept {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
	       std::isfinite(height) && width >= 0 && height >= 0;
}

Result<void> validate_rect_payload(const Reader& reader, std::size_t payload) {
	float x = 0;
	float y = 0;
	float width = 0;
	float height = 0;
	if (!reader.floating(payload, x) || !reader.floating(payload + 4, y) ||
	    !reader.floating(payload + 8, width) ||
	    !reader.floating(payload + 12, height) ||
	    !finite_rect(x, y, width, height)) {
		return Error{ErrorCode::corrupt_data, "display rect payload 不合法"};
	}
	return {};
}

}  // namespace

DisplayListBuilder::DisplayListBuilder() {
	reset();
}

void DisplayListBuilder::reset() {
	bytes_.clear();
	bytes_.resize(frame_header_size, std::byte{0});
	command_count_ = 0;
}

Result<std::size_t> DisplayListBuilder::append_command(
	DisplayOpcode opcode,
	std::size_t payload_size
) {
	if (command_count_ >= maximum_commands ||
	    payload_size > maximum_frame_bytes - command_header_size) {
		return Error{ErrorCode::out_of_range, "display command 超過格式上限"};
	}
	const auto raw_size = command_header_size + payload_size;
	const auto aligned_size = (raw_size + 7) & ~std::size_t{7};
	if (bytes_.size() > maximum_frame_bytes - aligned_size) {
		return Error{ErrorCode::out_of_range, "display frame 超過格式上限"};
	}
	const auto command_offset = bytes_.size();
	append_little(bytes_, static_cast<std::uint16_t>(opcode));
	append_little(bytes_, std::uint16_t{0});
	append_little(bytes_, static_cast<std::uint32_t>(aligned_size));
	bytes_.resize(command_offset + aligned_size, std::byte{0});
	++command_count_;
	return command_offset + command_header_size;
}

Result<void> DisplayListBuilder::add_rect(
	float x,
	float y,
	float width,
	float height,
	std::uint32_t color_rgba
) {
	if (!finite_rect(x, y, width, height)) {
		return Error{ErrorCode::invalid_argument, "DrawRect 參數不合法"};
	}
	auto payload = append_command(DisplayOpcode::draw_rect, 20);
	if (!payload.is_ok()) return payload.error();
	patch_float(bytes_, payload.value(), x);
	patch_float(bytes_, payload.value() + 4, y);
	patch_float(bytes_, payload.value() + 8, width);
	patch_float(bytes_, payload.value() + 12, height);
	patch_little(bytes_, payload.value() + 16, color_rgba);
	return {};
}

Result<void> DisplayListBuilder::add_glyph_run(
	std::int32_t baseline_x_26_6,
	std::int32_t baseline_y_26_6,
	std::int32_t font_size_26_6,
	std::uint32_t color_rgba,
	FontId font_id,
	GlyphDirection direction,
	std::span<const Glyph> glyphs
) {
	if (font_size_26_6 <= 0 || font_id == 0 || glyphs.empty() ||
	    (direction != GlyphDirection::ltr && direction != GlyphDirection::rtl) ||
	    glyphs.size() > maximum_glyphs) {
		return Error{ErrorCode::invalid_argument, "DrawGlyphRun 參數不合法"};
	}
	for (const auto& glyph : glyphs) {
		if (glyph.cluster_byte_offset > std::numeric_limits<std::uint32_t>::max()) {
			return Error{ErrorCode::out_of_range, "glyph cluster offset 超過 wire format"};
		}
	}
	auto payload = append_command(DisplayOpcode::draw_glyph_run, 32 + glyphs.size() * 24);
	if (!payload.is_ok()) return payload.error();
	patch_little(bytes_, payload.value(), font_id);
	patch_little(bytes_, payload.value() + 8, baseline_x_26_6);
	patch_little(bytes_, payload.value() + 12, baseline_y_26_6);
	patch_little(bytes_, payload.value() + 16, font_size_26_6);
	patch_little(bytes_, payload.value() + 20, color_rgba);
	patch_little(bytes_, payload.value() + 24, static_cast<std::uint32_t>(glyphs.size()));
	patch_little(bytes_, payload.value() + 28, static_cast<std::uint32_t>(direction));
	auto glyph_offset = payload.value() + 32;
	for (const auto& glyph : glyphs) {
		patch_little(bytes_, glyph_offset, glyph.glyph_id);
		patch_little(bytes_, glyph_offset + 4, glyph.x_advance);
		patch_little(bytes_, glyph_offset + 8, glyph.y_advance);
		patch_little(bytes_, glyph_offset + 12, glyph.x_offset);
		patch_little(bytes_, glyph_offset + 16, glyph.y_offset);
		patch_little(
			bytes_,
			glyph_offset + 20,
			static_cast<std::uint32_t>(glyph.cluster_byte_offset)
		);
		glyph_offset += 24;
	}
	return {};
}

Result<void> DisplayListBuilder::push_clip(float x, float y, float width, float height) {
	if (!finite_rect(x, y, width, height)) {
		return Error{ErrorCode::invalid_argument, "PushClip 參數不合法"};
	}
	auto payload = append_command(DisplayOpcode::push_clip, 16);
	if (!payload.is_ok()) return payload.error();
	patch_float(bytes_, payload.value(), x);
	patch_float(bytes_, payload.value() + 4, y);
	patch_float(bytes_, payload.value() + 8, width);
	patch_float(bytes_, payload.value() + 12, height);
	return {};
}

Result<void> DisplayListBuilder::pop_clip() {
	auto command = append_command(DisplayOpcode::pop_clip, 0);
	return command.is_ok() ? Result<void>{} : Result<void>{command.error()};
}

Result<void> DisplayListBuilder::push_transform(const std::array<float, 6>& affine) {
	if (!std::all_of(affine.begin(), affine.end(), [](float value) {
		return std::isfinite(value);
	})) {
		return Error{ErrorCode::invalid_argument, "PushTransform 包含非有限值"};
	}
	auto payload = append_command(DisplayOpcode::push_transform, 24);
	if (!payload.is_ok()) return payload.error();
	for (std::size_t i = 0; i < affine.size(); ++i) {
		patch_float(bytes_, payload.value() + i * 4, affine[i]);
	}
	return {};
}

Result<void> DisplayListBuilder::pop_transform() {
	auto command = append_command(DisplayOpcode::pop_transform, 0);
	return command.is_ok() ? Result<void>{} : Result<void>{command.error()};
}

void DisplayListBuilder::finalize(std::uint64_t frame_token) {
	patch_little(bytes_, 0, display_magic);
	patch_little(bytes_, 4, display_major);
	patch_little(bytes_, 6, display_minor);
	patch_little(bytes_, 8, static_cast<std::uint32_t>(frame_header_size));
	patch_little(bytes_, 12, static_cast<std::uint32_t>(bytes_.size()));
	patch_little(bytes_, 16, command_count_);
	patch_little(bytes_, 20, std::uint32_t{0});
	patch_little(bytes_, 24, frame_token);
}

Result<DisplayListSummary> validate_display_list(
	std::span<const std::byte> bytes,
	std::uint16_t supported_major,
	std::uint16_t supported_minor
) {
	if (bytes.size() < frame_header_size || bytes.size() > maximum_frame_bytes) {
		return Error{ErrorCode::corrupt_data, "display frame 大小不合法"};
	}
	Reader reader(bytes);
	std::uint32_t magic = 0;
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint32_t header_size = 0;
	std::uint32_t byte_size = 0;
	std::uint32_t command_count = 0;
	std::uint32_t flags = 0;
	std::uint64_t frame_token = 0;
	if (!reader.little(0, magic) || !reader.little(4, major) ||
	    !reader.little(6, minor) || !reader.little(8, header_size) ||
	    !reader.little(12, byte_size) || !reader.little(16, command_count) ||
	    !reader.little(20, flags) || !reader.little(24, frame_token)) {
		return Error{ErrorCode::corrupt_data, "display frame header 截斷"};
	}
	if (magic != display_magic || header_size != frame_header_size || flags != 0 ||
	    byte_size != bytes.size() || command_count > maximum_commands || frame_token == 0) {
		return Error{ErrorCode::corrupt_data, "display frame header invariant 失敗"};
	}
	if (major != supported_major || minor > supported_minor) {
		return Error{ErrorCode::version_mismatch, "display frame 版本不支援"};
	}
	std::size_t offset = frame_header_size;
	std::uint32_t clip_depth = 0;
	std::uint32_t transform_depth = 0;
	for (std::uint32_t index = 0; index < command_count; ++index) {
		std::uint16_t opcode_value = 0;
		std::uint16_t command_flags = 0;
		std::uint32_t command_size = 0;
		if (!reader.little(offset, opcode_value) ||
		    !reader.little(offset + 2, command_flags) ||
		    !reader.little(offset + 4, command_size) || command_flags != 0 ||
		    command_size < command_header_size || command_size % 8 != 0 ||
		    offset > bytes.size() || command_size > bytes.size() - offset) {
			return Error{ErrorCode::corrupt_data, "display command header 不合法"};
		}
		const auto opcode = static_cast<DisplayOpcode>(opcode_value);
		const auto payload = offset + command_header_size;
		std::size_t used_size = command_header_size;
		if (opcode == DisplayOpcode::draw_rect) {
			if (command_size != 32 || !validate_rect_payload(reader, payload).is_ok()) {
				return Error{ErrorCode::corrupt_data, "DrawRect payload 不合法"};
			}
			used_size = 28;
		} else if (opcode == DisplayOpcode::push_clip) {
			if (command_size != 24 || !validate_rect_payload(reader, payload).is_ok()) {
				return Error{ErrorCode::corrupt_data, "PushClip payload 不合法"};
			}
			used_size = 24;
			++clip_depth;
		} else if (opcode == DisplayOpcode::pop_clip) {
			if (command_size != 8 || clip_depth == 0) {
				return Error{ErrorCode::corrupt_data, "PopClip stack underflow"};
			}
			--clip_depth;
		} else if (opcode == DisplayOpcode::push_transform) {
			if (command_size != 32) {
				return Error{ErrorCode::corrupt_data, "PushTransform size 不合法"};
			}
			for (std::size_t i = 0; i < 6; ++i) {
				float value = 0;
				if (!reader.floating(payload + i * 4, value) || !std::isfinite(value)) {
					return Error{ErrorCode::corrupt_data, "PushTransform 值不合法"};
				}
			}
			used_size = 32;
			++transform_depth;
		} else if (opcode == DisplayOpcode::pop_transform) {
			if (command_size != 8 || transform_depth == 0) {
				return Error{ErrorCode::corrupt_data, "PopTransform stack underflow"};
			}
			--transform_depth;
		} else if (opcode == DisplayOpcode::draw_glyph_run) {
			std::uint64_t font_id = 0;
			std::int32_t font_size = 0;
			std::uint32_t glyph_count = 0;
			std::uint32_t direction = 0;
			if (command_size < 40 || !reader.little(payload, font_id) ||
			    !reader.little(payload + 16, font_size) ||
			    !reader.little(payload + 24, glyph_count) ||
			    !reader.little(payload + 28, direction) || font_id == 0 || font_size <= 0 ||
			    glyph_count == 0 || glyph_count > maximum_glyphs || direction > 1 ||
			    glyph_count > (std::numeric_limits<std::uint32_t>::max() - 40) / 24 ||
			    command_size != 40 + glyph_count * 24) {
				return Error{ErrorCode::corrupt_data, "DrawGlyphRun payload 不合法"};
			}
			used_size = command_size;
		} else {
			return Error{ErrorCode::corrupt_data, "display opcode 未知"};
		}
		for (std::size_t padding = used_size; padding < command_size; ++padding) {
			if (bytes[offset + padding] != std::byte{0}) {
				return Error{ErrorCode::corrupt_data, "display command padding 非零"};
			}
		}
		offset += command_size;
	}
	if (offset != bytes.size() || clip_depth != 0 || transform_depth != 0) {
		return Error{ErrorCode::corrupt_data, "display command count、長度或 stack 不平衡"};
	}
	return DisplayListSummary{frame_token, command_count, bytes.size()};
}

Result<DisplayListBuilder*> DisplayListPublisher::begin_frame() {
	if (building_ != no_slot) return &slots_[building_].builder;
	std::size_t target = no_slot;
	for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
		if (slot != front_ && slots_[slot].lease_count == 0) {
			target = slot;
			break;
		}
	}
	if (target == no_slot) {
		return Error{ErrorCode::invalid_state, "兩個 display slots 都仍被 lease"};
	}
	building_ = target;
	slots_[building_].builder.reset();
	return &slots_[building_].builder;
}

Result<std::uint64_t> DisplayListPublisher::publish() {
	if (building_ == no_slot) {
		return Error{ErrorCode::invalid_state, "尚未 begin display frame"};
	}
	auto& builder = slots_[building_].builder;
	if (builder.bytes_.size() > maximum_frame_bytes ||
	    builder.bytes_.size() > std::numeric_limits<std::uint32_t>::max()) {
		return Error{ErrorCode::out_of_range, "display frame 超過格式上限"};
	}
	const auto token = next_frame_token_;
	builder.finalize(token);
	auto valid = validate_display_list(builder.bytes_);
	if (!valid.is_ok()) return valid.error();
	slots_[building_].frame_token = token;
	front_ = building_;
	building_ = no_slot;
	++next_frame_token_;
	++stats_.published_frames;
	return token;
}

DisplayListPublisher::~DisplayListPublisher() noexcept {
	if (!leases_.empty()) std::terminate();
}

Result<DisplayListLease> DisplayListPublisher::acquire_front() {
	if (front_ == no_slot) return Error{ErrorCode::invalid_state, "尚未發布 display frame"};
	const auto lease_id = next_lease_id_++;
	leases_.emplace(lease_id, front_);
	++slots_[front_].lease_count;
	++stats_.acquired_leases;
	stats_.outstanding_leases = leases_.size();
	const auto& slot = slots_[front_];
	return DisplayListLease{
		slot.builder.bytes_.data(),
		slot.builder.bytes_.size(),
		slot.frame_token,
		lease_id,
	};
}

std::size_t DisplayListPublisher::retained_capacity() const noexcept {
	return slots_[0].builder.retained_capacity() + slots_[1].builder.retained_capacity();
}

Result<void> DisplayListPublisher::release(std::uint64_t lease_id) {
	const auto found = leases_.find(lease_id);
	if (found == leases_.end()) {
		return Error{ErrorCode::invalid_argument, "display lease 不存在或已釋放"};
	}
	auto& slot = slots_[found->second];
	if (slot.lease_count == 0) {
		return Error{ErrorCode::invalid_state, "display lease count underflow"};
	}
	--slot.lease_count;
	leases_.erase(found);
	++stats_.released_leases;
	stats_.outstanding_leases = leases_.size();
	return {};
}

}  // namespace krepis
