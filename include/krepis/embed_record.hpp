#pragma once

// DOC-0001 D3～D5：唯讀即時引用 record；只保存 stable ID 與 viewport 值。

#include "krepis/error.hpp"
#include "krepis/flow_sequence.hpp"
#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_store.hpp"
#include "krepis/spatial_container.hpp"

#include <cstdint>
#include <variant>

namespace krepis {

struct FlowRangeTarget {
	ContainerId source_flow;
	// 兩者同為 nil 表示原區間已全部刪除；只允許同時有值或同時為 nil。
	BlockId anchor_a;
	BlockId anchor_b;

	friend bool operator==(const FlowRangeTarget&, const FlowRangeTarget&) noexcept = default;
};

struct SpatialViewportTarget {
	ContainerId source_spatial;
	RectD viewport;
};

using EmbedTarget = std::variant<FlowRangeTarget, SpatialViewportTarget>;

class EmbedRecord final : public ObjectRecord {
public:
	[[nodiscard]] static Result<IntrusivePtr<const EmbedRecord>> create(
		std::uint64_t content_revision,
		EmbedTarget target
	);

	[[nodiscard]] const EmbedTarget& target() const noexcept { return target_; }
	[[nodiscard]] ContainerId source_container() const noexcept;

private:
	template <typename T, typename... Args>
	friend IntrusivePtr<const T> make_intrusive(Args&&... args);

	EmbedRecord(std::uint64_t content_revision, EmbedTarget target) noexcept;

	EmbedTarget target_;
};

[[nodiscard]] Result<FlowRangeTarget> repair_flow_range_after_removal(
	const FlowRangeTarget& target,
	const FlowSequence& source_before,
	BlockId removed
);

}  // namespace krepis
