#include "krepis/reference_resolver.hpp"

#include "krepis/embed_record.hpp"

#include <algorithm>

namespace krepis {
namespace {

bool active(const std::vector<ContainerId>& path, ContainerId source) {
	return std::find(path.begin(), path.end(), source) != path.end();
}

std::optional<std::size_t> rank_of(const FlowSequence& sequence, BlockId block) {
	for (std::size_t rank = 0; rank < sequence.block_count(); ++rank) {
		if (sequence.at(rank) == block) return rank;
	}
	return std::nullopt;
}

}  // namespace

ReferenceResolver::ReferenceResolver(const DocumentRevision& revision) noexcept
	: revision_(revision) {}

ResolvedContainerView ReferenceResolver::resolve_flow(ContainerId source) const {
	std::vector<ContainerId> path{source};
	const auto* sequence = revision_.flow_root(source);
	if (sequence == nullptr) {
		return {source, ReferenceResolutionStatus::missing_target, {}};
	}
	ResolvedContainerView result{source, ReferenceResolutionStatus::resolved, {}};
	result.blocks.reserve(sequence->block_count());
	for (std::size_t rank = 0; rank < sequence->block_count(); ++rank) {
		result.blocks.push_back(resolve_block(sequence->at(rank), std::nullopt, path));
	}
	if (result.blocks.empty()) result.status = ReferenceResolutionStatus::empty;
	return result;
}

ResolvedContainerView ReferenceResolver::resolve_spatial(
	ContainerId source,
	RectD viewport
) const {
	std::vector<ContainerId> path;
	return resolve_spatial_viewport(SpatialViewportTarget{source, viewport}, path);
}

ResolvedContainerView ReferenceResolver::resolve_flow_range(
	const FlowRangeTarget& target,
	std::vector<ContainerId>& active_path
) const {
	if (active(active_path, target.source_flow)) {
		return {target.source_flow, ReferenceResolutionStatus::cycle_cut, {}};
	}
	const auto* sequence = revision_.flow_root(target.source_flow);
	if (sequence == nullptr) {
		return {target.source_flow, ReferenceResolutionStatus::missing_target, {}};
	}
	if (target.anchor_a.is_nil() && target.anchor_b.is_nil()) {
		return {target.source_flow, ReferenceResolutionStatus::empty, {}};
	}
	auto a = rank_of(*sequence, target.anchor_a);
	auto b = rank_of(*sequence, target.anchor_b);
	if (!a.has_value() || !b.has_value()) {
		return {target.source_flow, ReferenceResolutionStatus::detached, {}};
	}
	const auto first = std::min(*a, *b);
	const auto last = std::max(*a, *b);
	active_path.push_back(target.source_flow);
	ResolvedContainerView result{target.source_flow, ReferenceResolutionStatus::resolved, {}};
	result.blocks.reserve(last - first + 1);
	for (std::size_t rank = first; rank <= last; ++rank) {
		result.blocks.push_back(resolve_block(sequence->at(rank), std::nullopt, active_path));
	}
	active_path.pop_back();
	return result;
}

ResolvedContainerView ReferenceResolver::resolve_spatial_viewport(
	const SpatialViewportTarget& target,
	std::vector<ContainerId>& active_path
) const {
	if (active(active_path, target.source_spatial)) {
		return {target.source_spatial, ReferenceResolutionStatus::cycle_cut, {}};
	}
	const auto* spatial = revision_.spatial_root(target.source_spatial);
	if (spatial == nullptr) {
		return {target.source_spatial, ReferenceResolutionStatus::missing_target, {}};
	}
	auto placements = spatial->query(target.viewport);
	active_path.push_back(target.source_spatial);
	ResolvedContainerView result{target.source_spatial, ReferenceResolutionStatus::resolved, {}};
	result.blocks.reserve(placements.size());
	for (const auto& placement : placements) {
		result.blocks.push_back(resolve_block(placement.child, placement.frame, active_path));
	}
	active_path.pop_back();
	if (result.blocks.empty()) result.status = ReferenceResolutionStatus::empty;
	return result;
}

ResolvedBlockView ReferenceResolver::resolve_block(
	BlockId block,
	std::optional<RectD> frame,
	std::vector<ContainerId>& active_path
) const {
	ResolvedBlockView result{
		block,
		ReferenceResolutionStatus::resolved,
		frame,
		std::nullopt,
	};
	auto record = revision_.record_for(block);
	if (record == nullptr) {
		result.status = ReferenceResolutionStatus::missing_target;
		return result;
	}
	const auto* embed = dynamic_cast<const EmbedRecord*>(record.get());
	if (embed == nullptr) return result;
	if (const auto* flow = std::get_if<FlowRangeTarget>(&embed->target())) {
		result.embed = resolve_flow_range(*flow, active_path);
	} else {
		result.embed = resolve_spatial_viewport(
			std::get<SpatialViewportTarget>(embed->target()),
			active_path
		);
	}
	return result;
}

}  // namespace krepis
