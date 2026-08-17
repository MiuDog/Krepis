#pragma once

// 依 LAY-0002 D10／D14：authority 內部的緊湊物件索引。
//
// **刻意與 `ObjectId` 分開放在不同 header。** 兩者的生命週期契約相反：
//   - `ObjectId`（object_id.hpp）是**永久身分**，可序列化、可跨 revision、可交給 client。
//   - `ObjectSlot`（本檔）是**authority 內部索引**，會因 compact 重新配置，
//     不得序列化、不得外流。
// 放在同一個 header 會讓兩者看起來同級，而那正是最容易出錯的地方。
//
// 本檔是 ObjectStore 與 LocationIndex 的**共用基礎**：兩者都以 ObjectSlot 為索引鍵（D14），
// 因此型別必須共用，不能各自定義或退化成裸整數。

#include <cstddef>
#include <cstdint>
#include <limits>

namespace krepis {

// Authority 內部的緊湊索引。
//
// 責任：作為 ObjectStore 記錄與 LocationIndex 位置的共同索引鍵。
// 不負責：作為持久身分——那是 ObjectId 的責任。
// 維持的不變條件：同一個 IdDirectoryGeneration 內一經配置便不改變，
//                 且不因刪除立即重用（D10）。
// 生命週期：值型別；有效範圍不超過產生它的 storage generation（D18）。
// 錯誤語意：invalid_value 表示「未配置」，不是錯誤碼。
// 執行緒安全程度：不可變。
// 可否複製／移動：兩者皆可，且為平凡操作。
struct ObjectSlot {
    static constexpr std::uint32_t invalid_value = 0xFFFFFFFFu;

    std::uint32_t value = invalid_value;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != invalid_value; }

    friend constexpr bool operator==(const ObjectSlot&, const ObjectSlot&) noexcept = default;
};

inline constexpr ObjectSlot invalid_object_slot{};

// 可配置的 slot 上限。`invalid_value` 保留為哨兵，因此不可配置。
//
// **這個上限有結構性意義**：它使分頁樹的深度有界（見下方 max_page_table_depth），
// 因而使容量計算不可能溢位。若將來 ObjectSlot 加寬為 64-bit，
// 必須重新檢查所有以此為前提的推論。
inline constexpr std::uint64_t max_object_slot_count = 0xFFFFFFFFull;  // 2^32 - 1

// Slot 配置的世代。compact 重新配置 slot 時遞增；
// 持有 slot 的 cache 必須核對世代（LAY-0002 D18 的 storage_generation）。
struct IdDirectoryGeneration {
    std::uint64_t value = 0;

    friend constexpr bool operator==(const IdDirectoryGeneration&,
                                     const IdDirectoryGeneration&) noexcept = default;
};

// 飽和乘法。溢位時回傳上限而非迴繞。
//
// 分頁樹的 `capacity()` 用它計算：`fanout^(depth+1)` 在 depth 夠深時會超出 `size_t`，
// 而 `64^11 = 2^66`，對 64-bit `size_t` 取模**恰好等於 0**——
// 迴繞後的 capacity 會回報 0，比溢位更難察覺（閘門 7／E1 第二輪）。
[[nodiscard]] constexpr std::size_t saturating_mul(std::size_t a, std::size_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    if (a > std::numeric_limits<std::size_t>::max() / b) {
        return std::numeric_limits<std::size_t>::max();
    }
    return a * b;
}

}  // namespace krepis
