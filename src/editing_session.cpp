#include "krepis/editing_session.hpp"

#include "krepis/paragraph_record.hpp"
#include "krepis/text_analysis.hpp"
#include "krepis/utf8.hpp"

#include <algorithm>
#include <utility>

namespace krepis {
namespace {

Result<void> validate_update(const CompositionUpdate& update) {
	if (auto valid = validate_utf8(update.provisional_utf8); !valid.is_ok()) return valid.error();
	const auto size = update.provisional_utf8.size();
	if (update.selection_byte_start > update.selection_byte_end ||
	    update.selection_byte_end > size ||
	    !is_utf8_boundary(update.provisional_utf8, update.selection_byte_start) ||
	    !is_utf8_boundary(update.provisional_utf8, update.selection_byte_end)) {
		return Error{ErrorCode::out_of_range, "composition selection 不是合法 UTF-8 byte range"};
	}
	std::size_t prior_end = 0;
	for (const auto& segment : update.segments) {
		if (segment.byte_start < prior_end || segment.byte_start > segment.byte_end ||
		    segment.byte_end > size ||
		    !is_utf8_boundary(update.provisional_utf8, segment.byte_start) ||
		    !is_utf8_boundary(update.provisional_utf8, segment.byte_end)) {
			return Error{ErrorCode::out_of_range, "composition segments 重疊或越過 UTF-8 boundary"};
		}
		prior_end = segment.byte_end;
	}
	return {};
}

const ParagraphRecord* paragraph_for(const DocumentRevision& revision, BlockId block) {
	auto record = revision.record_for(block);
	return dynamic_cast<const ParagraphRecord*>(record.get());
}

}  // namespace

EditingSession::EditingSession(std::uint64_t session_id) noexcept : session_id_(session_id) {}

Result<void> EditingSession::begin_composition(
	const DocumentRevision& revision,
	TextSelection replacement,
	CompositionUpdate update
) {
	if (composition_.has_value()) {
		return Error{ErrorCode::invalid_state, "EditingSession 已有 active composition"};
	}
	if (replacement.anchor.block != replacement.focus.block ||
	    replacement.anchor.base_content_revision != revision.snapshot_id().content_revision ||
	    replacement.focus.base_content_revision != revision.snapshot_id().content_revision) {
		return Error{ErrorCode::revision_conflict, "composition replacement selection 不屬於目前 revision"};
	}
	const auto* paragraph = paragraph_for(revision, replacement.anchor.block);
	if (paragraph == nullptr) {
		return Error{ErrorCode::not_found, "composition target Paragraph 不存在"};
	}
	auto analysis = analyze_text(paragraph->utf8(), {});
	if (!analysis.is_ok()) return analysis.error();
	const auto count = analysis.value().grapheme_boundaries.size() - 1;
	const auto start = std::min(
		replacement.anchor.grapheme_boundary,
		replacement.focus.grapheme_boundary
	);
	const auto end = std::max(
		replacement.anchor.grapheme_boundary,
		replacement.focus.grapheme_boundary
	);
	if (end > count) {
		return Error{ErrorCode::out_of_range, "composition replacement 超出 Paragraph grapheme range"};
	}
	if (auto valid = validate_update(update); !valid.is_ok()) return valid.error();
	composition_ = CompositionState{
		TextAnchor{
			replacement.anchor.block,
			revision.snapshot_id().content_revision,
			start,
			AnchorAffinity::upstream,
		},
		TextAnchor{
			replacement.anchor.block,
			revision.snapshot_id().content_revision,
			end,
			AnchorAffinity::downstream,
		},
		std::move(update),
		1,
	};
	return {};
}

Result<void> EditingSession::update_composition(CompositionUpdate update) {
	if (!composition_.has_value()) {
		return Error{ErrorCode::invalid_state, "沒有 active composition 可更新"};
	}
	if (auto valid = validate_update(update); !valid.is_ok()) return valid.error();
	composition_->update = std::move(update);
	++composition_->local_revision;
	return {};
}

void EditingSession::cancel_composition() noexcept {
	composition_.reset();
}

Result<ComposedParagraph> EditingSession::composed_paragraph(
	const DocumentRevision& revision
) const {
	if (!composition_.has_value()) {
		return Error{ErrorCode::invalid_state, "沒有 active composition 可合成"};
	}
	if (composition_->replace_start.base_content_revision !=
	    revision.snapshot_id().content_revision) {
		return Error{ErrorCode::revision_conflict, "composition anchor 尚未 rebase 到目前 revision"};
	}
	const auto* paragraph = paragraph_for(revision, composition_->replace_start.block);
	if (paragraph == nullptr) return Error{ErrorCode::not_found, "composition target 已刪除"};
	auto analysis = analyze_text(paragraph->utf8(), {});
	if (!analysis.is_ok()) return analysis.error();
	const auto count = analysis.value().grapheme_boundaries.size() - 1;
	const auto start = composition_->replace_start.grapheme_boundary;
	const auto end = composition_->replace_end.grapheme_boundary;
	if (start > end || end > count) {
		return Error{ErrorCode::invalid_state, "transformed composition range 無效"};
	}
	const auto byte_start = analysis.value().grapheme_boundaries[start];
	const auto byte_end = analysis.value().grapheme_boundaries[end];
	std::string rendered;
	rendered.reserve(
		paragraph->utf8().size() - (byte_end - byte_start) +
		composition_->update.provisional_utf8.size()
	);
	rendered.append(paragraph->utf8(), 0, byte_start);
	rendered.append(composition_->update.provisional_utf8);
	rendered.append(paragraph->utf8(), byte_end, std::string::npos);
	return ComposedParagraph{
		std::move(rendered),
		byte_start,
		byte_start + composition_->update.provisional_utf8.size(),
		composition_->local_revision,
	};
}

Result<void> EditingSession::rebase_composition(const CommitResult& accepted) {
	if (!composition_.has_value()) return {};
	if (paragraph_for(accepted.revision, composition_->replace_start.block) == nullptr) {
		cancel_composition();
		return {};
	}

	for (const auto& effect : accepted.text_edit_effects) {
		if (effect.block != composition_->replace_start.block) continue;
		const bool collapsed = composition_->replace_start.grapheme_boundary ==
		                       composition_->replace_end.grapheme_boundary;
		auto start = collapsed
			? transform_composition_anchor(composition_->replace_start, effect)
			: transform_text_anchor(composition_->replace_start, effect);
		auto end = collapsed
			? transform_composition_anchor(composition_->replace_end, effect)
			: transform_text_anchor(composition_->replace_end, effect);
		if (!start.is_ok()) return start.error();
		if (!end.is_ok()) return end.error();
		composition_->replace_start = start.value();
		composition_->replace_end = end.value();
	}
	composition_->replace_start.base_content_revision =
		accepted.revision.snapshot_id().content_revision;
	composition_->replace_end.base_content_revision =
		accepted.revision.snapshot_id().content_revision;
	return {};
}

Result<CommitResult> EditingSession::commit_composition(const DocumentRevision& base) {
	if (!composition_.has_value()) {
		return Error{ErrorCode::invalid_state, "沒有 active composition 可提交"};
	}
	if (composition_->replace_start.base_content_revision !=
	    base.snapshot_id().content_revision) {
		return Error{ErrorCode::revision_conflict, "composition 尚未 rebase 到 commit base"};
	}
	Transaction transaction(base.snapshot_id().content_revision);
	transaction.replace_paragraph_range(
		composition_->replace_start.block,
		composition_->replace_start.grapheme_boundary,
		composition_->replace_end.grapheme_boundary,
		composition_->update.provisional_utf8
	);
	auto committed = transaction.commit(base);
	if (!committed.is_ok()) return committed.error();
	cancel_composition();
	return committed;
}

}  // namespace krepis
