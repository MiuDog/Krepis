#pragma once

// 依 LAY-0002 D4 下半：FlowContainer 的累積高度索引。
// 以 FlowSequence 的分塊結構為基礎，儲存每個 Block 的 measured_height，
// 並在 internal node 維護 subtree_extent 聚合。
//
// 責任：prefix_extent、lower_bound_extent、update_extent。
// 不負責：權威順序（由 FlowSequence 管理）。
// 維持的不變條件：subtree_extent 等於所有子孫 leaf 的 extent 之和。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_id.hpp"
#include "krepis/snapshot_id.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace krepis {

enum class MeasurementStatus : std::uint8_t {
	estimated,
	measured,
};

struct LayoutEntry {
    BlockId block_id;
    double measured_height = 0.0;
	std::uint64_t source_content_revision = 0;
	MeasurementStatus status = MeasurementStatus::measured;
};

class FlowLayoutNode : public RefCounted {
public:
    [[nodiscard]] virtual bool is_leaf() const noexcept = 0;
    [[nodiscard]] virtual std::size_t block_count() const noexcept = 0;
    [[nodiscard]] virtual double subtree_extent() const noexcept = 0;

protected:
    FlowLayoutNode() noexcept = default;
    ~FlowLayoutNode() override = default;
};

class FlowLayoutLeaf final : public FlowLayoutNode {
public:
    explicit FlowLayoutLeaf(std::vector<LayoutEntry> entries) noexcept;

    [[nodiscard]] bool is_leaf() const noexcept override;
    [[nodiscard]] std::size_t block_count() const noexcept override;
    [[nodiscard]] double subtree_extent() const noexcept override;
    [[nodiscard]] std::span<const LayoutEntry> entries() const noexcept;

private:
    std::vector<LayoutEntry> entries_;
    double total_extent_;
};

struct LayoutChildEntry {
    IntrusivePtr<const FlowLayoutNode> child;
    std::size_t subtree_block_count = 0;
    double subtree_extent = 0.0;
};

class FlowLayoutInternal final : public FlowLayoutNode {
public:
    explicit FlowLayoutInternal(std::vector<LayoutChildEntry> children) noexcept;

    [[nodiscard]] bool is_leaf() const noexcept override;
    [[nodiscard]] std::size_t block_count() const noexcept override;
    [[nodiscard]] double subtree_extent() const noexcept override;
    [[nodiscard]] std::span<const LayoutChildEntry> children() const noexcept;

private:
    std::vector<LayoutChildEntry> children_;
    std::size_t total_block_count_;
    double total_extent_;
};

struct FlowLayoutConfig {
    std::size_t leaf_capacity = 64;
    std::size_t internal_fanout = 32;
};

// FlowContainer 的累積高度索引 handle。
//
// 責任：以 COW 方式更新高度、查詢 prefix extent 與 lower_bound_extent。
// 不負責：管理 Block 順序——順序由 FlowSequence 權威。
// 維持的不變條件：root 為 null 表示空索引；subtree_extent 聚合正確。
// 生命週期：值型別語意。
// 執行緒安全程度：同一實例不可併發修改。
class FlowLayoutIndex {
public:
    [[nodiscard]] static FlowLayoutIndex empty(FlowLayoutConfig config = {});

    [[nodiscard]] std::size_t block_count() const noexcept;
    [[nodiscard]] bool is_empty() const noexcept;
    [[nodiscard]] double total_extent() const noexcept;

    // 在 position 插入新的 LayoutEntry。前置條件：position <= block_count()。
    [[nodiscard]] FlowLayoutIndex insert(std::size_t position, LayoutEntry entry) const;

    // 移除 position 的 entry。前置條件：position < block_count()。
    [[nodiscard]] FlowLayoutIndex remove(std::size_t position) const;

    // 更新 position 的 measured_height。前置條件：position < block_count()。
    [[nodiscard]] FlowLayoutIndex update_extent(std::size_t position, double new_height) const;

	// 保留舊高度作 estimate，只把目標 entry 標為待重測並綁定來源 revision。
	[[nodiscard]] FlowLayoutIndex invalidate_extent(
		std::size_t position,
		std::uint64_t source_content_revision
	) const;

    // 前 position 個 block 的累積高度。前置條件：position <= block_count()。
    [[nodiscard]] double prefix_extent(std::size_t position) const;

    // 找到累積高度 >= y 的第一個 block position。
    // 用於 viewport 起點定位。回傳 block_count() 表示 y 超過所有 block。
    [[nodiscard]] std::size_t lower_bound_extent(double y) const;

    // 取得 position 的 entry。前置條件：position < block_count()。
    [[nodiscard]] const LayoutEntry& at(std::size_t position) const;

    [[nodiscard]] IntrusivePtr<const FlowLayoutNode> root() const noexcept;

private:
    FlowLayoutIndex(FlowLayoutConfig config,
                    IntrusivePtr<const FlowLayoutNode> root) noexcept;

    FlowLayoutConfig config_;
    IntrusivePtr<const FlowLayoutNode> root_;
};

}  // namespace krepis
