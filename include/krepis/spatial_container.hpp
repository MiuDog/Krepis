#pragma once

// DOC-0001／LAY-0002：SpatialContainer placement 與 viewport interval index。

#include "krepis/error.hpp"
#include "krepis/object_id.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace krepis {

struct RectD {
	double x = 0;
	double y = 0;
	double width = 0;
	double height = 0;

	[[nodiscard]] bool intersects(const RectD& other) const noexcept;
};

struct SpatialPlacement {
	std::uint64_t placement_key = 0;
	BlockId child;
	RectD frame;
	double source_width = 0;
	double source_height = 0;
	double vertical_scroll = 0;
};

class SpatialContainer {
public:
	[[nodiscard]] static Result<SpatialContainer> create(
		std::vector<SpatialPlacement> placements
	);

	[[nodiscard]] std::size_t placement_count() const noexcept { return placements_.size(); }
	[[nodiscard]] const SpatialPlacement& placement_at(std::size_t index) const;
	[[nodiscard]] const SpatialPlacement* find(std::uint64_t placement_key) const noexcept;
	[[nodiscard]] std::vector<SpatialPlacement> query(const RectD& viewport) const;

private:
	struct IntervalNode {
		std::size_t placement_index = 0;
		double subtree_max_bottom = 0;
		std::size_t left = no_node;
		std::size_t right = no_node;
	};

	static constexpr std::size_t no_node = static_cast<std::size_t>(-1);

	explicit SpatialContainer(std::vector<SpatialPlacement> placements);
	[[nodiscard]] std::size_t build_index(std::size_t begin, std::size_t end);
	void query_node(std::size_t node, const RectD& viewport,
	                std::vector<SpatialPlacement>& out) const;

	std::vector<SpatialPlacement> placements_;
	std::vector<IntervalNode> interval_nodes_;
	std::size_t interval_root_ = no_node;
};

}  // namespace krepis
