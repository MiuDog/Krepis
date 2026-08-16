#pragma once

// 依 DOC-0001 D6（生成器可替換、測試可用固定序列）與 DOC-0002 D3。

#include "krepis/object_id.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace krepis {

// 平台亂數來源。
//
// 責任：填入指定長度的位元組。
// 不負責：知道 ObjectId 的存在，或決定重試策略。
// 錯誤語意：**無法提供亂數是不可恢復狀況**，實作應依 FND-0002 D5 終止，不回報錯誤。
// 執行緒安全程度：實作必須可由多執行緒同時呼叫。
// 生命週期：由呼叫端擁有，必須長於使用它的 IdGenerator。
class RandomSource {
public:
    virtual ~RandomSource() = default;
    virtual void fill(std::span<std::byte> out) = 0;
};

// 取得本平台的亂數來源。Windows 使用 BCryptGenRandom。
//
// 依 architecture.md 的平台策略：實作藏在介面後，輸出是純位元組，不洩漏平台 handle。
[[nodiscard]] RandomSource& platform_random_source();

// ObjectId 生成器。
//
// 責任：產生永不為 nil 的 ObjectId。
// 不負責：記錄已產生的 ID，或保證跨行程唯一（那由 128-bit 空間的機率保障，
//         正確性則由 DOC-0001 D6 的匯入碰撞拒絕保障）。
// 執行緒安全程度：由各實作宣告，見下。
class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    [[nodiscard]] virtual ObjectId next() = 0;
};

// 正式生成器：由 RandomSource 取 16 位元組，產出 nil 時重試。
//
// 維持的不變條件：**永不回傳 nil**。
// 執行緒安全程度：與所用的 RandomSource 相同；platform_random_source() 可由多執行緒使用。
// 可否複製／移動：不可複製；持有來源參照，不擁有它。
class RandomIdGenerator final : public IdGenerator {
public:
    explicit RandomIdGenerator(RandomSource& source) noexcept : source_(&source) {}

    [[nodiscard]] ObjectId next() override;

private:
    RandomSource* source_;
};

// 測試用生成器：產生確定性遞增序列。
//
// 依 DOC-0001 D6「測試可使用固定序列」。**不得用於正式路徑** ——
// 其輸出可預測且會洩漏建立順序，違反 DOC-0001 D6 對 ObjectId 的要求。
//
// 維持的不變條件：永不回傳 nil（起始值不為零，且遞增不迴繞至零）。
// 執行緒安全程度：**不具執行緒安全**。
class SequentialIdGenerator final : public IdGenerator {
public:
    // seed 為第一個產生的 low 值；0 會被提升為 1 以跳過 nil。
    explicit SequentialIdGenerator(std::uint64_t seed = 1) noexcept
        : next_low_(seed == 0 ? 1 : seed) {}

    [[nodiscard]] ObjectId next() override;

private:
    std::uint64_t next_low_;
    std::uint64_t high_ = 0;
};

}  // namespace krepis
