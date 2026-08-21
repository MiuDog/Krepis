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

void Transaction::split_paragraph(
	ContainerId container,
	BlockId block,
	std::size_t grapheme_boundary,
	BlockId new_block
) {
	++structure_command_count_;
	split_ = SplitParagraph{container, block, grapheme_boundary, new_block};
}

void Transaction::merge_adjacent_paragraphs(
	ContainerId container,
	BlockId first,
	BlockId second
) {
	++structure_command_count_;
	merge_ = MergeParagraphs{container, first, second};
}

Result<CommitResult> Transaction::commit(const DocumentRevision& base) const {
	if (base.snapshot_id().content_revision != base_content_revision_) {
		return Error{ErrorCode::revision_conflict, "Transaction base revision 已過期"};
	}
	const auto command_kinds = (!replacements_.empty() ? 1u : 0u) +
	                           (split_.has_value() ? 1u : 0u) +
	                           (merge_.has_value() ? 1u : 0u);
	if (command_kinds == 0) {
		return Error{ErrorCode::invalid_argument, "Transaction 不得為空"};
	}
	if (command_kinds != 1) {
		return Error{ErrorCode::invalid_argument, "P1 結構 command 不得與其他 command 混用"};
	}
	if (structure_command_count_ > 1) {
		return Error{ErrorCode::invalid_argument, "P1 Transaction 一次只接受一個結構 command"};
	}

	const auto next_revision = base_content_revision_ + 1;
	if (split_.has_value()) {
		const auto& split = split_.value();
		if (split.new_block.is_nil() || base.resolve(split.new_block).is_valid()) {
			return Error{ErrorCode::invalid_argument, "split 的新 BlockId 必須尚未使用"};
		}
		const auto* sequence = base.flow_root(split.container);
		if (sequence == nullptr) {
			return Error{ErrorCode::not_found, "split 的 FlowContainer 不存在"};
		}
		const auto slot = base.resolve(split.block);
		const auto location = base.locations().lookup(slot);
		if (!slot.is_valid() || !location.is_flow() || location.owner != split.container) {
			return Error{ErrorCode::invalid_state, "split 目標不直接屬於指定 FlowContainer"};
		}
		const auto rank = sequence->find_block_in_leaf(location.flow.leaf_key, split.block);
		if (!rank.has_value()) {
			return Error{ErrorCode::invalid_state, "split locator 無法解析"};
		}
		auto existing = base.record_for(split.block);
		const auto* paragraph = dynamic_cast<const ParagraphRecord*>(existing.get());
		if (paragraph == nullptr) {
			return Error{ErrorCode::invalid_state, "split 目標不是 ParagraphRecord"};
		}
		auto analysis = analyze_text(paragraph->utf8(), {});
		if (!analysis.is_ok()) return analysis.error();
		if (split.grapheme_boundary >= analysis.value().grapheme_boundaries.size()) {
			return Error{ErrorCode::out_of_range, "split grapheme boundary 超出範圍"};
		}

		const auto byte_boundary = analysis.value().grapheme_boundaries[split.grapheme_boundary];
		const auto at_start = split.grapheme_boundary == 0;
		const std::string primary_after = at_start
			? paragraph->utf8()
			: paragraph->utf8().substr(0, byte_boundary);
		const std::string secondary_after = at_start
			? std::string{}
			: paragraph->utf8().substr(byte_boundary);
		const auto secondary_position = *rank + (at_start ? 0 : 1);
		auto primary_record = ParagraphRecord::create(next_revision, primary_after);
		auto secondary_record = ParagraphRecord::create(next_revision, secondary_after);
		if (!primary_record.is_ok()) return primary_record.error();
		if (!secondary_record.is_ok()) return secondary_record.error();
		auto next_sequence = sequence->insert(secondary_position, split.new_block);
		std::vector<FlowRecordMutation> mutations;
		mutations.push_back({split.block, std::move(primary_record).take(), false});
		mutations.push_back({split.new_block, std::move(secondary_record).take(), false});
		auto revision = base.with_atomic_flow_edit(
			split.container,
			std::move(next_sequence),
			mutations
		);
		if (!revision.is_ok()) return revision.error();
		auto committed = std::move(revision).take();
		if (!committed.validate().ok()) {
			return Error{ErrorCode::invalid_state, "split 產生的 DocumentRevision 驗證失敗"};
		}
		std::vector<LayoutInvalidation> invalidations{
			{split.block, next_revision, InvalidationStage::shaping},
			{split.new_block, next_revision, InvalidationStage::shaping},
		};
		std::vector<FlowStructureEditRecord> structures{
			{
				FlowStructureEditKind::split_paragraph,
				split.container,
				secondary_position,
				split.block,
				split.new_block,
				paragraph->utf8(),
				primary_after,
				secondary_after,
			},
		};
		return CommitResult{
			std::move(committed),
			std::move(invalidations),
			{},
			{},
			std::move(structures),
		};
	}
	if (merge_.has_value()) {
		const auto& merge = merge_.value();
		const auto* sequence = base.flow_root(merge.container);
		if (sequence == nullptr) {
			return Error{ErrorCode::not_found, "merge 的 FlowContainer 不存在"};
		}
		const auto first_slot = base.resolve(merge.first);
		const auto second_slot = base.resolve(merge.second);
		const auto first_location = base.locations().lookup(first_slot);
		const auto second_location = base.locations().lookup(second_slot);
		if (!first_location.is_flow() || !second_location.is_flow() ||
		    first_location.owner != merge.container || second_location.owner != merge.container) {
			return Error{ErrorCode::invalid_state, "merge 的兩個 Block 必須直接屬於同一 Flow"};
		}
		const auto first_rank = sequence->find_block_in_leaf(
			first_location.flow.leaf_key,
			merge.first
		);
		const auto second_rank = sequence->find_block_in_leaf(
			second_location.flow.leaf_key,
			merge.second
		);
		if (!first_rank.has_value() || !second_rank.has_value() || *second_rank != *first_rank + 1) {
			return Error{ErrorCode::invalid_argument, "merge 只接受相鄰且順序正確的 Paragraph"};
		}
		auto first_record = base.record_for(merge.first);
		auto second_record = base.record_for(merge.second);
		const auto* first_paragraph = dynamic_cast<const ParagraphRecord*>(first_record.get());
		const auto* second_paragraph = dynamic_cast<const ParagraphRecord*>(second_record.get());
		if (first_paragraph == nullptr || second_paragraph == nullptr) {
			return Error{ErrorCode::invalid_state, "merge 目標不是 ParagraphRecord"};
		}
		const auto primary_before = first_paragraph->utf8();
		const auto secondary_before = second_paragraph->utf8();
		const auto primary_after = primary_before + secondary_before;
		auto combined = ParagraphRecord::create(next_revision, primary_after);
		if (!combined.is_ok()) return combined.error();
		auto next_sequence = sequence->remove(*second_rank);
		std::vector<FlowRecordMutation> mutations;
		mutations.push_back({merge.first, std::move(combined).take(), false});
		mutations.push_back({merge.second, {}, true});
		auto revision = base.with_atomic_flow_edit(
			merge.container,
			std::move(next_sequence),
			mutations
		);
		if (!revision.is_ok()) return revision.error();
		auto committed = std::move(revision).take();
		if (!committed.validate().ok()) {
			return Error{ErrorCode::invalid_state, "merge 產生的 DocumentRevision 驗證失敗"};
		}
		std::vector<FlowStructureEditRecord> structures{
			{
				FlowStructureEditKind::merge_paragraphs,
				merge.container,
				*second_rank,
				merge.first,
				merge.second,
				primary_before,
				primary_after,
				secondary_before,
			},
		};
		return CommitResult{
			std::move(committed),
			{{merge.first, next_revision, InvalidationStage::shaping}},
			{},
			{},
			std::move(structures),
		};
	}
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
		{},
	};
}

}  // namespace krepis
