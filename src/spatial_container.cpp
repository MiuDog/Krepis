#include "krepis/spatial_container.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace krepis {
namespace {

bool valid_number(double value) noexcept {
	return std::isfinite(value);
}

bool valid_placement(const SpatialPlacement& placement) noexcept {
	return placement.placement_key != 0 && !placement.child.is_nil() &&
	       valid_number(placement.frame.x) && valid_number(placement.frame.y) &&
	       valid_number(placement.frame.width) && valid_number(placement.frame.height) &&
	       placement.frame.width > 0 && placement.frame.height > 0 &&
	       valid_number(placement.source_width) && valid_number(placement.source_height) &&
	       placement.source_width > 0 && placement.source_height > 0 &&
	       valid_number(placement.vertical_scroll) && placement.vertical_scroll >= 0;
}

}  // namespace

bool RectD::intersects(const RectD& other) const noexcept {
	return x < other.x + other.width && x + width > other.x &&
	       y < other.y + other.height && y + height > other.y;
}

Result<SpatialContainer> SpatialContainer::create(
	std::vector<SpatialPlacement> placements
) {
	std::unordered_set<std::uint64_t> keys;
	std::unordered_set<BlockId> children;
	for (const auto& placement : placements) {
		if (!valid_placement(placement)) {
			return Error{ErrorCode::invalid_argument, "Spatial placement 參數不合法"};
		}
		if (!keys.insert(placement.placement_key).second ||
		    !children.insert(placement.child).second) {
			return Error{ErrorCode::invalid_argument, "Spatial placement key 或 child 重複"};
		}
	}
	std::sort(placements.begin(), placements.end(), [](const auto& left, const auto& right) {
		if (left.frame.y != right.frame.y) return left.frame.y < right.frame.y;
		return left.placement_key < right.placement_key;
	});
	return SpatialContainer(std::move(placements));
}

SpatialContainer::SpatialContainer(std::vector<SpatialPlacement> placements)
	: placements_(std::move(placements)) {
	interval_nodes_.reserve(placements_.size());
	interval_root_ = build_index(0, placements_.size());
}

std::size_t SpatialContainer::build_index(std::size_t begin, std::size_t end) {
	if (begin == end) return no_node;
	const auto middle = begin + (end - begin) / 2;
	const auto node_index = interval_nodes_.size();
	interval_nodes_.push_back({middle, 0, no_node, no_node});
	const auto left = build_index(begin, middle);
	const auto right = build_index(middle + 1, end);
	auto maximum = placements_[middle].frame.y + placements_[middle].frame.height;
	if (left != no_node) maximum = std::max(maximum, interval_nodes_[left].subtree_max_bottom);
	if (right != no_node) maximum = std::max(maximum, interval_nodes_[right].subtree_max_bottom);
	interval_nodes_[node_index] = {middle, maximum, left, right};
	return node_index;
}

const SpatialPlacement& SpatialContainer::placement_at(std::size_t index) const {
	assert(index < placements_.size());
	return placements_[index];
}

const SpatialPlacement* SpatialContainer::find(std::uint64_t placement_key) const noexcept {
	for (const auto& placement : placements_) {
		if (placement.placement_key == placement_key) return &placement;
	}
	return nullptr;
}

std::vector<SpatialPlacement> SpatialContainer::query(const RectD& viewport) const {
	std::vector<SpatialPlacement> result;
	if (!valid_number(viewport.x) || !valid_number(viewport.y) ||
	    !valid_number(viewport.width) || !valid_number(viewport.height) ||
	    viewport.width <= 0 || viewport.height <= 0) {
		return result;
	}
	query_node(interval_root_, viewport, result);
	return result;
}

void SpatialContainer::query_node(
	std::size_t node,
	const RectD& viewport,
	std::vector<SpatialPlacement>& out
) const {
	if (node == no_node) return;
	const auto& interval = interval_nodes_[node];
	if (interval.left != no_node &&
	    interval_nodes_[interval.left].subtree_max_bottom > viewport.y) {
		query_node(interval.left, viewport, out);
	}
	const auto& placement = placements_[interval.placement_index];
	if (placement.frame.intersects(viewport)) out.push_back(placement);
	if (interval.right != no_node && placement.frame.y < viewport.y + viewport.height) {
		query_node(interval.right, viewport, out);
	}
}

}  // namespace krepis
