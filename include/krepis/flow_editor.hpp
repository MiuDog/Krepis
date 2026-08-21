#pragma once

// P1 Flow 垂直切片：將文件、編輯、composition、layout 與 display list 串成單一權威。

#include "krepis/display_list.hpp"
#include "krepis/document_revision.hpp"
#include "krepis/editing_session.hpp"
#include "krepis/flow_layout_index.hpp"
#include "krepis/id_generator.hpp"
#include "krepis/text_layout.hpp"
#include "krepis/undo.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace krepis {

struct FlowEditorStyle {
	double horizontal_padding = 24.0;
	double vertical_padding = 24.0;
	double block_spacing = 8.0;
	double overscan = 240.0;
	std::int32_t font_size_26_6 = 16 * 64;
	std::int32_t line_height_26_6 = 24 * 64;
	std::uint32_t text_rgba = 0xFF202020;
	std::uint32_t caret_rgba = 0xFF2F6FEB;
};

struct FlowEditorSelection {
	std::size_t block_position = 0;
	std::size_t grapheme_boundary = 0;
};

class FlowEditor {
public:
	[[nodiscard]] static Result<std::unique_ptr<FlowEditor>> create(
		const FontProvider& fonts,
		IdGenerator& ids,
		std::string initial_utf8,
		FlowEditorStyle style = {}
	);

	[[nodiscard]] const DocumentRevision& revision() const noexcept { return revision_; }
	[[nodiscard]] ContainerId container() const noexcept { return container_; }
	[[nodiscard]] const TextSelection& selection() const noexcept { return selection_; }
	[[nodiscard]] const FlowLayoutIndex& layout_index() const noexcept { return layout_; }
	[[nodiscard]] LayoutCacheStats layout_cache_stats() const noexcept {
		return layouter_.cache_stats();
	}
	[[nodiscard]] bool can_undo() const noexcept { return undo_.can_undo(); }
	[[nodiscard]] bool can_redo() const noexcept { return undo_.can_redo(); }

	[[nodiscard]] Result<void> set_caret(FlowEditorSelection selection);
	[[nodiscard]] Result<void> insert_text(
		std::string utf8,
		std::uint64_t committed_at_ms,
		std::uint64_t merge_group
	);
	[[nodiscard]] Result<void> insert_paragraph_break(std::uint64_t committed_at_ms);
	[[nodiscard]] Result<void> backspace(std::uint64_t committed_at_ms);
	[[nodiscard]] Result<void> undo();
	[[nodiscard]] Result<void> redo();

	[[nodiscard]] Result<void> begin_composition(CompositionUpdate update);
	[[nodiscard]] Result<void> update_composition(CompositionUpdate update);
	[[nodiscard]] Result<void> commit_composition(std::uint64_t committed_at_ms);
	void cancel_composition() noexcept;

	[[nodiscard]] Result<std::uint64_t> publish_display(
		DisplayListPublisher& publisher,
		double viewport_width,
		double viewport_height,
		double scroll_y
	);

private:
	FlowEditor(
		const FontProvider& fonts,
		IdGenerator& ids,
		DocumentRevision revision,
		ContainerId container,
		BlockId initial_block,
		FlowLayoutIndex layout,
		FlowEditorStyle style
	);

	[[nodiscard]] Result<void> accept_commit(
		CommitResult committed,
		UndoRecordOptions undo_options,
		BlockId caret_block,
		std::size_t caret_boundary
	);
	[[nodiscard]] Result<FlowLayoutIndex> layout_after_commit(
		const CommitResult& committed
	) const;
	[[nodiscard]] Result<std::size_t> grapheme_count(BlockId block) const;
	[[nodiscard]] Result<std::size_t> block_position(BlockId block) const;
	void rebuild_layout_index();
	void set_collapsed_selection(BlockId block, std::size_t boundary) noexcept;

	IdGenerator* ids_;
	DocumentRevision revision_;
	ContainerId container_;
	FlowLayoutIndex layout_;
	TextSelection selection_;
	EditingSession session_;
	UndoManager undo_;
	CachedParagraphLayouter layouter_;
	FlowEditorStyle style_;
};

}  // namespace krepis
