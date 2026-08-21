#pragma once

// DOC-0001 D10～D17：session-local IME composition overlay。

#include "krepis/editing.hpp"
#include "krepis/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace krepis {

enum class CompositionAttribute : std::uint8_t {
	raw_input,
	converted,
	target_converted,
	input_error,
};

struct CompositionSegment {
	std::size_t byte_start;
	std::size_t byte_end;
	CompositionAttribute attribute;
};

struct CompositionUpdate {
	std::string provisional_utf8;
	std::size_t selection_byte_start;
	std::size_t selection_byte_end;
	std::vector<CompositionSegment> segments;
};

struct CompositionState {
	TextAnchor replace_start;
	TextAnchor replace_end;
	CompositionUpdate update;
	std::uint64_t local_revision;
};

struct ComposedParagraph {
	std::string utf8;
	std::size_t composition_byte_start;
	std::size_t composition_byte_end;
	std::uint64_t composition_revision;
};

class EditingSession {
public:
	explicit EditingSession(std::uint64_t session_id) noexcept;

	[[nodiscard]] std::uint64_t session_id() const noexcept { return session_id_; }
	[[nodiscard]] const std::optional<CompositionState>& composition() const noexcept {
		return composition_;
	}

	[[nodiscard]] Result<void> begin_composition(
		const DocumentRevision& revision,
		TextSelection replacement,
		CompositionUpdate update
	);
	[[nodiscard]] Result<void> update_composition(CompositionUpdate update);
	void cancel_composition() noexcept;

	[[nodiscard]] Result<ComposedParagraph> composed_paragraph(
		const DocumentRevision& revision
	) const;
	[[nodiscard]] Result<void> rebase_composition(const CommitResult& accepted);
	[[nodiscard]] Result<CommitResult> commit_composition(const DocumentRevision& base);

private:
	std::uint64_t session_id_;
	std::optional<CompositionState> composition_;
};

}  // namespace krepis
