#include "krepis/transaction.hpp"

#include "krepis/paragraph_record.hpp"

#include <unordered_set>
#include <utility>

namespace krepis {

Transaction::Transaction(std::uint64_t base_content_revision) noexcept
	: base_content_revision_(base_content_revision) {}

void Transaction::replace_paragraph_text(BlockId block, std::string utf8) {
	replacements_.push_back({block, std::move(utf8)});
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

	// 步驟 1：驗證全部 command，並建立尚未發布的 immutable records。
	for (const auto& replacement : replacements_) {
		if (!targets.insert(replacement.block.raw()).second) {
			return Error{ErrorCode::invalid_argument, "同一 Transaction 不得重複修改同一 Block"};
		}

		auto existing = base.record_for(replacement.block);
		if (existing == nullptr) {
			return Error{ErrorCode::not_found, "Paragraph Block 不存在"};
		}
		if (dynamic_cast<const ParagraphRecord*>(existing.get()) == nullptr) {
			return Error{ErrorCode::invalid_state, "文字 command 的目標不是 ParagraphRecord"};
		}

		auto replacement_record = ParagraphRecord::create(next_revision, replacement.utf8);
		if (!replacement_record.is_ok()) return replacement_record.error();

		updates.push_back({replacement.block, std::move(replacement_record).take()});
		invalidations.push_back({
			replacement.block,
			next_revision,
			InvalidationStage::shaping,
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
	};
}

}  // namespace krepis
