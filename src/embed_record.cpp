#include "krepis/embed_record.hpp"

#include <cmath>
#include <optional>
#include <utility>

namespace krepis {
namespace {

bool valid_rect(const RectD& rect) noexcept {
	return std::isfinite(rect.x) && std::isfinite(rect.y) &&
	       std::isfinite(rect.width) && std::isfinite(rect.height) &&
	       rect.width > 0 && rect.height > 0;
}

std::optional<std::size_t> find_rank(const FlowSequence& sequence, BlockId block) {
	for (std::size_t rank = 0; rank < sequence.block_count(); ++rank) {
		if (sequence.at(rank) == block) return rank;
	}
	return std::nullopt;
}

}  // namespace

Result<IntrusivePtr<const EmbedRecord>> EmbedRecord::create(
	std::uint64_t content_revision,
	EmbedTarget target
) {
	if (const auto* flow = std::get_if<FlowRangeTarget>(&target)) {
		const bool both_empty = flow->anchor_a.is_nil() && flow->anchor_b.is_nil();
		const bool both_present = !flow->anchor_a.is_nil() && !flow->anchor_b.is_nil();
		if (flow->source_flow.is_nil() || (!both_empty && !both_present)) {
			return Error{ErrorCode::invalid_argument, "FlowRangeTarget anchors 狀態不合法"};
		}
	} else {
		const auto& spatial = std::get<SpatialViewportTarget>(target);
		if (spatial.source_spatial.is_nil() || !valid_rect(spatial.viewport)) {
			return Error{ErrorCode::invalid_argument, "SpatialViewportTarget 不合法"};
		}
	}
	return make_intrusive<EmbedRecord>(content_revision, std::move(target));
}

EmbedRecord::EmbedRecord(std::uint64_t content_revision, EmbedTarget target) noexcept
	: ObjectRecord(content_revision), target_(std::move(target)) {}

ContainerId EmbedRecord::source_container() const noexcept {
	if (const auto* flow = std::get_if<FlowRangeTarget>(&target_)) return flow->source_flow;
	return std::get<SpatialViewportTarget>(target_).source_spatial;
}

Result<FlowRangeTarget> repair_flow_range_after_removal(
	const FlowRangeTarget& target,
	const FlowSequence& source_before,
	BlockId removed
) {
	if (target.anchor_a.is_nil() && target.anchor_b.is_nil()) return target;
	auto a = find_rank(source_before, target.anchor_a);
	auto b = find_rank(source_before, target.anchor_b);
	auto removed_rank = find_rank(source_before, removed);
	if (!a.has_value() || !b.has_value() || !removed_rank.has_value()) {
		return Error{ErrorCode::invalid_state, "Flow range repair 的來源或 anchor 已 detached"};
	}
	if (target.anchor_a != removed && target.anchor_b != removed) return target;
	if (target.anchor_a == removed && target.anchor_b == removed) {
		return FlowRangeTarget{target.source_flow, BlockId{}, BlockId{}};
	}
	auto repaired = target;
	if (target.anchor_a == removed) {
		repaired.anchor_a = source_before.at(*a < *b ? *a + 1 : *a - 1);
	}
	if (target.anchor_b == removed) {
		repaired.anchor_b = source_before.at(*b < *a ? *b + 1 : *b - 1);
	}
	return repaired;
}

}  // namespace krepis
