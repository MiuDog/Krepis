#pragma once

// 依 LAY-0002 D14：ObjectSlot-indexed paged COW table。
// 儲存每個 Block 的 owner 與位置資訊，供反向查找使用。
//
// 責任：以 ObjectSlot（整數索引）快速查找與 COW 更新 LocationEntry。
// 不負責：管理 ObjectId → ObjectSlot 的對應——由 IdDirectory 負責。
// 維持的不變條件：page-table root 為 null 表示空索引；COW 保證舊 snapshot 不被修改。

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

class LocationPage : public RefCounted {
public:
    static constexpr std::size_t page_capacity = 64;

    explicit LocationPage(std::vector<LocationEntry> entries) noexcept;

    [[nodiscard]] std::span<const LocationEntry> entries() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<LocationEntry> entries_;
};

struct PageTableEntry {
    IntrusivePtr<const LocationPage> page;
};

class PageTableNode : public RefCounted {
public:
    static constexpr std::size_t fanout = 64;

    explicit PageTableNode(std::vector<PageTableEntry> children) noexcept;
    explicit PageTableNode(std::vector<IntrusivePtr<const PageTableNode>> internal_children) noexcept;

    [[nodiscard]] bool is_leaf_level() const noexcept;
    [[nodiscard]] std::span<const PageTableEntry> page_children() const noexcept;
    [[nodiscard]] std::span<const IntrusivePtr<const PageTableNode>> internal_children() const noexcept;

private:
    bool leaf_level_;
    std::vector<PageTableEntry> page_children_;
    std::vector<IntrusivePtr<const PageTableNode>> internal_children_;
};

// LocationIndex handle。
//
// 責任：以 slot index 快速查找與 COW 更新 LocationEntry。
// 不負責：ObjectId → slot 的解析。
// 維持的不變條件：slot 超出已配置範圍時回傳 empty entry 而非錯誤。
// 生命週期：值型別語意。
// 執行緒安全程度：同一實例不可併發修改；不同實例可併發讀取。
class LocationIndex {
public:
    [[nodiscard]] static LocationIndex empty();

    // 回傳 slot 的 entry。slot 超出範圍時回傳 empty entry。
    [[nodiscard]] LocationEntry lookup(std::size_t slot) const;

    // 設定 slot 的 entry。自動擴展容量。
    [[nodiscard]] LocationIndex set(std::size_t slot, LocationEntry entry) const;

    // 清除 slot 的 entry（設為 empty）。
    [[nodiscard]] LocationIndex clear(std::size_t slot) const;

    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    LocationIndex(std::vector<IntrusivePtr<const LocationPage>> pages) noexcept;

    std::vector<IntrusivePtr<const LocationPage>> pages_;
};

}  // namespace krepis
