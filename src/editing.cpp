#include "krepis/editing.hpp"

namespace krepis {
namespace {

enum class SameBoundaryInsertPolicy : std::uint8_t {
	use_affinity,
	stay_before,
};

Result<TextAnchor> transform_anchor(
	const TextAnchor& anchor,
	const ParagraphTextEditEffect& effect,
	SameBoundaryInsertPolicy insert_policy
) {
	if (anchor.block != effect.block) {
		return Error{ErrorCode::invalid_argument, "TextAnchor 與 edit effect 不屬於同一 Block"};
	}
	if (anchor.base_content_revision != effect.before_content_revision) {
		return Error{ErrorCode::revision_conflict, "TextAnchor base revision 與 edit effect 不符"};
	}
	if (effect.replaced_grapheme_start > effect.replaced_grapheme_end) {
		return Error{ErrorCode::invalid_state, "edit effect grapheme range 反向"};
	}

	auto boundary = anchor.grapheme_boundary;
	const auto start = effect.replaced_grapheme_start;
	const auto end = effect.replaced_grapheme_end;
	const auto inserted = effect.inserted_grapheme_count;
	if (start == end) {
		if (boundary > start) {
			boundary += inserted;
		} else if (boundary == start && insert_policy == SameBoundaryInsertPolicy::use_affinity &&
		           anchor.affinity == AnchorAffinity::downstream) {
			boundary += inserted;
		}
	} else if (boundary < start) {
		// Edit 完全在 anchor 後方。
	} else if (boundary <= end) {
		// D15：刪除收縮到接合點；replacement 則位於新內容之後。
		boundary = start + inserted;
	} else {
		const auto removed = end - start;
		if (inserted >= removed) {
			boundary += inserted - removed;
		} else {
			boundary -= removed - inserted;
		}
	}

	return TextAnchor{
		anchor.block,
		effect.after_content_revision,
		boundary,
		anchor.affinity,
	};
}

}  // namespace

Result<TextAnchor> transform_text_anchor(
	const TextAnchor& anchor,
	const ParagraphTextEditEffect& effect
) {
	return transform_anchor(anchor, effect, SameBoundaryInsertPolicy::use_affinity);
}

Result<TextAnchor> transform_composition_anchor(
	const TextAnchor& anchor,
	const ParagraphTextEditEffect& effect
) {
	return transform_anchor(anchor, effect, SameBoundaryInsertPolicy::stay_before);
}

}  // namespace krepis
