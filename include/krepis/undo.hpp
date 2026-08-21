#pragma once

// EDT-0001 D3～D4：由 typed command 宣告合併能力的全域 undo／redo 序列。

#include "krepis/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace krepis {

struct UndoRecordOptions {
	std::uint64_t committed_at_ms = 0;
	std::uint64_t merge_group = 0;
};

class UndoManager {
public:
	explicit UndoManager(std::uint64_t typing_merge_window_ms = 1'000) noexcept;

	[[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
	[[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
	[[nodiscard]] std::size_t undo_size() const noexcept { return undo_.size(); }
	[[nodiscard]] std::size_t redo_size() const noexcept { return redo_.size(); }

	[[nodiscard]] Result<void> record(
		const CommitResult& committed,
		UndoRecordOptions options
	);
	[[nodiscard]] Result<CommitResult> undo(const DocumentRevision& current);
	[[nodiscard]] Result<CommitResult> redo(const DocumentRevision& current);
	void clear() noexcept;

private:
	struct Entry {
		std::vector<ParagraphTextEditRecord> edits;
		std::vector<FlowStructureEditRecord> structures;
		UndoRecordOptions options;
	};

	[[nodiscard]] bool can_merge(const Entry& prior, const Entry& incoming) const;
	[[nodiscard]] Result<CommitResult> apply(
		const DocumentRevision& current,
		const Entry& entry,
		bool inverse
	) const;

	std::uint64_t typing_merge_window_ms_;
	std::vector<Entry> undo_;
	std::vector<Entry> redo_;
};

}  // namespace krepis
