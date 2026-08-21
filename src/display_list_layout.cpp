#include "krepis/display_list_layout.hpp"

#include <cstdint>
#include <limits>

namespace krepis {
namespace {

Result<std::int32_t> checked_narrow(std::int64_t value) {
	if (value < std::numeric_limits<std::int32_t>::min() ||
	    value > std::numeric_limits<std::int32_t>::max()) {
		return Error{ErrorCode::out_of_range, "paragraph display 座標超出 26.6 wire range"};
	}
	return static_cast<std::int32_t>(value);
}

}  // namespace

Result<void> append_paragraph_layout(
	DisplayListBuilder& builder,
	const ParagraphLayout& layout,
	std::int32_t origin_x_26_6,
	std::int32_t origin_y_26_6,
	std::int32_t font_size_26_6,
	std::uint32_t color_rgba
) {
	if (font_size_26_6 <= 0) {
		return Error{ErrorCode::invalid_argument, "paragraph display 字型尺寸必須大於零"};
	}

	// 步驟 1：在寫入任何 command 前完整驗證 run range 與累積座標。
	for (const auto& line : layout.lines) {
		if (line.glyph_run_offset > layout.glyph_runs.size() ||
		    line.glyph_run_count > layout.glyph_runs.size() - line.glyph_run_offset) {
			return Error{ErrorCode::invalid_argument, "LineFragment glyph run 範圍不合法"};
		}
		auto baseline_y = checked_narrow(
			static_cast<std::int64_t>(origin_y_26_6) + line.baseline_y_26_6
		);
		if (!baseline_y.is_ok()) return baseline_y.error();
		std::int64_t run_x = origin_x_26_6;
		for (std::size_t offset = 0; offset < line.glyph_run_count; ++offset) {
			const auto& run = layout.glyph_runs[line.glyph_run_offset + offset];
			auto baseline_x = checked_narrow(run_x);
			if (!baseline_x.is_ok()) return baseline_x.error();
			for (const auto& glyph : run.glyphs) {
				run_x += glyph.x_advance;
				if (run_x < std::numeric_limits<std::int32_t>::min() ||
				    run_x > std::numeric_limits<std::int32_t>::max()) {
					return Error{ErrorCode::out_of_range, "glyph run 累積 x advance 溢位"};
				}
			}
		}
	}

	// 步驟 2：以 builder checkpoint 寫入；任一 run 失敗即回滾整個 paragraph。
	const auto checkpoint_size = builder.bytes_.size();
	const auto checkpoint_count = builder.command_count_;
	for (const auto& line : layout.lines) {
		auto baseline_y = checked_narrow(
			static_cast<std::int64_t>(origin_y_26_6) + line.baseline_y_26_6
		);
		std::int64_t run_x = origin_x_26_6;
		for (std::size_t offset = 0; offset < line.glyph_run_count; ++offset) {
			const auto& run = layout.glyph_runs[line.glyph_run_offset + offset];
			auto baseline_x = checked_narrow(run_x);
			auto encoded = builder.add_glyph_run(
				baseline_x.value(),
				baseline_y.value(),
				font_size_26_6,
				color_rgba,
				run.font_id,
				run.direction,
				run.glyphs
			);
			if (!encoded.is_ok()) {
				builder.bytes_.resize(checkpoint_size);
				builder.command_count_ = checkpoint_count;
				return encoded.error();
			}
			for (const auto& glyph : run.glyphs) run_x += glyph.x_advance;
		}
	}
	return {};
}

}  // namespace krepis
