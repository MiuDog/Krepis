#include "krepis/utf8.hpp"

namespace krepis {
namespace {

[[nodiscard]] bool is_continuation(unsigned char value) noexcept {
	return value >= 0x80u && value <= 0xBFu;
}

[[nodiscard]] bool has_bytes(
	std::string_view text,
	std::size_t offset,
	std::size_t count
) noexcept {
	return offset <= text.size() && count <= text.size() - offset;
}

}  // namespace

Result<void> validate_utf8(std::string_view text) noexcept {
	std::size_t offset = 0;
	while (offset < text.size()) {
		const auto first = static_cast<unsigned char>(text[offset]);
		if (first <= 0x7Fu) {
			++offset;
			continue;
		}

		if (first >= 0xC2u && first <= 0xDFu) {
			if (!has_bytes(text, offset, 2) ||
				!is_continuation(static_cast<unsigned char>(text[offset + 1]))) {
				return Error{ErrorCode::invalid_argument, "文字包含不合法 UTF-8"};
			}
			offset += 2;
			continue;
		}

		if (first >= 0xE0u && first <= 0xEFu) {
			if (!has_bytes(text, offset, 3)) {
				return Error{ErrorCode::invalid_argument, "文字包含截斷 UTF-8"};
			}

			const auto second = static_cast<unsigned char>(text[offset + 1]);
			const auto third = static_cast<unsigned char>(text[offset + 2]);
			const bool second_valid =
				(first == 0xE0u) ? (second >= 0xA0u && second <= 0xBFu) :
				(first == 0xEDu) ? (second >= 0x80u && second <= 0x9Fu) :
				is_continuation(second);
			if (!second_valid || !is_continuation(third)) {
				return Error{ErrorCode::invalid_argument, "文字包含不合法 UTF-8 scalar"};
			}
			offset += 3;
			continue;
		}

		if (first >= 0xF0u && first <= 0xF4u) {
			if (!has_bytes(text, offset, 4)) {
				return Error{ErrorCode::invalid_argument, "文字包含截斷 UTF-8"};
			}

			const auto second = static_cast<unsigned char>(text[offset + 1]);
			const auto third = static_cast<unsigned char>(text[offset + 2]);
			const auto fourth = static_cast<unsigned char>(text[offset + 3]);
			const bool second_valid =
				(first == 0xF0u) ? (second >= 0x90u && second <= 0xBFu) :
				(first == 0xF4u) ? (second >= 0x80u && second <= 0x8Fu) :
				is_continuation(second);
			if (!second_valid || !is_continuation(third) || !is_continuation(fourth)) {
				return Error{ErrorCode::invalid_argument, "文字超出 Unicode scalar 範圍"};
			}
			offset += 4;
			continue;
		}

		return Error{ErrorCode::invalid_argument, "文字包含不合法 UTF-8 lead byte"};
	}

	return {};
}

bool is_utf8_boundary(std::string_view text, std::size_t byte_offset) noexcept {
	if (byte_offset > text.size()) return false;
	if (byte_offset == text.size()) return true;
	return !is_continuation(static_cast<unsigned char>(text[byte_offset]));
}

}  // namespace krepis
