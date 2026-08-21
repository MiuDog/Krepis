#include "krepis/editing.hpp"

#include "test_support.hpp"

using krepis::AnchorAffinity;
using krepis::BlockId;
using krepis::ObjectId;
using krepis::ParagraphTextEditEffect;
using krepis::TextAnchor;
using krepis::transform_composition_anchor;
using krepis::transform_text_anchor;
using krepis_test::expect;

namespace {

BlockId block(std::uint64_t value) {
	return BlockId{ObjectId{0, value}};
}

ParagraphTextEditEffect effect(
	std::size_t start,
	std::size_t end,
	std::size_t inserted
) {
	return ParagraphTextEditEffect{block(1), 10, 11, start, end, inserted};
}

TextAnchor anchor(std::size_t boundary, AnchorAffinity affinity = AnchorAffinity::downstream) {
	return TextAnchor{block(1), 10, boundary, affinity};
}

void test_same_boundary_insert_distinguishes_selection_and_composition() {
	const auto insertion = effect(2, 2, 1);
	auto downstream = transform_text_anchor(anchor(2), insertion);
	auto upstream = transform_text_anchor(anchor(2, AnchorAffinity::upstream), insertion);
	auto composition = transform_composition_anchor(anchor(2), insertion);
	expect(downstream.is_ok() && downstream.value().grapheme_boundary == 3,
	       "downstream selection 移到同位置插入之後");
	expect(upstream.is_ok() && upstream.value().grapheme_boundary == 2,
	       "upstream selection 留在同位置插入之前");
	expect(composition.is_ok() && composition.value().grapheme_boundary == 2,
	       "D14 composition 固定留在同位置插入之前");
}

void test_delete_and_replace_transform_anchor() {
	auto before = transform_composition_anchor(anchor(5), effect(1, 3, 0));
	expect(before.is_ok() && before.value().grapheme_boundary == 3,
	       "anchor 前方刪除使位置向前移");
	auto covered_delete = transform_composition_anchor(anchor(2), effect(1, 4, 0));
	expect(covered_delete.is_ok() && covered_delete.value().grapheme_boundary == 1,
	       "涵蓋 anchor 的刪除收縮到接合邊界");
	auto covered_replace = transform_composition_anchor(anchor(2), effect(1, 4, 1));
	expect(covered_replace.is_ok() && covered_replace.value().grapheme_boundary == 2,
	       "涵蓋 anchor 的替換落在 replacement content 後");
	auto after = transform_composition_anchor(anchor(1), effect(3, 5, 0));
	expect(after.is_ok() && after.value().grapheme_boundary == 1,
	       "anchor 後方刪除不改位置");
}

void test_transform_rejects_wrong_block_or_revision() {
	auto wrong_block = effect(0, 0, 1);
	wrong_block.block = block(2);
	expect(!transform_text_anchor(anchor(0), wrong_block).is_ok(),
	       "不同 Block 的 effect 不可套到 anchor");
	auto stale = anchor(0);
	stale.base_content_revision = 9;
	expect(!transform_text_anchor(stale, effect(0, 0, 1)).is_ok(),
	       "base revision 不符時 fail closed");
}

}  // namespace

int main() {
	test_same_boundary_insert_distinguishes_selection_and_composition();
	test_delete_and_replace_transform_anchor();
	test_transform_rejects_wrong_block_or_revision();
	return krepis_test::report("krepis.editing");
}
