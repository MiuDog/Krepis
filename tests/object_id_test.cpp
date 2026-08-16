#include "krepis/object_id.hpp"

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <string>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::ObjectId;
using krepis::nil_object_id;
using krepis_test::expect;

namespace {

[[nodiscard]] ObjectId round_trip_bytes(const ObjectId& id) {
    std::array<std::byte, ObjectId::encoded_size> bytes{};
    krepis::encode_object_id(id, std::span<std::byte, ObjectId::encoded_size>(bytes));
    return krepis::decode_object_id(
        std::span<const std::byte, ObjectId::encoded_size>(bytes));
}

void test_binary_round_trip() {
    const ObjectId samples[] = {
        nil_object_id,
        ObjectId{0, 1},
        ObjectId{1, 0},
        ObjectId{UINT64_MAX, UINT64_MAX},
        ObjectId{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL},
    };
    bool all_match = true;
    for (const ObjectId& id : samples) {
        if (round_trip_bytes(id) != id) {
            all_match = false;
        }
    }
    expect(all_match, "encode 後 decode 還原原值");
}

// DOC-0002 D4：正規編碼為 big-endian，high 的 8 位元組在前。
void test_canonical_byte_order() {
    const ObjectId id{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    std::array<std::byte, ObjectId::encoded_size> bytes{};
    krepis::encode_object_id(id, std::span<std::byte, ObjectId::encoded_size>(bytes));

    expect(std::to_integer<unsigned>(bytes[0]) == 0x01, "第一個位元組是 high 的最高位");
    expect(std::to_integer<unsigned>(bytes[7]) == 0xEF, "第八個位元組是 high 的最低位");
    expect(std::to_integer<unsigned>(bytes[8]) == 0xFE, "第九個位元組是 low 的最高位");
    expect(std::to_integer<unsigned>(bytes[15]) == 0x10, "最後一個位元組是 low 的最低位");
}

void test_text_round_trip() {
    const ObjectId id{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    const std::string text = krepis::object_id_to_text(id);

    expect(text.size() == ObjectId::text_size, "文字表示恰好 32 個字元");
    expect(text == "0123456789abcdeffedcba9876543210", "文字表示與正規編碼逐字對應");

    auto parsed = krepis::parse_object_id(text);
    expect(parsed.is_ok(), "自身的文字表示可被解析");
    expect(parsed.is_ok() && parsed.value() == id, "解析結果等於原值");
}

void test_nil_text() {
    const std::string text = krepis::object_id_to_text(nil_object_id);
    expect(text == std::string(32, '0'), "nil 的文字表示是 32 個零");

    auto parsed = krepis::parse_object_id(text);
    expect(parsed.is_ok() && parsed.value().is_nil(), "nil 的文字可被解析回 nil");
}

// DOC-0002 D4：ObjectId 不是 UUID，只接受單一嚴格格式。
void test_parse_rejects_non_canonical_forms() {
    const char* rejected[] = {
        "0123456789ABCDEFFEDCBA9876543210",      // 大寫
        "01234567-89ab-cdef-fedc-ba9876543210",  // UUID 破折號
        "0x0123456789abcdeffedcba9876543210",    // 前綴
        "0123456789abcdeffedcba987654321",       // 長度 31
        "0123456789abcdeffedcba98765432100",     // 長度 33
        "",                                      // 空字串
        "0123456789abcdeffedcba98765432g0",      // 非十六進位字元
    };
    bool all_rejected = true;
    for (const char* text : rejected) {
        auto parsed = krepis::parse_object_id(text);
        if (parsed.is_ok()) {
            all_rejected = false;
        } else if (parsed.error().code() != krepis::ErrorCode::invalid_argument) {
            all_rejected = false;
        }
    }
    expect(all_rejected, "非正規格式一律以 invalid_argument 拒絕");
}

// 以 nil 表示解析失敗會使「沒有物件」與「輸入損壞」無法區分。
void test_parse_failure_is_not_nil() {
    auto parsed = krepis::parse_object_id("not a valid object id at all!!!!");
    expect(!parsed.is_ok(), "損壞輸入被拒絕");
}

// DOC-0002 D1：比較為 {high, low} 無號字典序。
void test_ordering_is_high_then_low() {
    expect(ObjectId{0, 1} < ObjectId{0, 2}, "high 相同時比較 low");
    expect(ObjectId{0, UINT64_MAX} < ObjectId{1, 0}, "high 優先於 low");
    expect(nil_object_id < ObjectId{0, 1}, "nil 是最小值");
    expect(!(ObjectId{5, 5} < ObjectId{5, 5}), "相等時不小於");
}

void test_nil_detection() {
    expect(nil_object_id.is_nil(), "nil 被識別");
    expect(!ObjectId{0, 1}.is_nil(), "非零值不是 nil");
    expect(!ObjectId{1, 0}.is_nil(), "high 非零也不是 nil");
}

// DOC-0002 D2：型別只存在於編譯期，底層位元相同。
void test_typed_ids_share_representation() {
    const ObjectId raw{7, 9};
    const BlockId block{raw};
    const ContainerId container{raw};

    expect(block.raw() == container.raw(), "不同 tag 的底層位元相同");
    expect(block.is_nil() == raw.is_nil(), "nil 語意一致");
    expect(BlockId{} .is_nil(), "預設建構的 typed id 是 nil");
    expect(block == BlockId{raw}, "同型別同值相等");
}

}  // namespace

int main() {
    test_binary_round_trip();
    test_canonical_byte_order();
    test_text_round_trip();
    test_nil_text();
    test_parse_rejects_non_canonical_forms();
    test_parse_failure_is_not_nil();
    test_ordering_is_high_then_low();
    test_nil_detection();
    test_typed_ids_share_representation();
    return krepis_test::report("krepis.object_id");
}
