#include "krepis/undo.hpp"

#include "krepis/editing_session.hpp"
#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <string>
#include <utility>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::DocumentRevision;
using krepis::EditingSession;
using krepis::ObjectId;
using krepis::ParagraphRecord;
using krepis::TextEditMergePolicy;
using krepis::TextAnchor;
using krepis::TextSelection;
using krepis::Transaction;
using krepis::UndoManager;
using krepis::UndoRecordOptions;
using krepis_test::expect;

namespace {

BlockId block(std::uint64_t value) {
	return BlockId{ObjectId{0, value}};
}

ContainerId container(std::uint64_t value) {
	return ContainerId{ObjectId{1, value}};
}

DocumentRevision with_paragraph(const char* text) {
	auto revision = DocumentRevision::initial();
	auto record = ParagraphRecord::create(1, text);
	expect(record.is_ok(), "undo fixture Paragraph 建立成功");
	return revision.with_new_object(block(1), std::move(record).take());
}

std::string text_of(const DocumentRevision& revision) {
	auto record = revision.record_for(block(1));
	const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
	return paragraph == nullptr ? std::string{} : paragraph->utf8();
}

krepis::Result<krepis::CommitResult> insert(
	const DocumentRevision& base,
	std::size_t at,
	std::string text,
	TextEditMergePolicy policy
) {
	Transaction transaction(base.snapshot_id().content_revision);
	transaction.replace_paragraph_range(block(1), at, at, std::move(text), policy);
	return transaction.commit(base);
}

void test_continuous_typing_merges_and_redoes_as_one_entry() {
	auto base = with_paragraph("A");
	UndoManager undo(1'000);
	auto first = insert(base, 1, "B", TextEditMergePolicy::continuous_typing);
	expect(first.is_ok(), "第一個 typing transaction 成功");
	if (!first.is_ok()) return;
	expect(undo.record(first.value(), UndoRecordOptions{100, 7}).is_ok(),
	       "第一個 typing entry 可記錄");
	auto second = insert(
		first.value().revision,
		2,
		"C",
		TextEditMergePolicy::continuous_typing
	);
	expect(second.is_ok(), "第二個 typing transaction 成功");
	if (!second.is_ok()) return;
	expect(undo.record(second.value(), UndoRecordOptions{400, 7}).is_ok(),
	       "第二個 typing entry 可記錄");
	expect(undo.undo_size() == 1, "相鄰、同 merge group、時間窗內的 typing 合併");
	auto reverted = undo.undo(second.value().revision);
	expect(reverted.is_ok() && text_of(reverted.value().revision) == "A",
	       "一次 undo 移除合併後的 BC");
	expect(undo.can_redo(), "undo 後建立 redo entry");
	if (!reverted.is_ok()) return;
	auto restored = undo.redo(reverted.value().revision);
	expect(restored.is_ok() && text_of(restored.value().revision) == "ABC",
	       "一次 redo 還原合併後的 BC");
}

void test_pause_group_and_never_policy_close_merge() {
	auto base = with_paragraph("A");
	UndoManager undo(1'000);
	auto first = insert(base, 1, "B", TextEditMergePolicy::continuous_typing);
	if (!first.is_ok()) return;
	expect(undo.record(first.value(), UndoRecordOptions{100, 1}).is_ok(),
	       "pause fixture 第一筆可記錄");
	auto paused = insert(
		first.value().revision,
		2,
		"C",
		TextEditMergePolicy::continuous_typing
	);
	if (!paused.is_ok()) return;
	expect(undo.record(paused.value(), UndoRecordOptions{1'101, 1}).is_ok(),
	       "pause fixture 第二筆可記錄");
	expect(undo.undo_size() == 2, "超過時間窗一毫秒即關閉 typing merge");
	auto ime = insert(paused.value().revision, 3, "知", TextEditMergePolicy::never);
	if (!ime.is_ok()) return;
	expect(undo.record(ime.value(), UndoRecordOptions{1'200, 1}).is_ok(),
	       "never policy entry 可記錄");
	expect(undo.undo_size() == 3, "IME／never command 即使相鄰也不合併");
}

void test_failed_undo_preserves_history_and_new_edit_clears_redo() {
	auto base = with_paragraph("A");
	UndoManager undo;
	auto committed = insert(base, 1, "B", TextEditMergePolicy::never);
	if (!committed.is_ok()) return;
	expect(undo.record(committed.value(), UndoRecordOptions{100, 0}).is_ok(),
	       "failed undo fixture entry 可記錄");
	auto stale = undo.undo(base);
	expect(!stale.is_ok() && undo.undo_size() == 1 && undo.redo_size() == 0,
	       "undo commit 失敗不移動兩個 stack");
	auto reverted = undo.undo(committed.value().revision);
	if (!reverted.is_ok()) return;
	expect(undo.redo_size() == 1, "成功 undo 後 redo 可用");
	auto replacement = insert(
		reverted.value().revision,
		1,
		"X",
		TextEditMergePolicy::never
	);
	if (!replacement.is_ok()) return;
	expect(undo.record(replacement.value(), UndoRecordOptions{200, 0}).is_ok(),
	       "undo 後的新 transaction 可記錄");
	expect(!undo.can_redo(), "undo 後的新 transaction 清除 redo branch");
}

void test_replacement_round_trip_and_composition_is_never_merged() {
	auto base = with_paragraph("ABCD");
	UndoManager undo;
	Transaction replacement(base.snapshot_id().content_revision);
	replacement.replace_paragraph_range(block(1), 1, 3, "知");
	auto replaced = replacement.commit(base);
	expect(replaced.is_ok(), "selection replacement transaction 成功");
	if (!replaced.is_ok()) return;
	expect(undo.record(replaced.value(), UndoRecordOptions{100, 9}).is_ok(),
	       "selection replacement 可記錄");
	auto reverted = undo.undo(replaced.value().revision);
	expect(reverted.is_ok() && text_of(reverted.value().revision) == "ABCD",
	       "undo 以 removed UTF-8 還原 selection replacement");
	if (!reverted.is_ok()) return;
	auto restored = undo.redo(reverted.value().revision);
	expect(restored.is_ok() && text_of(restored.value().revision) == "A知D",
	       "redo 再次套用 selection replacement");

	UndoManager composition_undo;
	EditingSession session(71);
	TextSelection caret{
		TextAnchor{
			block(1),
			base.snapshot_id().content_revision,
			1,
			krepis::AnchorAffinity::upstream,
		},
		TextAnchor{
			block(1),
			base.snapshot_id().content_revision,
			1,
			krepis::AnchorAffinity::downstream,
		},
	};
	krepis::CompositionUpdate update{
		"知",
		0,
		3,
		{{0, 3, krepis::CompositionAttribute::target_converted}},
	};
	expect(session.begin_composition(base, caret, std::move(update)).is_ok(),
	       "composition undo fixture 開始成功");
	auto committed = session.commit_composition(base);
	expect(committed.is_ok(), "composition undo fixture commit 成功");
	if (!committed.is_ok()) return;
	expect(composition_undo.record(committed.value(), UndoRecordOptions{100, 9}).is_ok(),
	       "composition commit 可記錄為 undo entry");
	auto typing = insert(
		committed.value().revision,
		2,
		"X",
		TextEditMergePolicy::continuous_typing
	);
	if (!typing.is_ok()) return;
	expect(composition_undo.record(typing.value(), UndoRecordOptions{200, 9}).is_ok(),
	       "composition 後相鄰 typing 可記錄");
	expect(composition_undo.undo_size() == 2,
	       "一次 composition 確定即使同 merge group 也不與 typing 合併");
}

void test_split_and_merge_are_atomic_global_undo_entries() {
	const auto flow = container(1);
	auto split_base = with_paragraph("AB");
	split_base = split_base.with_flow_root(
		flow,
		krepis::FlowSequence::empty().insert(0, block(1))
	);
	Transaction split(split_base.snapshot_id().content_revision);
	split.split_paragraph(flow, block(1), 1, block(2));
	auto split_result = split.commit(split_base);
	expect(split_result.is_ok(), "structure undo fixture split 成功");
	if (!split_result.is_ok()) return;
	UndoManager split_undo;
	expect(split_undo.record(split_result.value(), UndoRecordOptions{100, 1}).is_ok(),
	       "split 可記錄為單一 global undo entry");
	auto unsplit = split_undo.undo(split_result.value().revision);
	expect(unsplit.is_ok(), "split 可一次 undo");
	if (!unsplit.is_ok()) return;
	expect(unsplit.value().invalidations.size() == 2,
	       "undo split 同時失效 primary 與 secondary Block");
	const auto* unsplit_flow = unsplit.value().revision.flow_root(flow);
	expect(unsplit_flow != nullptr && unsplit_flow->block_count() == 1 &&
	           unsplit_flow->at(0) == block(1) &&
	           text_of(unsplit.value().revision) == "AB" &&
	           unsplit.value().revision.record_for(block(2)) == nullptr,
	       "undo split 復原原 ID、原文字與原順序");
	auto resplit = split_undo.redo(unsplit.value().revision);
	expect(resplit.is_ok(), "split 可一次 redo");
	if (!resplit.is_ok()) return;
	const auto* resplit_flow = resplit.value().revision.flow_root(flow);
	expect(resplit_flow != nullptr && resplit_flow->block_count() == 2 &&
	           resplit_flow->at(0) == block(1) && resplit_flow->at(1) == block(2),
	       "redo split 重用同一 secondary BlockId");

	Transaction merge(resplit.value().revision.snapshot_id().content_revision);
	merge.merge_adjacent_paragraphs(flow, block(1), block(2));
	auto merge_result = merge.commit(resplit.value().revision);
	expect(merge_result.is_ok(), "structure undo fixture merge 成功");
	if (!merge_result.is_ok()) return;
	UndoManager merge_undo;
	expect(merge_undo.record(merge_result.value(), UndoRecordOptions{200, 0}).is_ok(),
	       "merge 可記錄為單一 global undo entry");
	auto unmerged = merge_undo.undo(merge_result.value().revision);
	expect(unmerged.is_ok(), "merge 可一次 undo");
	if (!unmerged.is_ok()) return;
	const auto* unmerged_flow = unmerged.value().revision.flow_root(flow);
	expect(unmerged_flow != nullptr && unmerged_flow->block_count() == 2 &&
	           unmerged_flow->at(1) == block(2),
	       "undo merge 復原 secondary BlockId 的原位置");
	auto secondary = unmerged.value().revision.record_for(block(2));
	const auto* secondary_paragraph = dynamic_cast<const ParagraphRecord*>(secondary.get());
	expect(text_of(unmerged.value().revision) == "A" &&
	           secondary_paragraph != nullptr && secondary_paragraph->utf8() == "B",
	       "undo merge 復原兩個 Paragraph 的原文字");
}

}  // namespace

int main() {
	test_continuous_typing_merges_and_redoes_as_one_entry();
	test_pause_group_and_never_policy_close_merge();
	test_failed_undo_preserves_history_and_new_edit_clears_redo();
	test_replacement_round_trip_and_composition_is_never_merged();
	test_split_and_merge_are_atomic_global_undo_entries();
	return krepis_test::report("krepis.undo");
}
