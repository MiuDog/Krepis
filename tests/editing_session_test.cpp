#include "krepis/editing_session.hpp"

#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <string>
#include <utility>

using krepis::AnchorAffinity;
using krepis::BlockId;
using krepis::CommitResult;
using krepis::CompositionAttribute;
using krepis::CompositionSegment;
using krepis::CompositionUpdate;
using krepis::DocumentRevision;
using krepis::EditingSession;
using krepis::ObjectId;
using krepis::ParagraphRecord;
using krepis::TextAnchor;
using krepis::TextSelection;
using krepis::Transaction;
using krepis_test::expect;

namespace {

BlockId block(std::uint64_t value) {
	return BlockId{ObjectId{0, value}};
}

DocumentRevision with_paragraph(DocumentRevision revision, BlockId id, const char* text) {
	auto record = ParagraphRecord::create(revision.snapshot_id().content_revision + 1, text);
	expect(record.is_ok(), "composition fixture Paragraph 建立成功");
	return revision.with_new_object(id, std::move(record).take());
}

std::string paragraph_text(const DocumentRevision& revision, BlockId id) {
	auto record = revision.record_for(id);
	const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
	return paragraph == nullptr ? std::string{} : paragraph->utf8();
}

TextSelection selection(BlockId id, std::uint64_t revision, std::size_t start, std::size_t end) {
	return TextSelection{
		TextAnchor{id, revision, start, AnchorAffinity::upstream},
		TextAnchor{id, revision, end, AnchorAffinity::downstream},
	};
}

CompositionUpdate update(std::string text) {
	const auto size = text.size();
	return CompositionUpdate{
		std::move(text),
		size,
		size,
		{{0, size, CompositionAttribute::raw_input}},
	};
}

void test_overlay_does_not_mutate_authority_and_cancel_is_free() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCD");
	EditingSession session(7);
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 1, 3),
		update("X")
	).is_ok(), "可在 committed selection 上開始 composition");
	auto composed = session.composed_paragraph(base);
	expect(composed.is_ok() && composed.value().utf8 == "AXD",
	       "overlay 視圖以 provisional text 取代 range");
	expect(paragraph_text(base, block(1)) == "ABCD", "overlay 不修改 ObjectStore authority");
	session.cancel_composition();
	expect(!session.composition().has_value(), "cancel 只清除 session overlay");
	expect(paragraph_text(base, block(1)) == "ABCD", "cancel 不產生文件修改");
}

void test_d14_remote_same_boundary_insert_stays_after_composition() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCD");
	EditingSession session(8);
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 2, 2),
		update("X")
	).is_ok(), "collapsed composition 開始成功");

	Transaction remote(base.snapshot_id().content_revision);
	remote.replace_paragraph_range(block(1), 2, 2, "Y");
	auto accepted = remote.commit(base);
	expect(accepted.is_ok(), "remote 同位置 insert 被 Authority 接受");
	if (!accepted.is_ok()) return;
	expect(session.rebase_composition(accepted.value()).is_ok(), "composition 通過 effect rebase");
	auto visible = session.composed_paragraph(accepted.value().revision);
	expect(visible.is_ok() && visible.value().utf8 == "ABXYCD",
	       "D14 overlay 留在同位置 remote insert 前方");
	auto committed = session.commit_composition(accepted.value().revision);
	expect(committed.is_ok() && paragraph_text(committed.value().revision, block(1)) == "ABXYCD",
	       "composition commit 結果不是 ABYXCD");
	expect(!session.composition().has_value(), "commit 成功後才清除 overlay");
}

void test_d15_replace_rebases_anchor_and_block_delete_cancels() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCDEF");
	EditingSession session(9);
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 4, 4),
		update("X")
	).is_ok(), "D15 fixture composition 開始成功");
	Transaction remote(base.snapshot_id().content_revision);
	remote.replace_paragraph_range(block(1), 1, 4, "Y");
	auto accepted = remote.commit(base);
	expect(accepted.is_ok() && session.rebase_composition(accepted.value()).is_ok(),
	       "涵蓋 anchor 的 replacement 可 rebase");
	if (!accepted.is_ok()) return;
	auto visible = session.composed_paragraph(accepted.value().revision);
	expect(visible.is_ok() && visible.value().utf8 == "AYXEF",
	       "D15 composition 位於 replacement content 後");

	auto deleted = accepted.value().revision.with_deleted_object(block(1));
	CommitResult deletion{std::move(deleted), {}, {}, {}};
	expect(session.rebase_composition(deletion).is_ok(), "Block delete rebase 本身成功");
	expect(!session.composition().has_value(), "target Block 被刪除時取消 composition");
}

void test_multiple_segments_and_commit_are_one_range_transaction() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCD");
	EditingSession session(10);
	CompositionUpdate mixed{
		"\xE3\x84\x93\xE7\x9F\xA5",
		3,
		6,
		{
			{0, 3, CompositionAttribute::raw_input},
			{3, 6, CompositionAttribute::target_converted},
		},
	};
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 1, 3),
		std::move(mixed)
	).is_ok(), "同一 composition 可同時有 raw 與 target-converted segments");
	auto committed = session.commit_composition(base);
	expect(committed.is_ok(), "composition 以單一 range transaction commit");
	if (committed.is_ok()) {
		expect(committed.value().text_edit_effects.size() == 1,
		       "composition commit 只產生一筆文字 effect");
		expect(paragraph_text(committed.value().revision, block(1)) ==
		           "A\xE3\x84\x93\xE7\x9F\xA5" "D",
		       "多 segment 仍整體一次成為 committed text");
	}
}

void test_failed_commit_preserves_overlay() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCD");
	EditingSession session(11);
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 2, 2),
		update("X")
	).is_ok(), "stale commit fixture composition 開始成功");
	auto newer = with_paragraph(base, block(2), "remote");
	auto failed = session.commit_composition(newer);
	expect(!failed.is_ok(), "尚未 rebase 的 composition 不可提交到較新 revision");
	expect(session.composition().has_value(), "commit 失敗時保留 overlay 供使用者重試");
}

void test_invalid_segment_update_is_rejected_without_mutating_state() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCD");
	EditingSession session(12);
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 2, 2),
		update("X")
	).is_ok(), "invalid update fixture composition 開始成功");
	const auto prior_revision = session.composition()->local_revision;
	CompositionUpdate invalid{
		"\xE7\x9F\xA5",
		0,
		3,
		{{1, 3, CompositionAttribute::target_converted}},
	};
	expect(!session.update_composition(std::move(invalid)).is_ok(),
	       "segment 不可切入 UTF-8 code point 中間");
	expect(session.composition()->local_revision == prior_revision,
	       "非法 update 不可改變 composition revision");
}

void test_unrelated_commit_only_advances_anchor_revision() {
	auto base = with_paragraph(DocumentRevision::initial(), block(1), "ABCD");
	base = with_paragraph(std::move(base), block(2), "remote");
	EditingSession session(13);
	expect(session.begin_composition(
		base,
		selection(block(1), base.snapshot_id().content_revision, 2, 2),
		update("X")
	).is_ok(), "unrelated commit fixture composition 開始成功");
	Transaction remote(base.snapshot_id().content_revision);
	remote.replace_paragraph_range(block(2), 0, 0, "Y");
	auto accepted = remote.commit(base);
	expect(accepted.is_ok() && session.rebase_composition(accepted.value()).is_ok(),
	       "無關 Block commit 可推進 composition base revision");
	if (!accepted.is_ok()) return;
	auto visible = session.composed_paragraph(accepted.value().revision);
	expect(visible.is_ok() && visible.value().utf8 == "ABXCD",
	       "無關 Block 修改不移動 composition range");
}

}  // namespace

int main() {
	test_overlay_does_not_mutate_authority_and_cancel_is_free();
	test_d14_remote_same_boundary_insert_stays_after_composition();
	test_d15_replace_rebases_anchor_and_block_delete_cancels();
	test_multiple_segments_and_commit_are_one_range_transaction();
	test_failed_commit_preserves_overlay();
	test_invalid_segment_update_is_rejected_without_mutating_state();
	test_unrelated_commit_only_advances_anchor_revision();
	return krepis_test::report("krepis.editing_session");
}
