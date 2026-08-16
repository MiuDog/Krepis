#include "krepis/leaf_key.hpp"

#include "test_support.hpp"

#include <vector>

using krepis::LeafKey;
using krepis::leaf_key_distribute;
using krepis::leaf_key_max;
using krepis::leaf_key_midpoint;
using krepis::leaf_key_min;
using krepis_test::expect;

namespace {

void test_ordering_is_high_then_low() {
    expect(LeafKey{0, 1} < LeafKey{0, 2}, "high 相同時比較 low");
    expect(LeafKey{0, UINT64_MAX} < LeafKey{1, 0}, "high 優先於 low");
    expect(leaf_key_min < leaf_key_max, "最小值小於最大值");
}

// LAY-0002 D13：一般 split 使用 midpoint insertion，為 O(1)。
void test_midpoint_lies_strictly_between() {
    const LeafKey left{0, 1000};
    const LeafKey right{0, 2000};
    auto middle = leaf_key_midpoint(left, right);

    expect(middle.has_value(), "有間隙時可取中點");
    expect(middle && left < *middle, "中點大於左界");
    expect(middle && *middle < right, "中點小於右界");
    expect(middle && middle->low == 1500, "等距中點");
}

void test_midpoint_spans_the_high_word() {
    // 跨越 64 位邊界的區間，驗證 128-bit 減法與位移正確。
    const LeafKey left{0, UINT64_MAX};
    const LeafKey right{2, 0};
    auto middle = leaf_key_midpoint(left, right);

    expect(middle.has_value(), "跨字區間可取中點");
    expect(middle && left < *middle && *middle < right, "中點落在區間內");
}

// 相鄰標籤之間沒有可用整數，呼叫端必須改走 relabel。
void test_midpoint_reports_exhausted_gap() {
    expect(!leaf_key_midpoint(LeafKey{0, 5}, LeafKey{0, 6}).has_value(),
           "相鄰時回報無間隙");
    expect(!leaf_key_midpoint(LeafKey{0, UINT64_MAX}, LeafKey{1, 0}).has_value(),
           "跨字相鄰時同樣回報無間隙");
}

// 反覆在同一側 split 會逐步耗盡間隙 —— 這正是 D13 需要 relabel 的原因。
void test_repeated_split_eventually_exhausts_gap() {
    LeafKey left{0, 0};
    const LeafKey right{0, 64};
    int successful_splits = 0;
    while (auto middle = leaf_key_midpoint(left, right)) {
        left = *middle;
        ++successful_splits;
        if (successful_splits > 128) {
            break;  // 保護：不應發生
        }
    }
    expect(successful_splits > 0, "初期可正常 split");
    expect(successful_splits <= 64, "間隙有限，最終耗盡");
}

void test_distribute_is_strictly_increasing_and_inside() {
    const LeafKey left{0, 0};
    const LeafKey right{0, 10000};
    constexpr std::size_t count = 16;
    std::vector<LeafKey> keys(count);

    const bool ok = leaf_key_distribute(left, right, count, keys.data());
    expect(ok, "寬區間可容納 16 個標籤");

    bool increasing = true;
    for (std::size_t i = 1; i < count; ++i) {
        if (!(keys[i - 1] < keys[i])) {
            increasing = false;
        }
    }
    expect(increasing, "重新編號後嚴格遞增");
    expect(left < keys.front(), "第一個標籤大於左界");
    expect(keys.back() < right, "最後一個標籤小於右界");
}

void test_distribute_uses_full_128_bit_space() {
    constexpr std::size_t count = 32;
    std::vector<LeafKey> keys(count);
    const bool ok = leaf_key_distribute(leaf_key_min, leaf_key_max, count, keys.data());

    expect(ok, "整個 128-bit 空間可重新編號");
    expect(keys.front().high > 0, "標籤分佈到 high 字，未擠在低位");

    bool increasing = true;
    for (std::size_t i = 1; i < count; ++i) {
        if (!(keys[i - 1] < keys[i])) {
            increasing = false;
        }
    }
    expect(increasing, "全空間重新編號後嚴格遞增");
}

// 區間容不下要求的數量時必須回報失敗，讓呼叫端擴張 window。
void test_distribute_reports_insufficient_room() {
    std::vector<LeafKey> keys(8);
    const bool ok = leaf_key_distribute(LeafKey{0, 0}, LeafKey{0, 4}, 8, keys.data());
    expect(!ok, "區間過窄時回報失敗");
}

}  // namespace

int main() {
    test_ordering_is_high_then_low();
    test_midpoint_lies_strictly_between();
    test_midpoint_spans_the_high_word();
    test_midpoint_reports_exhausted_gap();
    test_repeated_split_eventually_exhausts_gap();
    test_distribute_is_strictly_increasing_and_inside();
    test_distribute_uses_full_128_bit_space();
    test_distribute_reports_insufficient_room();
    return krepis_test::report("krepis.leaf_key");
}
