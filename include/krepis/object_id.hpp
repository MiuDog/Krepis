#pragma once

// 依 DOC-0001 D6 與 DOC-0002：Workspace 全域唯一的 128-bit 永久身分。
// 資料流與拒絕路徑見 docs/architecture/object-id-generation-and-encoding.md。

#include "krepis/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace krepis {

// Workspace 全域唯一的物件身分。
//
// 責任：作為所有可引用物件的永久身分，並提供跨儲存邊界的正規編碼。
// 不負責：表達 owner、位置、物件種類或建立順序（DOC-0001 D6 明文禁止）。
// 維持的不變條件：`{0, 0}` 恆為 nil，代表「沒有物件」；生成器永不產生它。
// 擁有哪些資源：無。
// 生命週期：值型別。
// 錯誤語意：本型別的操作不失敗；解析由自由函式回傳 Result。
// 執行緒安全程度：不可變，可跨執行緒自由共享。
// 可否複製／移動：兩者皆可，且為平凡操作。
struct ObjectId {
    // MSVC 無可依賴的原生 128-bit 整數；以兩個 uint64_t 組成，
    // 與 LAY-0002 D13 的 LeafKey 保持同一慣例。
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    // 正規編碼的長度：high 的 8 位元組在前，各自 big-endian。
    static constexpr std::size_t encoded_size = 16;

    // 文字表示的長度：32 個小寫十六進位字元，無分隔符。
    static constexpr std::size_t text_size = 32;

    [[nodiscard]] constexpr bool is_nil() const noexcept { return high == 0 && low == 0; }

    friend constexpr bool operator==(const ObjectId&, const ObjectId&) noexcept = default;

    // 比較採 {high, low} 無號字典序。
    // **此順序沒有文件語意**，只用於容器索引與確定性測試（DOC-0002 D1）。
    friend constexpr auto operator<=>(const ObjectId& a, const ObjectId& b) noexcept {
        if (auto cmp = a.high <=> b.high; cmp != 0) {
            return cmp;
        }
        return a.low <=> b.low;
    }
};

inline constexpr ObjectId nil_object_id{};

// 寫入正規編碼（16 位元組，big-endian）。
void encode_object_id(const ObjectId& id, std::span<std::byte, ObjectId::encoded_size> out) noexcept;

// 由正規編碼還原。長度由型別保證，因此不會失敗。
[[nodiscard]] ObjectId decode_object_id(
    std::span<const std::byte, ObjectId::encoded_size> bytes) noexcept;

// 產生 32 個小寫十六進位字元。
[[nodiscard]] std::string object_id_to_text(const ObjectId& id);

// 解析文字表示。
//
// **只接受**恰好 32 個小寫十六進位字元；大寫、分隔符、`0x` 前綴與任何長度偏差一律拒絕。
// 失敗回傳 Error 而**不是** nil —— 以 nil 表示失敗會使「沒有物件」與「輸入損壞」
// 無法區分，那是靜默錯誤（DOC-0002 D4）。
[[nodiscard]] Result<ObjectId> parse_object_id(std::string_view text);

// 以 tag 區分用途的 ObjectId 包裝。
//
// 責任：在編譯期阻止把某一種 ID 傳給需要另一種 ID 的介面。
// 不負責：改變底層位元或 nil 語意 —— 型別只存在於編譯期。
// 維持的不變條件：與 ObjectId 相同。
// 生命週期：值型別。
// 執行緒安全程度：不可變。
// 可否複製／移動：兩者皆可。
template <typename Tag>
class TypedObjectId {
public:
    constexpr TypedObjectId() noexcept = default;

    // 由底層身分建構。刻意 explicit —— 跨型別使用必須明確表達意圖（DOC-0002 D2）。
    constexpr explicit TypedObjectId(ObjectId raw) noexcept : raw_(raw) {}

    [[nodiscard]] constexpr const ObjectId& raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return raw_.is_nil(); }

    friend constexpr bool operator==(const TypedObjectId&, const TypedObjectId&) noexcept = default;
    friend constexpr auto operator<=>(const TypedObjectId& a, const TypedObjectId& b) noexcept {
        return a.raw_ <=> b.raw_;
    }

private:
    ObjectId raw_{};
};

// Tag 型別只用於區分，永不定義。
struct PageTag;
struct ContainerTag;
struct BlockTag;
struct EmbedTag;

using PageId = TypedObjectId<PageTag>;
using ContainerId = TypedObjectId<ContainerTag>;
using BlockId = TypedObjectId<BlockTag>;
using EmbedId = TypedObjectId<EmbedTag>;

}  // namespace krepis

namespace std {

template <>
struct hash<krepis::ObjectId> {
    [[nodiscard]] size_t operator()(const krepis::ObjectId& id) const noexcept {
        // 值本身已是高品質亂數（DOC-0002 D3），混合兩半即可，不需再做雜湊擴散。
        return static_cast<size_t>(id.high ^ (id.low * 0x9E3779B97F4A7C15ULL));
    }
};

template <typename Tag>
struct hash<krepis::TypedObjectId<Tag>> {
    [[nodiscard]] size_t operator()(const krepis::TypedObjectId<Tag>& id) const noexcept {
        return hash<krepis::ObjectId>{}(id.raw());
    }
};

}  // namespace std
