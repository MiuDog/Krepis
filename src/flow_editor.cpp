#include "krepis/flow_editor.hpp"

#include "krepis/layout_invalidation.hpp"
#include "krepis/paragraph_record.hpp"
#include "krepis/text_analysis.hpp"

#include <utility>

namespace krepis {
namespace {

FlowLayoutIndex make_layout_index(
	const DocumentRevision& revision,
	ContainerId container,
	const FlowEditorStyle& style
) {
	auto result = FlowLayoutIndex::empty();
	const auto* sequence = revision.flow_root(container);
	if (sequence == nullptr) return result;
	const auto estimate = static_cast<double>(style.line_height_26_6) / 64.0 +
	                      style.block_spacing;
	for (std::size_t position = 0; position < sequence->block_count(); ++position) {
		result = result.insert(position, LayoutEntry{
			sequence->at(position),
			estimate,
			revision.snapshot_id().content_revision,
			MeasurementStatus::estimated,
		});
	}
	return result;
}

}  // namespace

Result<std::unique_ptr<FlowEditor>> FlowEditor::create(
	const FontProvider& fonts,
	IdGenerator& ids,
	std::string initial_utf8,
	FlowEditorStyle style
) {
	if (style.horizontal_padding < 0 || style.vertical_padding < 0 ||
	    style.block_spacing < 0 || style.overscan < 0 ||
	    style.font_size_26_6 <= 0 || style.line_height_26_6 <= 0) {
		return Error{ErrorCode::invalid_argument, "FlowEditor style 不合法"};
	}
	const ContainerId container{ids.next()};
	const BlockId block{ids.next()};
	auto paragraph = ParagraphRecord::create(1, std::move(initial_utf8));
	if (!paragraph.is_ok()) return paragraph.error();
	auto revision = DocumentRevision::initial().with_new_object(
		block,
		std::move(paragraph).take()
	);
	revision = revision.with_flow_root(
		container,
		FlowSequence::empty().insert(0, block)
	);
	if (!revision.validate().ok()) {
		return Error{ErrorCode::invalid_state, "FlowEditor 初始 revision 驗證失敗"};
	}
	auto layout = make_layout_index(revision, container, style);
	return std::unique_ptr<FlowEditor>(new FlowEditor(
		fonts,
		ids,
		std::move(revision),
		container,
		block,
		std::move(layout),
		style
	));
}

FlowEditor::FlowEditor(
	const FontProvider& fonts,
	IdGenerator& ids,
	DocumentRevision revision,
	ContainerId container,
	BlockId initial_block,
	FlowLayoutIndex layout,
	FlowEditorStyle style
) : ids_(&ids),
	revision_(std::move(revision)),
	container_(container),
	layout_(std::move(layout)),
	selection_{},
	session_(1),
	undo_{},
	layouter_(fonts),
	style_(style) {
	set_collapsed_selection(initial_block, 0);
}

Result<std::size_t> FlowEditor::grapheme_count(BlockId block) const {
	auto record = revision_.record_for(block);
	const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
	if (paragraph == nullptr) {
		return Error{ErrorCode::not_found, "FlowEditor Paragraph 不存在"};
	}
	auto analysis = analyze_text(paragraph->utf8(), {});
	if (!analysis.is_ok()) return analysis.error();
	return analysis.value().grapheme_boundaries.size() - 1;
}

Result<std::size_t> FlowEditor::block_position(BlockId block) const {
	const auto* sequence = revision_.flow_root(container_);
	const auto slot = revision_.resolve(block);
	if (sequence == nullptr || !slot.is_valid()) {
		return Error{ErrorCode::not_found, "FlowEditor Block 不存在"};
	}
	const auto location = revision_.locations().lookup(slot);
	if (!location.is_flow() || location.owner != container_) {
		return Error{ErrorCode::invalid_state, "FlowEditor Block owner 不符"};
	}
	const auto position = sequence->find_block_in_leaf(location.flow.leaf_key, block);
	if (!position.has_value()) {
		return Error{ErrorCode::invalid_state, "FlowEditor locator 無法解析"};
	}
	return *position;
}

Result<void> FlowEditor::set_caret(FlowEditorSelection requested) {
	const auto* sequence = revision_.flow_root(container_);
	if (sequence == nullptr || requested.block_position >= sequence->block_count()) {
		return Error{ErrorCode::out_of_range, "caret Block position 超出 Flow 範圍"};
	}
	const auto block = sequence->at(requested.block_position);
	auto count = grapheme_count(block);
	if (!count.is_ok()) return count.error();
	if (requested.grapheme_boundary > count.value()) {
		return Error{ErrorCode::out_of_range, "caret grapheme boundary 超出 Paragraph 範圍"};
	}
	session_.cancel_composition();
	set_collapsed_selection(block, requested.grapheme_boundary);
	return {};
}

void FlowEditor::set_collapsed_selection(BlockId block, std::size_t boundary) noexcept {
	const auto revision = revision_.snapshot_id().content_revision;
	selection_ = TextSelection{
		TextAnchor{block, revision, boundary, AnchorAffinity::upstream},
		TextAnchor{block, revision, boundary, AnchorAffinity::downstream},
	};
}

Result<void> FlowEditor::accept_commit(
	CommitResult committed,
	UndoRecordOptions undo_options,
	BlockId caret_block,
	std::size_t caret_boundary
) {
	auto integrated = layout_after_commit(committed);
	if (!integrated.is_ok()) return integrated.error();
	auto recorded = undo_.record(committed, undo_options);
	if (!recorded.is_ok()) return recorded.error();
	revision_ = std::move(committed.revision);
	layout_ = std::move(integrated).take();
	set_collapsed_selection(caret_block, caret_boundary);
	return {};
}

Result<FlowLayoutIndex> FlowEditor::layout_after_commit(
	const CommitResult& committed
) const {
	if (committed.flow_structure_records.empty()) {
		return apply_layout_invalidations(
			committed.revision,
			container_,
			layout_,
			committed.invalidations
		);
	}
	if (committed.flow_structure_records.size() != 1) {
		return Error{ErrorCode::invalid_state, "P1 FlowEditor 只接受一個結構 edit record"};
	}
	const auto& edit = committed.flow_structure_records.front();
	const auto* sequence = committed.revision.flow_root(container_);
	if (sequence == nullptr) {
		return Error{ErrorCode::invalid_state, "結構 commit 缺少 FlowSequence"};
	}
	auto result = layout_;
	const auto estimate = static_cast<double>(style_.line_height_26_6) / 64.0 +
	                      style_.block_spacing;
	if (sequence->block_count() == layout_.block_count() + 1) {
		if (edit.secondary_position > result.block_count()) {
			return Error{ErrorCode::invalid_state, "結構 insert 位置超出 layout 範圍"};
		}
		result = result.insert(edit.secondary_position, LayoutEntry{
			edit.secondary_block,
			estimate,
			committed.revision.snapshot_id().content_revision,
			MeasurementStatus::estimated,
		});
	} else if (sequence->block_count() + 1 == layout_.block_count()) {
		if (edit.secondary_position >= result.block_count() ||
		    result.at(edit.secondary_position).block_id != edit.secondary_block) {
			return Error{ErrorCode::invalid_state, "結構 remove 位置與 layout 分岔"};
		}
		result = result.remove(edit.secondary_position);
	} else {
		return Error{ErrorCode::invalid_state, "結構 commit 的 Block 數變化不是一"};
	}
	const auto primary_slot = committed.revision.resolve(edit.primary_block);
	const auto primary_location = committed.revision.locations().lookup(primary_slot);
	if (!primary_location.is_flow() || primary_location.owner != container_) {
		return Error{ErrorCode::invalid_state, "結構 commit 的 primary locator 不合法"};
	}
	const auto primary_position = sequence->find_block_in_leaf(
		primary_location.flow.leaf_key,
		edit.primary_block
	);
	if (!primary_position.has_value() ||
	    result.at(*primary_position).block_id != edit.primary_block) {
		return Error{ErrorCode::invalid_state, "結構 commit 的 primary layout entry 分岔"};
	}
	return result.invalidate_extent(
		*primary_position,
		committed.revision.snapshot_id().content_revision
	);
}

void FlowEditor::rebuild_layout_index() {
	layout_ = make_layout_index(revision_, container_, style_);
}

}  // namespace krepis
