#include "krepis/layout_invalidation.hpp"

namespace krepis {

Result<FlowLayoutIndex> apply_layout_invalidations(
	const DocumentRevision& revision,
	ContainerId container,
	const FlowLayoutIndex& layout,
	std::span<const LayoutInvalidation> invalidations
) {
	const auto* sequence = revision.flow_root(container);
	if (sequence == nullptr) {
		return Error{ErrorCode::not_found, "FlowContainer 沒有 FlowSequence"};
	}
	if (sequence->block_count() != layout.block_count()) {
		return Error{ErrorCode::invalid_state, "FlowSequence 與 FlowLayoutIndex 數量不一致"};
	}

	auto result = layout;
	for (const auto& invalidation : invalidations) {
		if (invalidation.source_content_revision != revision.snapshot_id().content_revision) {
			return Error{ErrorCode::revision_conflict, "Layout invalidation 的來源 revision 不符"};
		}

		const auto slot = revision.resolve(invalidation.block);
		if (!slot.is_valid()) {
			return Error{ErrorCode::not_found, "Layout invalidation 的 Block 不存在"};
		}

		const auto location = revision.locations().lookup(slot);
		if (!location.is_flow()) continue;
		if (location.owner != container) continue;

		auto position = sequence->find_block_in_leaf(location.flow.leaf_key, invalidation.block);
		if (!position.has_value()) {
			return Error{ErrorCode::invalid_state, "LocationIndex 的 LeafKey 無法解析 Block"};
		}
		if (result.at(*position).block_id != invalidation.block) {
			return Error{ErrorCode::invalid_state, "FlowLayoutIndex 的 BlockId 與 FlowSequence 不一致"};
		}

		// Paint／hit-test 不會使高度 cache 過期；它們由後續 display-list cache 處理。
		if (invalidation.stage <= InvalidationStage::extent) {
			result = result.invalidate_extent(*position, invalidation.source_content_revision);
		}
	}

	return result;
}

}  // namespace krepis
