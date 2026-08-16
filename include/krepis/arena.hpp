#pragma once

// 依 FND-0002 D2：display list 等每幀產物配置於 arena（bump allocator），
// 用畢整塊丟棄，**不做個別釋放**。以一般 heap 配置意味著每幀數千次 malloc／free。
//
// 依 FND-0002 D5：配置失敗代表系統已無記憶體，直接終止，**不視為可恢復的失敗**。
// 因此本型別的配置介面不回傳 Result。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace krepis {

// 單一擁有者的 bump allocator。
//
// 責任：以極低成本提供一段生命週期一致的記憶體，並在 reset() 時一次全數回收。
// 不負責：個別釋放、重新配置、跨執行緒同步、追蹤物件解構
//         （**只可配置 trivially destructible 的型別**）。
// 維持的不變條件：至少持有一個 block；bytes_used() 永不超過 bytes_reserved()。
// 擁有哪些資源：一條由 malloc 取得的 block 串列。
// 生命週期：block 記憶體在 reset() 或解構前持續有效。
//           reset() **不縮減**已保留的容量 —— 每幀重用的場景刻意保留高水位，避免反覆配置。
// 錯誤語意：配置失敗即終止（FND-0002 D5）；容量不足不是錯誤，會自動新增 block。
// 執行緒安全程度：**不具執行緒安全**。單一擁有者使用，跨執行緒交接需外部同步。
// 可否複製／移動：不可複製；可移動。
class Arena {
public:
    // 預設 block 大小。單一 block 容不下的請求會取得專屬的較大 block。
    static constexpr std::size_t default_block_bytes = 64 * 1024;

    explicit Arena(std::size_t block_bytes = default_block_bytes);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    // 配置 bytes 個位元組並對齊至 alignment。
    // alignment 必須是 2 的冪；違反為程式錯誤，以 assert 攔截。
    // 永不回傳 nullptr —— 記憶體不足時終止。
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment);

    // 配置 count 個 T 的連續空間。**不呼叫建構子。**
    template <typename T>
    [[nodiscard]] T* allocate_array(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena 不追蹤解構，只可配置 trivially destructible 的型別");
        if (count == 0) {
            return nullptr;
        }
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    // 一次回收全部配置。已保留的 block 予以保留供重用，容量不縮減。
    void reset() noexcept;

    // 目前已配置出去的位元組數（含對齊填補）。
    [[nodiscard]] std::size_t bytes_used() const noexcept { return bytes_used_; }

    // 目前所有 block 的容量總和。
    [[nodiscard]] std::size_t bytes_reserved() const noexcept { return bytes_reserved_; }

    // 目前的 block 數量。供測試驗證成長行為。
    [[nodiscard]] std::size_t block_count() const noexcept;

private:
    struct Block {
        Block* next;
        std::size_t capacity;
        std::size_t used;
        // 資料緊接在本結構之後，見 arena.cpp 的配置方式。
        [[nodiscard]] std::uint8_t* data() noexcept {
            return reinterpret_cast<std::uint8_t*>(this) + sizeof(Block);
        }
    };

    void push_block(std::size_t capacity);
    void release_all() noexcept;

    Block* head_ = nullptr;     // block 串列頭，永遠非空（建構後）
    Block* tail_ = nullptr;     // 串列尾，新 block 一律接在此後
    Block* current_ = nullptr;  // 目前用於配置的 block；reset() 後回到 head_
    std::size_t block_bytes_ = default_block_bytes;
    std::size_t bytes_used_ = 0;
    std::size_t bytes_reserved_ = 0;
};

}  // namespace krepis
