#include "krepis/undo.hpp"

#include "krepis/paragraph_record.hpp"
#include "krepis/text_analysis.hpp"

#include <utility>

namespace krepis {
namespace {

Result<std::size_t> grapheme_count(const std::string& utf8) {
	auto analysis = analyze_text(utf8, {});
	if (!analysis.is_ok()) return analysis.error();
	return analysis.value().grapheme_boundaries.size() - 1;
}

}  // namespace

UndoManager::UndoManager(std::uint64_t typing_merge_window_ms) noexcept
	: typing_merge_window_ms_(typing_merge_window_ms) {}

Result<void> UndoManager::record(
	const CommitResult& committed,
	UndoRecordOptions options
) {
	if (committed.text_edit_records.empty() && committed.flow_structure_records.empty()) {
		return Error{ErrorCode::invalid_argument, "CommitResult 沒有可 undo 的 typed command"};
	}
	if (!committed.text_edit_records.empty() && !committed.flow_structure_records.empty()) {
		return Error{ErrorCode::invalid_argument, "P1 undo entry 不接受文字與結構 command 混用"};
	}
	Entry incoming{
		committed.text_edit_records,
		committed.flow_structure_records,
		options,
	};
	if (!undo_.empty() && can_merge(undo_.back(), incoming)) {
		auto& prior = undo_.back();
		prior.edits.front().inserted_utf8.append(incoming.edits.front().inserted_utf8);
		prior.edits.front().effect.inserted_grapheme_count +=
			incoming.edits.front().effect.inserted_grapheme_count;
		prior.edits.front().effect.after_content_revision =
			incoming.edits.front().effect.after_content_revision;
		prior.options.committed_at_ms = options.committed_at_ms;
	} else {
		undo_.push_back(std::move(incoming));
	}
	redo_.clear();
	return {};
}

bool UndoManager::can_merge(const Entry& prior, const Entry& incoming) const {
	if (!prior.structures.empty() || !incoming.structures.empty() ||
	    prior.edits.size() != 1 || incoming.edits.size() != 1 ||
	    prior.options.merge_group == 0 ||
	    prior.options.merge_group != incoming.options.merge_group ||
	    incoming.options.committed_at_ms < prior.options.committed_at_ms ||
	    incoming.options.committed_at_ms - prior.options.committed_at_ms >
	        typing_merge_window_ms_) {
		return false;
	}
	const auto& left = prior.edits.front();
	const auto& right = incoming.edits.front();
	return left.merge_policy == TextEditMergePolicy::continuous_typing &&
	       right.merge_policy == TextEditMergePolicy::continuous_typing &&
	       left.effect.block == right.effect.block &&
	       left.removed_utf8.empty() && right.removed_utf8.empty() &&
	       right.effect.replaced_grapheme_start ==
	           left.effect.replaced_grapheme_start + left.effect.inserted_grapheme_count &&
	       right.effect.replaced_grapheme_end == right.effect.replaced_grapheme_start;
}

Result<CommitResult> UndoManager::apply(
	const DocumentRevision& current,
	const Entry& entry,
	bool inverse
) const {
	if (!entry.structures.empty()) {
		if (!entry.edits.empty() || entry.structures.size() != 1) {
			return Error{ErrorCode::invalid_state, "P1 undo entry 的結構 command 數量不合法"};
		}
		const auto& edit = entry.structures.front();
		const auto* current_sequence = current.flow_root(edit.container);
		if (current_sequence == nullptr) {
			return Error{ErrorCode::not_found, "undo 的 FlowContainer 不存在"};
		}
		const auto secondary_should_exist = edit.kind == FlowStructureEditKind::split_paragraph
			? !inverse
			: inverse;
		auto next_sequence = *current_sequence;
		if (secondary_should_exist) {
			if (edit.secondary_position > next_sequence.block_count()) {
				return Error{ErrorCode::out_of_range, "undo 回復位置超出 Flow 範圍"};
			}
			const auto secondary_slot = current.resolve(edit.secondary_block);
			if (secondary_slot.is_valid() &&
			    !current.locations().lookup(secondary_slot).is_empty()) {
				return Error{ErrorCode::invalid_state, "undo 不得重複插入已有 owner 的 Block"};
			}
			next_sequence = next_sequence.insert(edit.secondary_position, edit.secondary_block);
		} else {
			if (edit.secondary_position >= next_sequence.block_count() ||
			    next_sequence.at(edit.secondary_position) != edit.secondary_block) {
				return Error{ErrorCode::invalid_state, "undo 的結構位置已與 history 分岔"};
			}
			next_sequence = next_sequence.remove(edit.secondary_position);
		}

		const auto& primary_text = inverse
			? edit.primary_before_utf8
			: edit.primary_after_utf8;
		auto primary = ParagraphRecord::create(
			current.snapshot_id().content_revision + 1,
			primary_text
		);
		if (!primary.is_ok()) return primary.error();
		std::vector<FlowRecordMutation> mutations;
		mutations.push_back({edit.primary_block, std::move(primary).take(), false});
		if (secondary_should_exist) {
			auto secondary = ParagraphRecord::create(
				current.snapshot_id().content_revision + 1,
				edit.secondary_utf8
			);
			if (!secondary.is_ok()) return secondary.error();
			mutations.push_back({edit.secondary_block, std::move(secondary).take(), false});
		} else {
			mutations.push_back({edit.secondary_block, {}, true});
		}
		auto revision = current.with_atomic_flow_edit(
			edit.container,
			std::move(next_sequence),
			mutations
		);
		if (!revision.is_ok()) return revision.error();
		auto committed = std::move(revision).take();
		if (!committed.validate().ok()) {
			return Error{ErrorCode::invalid_state, "undo 結構 command 產生無效 revision"};
		}
		return CommitResult{
			std::move(committed),
			{{
				edit.primary_block,
				current.snapshot_id().content_revision + 1,
				InvalidationStage::shaping,
			}, {
				edit.secondary_block,
				current.snapshot_id().content_revision + 1,
				InvalidationStage::shaping,
			}},
			{},
			{},
			{edit},
		};
	}

	Transaction transaction(current.snapshot_id().content_revision);
	for (const auto& edit : entry.edits) {
		auto removed_count = grapheme_count(edit.removed_utf8);
		if (!removed_count.is_ok()) return removed_count.error();
		const auto start = edit.effect.replaced_grapheme_start;
		const auto end = start + (inverse
			? edit.effect.inserted_grapheme_count
			: removed_count.value());
		transaction.replace_paragraph_range(
			edit.effect.block,
			start,
			end,
			inverse ? edit.removed_utf8 : edit.inserted_utf8
		);
	}
	return transaction.commit(current);
}

Result<CommitResult> UndoManager::undo(const DocumentRevision& current) {
	if (undo_.empty()) return Error{ErrorCode::invalid_state, "沒有可 undo 的 entry"};
	auto result = apply(current, undo_.back(), true);
	if (!result.is_ok()) return result.error();
	redo_.push_back(std::move(undo_.back()));
	undo_.pop_back();
	return result;
}

Result<CommitResult> UndoManager::redo(const DocumentRevision& current) {
	if (redo_.empty()) return Error{ErrorCode::invalid_state, "沒有可 redo 的 entry"};
	auto result = apply(current, redo_.back(), false);
	if (!result.is_ok()) return result.error();
	undo_.push_back(std::move(redo_.back()));
	redo_.pop_back();
	return result;
}

void UndoManager::clear() noexcept {
	undo_.clear();
	redo_.clear();
}

}  // namespace krepis
