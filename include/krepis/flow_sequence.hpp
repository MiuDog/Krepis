#pragma once

// 依 LAY-0002 D4：FlowContainer 的權威 child sequence。
// Chunked B+ tree（Block rope），以 COW 發布不可變 revision（D8）。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/leaf_key.hpp"
#include "krepis/object_id.hpp"

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace krepis {

// Leaf 容量、internal fanout 與重平衡門檻。
//
// 數值由 benchmark 定案（2026-08-17，見 tasks/lay-0002-chunking-parameters-report.md）：
// 全部候選設定都在 frame budget 的 1/12 以下，因此**延遲不是選擇依據**；
// 定案依「每次編輯的節點配置數」決定——leaf=64 是節點數掉到 3 的最小值。
//
// merge_low_water 必須 < leaf_capacity / 2，否則 split 與 merge 會在同一邊界震盪（D16）。
struct FlowSequenceConfig {
    std::size_t leaf_capacity = 64;
    std::size_t internal_fanout = 32;
    std::size_t merge_low_water = 24;
    // D22：2026-08-21 benchmark 定案；見 tasks/lay-0002-leaf-key-relabel-report.md。
    std::size_t initial_relabel_window = 64;
};

class FlowSequenceInsertResult;

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
    FlowLeafNode(LeafKey key, std::vector<BlockId> blocks) noexcept;

    [[nodiscard]] bool is_leaf() const noexcept override;
    [[nodiscard]] std::size_t block_count() const noexcept override;
    [[nodiscard]] std::span<const BlockId> blocks() const noexcept;
    [[nodiscard]] const LeafKey& key() const noexcept { return key_; }

private:
    LeafKey key_;
    std::vector<BlockId> blocks_;
};

// Internal node 的 child entry：child pointer、該子樹的 block 總數與最小 LeafKey。
struct ChildEntry {
    IntrusivePtr<const FlowSequenceNode> child;
    std::size_t subtree_block_count = 0;
    LeafKey min_leaf_key{};
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
// 不負責：發布 LocationIndex（由 DocumentRevision 依 typed edit result 原子處理）、layout extent。
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

    // 回傳包含 position 的 leaf 的 LeafKey。前置條件：position < block_count()。
    [[nodiscard]] LeafKey leaf_key_at(std::size_t position) const;

    // 依 LeafKey 尋找該 leaf 的起始 rank（第一個 block 的 position）。
    // 找不到回傳 block_count()（等同 past-the-end）。
    [[nodiscard]] std::size_t find_by_key(const LeafKey& key) const;

	// 依 LocationIndex 的 LeafKey 在單一 leaf 內尋找 Block，成功回傳全域 rank。
	// 成本為 O(tree height + leaf capacity)，不掃描整份文件。
	[[nodiscard]] std::optional<std::size_t> find_block_in_leaf(
		const LeafKey& key,
		BlockId block
	) const;

    // 前置條件：position <= block_count()。回傳包含插入結果的新 FlowSequence。
    [[nodiscard]] FlowSequence insert(std::size_t position, BlockId block_id) const;

    // D22：回傳新 sequence、來源 root、所有 locator 更新與可觀察 relabel 診斷。
    // DocumentRevision 必須核對 source_root 後才可原子發布。
    [[nodiscard]] FlowSequenceInsertResult insert_with_updates(
        std::size_t position, BlockId block_id) const;

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

struct FlowLocatorUpdate {
    BlockId block;
    LeafKey leaf_key;
};

struct FlowSequenceEditDiagnostics {
    std::size_t relabeled_leaf_count = 0;
    std::size_t relabel_window = 0;
    bool global_rebuild = false;
};

// D22：一次 Flow 插入的完整結果。sequence 與 locator_updates 必須一起發布。
class FlowSequenceInsertResult {
public:
    [[nodiscard]] const IntrusivePtr<const FlowSequenceNode>& source_root() const noexcept;
    [[nodiscard]] const FlowSequence& sequence() const noexcept;
    [[nodiscard]] FlowSequence take_sequence() && noexcept;
    [[nodiscard]] std::span<const FlowLocatorUpdate> locator_updates() const noexcept;
    [[nodiscard]] const FlowSequenceEditDiagnostics& diagnostics() const noexcept;

private:
    friend class FlowSequence;

    FlowSequenceInsertResult(
        IntrusivePtr<const FlowSequenceNode> source_root,
        FlowSequence sequence,
        std::vector<FlowLocatorUpdate> locator_updates,
        FlowSequenceEditDiagnostics diagnostics) noexcept;

    IntrusivePtr<const FlowSequenceNode> source_root_;
    FlowSequence sequence_;
    std::vector<FlowLocatorUpdate> locator_updates_;
    FlowSequenceEditDiagnostics diagnostics_;
};

// 依 LAY-0002 D15：snapshot-bound 的跨 leaf 走訪 cursor。
//
// 責任：以 ancestor stack 在 FlowSequence 上前進與後退，不依賴 sibling pointer。
// 不負責：跨 revision 使用——cursor 綁定建立時的 snapshot。
// 維持的不變條件：持有 root 的 IntrusivePtr，借用內部 node pointer。
// 生命週期：cursor 有效期間 root 及其子孫不會被回收。
// 錯誤語意：在無效 cursor 上存取 current() 以 assert 攔截。
// 執行緒安全程度：單一執行緒使用。
// 可否複製／移動：可搬移，不可複製（ancestor stack 為借用路徑，複製語意不明確）。
class TreeCursor {
public:
    // 前置條件：position < seq.block_count()。空序列不能建立 cursor。
    TreeCursor(const FlowSequence& seq, std::size_t position);

    TreeCursor(const TreeCursor&) = delete;
    TreeCursor& operator=(const TreeCursor&) = delete;

    // Move 必須自訂：leaf_ 是借用 pointer，預設 move 會**複製**它，
    // 使 moved-from cursor 的 is_valid() 謊報 true 卻不再持有 owning root。
    // 搬移後來源一律歸零，符合「moved-from 為有效但未指定狀態」的慣例。
    TreeCursor(TreeCursor&& other) noexcept;
    TreeCursor& operator=(TreeCursor&& other) noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] BlockId current() const;
    [[nodiscard]] std::size_t position() const noexcept;

    // 前進一個 Block。回傳 false 表示已在最後一個 Block，cursor 變為無效。
    bool advance();
    // 後退一個 Block。回傳 false 表示已在第一個 Block，cursor 保持在原位。
    bool retreat();

private:
    struct Frame {
        const FlowInternalNode* node;
        std::size_t child_index;
    };

    IntrusivePtr<const FlowSequenceNode> root_;
    std::vector<Frame> ancestors_;
    const FlowLeafNode* leaf_ = nullptr;
    std::size_t local_offset_ = 0;
    std::size_t global_position_ = 0;
};

}  // namespace krepis
