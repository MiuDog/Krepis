#pragma once

// 依 LAY-0002 D13：authority-owned 的 128-bit 稀疏排序標籤。
//
// **不得序列化為 selection、reference 或 undo anchor** —— LeafKey 可在 compact／重建時改變。
// 持久錨點一律使用 stable BlockId（DOC-0001 D2）。

#include <compare>
#include <cstdint>
#include <optional>

namespace krepis {

// FlowSequence 邏輯 leaf 的排序標籤。
//
// 責任：在不重寫鄰居的前提下，於兩個既有標籤之間插入新標籤。
// 不負責：作為持久身分。它是 authority 擁有的內部 handle，會因 relabel 而改變。
// 維持的不變條件：比較為 {high, low} 無號字典序，且該順序即為 leaf 的邏輯順序。
// 擁有哪些資源：無。
// 生命週期：值型別；有效範圍不超過產生它的 storage generation（LAY-0002 D18）。
// 錯誤語意：midpoint 在無間隙時回傳空值，由呼叫端觸發 relabel，不是錯誤。
// 執行緒安全程度：不可變。
// 可否複製／移動：兩者皆可，且為平凡操作。
struct LeafKey {
    // MSVC 無可依賴的原生 128-bit 整數（LAY-0002 D13）。
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend constexpr bool operator==(const LeafKey&, const LeafKey&) noexcept = default;

    friend constexpr auto operator<=>(const LeafKey& a, const LeafKey& b) noexcept {
        if (auto cmp = a.high <=> b.high; cmp != 0) {
            return cmp;
        }
        return a.low <=> b.low;
    }
};

// 整個 128-bit 空間的邊界。新序列的第一個標籤應取這兩者的中點，
// 使兩側都保有最大可用間隙。
inline constexpr LeafKey leaf_key_min{0, 0};
inline constexpr LeafKey leaf_key_max{UINT64_MAX, UINT64_MAX};

// 取兩個標籤的中點。
//
// 前置條件：`left < right`（違反為程式錯誤，以 assert 攔截）。
// 回傳空值表示 left 與 right **相鄰**，兩者之間沒有可用整數 ——
// 此時呼叫端必須依 LAY-0002 D13 對附近 window 重新編號。
//
// 複雜度：O(1)。
[[nodiscard]] std::optional<LeafKey> leaf_key_midpoint(const LeafKey& left,
                                                       const LeafKey& right) noexcept;

// 在 [left, right] 之間產生 count 個等距標籤，寫入 out。
//
// 供 relabel 使用。前置條件：`left < right`、`count >= 1`、`out` 至少可容納 count 個元素。
// 回傳 false 表示區間容不下 count 個相異標籤，呼叫端必須擴張 window（LAY-0002 D13）。
//
// 複雜度：O(count)。
[[nodiscard]] bool leaf_key_distribute(const LeafKey& left, const LeafKey& right,
                                       std::size_t count, LeafKey* out) noexcept;

}  // namespace krepis
