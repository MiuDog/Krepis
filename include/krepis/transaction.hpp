#pragma once

// EDT-0001：單層原子 Transaction。
// 第一條實作路徑只包含 ReplaceParagraphText typed command。

#include "krepis/document_revision.hpp"
#include "krepis/error.hpp"
#include "krepis/editing.hpp"
#include "krepis/object_id.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace krepis {

enum class InvalidationStage : std::uint8_t {
	shaping,
	line_break,
	extent,
	paint,
	hit_test,
};

struct LayoutInvalidation {
	BlockId block;
	std::uint64_t source_content_revision = 0;
	InvalidationStage stage = InvalidationStage::shaping;
};

enum class TextEditMergePolicy : std::uint8_t {
	never,
	continuous_typing,
};

struct ParagraphTextEditRecord {
	ParagraphTextEditEffect effect;
	std::string removed_utf8;
	std::string inserted_utf8;
	TextEditMergePolicy merge_policy = TextEditMergePolicy::never;
};

enum class FlowStructureEditKind : std::uint8_t {
	split_paragraph,
	merge_paragraphs,
};

struct FlowStructureEditRecord {
	FlowStructureEditKind kind;
	ContainerId container;
	std::size_t secondary_position;
	BlockId primary_block;
	BlockId secondary_block;
	std::string primary_before_utf8;
	std::string primary_after_utf8;
	std::string secondary_utf8;
};

struct CommitResult {
	DocumentRevision revision;
	std::vector<LayoutInvalidation> invalidations;
	std::vector<ParagraphTextEditEffect> text_edit_effects;
	std::vector<ParagraphTextEditRecord> text_edit_records;
	std::vector<FlowStructureEditRecord> flow_structure_records;
};

class Transaction {
public:
	explicit Transaction(std::uint64_t base_content_revision) noexcept;

	void replace_paragraph_text(BlockId block, std::string utf8);
	void replace_paragraph_range(
		BlockId block,
		std::size_t grapheme_start,
		std::size_t grapheme_end,
		std::string utf8,
		TextEditMergePolicy merge_policy = TextEditMergePolicy::never
	);
	void split_paragraph(
		ContainerId container,
		BlockId block,
		std::size_t grapheme_boundary,
		BlockId new_block
	);
	void merge_adjacent_paragraphs(
		ContainerId container,
		BlockId first,
		BlockId second
	);

	[[nodiscard]] Result<CommitResult> commit(const DocumentRevision& base) const;

private:
	struct ReplaceParagraphText {
		BlockId block;
		std::size_t grapheme_start;
		std::size_t grapheme_end;
		bool whole_paragraph;
		std::string utf8;
		TextEditMergePolicy merge_policy;
	};
	struct SplitParagraph {
		ContainerId container;
		BlockId block;
		std::size_t grapheme_boundary;
		BlockId new_block;
	};
	struct MergeParagraphs {
		ContainerId container;
		BlockId first;
		BlockId second;
	};

	std::uint64_t base_content_revision_;
	std::vector<ReplaceParagraphText> replacements_;
	std::optional<SplitParagraph> split_;
	std::optional<MergeParagraphs> merge_;
	std::size_t structure_command_count_ = 0;
};

}  // namespace krepis
