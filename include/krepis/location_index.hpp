#pragma once

// 依 LAY-0002 D14：ObjectSlot-indexed paged COW table。
// 儲存每個 Block 的 owner 與位置資訊，供反向查找使用。
//
// 責任：以 ObjectSlot（整數索引）快速查找與 COW 更新 LocationEntry。
// 不負責：管理 ObjectId → ObjectSlot 的對應——由 IdDirectory 負責。
// 維持的不變條件：page-table root 為 null 表示空索引；更新只複製受影響的短路徑。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/leaf_key.hpp"
#include "krepis/object_id.hpp"

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace krepis {

struct FlowLocator {
    LeafKey leaf_key;
};

struct SpatialLocator {
    std::uint64_t placement_key = 0;
};

struct LocationEntry {
    ContainerId owner;
    enum class Kind : std::uint8_t { empty, flow, spatial } kind = Kind::empty;
    FlowLocator flow{};
    SpatialLocator spatial{};

    [[nodiscard]] bool is_empty() const noexcept { return kind == Kind::empty; }
    [[nodiscard]] bool is_flow() const noexcept { return kind == Kind::flow; }
    [[nodiscard]] bool is_spatial() const noexcept { return kind == Kind::spatial; }
};

LocationEntry make_flow_location(ContainerId owner, LeafKey key);
LocationEntry make_spatial_location(ContainerId owner, std::uint64_t placement_key);

// --- Paged COW table internals ---

// 一頁位置資訊。發布後不可變；新舊 revision 共享未改動的 page（D14）。
class LocationPage final : public RefCounted {
public:
    static constexpr std::size_t page_capacity = 64;

    explicit LocationPage(std::vector<LocationEntry> entries) noexcept;

    [[nodiscard]] std::span<const LocationEntry> entries() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<LocationEntry> entries_;
};

// Page table 的節點。
//
// 責任：以固定 fanout 的樹狀結構定位 LocationPage，使更新只複製 root 到該 page 的短路徑。
// 不負責：保存位置內容 —— 那是 LocationPage 的責任。
// 維持的不變條件：同一棵樹的所有葉層節點深度相同；children 長度不超過 fanout。
// 生命週期：不可變；owning edge 只向下形成 DAG（D17）。
// 執行緒安全程度：不可變，可跨執行緒共享。
//
// 為何需要這一層：先前版本以扁平 `std::vector<IntrusivePtr<const LocationPage>>` 當 root，
// 每次更新都複製整個 vector——10 萬個 Block 即 1,563 個指標。
// 那違反 D14「只複製 page-table 的短路徑」（閘門 7／E1）。
class PageTableNode final : public RefCounted {
public:
    static constexpr std::size_t fanout = 64;

    // 葉層：直接持有 LocationPage。
    explicit PageTableNode(std::vector<IntrusivePtr<const LocationPage>> pages) noexcept;
    // 內層：持有下一層 PageTableNode。
    explicit PageTableNode(std::vector<IntrusivePtr<const PageTableNode>> children) noexcept;

    [[nodiscard]] bool is_leaf_level() const noexcept { return leaf_level_; }
    [[nodiscard]] std::span<const IntrusivePtr<const LocationPage>> pages() const noexcept;
    [[nodiscard]] std::span<const IntrusivePtr<const PageTableNode>> children() const noexcept;

private:
    bool leaf_level_;
    std::vector<IntrusivePtr<const LocationPage>> pages_;
    std::vector<IntrusivePtr<const PageTableNode>> children_;
};

// LocationIndex handle。
//
// 責任：以 slot index 快速查找與 COW 更新 LocationEntry。
// 不負責：ObjectId → slot 的解析。
// 維持的不變條件：slot 超出已配置範圍時回傳 empty entry 而非錯誤。
// 生命週期：值型別語意；內部以 IntrusivePtr 共享 page 與 page-table 節點。
// 執行緒安全程度：同一實例不可併發修改；不同實例可併發讀取。
class LocationIndex {
public:
    [[nodiscard]] static LocationIndex empty();

    // 回傳 slot 的 entry。slot 超出範圍時回傳 empty entry。
    [[nodiscard]] LocationEntry lookup(std::size_t slot) const;

    // 設定 slot 的 entry。自動擴展容量，且只複製 root 到該 page 的路徑。
    [[nodiscard]] LocationIndex set(std::size_t slot, LocationEntry entry) const;

    // 清除 slot 的 entry（設為 empty）。
    [[nodiscard]] LocationIndex clear(std::size_t slot) const;

    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    LocationIndex(IntrusivePtr<const PageTableNode> root, std::size_t depth) noexcept;

    // depth 為 0 表示空索引；depth 1 表示 root 是葉層（直接持 page）。
    IntrusivePtr<const PageTableNode> root_;
    std::size_t depth_ = 0;
};

}  // namespace krepis
