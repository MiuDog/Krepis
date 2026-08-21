#include "krepis/paragraph_record.hpp"

#include <cstddef>
#include <utility>

namespace krepis {
namespace {

[[nodiscard]] bool is_continuation(unsigned char value) noexcept {
	return value >= 0x80u && value <= 0xBFu;
}

[[nodiscard]] bool has_bytes(const std::string& text, std::size_t offset, std::size_t count) noexcept {
	return count <= text.size() - offset;
}

[[nodiscard]] bool is_valid_utf8(const std::string& text) noexcept {
	std::size_t offset = 0;
	while (offset < text.size()) {
		const auto first = static_cast<unsigned char>(text[offset]);
		if (first <= 0x7Fu) {
			++offset;
			continue;
		}

		// 步驟 1：驗證二位元組序列，並排除 overlong lead byte。
		if (first >= 0xC2u && first <= 0xDFu) {
			if (!has_bytes(text, offset, 2)) return false;
			if (!is_continuation(static_cast<unsigned char>(text[offset + 1]))) return false;

			offset += 2;
			continue;
		}

		// 步驟 2：驗證三位元組序列，排除 overlong 與 UTF-16 surrogate。
		if (first >= 0xE0u && first <= 0xEFu) {
			if (!has_bytes(text, offset, 3)) return false;

			const auto second = static_cast<unsigned char>(text[offset + 1]);
			const auto third = static_cast<unsigned char>(text[offset + 2]);
			if (!is_continuation(third)) return false;
			if (first == 0xE0u && (second < 0xA0u || second > 0xBFu)) return false;
			if (first == 0xEDu && (second < 0x80u || second > 0x9Fu)) return false;
			if (first != 0xE0u && first != 0xEDu && !is_continuation(second)) return false;

			offset += 3;
			continue;
		}

		// 步驟 3：驗證四位元組序列，限制 Unicode scalar 不超過 U+10FFFF。
		if (first >= 0xF0u && first <= 0xF4u) {
			if (!has_bytes(text, offset, 4)) return false;

			const auto second = static_cast<unsigned char>(text[offset + 1]);
			const auto third = static_cast<unsigned char>(text[offset + 2]);
			const auto fourth = static_cast<unsigned char>(text[offset + 3]);
			if (!is_continuation(third) || !is_continuation(fourth)) return false;
			if (first == 0xF0u && (second < 0x90u || second > 0xBFu)) return false;
			if (first == 0xF4u && (second < 0x80u || second > 0x8Fu)) return false;
			if (first != 0xF0u && first != 0xF4u && !is_continuation(second)) return false;

			offset += 4;
			continue;
		}

		return false;
	}

	return true;
}

}  // namespace

ParagraphRecord::ParagraphRecord(std::uint64_t content_revision, std::string utf8) noexcept
	: ObjectRecord(content_revision), utf8_(std::move(utf8)) {}

Result<IntrusivePtr<const ParagraphRecord>> ParagraphRecord::create(
	std::uint64_t content_revision,
	std::string utf8
) {
	if (!is_valid_utf8(utf8)) {
		return Error{ErrorCode::invalid_argument, "Paragraph 文字必須是合法 UTF-8"};
	}

	return make_intrusive<ParagraphRecord>(content_revision, std::move(utf8));
}

}  // namespace krepis
