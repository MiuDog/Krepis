#include "krepis/flow_editor.hpp"

#include "krepis/display_list_layout.hpp"
#include "krepis/paragraph_record.hpp"
#include "krepis/text_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace krepis {
namespace {

Result<std::int32_t> to_26_6(double value) {
	if (!std::isfinite(value)) {
		return Error{ErrorCode::invalid_argument, "viewport 座標不得為 NaN 或 infinity"};
	}
	const auto scaled = std::round(value * 64.0);
	if (scaled < std::numeric_limits<std::int32_t>::min() ||
	    scaled > std::numeric_limits<std::int32_t>::max()) {
		return Error{ErrorCode::out_of_range, "viewport 座標超出 26.6 範圍"};
	}
	return static_cast<std::int32_t>(scaled);
}

const CaretStop* find_caret(const ParagraphLayout& layout, std::size_t byte_offset) {
	const auto found = std::find_if(
		layout.caret_stops.begin(),
		layout.caret_stops.end(),
		[byte_offset](const auto& stop) { return stop.byte_offset == byte_offset; }
	);
	return found == layout.caret_stops.end() ? nullptr : &*found;
}

double line_top(const ParagraphLayout& layout, std::size_t line_index) {
	double result = 0;
	for (std::size_t index = 0; index < line_index; ++index) {
		result += static_cast<double>(layout.lines[index].height_26_6) / 64.0;
	}
	return result;
}

}  // namespace

Result<std::uint64_t> FlowEditor::publish_display(
	DisplayListPublisher& publisher,
	double viewport_width,
	double viewport_height,
	double scroll_y
) {
	if (!std::isfinite(viewport_width) || !std::isfinite(viewport_height) ||
	    !std::isfinite(scroll_y) || viewport_width <= style_.horizontal_padding * 2.0 ||
	    viewport_height <= 0 || scroll_y < 0) {
		return Error{ErrorCode::invalid_argument, "FlowEditor viewport 不合法"};
	}
	auto width = to_26_6(viewport_width - style_.horizontal_padding * 2.0);
	if (!width.is_ok()) return width.error();
	auto origin_x = to_26_6(style_.horizontal_padding);
	if (!origin_x.is_ok()) return origin_x.error();
	auto begun = publisher.begin_frame();
	if (!begun.is_ok()) return begun.error();
	auto& builder = *begun.value();
	builder.reset();

	const auto* sequence = revision_.flow_root(container_);
	if (sequence == nullptr || sequence->block_count() != layout_.block_count()) {
		return Error{ErrorCode::invalid_state, "FlowSequence 與 FlowLayoutIndex 數量分岔"};
	}
	const auto visible_start = std::max(0.0, scroll_y - style_.overscan);
	auto position = layout_.lower_bound_extent(visible_start);
	const auto visible_end = scroll_y + viewport_height + style_.overscan;
	while (position < sequence->block_count()) {
		const auto block_top = style_.vertical_padding + layout_.prefix_extent(position);
		if (block_top > visible_end) break;
		const auto block = sequence->at(position);
		auto record = revision_.record_for(block);
		const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
		if (paragraph == nullptr) {
			return Error{ErrorCode::invalid_state, "FlowEditor P1 只能排版 ParagraphRecord"};
		}

		std::string_view display_text = paragraph->utf8();
		std::string composed_storage;
		std::uint64_t composition_revision = 0;
		std::size_t caret_byte = 0;
		std::size_t composition_start = 0;
		std::size_t composition_end = 0;
		if (session_.composition().has_value() &&
		    session_.composition()->replace_start.block == block) {
			auto composed = session_.composed_paragraph(revision_);
			if (!composed.is_ok()) return composed.error();
			composed_storage = composed.value().utf8;
			display_text = composed_storage;
			composition_revision = composed.value().composition_revision;
			composition_start = composed.value().composition_byte_start;
			composition_end = composed.value().composition_byte_end;
			caret_byte = composition_start +
			             session_.composition()->update.selection_byte_end;
		} else if (selection_.focus.block == block) {
			auto analysis = analyze_text(display_text, {});
			if (!analysis.is_ok()) return analysis.error();
			if (selection_.focus.grapheme_boundary >=
			    analysis.value().grapheme_boundaries.size()) {
				return Error{ErrorCode::invalid_state, "caret 與 Paragraph grapheme 分岔"};
			}
			caret_byte = analysis.value().grapheme_boundaries[
				selection_.focus.grapheme_boundary
			];
		}

		auto paragraph_layout = layouter_.layout(
			display_text,
			{},
			BaseDirection::auto_ltr,
			paragraph->content_revision(),
			composition_revision,
			0,
			style_.font_size_26_6,
			width.value(),
			style_.line_height_26_6
		);
		if (!paragraph_layout.is_ok()) return paragraph_layout.error();
		const auto measured_height =
			static_cast<double>(paragraph_layout.value().total_height_26_6) / 64.0 +
			style_.block_spacing;
		const auto& old_entry = layout_.at(position);
		if (old_entry.measured_height != measured_height ||
		    old_entry.source_content_revision != paragraph->content_revision() ||
		    old_entry.status != MeasurementStatus::measured) {
			layout_ = layout_.remove(position).insert(position, LayoutEntry{
				block,
				measured_height,
				paragraph->content_revision(),
				MeasurementStatus::measured,
			});
		}
		const auto draw_top = style_.vertical_padding + layout_.prefix_extent(position) -
		                      scroll_y;
		auto origin_y = to_26_6(draw_top);
		if (!origin_y.is_ok()) return origin_y.error();
		auto appended = append_paragraph_layout(
			builder,
			paragraph_layout.value(),
			origin_x.value(),
			origin_y.value(),
			style_.font_size_26_6,
			style_.text_rgba
		);
		if (!appended.is_ok()) return appended.error();

		if (selection_.focus.block == block || composition_revision != 0) {
			const auto* caret = find_caret(paragraph_layout.value(), caret_byte);
			if (caret != nullptr && caret->line_index < paragraph_layout.value().lines.size()) {
				const auto caret_x = style_.horizontal_padding +
				                     static_cast<double>(caret->x_26_6) / 64.0;
				const auto caret_y = draw_top + line_top(paragraph_layout.value(), caret->line_index);
				const auto caret_height = static_cast<double>(
					paragraph_layout.value().lines[caret->line_index].height_26_6
				) / 64.0;
				auto caret_result = builder.add_rect(
					static_cast<float>(caret_x),
					static_cast<float>(caret_y),
					1.5F,
					static_cast<float>(caret_height),
					style_.caret_rgba
				);
				if (!caret_result.is_ok()) return caret_result.error();
			}
		}
		if (composition_revision != 0 && composition_end > composition_start) {
			const auto* start = find_caret(paragraph_layout.value(), composition_start);
			const auto* end = find_caret(paragraph_layout.value(), composition_end);
			if (start != nullptr && end != nullptr && start->line_index == end->line_index) {
				const auto underline_x = style_.horizontal_padding +
				                          static_cast<double>(std::min(start->x_26_6, end->x_26_6)) / 64.0;
				const auto underline_width = static_cast<double>(
					std::abs(end->x_26_6 - start->x_26_6)
				) / 64.0;
				const auto underline_y = draw_top +
				                         line_top(paragraph_layout.value(), start->line_index) +
				                         static_cast<double>(
					                         paragraph_layout.value().lines[start->line_index].height_26_6
				                         ) / 64.0 - 2.0;
				auto underline = builder.add_rect(
					static_cast<float>(underline_x),
					static_cast<float>(underline_y),
					static_cast<float>(underline_width),
					1.0F,
					style_.caret_rgba
				);
				if (!underline.is_ok()) return underline.error();
			}
		}
		++position;
	}
	return publisher.publish();
}

}  // namespace krepis
