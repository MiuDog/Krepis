#pragma once

// 依 LAY-0002 D4：FlowContainer 的權威 child sequence。
// Chunked B+ tree（Block rope），以 COW 發布不可變 revision（D8）。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_id.hpp"

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

namespace krepis {

// Leaf 容量、internal fanout 與重平衡門檻。數值由 benchmark 決定（D16）。
struct FlowSequenceConfig {
    std::size_t leaf_capacity = 64;
    std::size_t internal_fanout = 32;
    std::size_t merge_low_water = 16;
};

// B+ tree 節點基底。發布後不可變（D8）。
//
// 責任：提供多型分派與 block 計數。
// 不負責：管理生命週期 —— 由 IntrusivePtr 透過 RefCounted 處理。
// 維持的不變條件：block_count 等於所有子孫 leaf 的 BlockId 總數。
// 生命週期：建構後不可變；由 IntrusivePtr<const FlowSequenceNode> 持有。
// 執行緒安全程度：不可變，可跨執行緒共享。
class FlowSequenceNode : public RefCounted {
public:
    [[nodiscard]] virtual bool is_leaf() const noexcept = 0;
    [[nodiscard]] virtual std::size_t block_count() const noexcept = 0;

protected:
    FlowSequenceNode() noexcept = default;
    ~FlowSequenceNode() override = default;
};

// Leaf 節點：儲存連續的 BlockId 序列。
//
// 維持的不變條件：blocks 非空（空 leaf 在刪除時移除而非保留）。
// 可否複製／移動：不可（RefCounted）。
class FlowLeafNode final : public FlowSequenceNode {
public:
    explicit FlowLeafNode(std::vector<BlockId> blocks) noexcept;

    [[nodiscard]] bool is_leaf() const noexcept override;
    [[nodiscard]] std::size_t block_count() const noexcept override;
    [[nodiscard]] std::span<const BlockId> blocks() const noexcept;

private:
    std::vector<BlockId> blocks_;
};

// Internal node 的 child entry：child pointer 與該子樹的 block 總數。
struct ChildEntry {
    IntrusivePtr<const FlowSequenceNode> child;
    std::size_t subtree_block_count = 0;
};

// Internal 節點：儲存 child entry 序列，以 subtree_block_count 導航 rank。
//
// 維持的不變條件：至少兩個 child；total_block_count 等於所有 child 的 subtree_block_count 之和。
// 可否複製／移動：不可（RefCounted）。
class FlowInternalNode final : public FlowSequenceNode {
public:
    explicit FlowInternalNode(std::vector<ChildEntry> children) noexcept;

    [[nodiscard]] bool is_leaf() const noexcept override;
    [[nodiscard]] std::size_t block_count() const noexcept override;
    [[nodiscard]] std::span<const ChildEntry> children() const noexcept;

private:
    std::vector<ChildEntry> children_;
    std::size_t total_block_count_;
};

// FlowContainer 的權威 child sequence handle。
//
// 責任：提供 rank-based 的 COW 結構操作，回傳新的不可變 FlowSequence。
// 不負責：LeafKey 管理（由 authority 外部維護）、layout extent。
// 維持的不變條件：root 為 null 表示空序列；非 null 時 root 的 block_count 即為總數。
// 擁有哪些資源：透過 IntrusivePtr 共享 B+ tree 子樹。
// 生命週期：值型別語意（可複製、可搬移）；copy 共享子樹。
// 錯誤語意：position 越界以 assert 攔截。
// 執行緒安全程度：同一實例不可併發修改；不同實例可併發讀取。
class FlowSequence {
public:
    [[nodiscard]] static FlowSequence empty(FlowSequenceConfig config = {});

    [[nodiscard]] std::size_t block_count() const noexcept;
    [[nodiscard]] bool is_empty() const noexcept;

    // 前置條件：position < block_count()。
    [[nodiscard]] BlockId at(std::size_t position) const;

    // 前置條件：position <= block_count()。回傳包含插入結果的新 FlowSequence。
    [[nodiscard]] FlowSequence insert(std::size_t position, BlockId block_id) const;

    // 前置條件：position < block_count()。回傳移除後的新 FlowSequence。
    [[nodiscard]] FlowSequence remove(std::size_t position) const;

    [[nodiscard]] const FlowSequenceConfig& config() const noexcept;
    [[nodiscard]] IntrusivePtr<const FlowSequenceNode> root() const noexcept;

private:
    FlowSequence(FlowSequenceConfig config,
                 IntrusivePtr<const FlowSequenceNode> root) noexcept;

    FlowSequenceConfig config_;
    IntrusivePtr<const FlowSequenceNode> root_;
};

}  // namespace krepis
