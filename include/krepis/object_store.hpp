#pragma once

// 依 LAY-0002 D10：ObjectStore 使用穩定 slot 與分頁 copy-on-write。
//
//   ObjectId -> IdDirectoryGeneration -> ObjectSlot
//   ObjectStoreSnapshot(page_table_root) -> RecordPage -> RecordPtr
//
// 分層的用意：`ObjectId` 是永久身分（DOC-0002），`ObjectSlot` 是 authority 內部的
// 緊湊索引。把兩者分開，使記錄查找是陣列索引而非雜湊查表，
// 且 compact 可以重新配置 slot 而**不動 ObjectId**。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_id.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace krepis {

// Authority 內部的緊湊索引。**不得序列化、不得外流給 client**——
// 它會因 compact 而改變，只有 ObjectId 是持久身分。
struct ObjectSlot {
    static constexpr std::uint32_t invalid_value = 0xFFFFFFFFu;

    std::uint32_t value = invalid_value;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != invalid_value; }

    friend constexpr bool operator==(const ObjectSlot&, const ObjectSlot&) noexcept = default;
};

inline constexpr ObjectSlot invalid_object_slot{};

// Slot 配置的世代。compact 重新配置 slot 時遞增；
// 持有 slot 的 cache 必須核對世代（LAY-0002 D18 的 storage_generation）。
struct IdDirectoryGeneration {
    std::uint64_t value = 0;

    friend constexpr bool operator==(const IdDirectoryGeneration&,
                                     const IdDirectoryGeneration&) noexcept = default;
};

// 所有可儲存物件的基底。發布後不可變（D8）。
//
// 責任：攜帶 content revision，供 layout cache 判斷是否需重新量測（D1）。
// 不負責：定義具體 schema —— 那屬於 DOC 決策，不在儲存層。
// 生命週期：由 IntrusivePtr<const ObjectRecord> 持有。
// 執行緒安全程度：不可變，可跨執行緒共享。
//
// **owning edge 限制（閘門 7／E1）**：這是本庫少數刻意可擴充的 RefCounted 型別。
// 衍生的具體 record **不得以 IntrusivePtr 持有其他 record**——
// 跨記錄的關係一律以穩定 ID（ObjectId／BlockId）表達，由 ObjectStore 解析。
//
// 理由：D17 保證無循環的論證是「owning edge 只向下形成 DAG，且 bottom-up 建構」，
// 那對樹狀節點成立，但**記錄之間的關係是圖，不是樹**。若允許 record 直接持有 record，
// A→B→A 就會形成 reclamation queue 永遠回收不掉的循環，而且沒有任何診斷會報錯。
// 以 ID 表達關係使循環變成資料問題（可驗證、可修復），而不是記憶體洩漏。
class ObjectRecord : public RefCounted {
public:
    [[nodiscard]] std::uint64_t content_revision() const noexcept { return content_revision_; }

protected:
    explicit ObjectRecord(std::uint64_t content_revision) noexcept
        : content_revision_(content_revision) {}
    ~ObjectRecord() override = default;

private:
    std::uint64_t content_revision_;
};

// ObjectId <-> ObjectSlot 的雙向對應。
//
// 責任：配置穩定 slot，並提供雙向解析。
// 不負責：保存記錄內容 —— 那是 ObjectStoreSnapshot 的責任。
// 維持的不變條件：同一世代內，slot 一經配置便不改變，且不因刪除立即重用（D10）。
// 生命週期：immutable；以 IntrusivePtr<const IdDirectory> 由各 revision 共享。
// 執行緒安全程度：不可變，可跨執行緒共享。
//
// **已知成本**：`allocate` 複製整份對應表，為 O(n)。這只在**建立新物件**時發生，
// 一般編輯（修改既有 Block）不觸發。若 benchmark 顯示這是瓶頸，
// D10 允許改為 authority 獨有的 append-only 共享結構——
// 舊 snapshot 即使解析到新 slot，也會因自己的 page table 沒有該記錄而得到 NotFound。
// **在有量測證據之前不做這個最佳化。**
class IdDirectory final : public RefCounted {
public:
    [[nodiscard]] static IntrusivePtr<const IdDirectory> empty();

    // 找不到時回傳 invalid_object_slot。
    [[nodiscard]] ObjectSlot resolve(const ObjectId& id) const;

    // 反向解析。slot 無效或超出範圍時回傳 nil_object_id。
    [[nodiscard]] ObjectId id_for(ObjectSlot slot) const;

    struct AllocateResult {
        IntrusivePtr<const IdDirectory> directory;
        ObjectSlot slot;
    };

    // 為 id 配置 slot。若已存在則回傳既有 slot 且 directory 不變（不重複配置）。
    [[nodiscard]] AllocateResult allocate(const ObjectId& id) const;

    [[nodiscard]] std::size_t slot_count() const noexcept { return slot_to_id_.size(); }
    [[nodiscard]] IdDirectoryGeneration generation() const noexcept { return generation_; }

    // 供 make_intrusive 使用；外部請用 empty() 與 allocate()。
    IdDirectory(std::vector<ObjectId> slot_to_id,
                std::unordered_map<ObjectId, std::uint32_t> id_to_slot,
                IdDirectoryGeneration generation);

private:
    std::vector<ObjectId> slot_to_id_;
    std::unordered_map<ObjectId, std::uint32_t> id_to_slot_;
    IdDirectoryGeneration generation_;
};

// 一頁記錄。
//
// 維持的不變條件：entries 大小固定為 page_capacity。
// 生命週期：不可變；新舊 revision 共享未改動的 page（D10）。
class RecordPage final : public RefCounted {
public:
    static constexpr std::size_t page_capacity = 64;

    struct Entry {
        IntrusivePtr<const ObjectRecord> record;
        // 明確刪除的 slot。與「從未配置」不同：前者代表物件曾存在，
        // 後者代表舊 snapshot 看不到後來新增的物件（兩者都回傳 null 記錄）。
        bool tombstoned = false;
    };

    explicit RecordPage(std::vector<Entry> entries) noexcept;

    [[nodiscard]] const Entry& at(std::size_t offset) const;

private:
    std::vector<Entry> entries_;
};

// 某個 revision 所見的完整記錄集合。
//
// 責任：以 slot 查找記錄，並以 COW 產生新版本。
// 不負責：slot 配置 —— 那是 IdDirectory 的責任。
// 維持的不變條件：修改只複製受影響的 page，其餘由新舊 revision 共享。
// 生命週期：值型別語意；內部以 IntrusivePtr 共享 page。
// 執行緒安全程度：同一實例不可併發修改；不同實例可併發讀取。
class ObjectStoreSnapshot {
public:
    [[nodiscard]] static ObjectStoreSnapshot empty();

    // 取得記錄。未配置、已刪除或超出範圍時回傳 null。
    [[nodiscard]] IntrusivePtr<const ObjectRecord> get(ObjectSlot slot) const;

    // 是否有可見的記錄（非 null 且非 tombstone）。
    [[nodiscard]] bool contains(ObjectSlot slot) const;

    // 是否為明確刪除的 slot（用於區分「刪除」與「從未存在」）。
    [[nodiscard]] bool is_tombstoned(ObjectSlot slot) const;

    // 寫入記錄。只複製該 slot 所在的 page 與 page table 的短路徑。
    [[nodiscard]] ObjectStoreSnapshot with_record(ObjectSlot slot,
                                                  IntrusivePtr<const ObjectRecord> record) const;

    // 在新 revision 寫入 tombstone。舊 snapshot 仍能從自己的 page 讀到舊記錄（D10）。
    [[nodiscard]] ObjectStoreSnapshot with_tombstone(ObjectSlot slot) const;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return pages_.size() * RecordPage::page_capacity;
    }

private:
    explicit ObjectStoreSnapshot(std::vector<IntrusivePtr<const RecordPage>> pages) noexcept;

    std::vector<IntrusivePtr<const RecordPage>> pages_;
};

}  // namespace krepis
