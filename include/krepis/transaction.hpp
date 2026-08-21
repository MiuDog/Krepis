#pragma once

// EDT-0001：單層原子 Transaction。
// 第一條實作路徑只包含 ReplaceParagraphText typed command。

#include "krepis/document_revision.hpp"
#include "krepis/error.hpp"
#include "krepis/editing.hpp"
#include "krepis/object_id.hpp"

#include <cstdint>
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

struct CommitResult {
	DocumentRevision revision;
	std::vector<LayoutInvalidation> invalidations;
	std::vector<ParagraphTextEditEffect> text_edit_effects;
};

class Transaction {
public:
	explicit Transaction(std::uint64_t base_content_revision) noexcept;

	void replace_paragraph_text(BlockId block, std::string utf8);
	void replace_paragraph_range(
		BlockId block,
		std::size_t grapheme_start,
		std::size_t grapheme_end,
		std::string utf8
	);

	[[nodiscard]] Result<CommitResult> commit(const DocumentRevision& base) const;

private:
	struct ReplaceParagraphText {
		BlockId block;
		std::size_t grapheme_start;
		std::size_t grapheme_end;
		bool whole_paragraph;
		std::string utf8;
	};

	std::uint64_t base_content_revision_;
	std::vector<ReplaceParagraphText> replacements_;
};

}  // namespace krepis
