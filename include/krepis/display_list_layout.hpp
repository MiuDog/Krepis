#pragma once

// 將核心的 ParagraphLayout 序列化為 DrawGlyphRun，不在邊界重新 shaping。

#include "krepis/display_list.hpp"
#include "krepis/text_layout.hpp"

#include <cstdint>

namespace krepis {

[[nodiscard]] Result<void> append_paragraph_layout(
	DisplayListBuilder& builder,
	const ParagraphLayout& layout,
	std::int32_t origin_x_26_6,
	std::int32_t origin_y_26_6,
	std::int32_t font_size_26_6,
	std::uint32_t color_rgba
);

}  // namespace krepis
