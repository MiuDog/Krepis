#include "krepis/reference_resolver.hpp"

#include "krepis/embed_record.hpp"
#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <utility>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::DocumentRevision;
using krepis::EmbedRecord;
using krepis::FlowRangeTarget;
using krepis::FlowSequence;
using krepis::ObjectId;
using krepis::ParagraphRecord;
using krepis::RectD;
using krepis::ReferenceResolutionStatus;
using krepis::ReferenceResolver;
using krepis::SpatialContainer;
using krepis::SpatialPlacement;
using krepis::SpatialViewportTarget;
using krepis_test::expect;

namespace {

BlockId block(std::uint64_t value) {
	return BlockId{ObjectId{1, value}};
}

ContainerId container(std::uint64_t value) {
	return ContainerId{ObjectId{2, value}};
}

DocumentRevision add_paragraph(DocumentRevision revision, BlockId id, const char* text) {
	auto record = ParagraphRecord::create(revision.snapshot_id().content_revision + 1, text);
	expect(record.is_ok(), "reference fixture Paragraph 建立成功");
	return revision.with_new_object(id, std::move(record).take());
}

DocumentRevision add_embed(
	DocumentRevision revision,
	BlockId id,
	krepis::EmbedTarget target
) {
	auto record = EmbedRecord::create(
		revision.snapshot_id().content_revision + 1,
		std::move(target)
	);
	expect(record.is_ok(), "reference fixture Embed 建立成功");
	return revision.with_new_object(id, std::move(record).take());
}

void test_flow_range_order_empty_detached_and_repair() {
	auto sequence = FlowSequence::empty()
		.insert(0, block(1))
		.insert(1, block(2))
		.insert(2, block(3))
		.insert(3, block(4));
	auto repaired_front = krepis::repair_flow_range_after_removal(
		FlowRangeTarget{container(1), block(1), block(4)},
		sequence,
		block(1)
	);
	expect(repaired_front.is_ok() && repaired_front.value().anchor_a == block(2) &&
	           repaired_front.value().anchor_b == block(4),
	       "leading endpoint 刪除後向原區間內側收縮");
	auto repaired_reverse = krepis::repair_flow_range_after_removal(
		FlowRangeTarget{container(1), block(4), block(2)},
		sequence,
		block(4)
	);
	expect(repaired_reverse.is_ok() && repaired_reverse.value().anchor_a == block(3),
	       "反向 endpoints 仍依原區間方向向內收縮");
	auto emptied = krepis::repair_flow_range_after_removal(
		FlowRangeTarget{container(1), block(2), block(2)},
		sequence,
		block(2)
	);
	expect(emptied.is_ok() && emptied.value().anchor_a.is_nil() &&
	           emptied.value().anchor_b.is_nil(),
	       "單一節點 range 被刪除後成為 empty，不降級到頁首");

	auto revision = DocumentRevision::initial();
	for (std::uint64_t id = 1; id <= 4; ++id) revision = add_paragraph(revision, block(id), "P");
	revision = add_embed(
		std::move(revision),
		block(10),
		FlowRangeTarget{container(1), block(4), block(2)}
	);
	revision = revision.with_flow_root(container(1), sequence);
	revision = revision.with_flow_root(container(9), FlowSequence::empty().insert(0, block(10)));
	auto resolved = ReferenceResolver(revision).resolve_flow(container(9));
	const auto& embedded = *resolved.blocks.front().embed;
	expect(embedded.blocks.size() == 3 && embedded.blocks[0].block == block(2) &&
	           embedded.blocks[2].block == block(4),
	       "anchor 對調只影響解析順序，不修改儲存資料");
}

void test_bidirectional_cycle_is_cut_per_active_path_not_globally() {
	auto revision = DocumentRevision::initial();
	revision = add_paragraph(std::move(revision), block(1), "source");
	revision = add_embed(
		std::move(revision),
		block(2),
		SpatialViewportTarget{container(2), RectD{0, 0, 100, 100}}
	);
	revision = add_embed(
		std::move(revision),
		block(3),
		SpatialViewportTarget{container(2), RectD{0, 0, 100, 100}}
	);
	revision = add_embed(
		std::move(revision),
		block(4),
		FlowRangeTarget{container(1), block(1), block(3)}
	);
	revision = revision.with_flow_root(
		container(1),
		FlowSequence::empty().insert(0, block(1)).insert(1, block(2)).insert(2, block(3))
	);
	auto spatial = SpatialContainer::create({
		SpatialPlacement{1, block(4), RectD{10, 10, 20, 20}, 20, 20, 0},
	});
	if (!spatial.is_ok()) return;
	revision = revision.with_spatial_root(container(2), std::move(spatial).take());

	auto resolved = ReferenceResolver(revision).resolve_flow(container(1));
	expect(resolved.blocks[1].embed->status == ReferenceResolutionStatus::resolved &&
	           resolved.blocks[2].embed->status == ReferenceResolutionStatus::resolved,
	       "同一 Spatial source 在兩個 sibling branch 都正常展開");
	const auto& first_spatial = *resolved.blocks[1].embed;
	expect(first_spatial.blocks.front().embed->status == ReferenceResolutionStatus::cycle_cut,
	       "Spatial 反向引用 active Flow source 時只產生 cycle_cut 終止框");
}

void test_missing_empty_and_detached_are_distinct() {
	auto revision = DocumentRevision::initial();
	revision = add_embed(
		std::move(revision),
		block(10),
		FlowRangeTarget{container(1), BlockId{}, BlockId{}}
	);
	revision = add_embed(
		std::move(revision),
		block(11),
		FlowRangeTarget{container(1), block(99), block(99)}
	);
	revision = add_embed(
		std::move(revision),
		block(12),
		SpatialViewportTarget{container(99), RectD{0, 0, 10, 10}}
	);
	revision = revision.with_flow_root(container(1), FlowSequence::empty());
	revision = revision.with_flow_root(
		container(8),
		FlowSequence::empty().insert(0, block(10)).insert(1, block(11)).insert(2, block(12))
	);
	auto resolved = ReferenceResolver(revision).resolve_flow(container(8));
	expect(resolved.blocks[0].embed->status == ReferenceResolutionStatus::empty,
	       "已修復為 nil anchors 的 range 回 empty");
	expect(resolved.blocks[1].embed->status == ReferenceResolutionStatus::detached,
	       "anchor 不在指定 Flow 中回 detached");
	expect(resolved.blocks[2].embed->status == ReferenceResolutionStatus::missing_target,
	       "不存在的 Spatial source 回 missing_target");
}

void test_flow_removal_repairs_all_references_in_same_revision() {
	auto revision = DocumentRevision::initial();
	for (std::uint64_t id = 1; id <= 3; ++id) {
		revision = add_paragraph(std::move(revision), block(id), "P");
	}
	revision = add_embed(
		std::move(revision),
		block(10),
		FlowRangeTarget{container(1), block(1), block(3)}
	);
	revision = revision.with_flow_root(
		container(1),
		FlowSequence::empty().insert(0, block(1)).insert(1, block(2)).insert(2, block(3))
	);
	revision = revision.with_flow_root(container(9), FlowSequence::empty().insert(0, block(10)));
	expect(revision.references().referencing(container(1)).size() == 1,
	       "Embed 建立時同步加入 reverse-reference index");
	const auto before_revision = revision.snapshot_id().content_revision;
	auto removed = revision.with_flow_block_removal(container(1), block(1), true);
	expect(removed.is_ok(), "刪除 Flow endpoint 與修復 reference 可原子發布");
	if (!removed.is_ok()) return;
	expect(removed.value().snapshot_id().content_revision == before_revision + 1,
	       "移除、tombstone、locator 與 anchor 修復只增加一次 revision");
	const auto* source = removed.value().flow_root(container(1));
	expect(source != nullptr && source->block_count() == 2 && source->at(0) == block(2),
	       "來源 Flow 已移除 endpoint");
	expect(removed.value().record_for(block(1)) == nullptr,
	       "delete_record 同 revision 寫入 endpoint tombstone");
	auto embed_record = removed.value().record_for(block(10));
	const auto* embed = dynamic_cast<const EmbedRecord*>(embed_record.get());
	const auto* target = embed == nullptr
		? nullptr
		: std::get_if<FlowRangeTarget>(&embed->target());
	expect(target != nullptr && target->anchor_a == block(2) && target->anchor_b == block(3),
	       "所有引用該來源的 range endpoint 同 revision 向內修復");
	expect(removed.value().validate().ok(), "原子 Flow removal 後所有衍生索引一致");
	expect(std::get<FlowRangeTarget>(
		dynamic_cast<const EmbedRecord*>(revision.record_for(block(10)).get())->target()
	).anchor_a == block(1), "舊 snapshot 的 Embed anchor 不被改寫");

	auto second = removed.value().with_flow_block_removal(container(1), block(2), true);
	if (!second.is_ok()) return;
	auto last = second.value().with_flow_block_removal(container(1), block(3), true);
	expect(last.is_ok(), "連續刪除到最後一個 range 節點仍可提交");
	if (!last.is_ok()) return;
	auto final_record = last.value().record_for(block(10));
	const auto& final_target = std::get<FlowRangeTarget>(
		dynamic_cast<const EmbedRecord*>(final_record.get())->target()
	);
	expect(final_target.anchor_a.is_nil() && final_target.anchor_b.is_nil(),
	       "原 range 全部刪除後儲存 empty anchors，不改指頁首");
}

void test_reference_index_tracks_retarget_and_delete() {
	auto revision = DocumentRevision::initial();
	revision = add_embed(
		std::move(revision),
		block(10),
		FlowRangeTarget{container(1), BlockId{}, BlockId{}}
	);
	expect(revision.references().referencing(container(1)).size() == 1,
	       "新 Embed 加入來源 reverse index");
	auto replacement = EmbedRecord::create(
		revision.snapshot_id().content_revision + 1,
		SpatialViewportTarget{container(2), RectD{0, 0, 10, 10}}
	);
	if (!replacement.is_ok()) return;
	revision = revision.with_updated_record(block(10), std::move(replacement).take());
	expect(revision.references().referencing(container(1)).empty() &&
	           revision.references().referencing(container(2)).size() == 1,
	       "Embed 改指時原子移除舊 source 並加入新 source");
	revision = revision.with_deleted_object(block(10));
	expect(revision.references().referencing(container(2)).empty(),
	       "Embed tombstone 不在 reverse index 留下 stale ID");
}

}  // namespace

int main() {
	test_flow_range_order_empty_detached_and_repair();
	test_bidirectional_cycle_is_cut_per_active_path_not_globally();
	test_missing_empty_and_detached_are_distinct();
	test_flow_removal_repairs_all_references_in_same_revision();
	test_reference_index_tracks_retarget_and_delete();
	return krepis_test::report("krepis.reference_resolver");
}
