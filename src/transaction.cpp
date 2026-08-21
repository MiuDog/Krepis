#include "krepis/transaction.hpp"

#include "krepis/paragraph_record.hpp"
#include "krepis/text_analysis.hpp"

#include <unordered_set>
#include <utility>

namespace krepis {

Transaction::Transaction(std::uint64_t base_content_revision) noexcept
	: base_content_revision_(base_content_revision) {}

void Transaction::replace_paragraph_text(BlockId block, std::string utf8) {
	replacements_.push_back({
		block,
		0,
		0,
		true,
		std::move(utf8),
		TextEditMergePolicy::never,
	});
}

void Transaction::replace_paragraph_range(
	BlockId block,
	std::size_t grapheme_start,
	std::size_t grapheme_end,
	std::string utf8,
	TextEditMergePolicy merge_policy
) {
	replacements_.push_back({
		block,
		grapheme_start,
		grapheme_end,
		false,
		std::move(utf8),
		merge_policy,
	});
}

Result<CommitResult> Transaction::commit(const DocumentRevision& base) const {
	if (base.snapshot_id().content_revision != base_content_revision_) {
		return Error{ErrorCode::revision_conflict, "Transaction base revision 已過期"};
	}
	if (replacements_.empty()) {
		return Error{ErrorCode::invalid_argument, "Transaction 不得為空"};
	}

	const auto next_revision = base_content_revision_ + 1;
	std::unordered_set<ObjectId> targets;
	targets.reserve(replacements_.size());

	std::vector<RecordUpdate> updates;
	updates.reserve(replacements_.size());

	std::vector<LayoutInvalidation> invalidations;
	invalidations.reserve(replacements_.size());
	std::vector<ParagraphTextEditEffect> text_edit_effects;
	text_edit_effects.reserve(replacements_.size());
	std::vector<ParagraphTextEditRecord> text_edit_records;
	text_edit_records.reserve(replacements_.size());

	// 步驟 1：驗證全部 command，並建立尚未發布的 immutable records。
	for (const auto& replacement : replacements_) {
		if (!targets.insert(replacement.block.raw()).second) {
			return Error{ErrorCode::invalid_argument, "同一 Transaction 不得重複修改同一 Block"};
		}

		auto existing = base.record_for(replacement.block);
		if (existing == nullptr) {
			return Error{ErrorCode::not_found, "Paragraph Block 不存在"};
		}
		const auto* paragraph = dynamic_cast<const ParagraphRecord*>(existing.get());
		if (paragraph == nullptr) {
			return Error{ErrorCode::invalid_state, "文字 command 的目標不是 ParagraphRecord"};
		}

		auto old_analysis = analyze_text(paragraph->utf8(), {});
		if (!old_analysis.is_ok()) return old_analysis.error();
		auto inserted_analysis = analyze_text(replacement.utf8, {});
		if (!inserted_analysis.is_ok()) return inserted_analysis.error();
		const auto old_count = old_analysis.value().grapheme_boundaries.size() - 1;
		const auto inserted_count = inserted_analysis.value().grapheme_boundaries.size() - 1;
		const auto start = replacement.whole_paragraph ? 0 : replacement.grapheme_start;
		const auto end = replacement.whole_paragraph ? old_count : replacement.grapheme_end;
		if (start > end || end > old_count) {
			return Error{ErrorCode::out_of_range, "Paragraph replace range 超出 grapheme boundary"};
		}
		const auto byte_start = old_analysis.value().grapheme_boundaries[start];
		const auto byte_end = old_analysis.value().grapheme_boundaries[end];
		const std::string removed(
			paragraph->utf8(),
			byte_start,
			byte_end - byte_start
		);
		std::string updated;
		updated.reserve(paragraph->utf8().size() - (byte_end - byte_start) + replacement.utf8.size());
		updated.append(paragraph->utf8(), 0, byte_start);
		updated.append(replacement.utf8);
		updated.append(paragraph->utf8(), byte_end, std::string::npos);

		auto replacement_record = ParagraphRecord::create(next_revision, std::move(updated));
		if (!replacement_record.is_ok()) return replacement_record.error();

		updates.push_back({replacement.block, std::move(replacement_record).take()});
		invalidations.push_back({
			replacement.block,
			next_revision,
			InvalidationStage::shaping,
		});
		auto effect = ParagraphTextEditEffect{
			replacement.block,
			base_content_revision_,
			next_revision,
			start,
			end,
			inserted_count,
		};
		text_edit_effects.push_back(effect);
		text_edit_records.push_back(ParagraphTextEditRecord{
			effect,
			removed,
			replacement.utf8,
			replacement.merge_policy,
		});
	}

	// 步驟 2：全部 command 通過後才批次建立一個新 DocumentRevision。
	auto revision = base.with_updated_records(updates);
	if (!revision.is_ok()) return revision.error();

	auto committed_revision = std::move(revision).take();
	if (!committed_revision.validate().ok()) {
		return Error{ErrorCode::invalid_state, "Transaction 產生的 DocumentRevision 驗證失敗"};
	}

	return CommitResult{
		std::move(committed_revision),
		std::move(invalidations),
		std::move(text_edit_effects),
		std::move(text_edit_records),
	};
}

}  // namespace krepis
