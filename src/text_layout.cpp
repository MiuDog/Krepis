#include "krepis/text_layout.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace krepis {
namespace {

[[nodiscard]] std::int32_t narrow_advance(std::int64_t value) noexcept {
	if (value > std::numeric_limits<std::int32_t>::max()) {
		return std::numeric_limits<std::int32_t>::max();
	}
	return static_cast<std::int32_t>(value);
}

[[nodiscard]] std::vector<std::int64_t> grapheme_advances(
	const TextAnalysis& analysis,
	const std::vector<GlyphRun>& runs
) {
	const auto count = analysis.grapheme_boundaries.empty()
		? 0
		: analysis.grapheme_boundaries.size() - 1;
	std::vector<std::int64_t> result(count, 0);
	for (const auto& run : runs) {
		std::map<std::size_t, std::int64_t> clusters;
		for (const auto& glyph : run.glyphs) {
			clusters[glyph.cluster_byte_offset] += std::llabs(glyph.x_advance);
		}
		for (auto current = clusters.begin(); current != clusters.end(); ++current) {
			const auto next = std::next(current);
			const auto cluster_limit = next == clusters.end()
				? run.byte_offset + run.byte_length
				: next->first;
			std::vector<std::size_t> covered;
			for (std::size_t index = 0; index < count; ++index) {
				const auto begin = analysis.grapheme_boundaries[index];
				const auto end = analysis.grapheme_boundaries[index + 1];
				if (begin >= current->first && end <= cluster_limit) covered.push_back(index);
			}
			if (covered.empty()) continue;
			const auto share = current->second / static_cast<std::int64_t>(covered.size());
			auto remainder = current->second % static_cast<std::int64_t>(covered.size());
			for (const auto index : covered) {
				result[index] += share + (remainder-- > 0 ? 1 : 0);
			}
		}
	}
	return result;
}

[[nodiscard]] std::int64_t range_width(
	const TextAnalysis& analysis,
	const std::vector<std::int64_t>& advances,
	std::size_t begin,
	std::size_t end
) noexcept {
	std::int64_t result = 0;
	for (std::size_t index = 0; index < advances.size(); ++index) {
		if (analysis.grapheme_boundaries[index] >= begin &&
		    analysis.grapheme_boundaries[index + 1] <= end) {
			result += advances[index];
		}
	}
	return result;
}

struct BreakPoint {
	std::size_t byte_offset;
	bool mandatory;
};

[[nodiscard]] std::vector<BreakPoint> approved_breaks(
	std::size_t text_size,
	const TextAnalysis& analysis
) {
	std::vector<BreakPoint> result;
	for (const auto& point : analysis.line_breaks) {
		if (!std::binary_search(
			analysis.grapheme_boundaries.begin(),
			analysis.grapheme_boundaries.end(),
			point.byte_offset
		)) {
			continue;
		}
		result.push_back(BreakPoint{
			point.byte_offset,
			point.kind == BreakKind::mandatory,
		});
	}
	if (result.empty() || result.back().byte_offset != text_size) {
		result.push_back(BreakPoint{text_size, false});
	}
	std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		return left.byte_offset < right.byte_offset;
	});
	std::vector<BreakPoint> merged;
	for (const auto& point : result) {
		if (!merged.empty() && merged.back().byte_offset == point.byte_offset) {
			merged.back().mandatory = merged.back().mandatory || point.mandatory;
		} else {
			merged.push_back(point);
		}
	}
	result = std::move(merged);
	return result;
}

[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> choose_lines(
	const TextAnalysis& analysis,
	const std::vector<std::int64_t>& advances,
	std::size_t text_size,
	std::int32_t width_26_6
) {
	if (text_size == 0) return {{0, 0}};
	const auto breaks = approved_breaks(text_size, analysis);
	std::vector<std::pair<std::size_t, std::size_t>> result;
	std::size_t line_start = 0;
	std::size_t prior_break = 0;
	std::int64_t line_width = 0;
	for (const auto& point : breaks) {
		const auto unit_width = range_width(analysis, advances, prior_break, point.byte_offset);
		if (prior_break > line_start && line_width + unit_width > width_26_6) {
			result.emplace_back(line_start, prior_break);
			line_start = prior_break;
			line_width = 0;
		}
		line_width += unit_width;
		prior_break = point.byte_offset;
		if (point.mandatory) {
			result.emplace_back(line_start, point.byte_offset);
			line_start = point.byte_offset;
			line_width = 0;
		}
	}
	if (line_start < text_size) result.emplace_back(line_start, text_size);
	return result;
}

void append_carets(
	const TextAnalysis& analysis,
	const std::vector<std::int64_t>& advances,
	std::size_t line_index,
	std::vector<CaretStop>& output
) {
	std::int64_t run_x = 0;
	for (const auto& bidi : analysis.bidi_runs) {
		std::vector<std::size_t> graphemes;
		const auto limit = bidi.byte_offset + bidi.byte_length;
		for (std::size_t index = 0; index < advances.size(); ++index) {
			if (analysis.grapheme_boundaries[index] >= bidi.byte_offset &&
			    analysis.grapheme_boundaries[index + 1] <= limit) {
				graphemes.push_back(index);
			}
		}
		if (bidi.embedding_level % 2u != 0u) std::reverse(graphemes.begin(), graphemes.end());
		for (const auto index : graphemes) {
			const auto begin = analysis.grapheme_boundaries[index];
			const auto end = analysis.grapheme_boundaries[index + 1];
			if (bidi.embedding_level % 2u == 0u) {
				output.push_back(CaretStop{begin, line_index, narrow_advance(run_x)});
				run_x += advances[index];
				output.push_back(CaretStop{end, line_index, narrow_advance(run_x)});
			} else {
				output.push_back(CaretStop{end, line_index, narrow_advance(run_x)});
				run_x += advances[index];
				output.push_back(CaretStop{begin, line_index, narrow_advance(run_x)});
			}
		}
	}
}

}  // namespace

ParagraphLayouter::ParagraphLayouter(const FontProvider& provider) : shaper_(provider) {}

Result<ParagraphLayout> ParagraphLayouter::layout(
	std::string_view utf8,
	std::string_view language,
	BaseDirection base_direction,
	std::uint64_t source_revision,
	std::int32_t font_size_26_6,
	std::int32_t width_26_6,
	std::int32_t line_height_26_6
) {
	if (width_26_6 <= 0 || line_height_26_6 <= 0) {
		return Error{ErrorCode::invalid_argument, "layout width 與 line height 必須大於零"};
	}
	auto provisional = shaper_.shape(utf8, language, base_direction, font_size_26_6);
	if (!provisional.is_ok()) return provisional.error();
	const auto provisional_advances = grapheme_advances(
		provisional.value().analysis,
		provisional.value().glyph_runs
	);
	auto ranges = choose_lines(
		provisional.value().analysis,
		provisional_advances,
		utf8.size(),
		width_26_6
	);
	const auto break_points = approved_breaks(utf8.size(), provisional.value().analysis);

	ParagraphLayout result{source_revision, width_26_6, 0, {}, {}, {}};
	std::int64_t block_y = 0;
	for (std::size_t range_index = 0; range_index < ranges.size();) {
		const auto begin = ranges[range_index].first;
		const auto end = ranges[range_index].second;
		auto visible_end = end;
		while (visible_end > begin &&
		       (utf8[visible_end - 1] == '\n' || utf8[visible_end - 1] == '\r')) {
			--visible_end;
		}
		if (visible_end == begin) {
			const auto line_index = result.lines.size();
			result.lines.push_back(LineFragment{
				begin,
				end - begin,
				narrow_advance(block_y + font_size_26_6),
				0,
				line_height_26_6,
				result.glyph_runs.size(),
				0,
				false,
			});
			block_y += line_height_26_6;
			result.caret_stops.push_back(CaretStop{begin, line_index, 0});
			if (end != begin) result.caret_stops.push_back(CaretStop{end, line_index, 0});
			++range_index;
			continue;
		}
		auto shaped_line = shaper_.shape_line(
			utf8,
			provisional.value().analysis,
			language,
			base_direction,
			font_size_26_6,
			begin,
			visible_end - begin
		);
		if (!shaped_line.is_ok()) return shaped_line.error();
		const auto final_advances = grapheme_advances(
			shaped_line.value().analysis,
			shaped_line.value().glyph_runs
		);
		const auto final_width = range_width(
			shaped_line.value().analysis,
			final_advances,
			begin,
			visible_end
		);
		if (final_width > width_26_6) {
			auto split = begin;
			for (const auto& point : break_points) {
				if (point.byte_offset > begin && point.byte_offset < visible_end) {
					split = point.byte_offset;
				}
			}
			if (split != begin) {
				const auto end_is_mandatory = std::any_of(
					break_points.begin(),
					break_points.end(),
					[end](const auto& point) {
						return point.byte_offset == end && point.mandatory;
					}
				);
				ranges[range_index].second = split;
				if (!end_is_mandatory && range_index + 1 < ranges.size()) {
					ranges[range_index + 1].first = split;
				} else {
					ranges.insert(
						ranges.begin() + static_cast<std::ptrdiff_t>(range_index + 1),
						{split, end}
					);
				}
				continue;
			}
		}
		const auto run_offset = result.glyph_runs.size();
		std::int32_t ascender = 0;
		std::int32_t descender = 0;
		std::int32_t line_gap = 0;
		for (auto& run : shaped_line.value().glyph_runs) {
			ascender = std::max(ascender, run.ascender_26_6);
			descender = std::min(descender, run.descender_26_6);
			line_gap = std::max(line_gap, run.line_gap_26_6);
			result.glyph_runs.push_back(std::move(run));
		}
		const auto natural_height = static_cast<std::int64_t>(ascender) - descender + line_gap;
		const auto actual_height = std::max<std::int64_t>(line_height_26_6, natural_height);
		const auto leading = actual_height - natural_height;
		const auto baseline = block_y + leading / 2 + ascender;
		const auto line_index = result.lines.size();
		result.lines.push_back(LineFragment{
			begin,
			end - begin,
			narrow_advance(baseline),
			narrow_advance(final_width),
			narrow_advance(actual_height),
			run_offset,
			result.glyph_runs.size() - run_offset,
			final_width > width_26_6,
		});
		block_y += actual_height;
		append_carets(
			shaped_line.value().analysis,
			final_advances,
			line_index,
			result.caret_stops
		);
		if (visible_end != end) {
			result.caret_stops.push_back(CaretStop{
				end,
				line_index,
				narrow_advance(final_width),
			});
		}
		++range_index;
	}
	result.total_height_26_6 = narrow_advance(block_y);
	return result;
}

class CachedParagraphLayouter::Impl {
public:
	Impl(const FontProvider& provider, std::size_t capacity)
		: provider_(provider), layouter_(provider), capacity_(capacity) {}

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
	) {
		Key key{
			std::string(utf8),
			std::string(language),
			base_direction,
			source_revision,
			composition_revision,
			feature_set_revision,
			provider_.font_set_revision(),
			font_size_26_6,
			width_26_6,
			line_height_26_6,
		};
		for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
			if (entry->key == key) {
				++hits_;
				entries_.splice(entries_.begin(), entries_, entry);
				return entries_.front().layout;
			}
		}

		++misses_;
		auto computed = layouter_.layout(
			utf8,
			language,
			base_direction,
			source_revision,
			font_size_26_6,
			width_26_6,
			line_height_26_6
		);
		if (!computed.is_ok()) return computed.error();
		if (capacity_ > 0) {
			entries_.push_front(Entry{std::move(key), computed.value()});
			if (entries_.size() > capacity_) {
				entries_.pop_back();
				++evictions_;
			}
		}
		return computed.value();
	}

	[[nodiscard]] LayoutCacheStats stats() const noexcept {
		return LayoutCacheStats{hits_, misses_, evictions_, entries_.size(), capacity_};
	}

private:
	struct Key {
		std::string utf8;
		std::string language;
		BaseDirection base_direction;
		std::uint64_t source_revision;
		std::uint64_t composition_revision;
		std::uint64_t feature_set_revision;
		std::uint64_t font_set_revision;
		std::int32_t font_size_26_6;
		std::int32_t width_26_6;
		std::int32_t line_height_26_6;

		[[nodiscard]] bool operator==(const Key&) const = default;
	};

	struct Entry {
		Key key;
		ParagraphLayout layout;
	};

	const FontProvider& provider_;
	ParagraphLayouter layouter_;
	std::size_t capacity_;
	std::size_t hits_ = 0;
	std::size_t misses_ = 0;
	std::size_t evictions_ = 0;
	std::list<Entry> entries_;
};

CachedParagraphLayouter::CachedParagraphLayouter(
	const FontProvider& provider,
	std::size_t capacity
) : impl_(std::make_unique<Impl>(provider, capacity)) {}

CachedParagraphLayouter::~CachedParagraphLayouter() = default;

Result<ParagraphLayout> CachedParagraphLayouter::layout(
	std::string_view utf8,
	std::string_view language,
	BaseDirection base_direction,
	std::uint64_t source_revision,
	std::uint64_t composition_revision,
	std::uint64_t feature_set_revision,
	std::int32_t font_size_26_6,
	std::int32_t width_26_6,
	std::int32_t line_height_26_6
) {
	return impl_->layout(
		utf8,
		language,
		base_direction,
		source_revision,
		composition_revision,
		feature_set_revision,
		font_size_26_6,
		width_26_6,
		line_height_26_6
	);
}

LayoutCacheStats CachedParagraphLayouter::cache_stats() const noexcept {
	return impl_->stats();
}

}  // namespace krepis
