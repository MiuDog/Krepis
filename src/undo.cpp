#include "krepis/undo.hpp"

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
	if (committed.text_edit_records.empty()) {
		return Error{ErrorCode::invalid_argument, "CommitResult 沒有可 undo 的 typed command"};
	}
	Entry incoming{committed.text_edit_records, options};
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
	if (prior.edits.size() != 1 || incoming.edits.size() != 1 ||
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
