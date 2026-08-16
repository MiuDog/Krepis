#pragma once

// 依 LAY-0002 D18：SnapshotId 分離 content revision 與 storage generation。

#include <compare>
#include <cstdint>

namespace krepis {

// 一次 authority 發布的完整識別。
//
// 責任：讓持有內部 handle（ObjectSlot、LeafKey、node pointer）的工作能判斷結果是否仍適用。
// 不負責：表達時間或作者。
// 維持的不變條件：兩軸都單調遞增，且只在 authority 的原子發布中一起變動。
// 生命週期：值型別。
// 執行緒安全程度：不可變。
// 可否複製／移動：兩者皆可。
struct SnapshotId {
    // 使用者可觀察的內容或 ownership transaction 使其遞增。
    // Collaboration 的 base revision **只以此軸判斷語意先後**，
    // 避免 maintenance 被誤認成使用者衝突（LAY-0002 D18）。
    std::uint64_t content_revision = 0;

    // Slot compact、LeafKey 全域重建等不改內容的內部重排使其遞增。
    std::uint64_t storage_generation = 0;

    friend constexpr bool operator==(const SnapshotId&, const SnapshotId&) noexcept = default;

    // 內容的語意先後**只看 content_revision**。
    [[nodiscard]] constexpr bool content_precedes(const SnapshotId& other) const noexcept {
        return content_revision < other.content_revision;
    }

    // 持有內部 handle 的工作必須用這個判斷，不得只比 content_revision。
    //
    // P1 採保守 exact-match：任一軸不同就丟棄結果。
    // 放寬需要先正式定義 dependency key（LAY-0002 D18）。
    [[nodiscard]] constexpr bool handles_valid_for(const SnapshotId& current) const noexcept {
        return *this == current;
    }
};

}  // namespace krepis
