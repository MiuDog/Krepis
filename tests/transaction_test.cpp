#include "krepis/transaction.hpp"

#include "krepis/intrusive_ptr.hpp"
#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <string>

using krepis::BlockId;
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

}  // namespace

int main() {
	test_commits_multiple_replacements_as_one_revision();
	test_missing_target_rejects_every_command();
	test_non_paragraph_and_duplicate_targets_are_rejected();
	test_stale_base_invalid_utf8_and_empty_transaction_are_rejected();
	test_range_replace_uses_grapheme_boundaries_and_reports_effect();

	shutdown_default_reclamation_queue();
	return krepis_test::report("krepis.transaction");
}
