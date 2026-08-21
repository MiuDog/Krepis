#pragma once

// DOC-0001 D14～D16、EDT-0001：grapheme anchor、兩種 selection 與文字 edit transformation。

#include "krepis/error.hpp"
#include "krepis/object_id.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace krepis {

enum class AnchorAffinity : std::uint8_t {
	upstream,
	downstream,
};

struct TextAnchor {
	BlockId block;
	std::uint64_t base_content_revision;
	std::size_t grapheme_boundary;
	AnchorAffinity affinity;
};

struct TextSelection {
	TextAnchor anchor;
	TextAnchor focus;
};

struct SpatialSelection {
	ContainerId container;
	std::vector<BlockId> nodes;
};

using Selection = std::variant<TextSelection, SpatialSelection>;

struct ParagraphTextEditEffect {
	BlockId block;
	std::uint64_t before_content_revision;
	std::uint64_t after_content_revision;
	std::size_t replaced_grapheme_start;
	std::size_t replaced_grapheme_end;
	std::size_t inserted_grapheme_count;
};

// 一般 selection anchor：同位置 pure insert 由 affinity 決定停在插入前或後。
[[nodiscard]] Result<TextAnchor> transform_text_anchor(
	const TextAnchor& anchor,
	const ParagraphTextEditEffect& effect
);

// Active composition：DOC-0001 D14 規定同位置 pure insert 一律留在新內容前。
[[nodiscard]] Result<TextAnchor> transform_composition_anchor(
	const TextAnchor& anchor,
	const ParagraphTextEditEffect& effect
);

}  // namespace krepis
