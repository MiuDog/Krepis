#pragma once

// TXT-0001 Phase 2C：以合法斷點切行，再逐行 bidi reorder 與 reshaping。

#include "krepis/text_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace krepis {

struct LineFragment {
	std::size_t byte_offset;
	std::size_t byte_length;
	std::int32_t baseline_y_26_6;
	std::int32_t width_26_6;
	std::int32_t height_26_6;
	std::size_t glyph_run_offset;
	std::size_t glyph_run_count;
	bool overflow;
};

struct CaretStop {
	std::size_t byte_offset;
	std::size_t line_index;
	std::int32_t x_26_6;
};

struct ParagraphLayout {
	std::uint64_t source_revision;
	std::int32_t width_26_6;
	std::int32_t total_height_26_6;
	std::vector<LineFragment> lines;
	std::vector<GlyphRun> glyph_runs;
	std::vector<CaretStop> caret_stops;
};

class ParagraphLayouter {
public:
	explicit ParagraphLayouter(const FontProvider& provider);

	[[nodiscard]] Result<ParagraphLayout> layout(
		std::string_view utf8,
		std::string_view language,
		BaseDirection base_direction,
		std::uint64_t source_revision,
		std::int32_t font_size_26_6,
		std::int32_t width_26_6,
		std::int32_t line_height_26_6
	);

private:
	TextShaper shaper_;
};

struct LayoutCacheStats {
	std::size_t hits;
	std::size_t misses;
	std::size_t evictions;
	std::size_t size;
	std::size_t capacity;
};

class CachedParagraphLayouter {
public:
	static constexpr std::size_t default_capacity = 256;

	CachedParagraphLayouter(
		const FontProvider& provider,
		std::size_t capacity = default_capacity
	);
	~CachedParagraphLayouter();

	CachedParagraphLayouter(const CachedParagraphLayouter&) = delete;
	CachedParagraphLayouter& operator=(const CachedParagraphLayouter&) = delete;

	[[nodiscard]] Result<ParagraphLayout> layout(
		std::string_view utf8,
		std::string_view language,
		BaseDirection base_direction,
		std::uint64_t source_revision,
		std::uint64_t composition_revision,
		std::uint64_t feature_set_revision,
		std::int32_t font_size_26_6,
		std::int32_t width_26_6,
		std::int32_t line_height_26_6
	);

	[[nodiscard]] LayoutCacheStats cache_stats() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace krepis
