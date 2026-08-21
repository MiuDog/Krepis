#pragma once

// TXT-0001 Phase 2B：核心擁有 fallback 決策，平台只提供候選順序與穩定字型 bytes。

#include "krepis/error.hpp"
#include "krepis/text_analysis.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace krepis {

using FontId = std::uint64_t;

struct FontDataView {
	std::span<const std::byte> bytes;
	std::uint32_t face_index;
};

class FontProvider {
public:
	virtual ~FontProvider() = default;

	[[nodiscard]] virtual std::uint64_t font_set_revision() const noexcept = 0;
	[[nodiscard]] virtual std::vector<FontId> candidates(
		std::uint32_t script_tag,
		std::string_view language
	) const = 0;

	// 成功 view 的 bytes 必須保持有效，直到 font_set_revision 改變或 provider 被銷毀。
	// provider 不得在一次 TextShaper::shape 呼叫途中改變 revision 或使既有 view 失效。
	[[nodiscard]] virtual Result<FontDataView> open(FontId font_id) const = 0;
};

enum class GlyphDirection : std::uint8_t {
	ltr,
	rtl,
};

struct Glyph {
	std::uint32_t glyph_id;
	std::size_t cluster_byte_offset;
	std::int32_t x_advance;
	std::int32_t y_advance;
	std::int32_t x_offset;
	std::int32_t y_offset;
};

struct GlyphRun {
	FontId font_id;
	std::size_t byte_offset;
	std::size_t byte_length;
	GlyphDirection direction;
	std::uint32_t script_tag;
	std::vector<Glyph> glyphs;
};

struct ShapedParagraph {
	TextAnalysis analysis;
	std::vector<GlyphRun> glyph_runs;
};

class TextShaper {
public:
	explicit TextShaper(const FontProvider& provider);
	~TextShaper();

	TextShaper(const TextShaper&) = delete;
	TextShaper& operator=(const TextShaper&) = delete;
	TextShaper(TextShaper&&) noexcept;
	TextShaper& operator=(TextShaper&&) noexcept;

	// 同一 instance 會修改 coverage cache，不可由多執行緒同時呼叫。
	[[nodiscard]] Result<ShapedParagraph> shape(
		std::string_view utf8,
		std::string_view language,
		BaseDirection base_direction,
		std::int32_t font_size_26_6
	);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace krepis
