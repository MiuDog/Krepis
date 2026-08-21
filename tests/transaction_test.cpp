#include "krepis/transaction.hpp"

#include "krepis/intrusive_ptr.hpp"
#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <string>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::DocumentRevision;
using krepis::ErrorCode;
using krepis::InvalidationStage;
using krepis::ObjectId;
using krepis::ObjectRecord;
using krepis::ParagraphRecord;
using krepis::TextEditMergePolicy;
using krepis::Transaction;
using krepis::make_intrusive;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

class OtherRecord final : public ObjectRecord {
public:
	explicit OtherRecord(std::uint64_t revision) noexcept : ObjectRecord(revision) {}
};

BlockId make_block(std::uint64_t value) {
	return BlockId{ObjectId{0, value}};
}

ContainerId make_container(std::uint64_t value) {
	return ContainerId{ObjectId{1, value}};
}

DocumentRevision with_paragraph(DocumentRevision revision, BlockId block, const char* text) {
	auto record = ParagraphRecord::create(revision.snapshot_id().content_revision + 1, text);
	expect(record.is_ok(), "測試 fixture Paragraph 建立成功");
	return revision.with_new_object(block, std::move(record).take());
}

const ParagraphRecord* paragraph_for(const DocumentRevision& revision, BlockId block) {
	auto record = revision.record_for(block);
	return dynamic_cast<const ParagraphRecord*>(record.get());
}

void test_commits_multiple_replacements_as_one_revision() {
	auto base = with_paragraph(DocumentRevision::initial(), make_block(1), "A");
	base = with_paragraph(std::move(base), make_block(2), "B");
	const auto base_revision = base.snapshot_id().content_revision;

	Transaction transaction(base_revision);
	transaction.replace_paragraph_text(make_block(1), "A1");
	transaction.replace_paragraph_text(make_block(2), "B1");
	auto result = transaction.commit(base);

	expect(result.is_ok(), "兩個合法 command 一起成功");
	if (!result.is_ok()) return;

	auto committed = std::move(result).take();
	expect(committed.revision.snapshot_id().content_revision == base_revision + 1,
	       "整個 Transaction 只增加一次 revision");
	expect(paragraph_for(committed.revision, make_block(1))->utf8() == "A1", "第一筆更新存在");
	expect(paragraph_for(committed.revision, make_block(2))->utf8() == "B1", "第二筆更新存在");
	expect(paragraph_for(base, make_block(1))->utf8() == "A", "舊 snapshot 第一筆不變");
	expect(paragraph_for(base, make_block(2))->utf8() == "B", "舊 snapshot 第二筆不變");
	expect(committed.invalidations.size() == 2, "每個修改 Block 產生一項失效");
	expect(committed.invalidations[0].stage == InvalidationStage::shaping,
	       "Paragraph 文字從 shaping 開始失效");
	expect(committed.invalidations[0].source_content_revision == base_revision + 1,
	       "失效綁定新 revision");
}

void test_missing_target_rejects_every_command() {
	auto base = with_paragraph(DocumentRevision::initial(), make_block(1), "A");
	Transaction transaction(base.snapshot_id().content_revision);
	transaction.replace_paragraph_text(make_block(1), "changed");
	transaction.replace_paragraph_text(make_block(99), "missing");

	auto result = transaction.commit(base);
	expect(!result.is_ok(), "任一目標不存在則整體拒絕");
	if (!result.is_ok()) {
		expect(result.error().code() == ErrorCode::not_found, "不存在回傳 not_found");
	}
	expect(paragraph_for(base, make_block(1))->utf8() == "A", "拒絕後 base 不變");
}

void test_non_paragraph_and_duplicate_targets_are_rejected() {
	auto base = DocumentRevision::initial().with_new_object(make_block(7), make_intrusive<OtherRecord>(1));
	Transaction wrong_type(base.snapshot_id().content_revision);
	wrong_type.replace_paragraph_text(make_block(7), "text");
	auto type_result = wrong_type.commit(base);
	expect(!type_result.is_ok(), "非 Paragraph record 拒絕文字 command");
	if (!type_result.is_ok()) {
		expect(type_result.error().code() == ErrorCode::invalid_state, "型別錯誤回 invalid_state");
	}

	auto paragraph_base = with_paragraph(DocumentRevision::initial(), make_block(1), "A");
	Transaction duplicate(paragraph_base.snapshot_id().content_revision);
	duplicate.replace_paragraph_text(make_block(1), "B");
	duplicate.replace_paragraph_text(make_block(1), "C");
	auto duplicate_result = duplicate.commit(paragraph_base);
	expect(!duplicate_result.is_ok(), "同一 Transaction 重複目標拒絕");
	if (!duplicate_result.is_ok()) {
		expect(duplicate_result.error().code() == ErrorCode::invalid_argument,
		       "重複目標回 invalid_argument");
	}
}

void test_stale_base_invalid_utf8_and_empty_transaction_are_rejected() {
	auto base = with_paragraph(DocumentRevision::initial(), make_block(1), "A");

	Transaction stale(base.snapshot_id().content_revision - 1);
	stale.replace_paragraph_text(make_block(1), "B");
	auto stale_result = stale.commit(base);
	expect(!stale_result.is_ok(), "stale base 拒絕");
	if (!stale_result.is_ok()) {
		expect(stale_result.error().code() == ErrorCode::revision_conflict,
		       "stale base 回 revision_conflict");
	}

	Transaction invalid_utf8(base.snapshot_id().content_revision);
	invalid_utf8.replace_paragraph_text(make_block(1), std::string("\xC0\xAF", 2));
	auto utf8_result = invalid_utf8.commit(base);
	expect(!utf8_result.is_ok(), "不合法 UTF-8 拒絕整個 Transaction");

	Transaction empty(base.snapshot_id().content_revision);
	auto empty_result = empty.commit(base);
	expect(!empty_result.is_ok(), "空 Transaction 拒絕");
}

void test_range_replace_uses_grapheme_boundaries_and_reports_effect() {
	auto base = with_paragraph(
		DocumentRevision::initial(),
		make_block(1),
		"A cafe\xCC\x81 Z"
	);
	const auto base_revision = base.snapshot_id().content_revision;
	Transaction transaction(base_revision);
	// Grapheme 2..6 是 cafe + combining acute；不得以 byte offset 拆 combining mark。
	transaction.replace_paragraph_range(
		make_block(1),
		2,
		6,
		"X",
		TextEditMergePolicy::continuous_typing
	);
	auto result = transaction.commit(base);
	expect(result.is_ok(), "grapheme range replacement 可提交");
	if (!result.is_ok()) return;
	expect(paragraph_for(result.value().revision, make_block(1))->utf8() == "A X Z",
	       "range replacement 以 grapheme boundary 取代完整 combining sequence");
	expect(result.value().text_edit_effects.size() == 1, "commit 回傳一筆 anchor transformation effect");
	if (!result.value().text_edit_effects.empty()) {
		const auto& effect = result.value().text_edit_effects.front();
		expect(effect.replaced_grapheme_start == 2 && effect.replaced_grapheme_end == 6 &&
		           effect.inserted_grapheme_count == 1,
		       "effect 保存舊 range 與新 grapheme 數");
	}
	expect(result.value().text_edit_records.size() == 1,
	       "commit 回傳一筆 typed undo record");
	if (!result.value().text_edit_records.empty()) {
		const auto& record = result.value().text_edit_records.front();
		expect(record.removed_utf8 == "cafe\xCC\x81" && record.inserted_utf8 == "X",
		       "undo record 保存完整 grapheme range 的舊值與新值");
		expect(record.merge_policy == TextEditMergePolicy::continuous_typing,
		       "typed command 的 merge policy 原樣進入 CommitResult");
	}

	Transaction out_of_range(base_revision);
	out_of_range.replace_paragraph_range(make_block(1), 2, 99, "X");
	expect(!out_of_range.commit(base).is_ok(), "超出 grapheme 數的 range fail closed");
}

void test_enter_split_preserves_edge_block_and_uses_one_revision() {
	const auto container = make_container(1);
	for (std::size_t boundary = 0; boundary <= 2; ++boundary) {
		auto base = with_paragraph(DocumentRevision::initial(), make_block(1), "AB");
		base = base.with_flow_root(
			container,
			krepis::FlowSequence::empty().insert(0, make_block(1))
		);
		const auto base_revision = base.snapshot_id().content_revision;
		Transaction transaction(base_revision);
		transaction.split_paragraph(container, make_block(1), boundary, make_block(2));
		auto result = transaction.commit(base);
		expect(result.is_ok(), "Enter split 在行首、中間、行尾都可提交");
		if (!result.is_ok()) continue;
		const auto& revision = result.value().revision;
		expect(revision.snapshot_id().content_revision == base_revision + 1,
		       "split 的 record、順序與 locator 只發布一個 revision");
		const auto* sequence = revision.flow_root(container);
		expect(sequence != nullptr && sequence->block_count() == 2,
		       "split 後 Flow 恰有兩個 Block");
		if (sequence == nullptr) continue;
		const auto* primary = paragraph_for(revision, make_block(1));
		const auto* secondary = paragraph_for(revision, make_block(2));
		if (boundary == 0) {
			expect(sequence->at(0) == make_block(2) && sequence->at(1) == make_block(1),
			       "行首 Enter 在原 Block 前方加新 ID");
			expect(primary != nullptr && primary->utf8() == "AB" &&
			           secondary != nullptr && secondary->utf8().empty(),
			       "行首 Enter 不改原 Block 文字");
		} else {
			expect(sequence->at(0) == make_block(1) && sequence->at(1) == make_block(2),
			       "中間或行尾 Enter 在原 Block 後方加新 ID");
			expect(primary != nullptr && primary->utf8() == (boundary == 1 ? "A" : "AB") &&
			           secondary != nullptr && secondary->utf8() == (boundary == 1 ? "B" : ""),
			       "中間 Enter 分割文字，行尾 Enter 不改原 Block");
		}
		expect(result.value().flow_structure_records.size() == 1,
		       "split 產生一筆 typed structure undo record");
	}
}

void test_merge_adjacent_paragraphs_keeps_first_id() {
	const auto container = make_container(2);
	auto base = with_paragraph(DocumentRevision::initial(), make_block(1), "AB");
	base = with_paragraph(std::move(base), make_block(2), "CD");
	base = base.with_flow_root(
		container,
		krepis::FlowSequence::empty()
			.insert(0, make_block(1))
			.insert(1, make_block(2))
	);
	const auto base_revision = base.snapshot_id().content_revision;
	Transaction transaction(base_revision);
	transaction.merge_adjacent_paragraphs(container, make_block(1), make_block(2));
	auto result = transaction.commit(base);
	expect(result.is_ok(), "相鄰 Paragraph 可原子合併");
	if (!result.is_ok()) return;
	const auto* sequence = result.value().revision.flow_root(container);
	expect(sequence != nullptr && sequence->block_count() == 1 &&
	           sequence->at(0) == make_block(1),
	       "merge 保留前一 BlockId 並移除後一 ID");
	expect(paragraph_for(result.value().revision, make_block(1))->utf8() == "ABCD" &&
	           result.value().revision.record_for(make_block(2)) == nullptr,
	       "merge 串接兩段文字並 tombstone 後一 record");
	expect(result.value().revision.snapshot_id().content_revision == base_revision + 1,
	       "merge 只發布一個 revision");
}

void test_multiple_structure_commands_fail_closed() {
	const auto flow = make_container(3);
	auto base = with_paragraph(DocumentRevision::initial(), make_block(1), "AB");
	base = base.with_flow_root(
		flow,
		krepis::FlowSequence::empty().insert(0, make_block(1))
	);
	Transaction transaction(base.snapshot_id().content_revision);
	transaction.split_paragraph(flow, make_block(1), 1, make_block(2));
	transaction.split_paragraph(flow, make_block(1), 1, make_block(3));
	auto result = transaction.commit(base);
	expect(!result.is_ok() && result.error().code() == ErrorCode::invalid_argument,
	       "同一 Transaction 的第二個 structure command 不得靜默覆寫第一個");
}

}  // namespace

int main() {
	test_commits_multiple_replacements_as_one_revision();
	test_missing_target_rejects_every_command();
	test_non_paragraph_and_duplicate_targets_are_rejected();
	test_stale_base_invalid_utf8_and_empty_transaction_are_rejected();
	test_range_replace_uses_grapheme_boundaries_and_reports_effect();
	test_enter_split_preserves_edge_block_and_uses_one_revision();
	test_merge_adjacent_paragraphs_keeps_first_id();
	test_multiple_structure_commands_fail_closed();

	shutdown_default_reclamation_queue();
	return krepis_test::report("krepis.transaction");
}
