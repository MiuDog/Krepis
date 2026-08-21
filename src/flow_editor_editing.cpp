#include "krepis/flow_editor.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace krepis {

Result<void> FlowEditor::insert_text(
	std::string utf8,
	std::uint64_t committed_at_ms,
	std::uint64_t merge_group
) {
	if (session_.composition().has_value()) {
		return Error{ErrorCode::invalid_state, "active composition 必須先 commit 或 cancel"};
	}
	if (selection_.anchor.block != selection_.focus.block) {
		return Error{ErrorCode::unsupported, "P1 FlowEditor 尚未開放跨 Block selection replacement"};
	}
	const auto start = std::min(
		selection_.anchor.grapheme_boundary,
		selection_.focus.grapheme_boundary
	);
	const auto end = std::max(
		selection_.anchor.grapheme_boundary,
		selection_.focus.grapheme_boundary
	);
	Transaction transaction(revision_.snapshot_id().content_revision);
	transaction.replace_paragraph_range(
		selection_.anchor.block,
		start,
		end,
		std::move(utf8),
		TextEditMergePolicy::continuous_typing
	);
	auto result = transaction.commit(revision_);
	if (!result.is_ok()) return result.error();
	const auto inserted = result.value().text_edit_effects.front().inserted_grapheme_count;
	return accept_commit(
		std::move(result).take(),
		UndoRecordOptions{committed_at_ms, merge_group},
		selection_.anchor.block,
		start + inserted
	);
}

Result<void> FlowEditor::insert_paragraph_break(std::uint64_t committed_at_ms) {
	if (session_.composition().has_value()) {
		return Error{ErrorCode::invalid_state, "Enter 前必須先確定 composition"};
	}
	if (selection_.anchor.block != selection_.focus.block ||
	    selection_.anchor.grapheme_boundary != selection_.focus.grapheme_boundary) {
		return Error{ErrorCode::unsupported, "P1 Enter 尚不接受非折疊 selection"};
	}
	const BlockId new_block{ids_->next()};
	Transaction transaction(revision_.snapshot_id().content_revision);
	transaction.split_paragraph(
		container_,
		selection_.anchor.block,
		selection_.anchor.grapheme_boundary,
		new_block
	);
	auto result = transaction.commit(revision_);
	if (!result.is_ok()) return result.error();
	return accept_commit(
		std::move(result).take(),
		UndoRecordOptions{committed_at_ms, 0},
		new_block,
		0
	);
}

Result<void> FlowEditor::backspace(std::uint64_t committed_at_ms) {
	if (session_.composition().has_value()) {
		return Error{ErrorCode::invalid_state, "Backspace 的 composition 行為由平台 IME 先處理"};
	}
	const auto block = selection_.anchor.block;
	const auto boundary = selection_.anchor.grapheme_boundary;
	if (block != selection_.focus.block || boundary != selection_.focus.grapheme_boundary) {
		return insert_text({}, committed_at_ms, 0);
	}
	if (boundary > 0) {
		Transaction transaction(revision_.snapshot_id().content_revision);
		transaction.replace_paragraph_range(block, boundary - 1, boundary, {});
		auto result = transaction.commit(revision_);
		if (!result.is_ok()) return result.error();
		return accept_commit(
			std::move(result).take(),
			UndoRecordOptions{committed_at_ms, 0},
			block,
			boundary - 1
		);
	}
	auto position = block_position(block);
	if (!position.is_ok()) return position.error();
	if (position.value() == 0) {
		return Error{ErrorCode::invalid_state, "文件開頭沒有可合併的前一個 Paragraph"};
	}
	const auto* sequence = revision_.flow_root(container_);
	const auto previous = sequence->at(position.value() - 1);
	auto previous_count = grapheme_count(previous);
	if (!previous_count.is_ok()) return previous_count.error();
	Transaction transaction(revision_.snapshot_id().content_revision);
	transaction.merge_adjacent_paragraphs(container_, previous, block);
	auto result = transaction.commit(revision_);
	if (!result.is_ok()) return result.error();
	return accept_commit(
		std::move(result).take(),
		UndoRecordOptions{committed_at_ms, 0},
		previous,
		previous_count.value()
	);
}

Result<void> FlowEditor::undo() {
	session_.cancel_composition();
	auto result = undo_.undo(revision_);
	if (!result.is_ok()) return result.error();
	auto next_layout = layout_after_commit(result.value());
	if (!next_layout.is_ok()) std::terminate();
	const auto prior_block = selection_.anchor.block;
	const auto prior_boundary = selection_.anchor.grapheme_boundary;
	revision_ = std::move(result.value().revision);
	layout_ = std::move(next_layout).take();
	if (revision_.record_for(prior_block) != nullptr) {
		auto count = grapheme_count(prior_block);
		if (!count.is_ok()) return count.error();
		set_collapsed_selection(prior_block, std::min(prior_boundary, count.value()));
		return {};
	}
	const auto& structure = result.value().flow_structure_records.front();
	auto count = grapheme_count(structure.primary_block);
	if (!count.is_ok()) return count.error();
	set_collapsed_selection(structure.primary_block, count.value());
	return {};
}

Result<void> FlowEditor::redo() {
	session_.cancel_composition();
	auto result = undo_.redo(revision_);
	if (!result.is_ok()) return result.error();
	auto next_layout = layout_after_commit(result.value());
	if (!next_layout.is_ok()) std::terminate();
	const auto prior_block = selection_.anchor.block;
	const auto prior_boundary = selection_.anchor.grapheme_boundary;
	revision_ = std::move(result.value().revision);
	layout_ = std::move(next_layout).take();
	if (revision_.record_for(prior_block) != nullptr) {
		auto count = grapheme_count(prior_block);
		if (!count.is_ok()) return count.error();
		set_collapsed_selection(prior_block, std::min(prior_boundary, count.value()));
		return {};
	}
	const auto& structure = result.value().flow_structure_records.front();
	auto count = grapheme_count(structure.primary_block);
	if (!count.is_ok()) return count.error();
	set_collapsed_selection(structure.primary_block, count.value());
	return {};
}

Result<void> FlowEditor::begin_composition(CompositionUpdate update) {
	return session_.begin_composition(revision_, selection_, std::move(update));
}

Result<void> FlowEditor::update_composition(CompositionUpdate update) {
	return session_.update_composition(std::move(update));
}

Result<void> FlowEditor::commit_composition(std::uint64_t committed_at_ms) {
	if (!session_.composition().has_value()) {
		return Error{ErrorCode::invalid_state, "沒有 active composition 可提交"};
	}
	const auto block = session_.composition()->replace_start.block;
	const auto start = session_.composition()->replace_start.grapheme_boundary;
	auto result = session_.commit_composition(revision_);
	if (!result.is_ok()) return result.error();
	const auto inserted = result.value().text_edit_effects.front().inserted_grapheme_count;
	return accept_commit(
		std::move(result).take(),
		UndoRecordOptions{committed_at_ms, 0},
		block,
		start + inserted
	);
}

void FlowEditor::cancel_composition() noexcept {
	session_.cancel_composition();
}

}  // namespace krepis
