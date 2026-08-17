#include "krepis/location_index.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "test_support.hpp"

#include <limits>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::FlowLocator;
using krepis::LeafKey;
using krepis::LocationEntry;
using krepis::LocationIndex;
using krepis::LocationPage;
using krepis::ObjectId;
using krepis::ObjectSlot;
using krepis::invalid_object_slot;
using krepis::make_flow_location;
using krepis::make_spatial_location;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

ContainerId make_container(std::uint64_t n) {
    return ContainerId{ObjectId{0, n}};
}

ObjectSlot slot_of(std::uint32_t n) {
    return ObjectSlot{n};
}

void test_empty_index() {
    auto idx = LocationIndex::empty();
    expect(idx.capacity() == 0, "空索引 capacity == 0");
    expect(idx.lookup(slot_of(0)).is_empty(), "空索引 lookup 回傳 empty");
    expect(idx.lookup(slot_of(1000)).is_empty(), "超出範圍 lookup 回傳 empty");
}

// 閘門 7／E1 第二輪：索引鍵是 ObjectSlot，無效 slot 必須被擋下而非當成整數使用。
void test_invalid_slot_is_rejected() {
    auto idx = LocationIndex::empty()
                   .set(slot_of(1), make_flow_location(make_container(1), LeafKey{1, 0}));

    expect(idx.lookup(invalid_object_slot).is_empty(), "無效 slot 的 lookup 回傳 empty");
    expect(!invalid_object_slot.is_valid(), "invalid_object_slot 確實無效");
}

void test_set_and_lookup() {
    auto idx = LocationIndex::empty();
    LeafKey key{100, 200};

    auto idx2 = idx.set(slot_of(5), make_flow_location(make_container(1), key));

    expect(idx.lookup(slot_of(5)).is_empty(), "原索引不變");

    auto entry = idx2.lookup(slot_of(5));
    expect(entry.is_flow(), "設定後為 flow 類型");
    expect(entry.owner == make_container(1), "owner 正確");
    expect(entry.flow.leaf_key == key, "leaf_key 正確");
}

void test_set_spatial() {
    auto idx = LocationIndex::empty()
                   .set(slot_of(10), make_spatial_location(make_container(2), 42));

    auto entry = idx.lookup(slot_of(10));
    expect(entry.is_spatial(), "spatial 類型正確");
    expect(entry.owner == make_container(2), "owner 正確");
    expect(entry.spatial.placement_key == 42, "placement_key 正確");
}

void test_clear() {
    auto idx = LocationIndex::empty()
                   .set(slot_of(5), make_flow_location(make_container(1), LeafKey{1, 2}));

    auto cleared = idx.clear(slot_of(5));
    expect(cleared.lookup(slot_of(5)).is_empty(), "clear 後為 empty");
    expect(idx.lookup(slot_of(5)).is_flow(), "原索引不變");
}

void test_cow_page_sharing() {
    auto idx = LocationIndex::empty()
                   .set(slot_of(0), make_flow_location(make_container(1), LeafKey{1, 0}))
                   .set(slot_of(1), make_flow_location(make_container(2), LeafKey{2, 0}));

    auto idx2 = idx.set(slot_of(0), make_flow_location(make_container(3), LeafKey{3, 0}));

    expect(idx.lookup(slot_of(0)).owner == make_container(1), "原 idx slot 0 不變");
    expect(idx2.lookup(slot_of(0)).owner == make_container(3), "idx2 slot 0 已更新");
    expect(idx2.lookup(slot_of(1)).owner == make_container(2), "idx2 slot 1 未改變");
}

void test_cross_page_slots() {
    auto idx = LocationIndex::empty();

    const auto slot_a = slot_of(0);
    const auto slot_b = slot_of(LocationPage::page_capacity);
    const auto slot_c = slot_of(LocationPage::page_capacity * 2 + 10);

    idx = idx.set(slot_a, make_flow_location(make_container(1), LeafKey{1, 0}));
    idx = idx.set(slot_b, make_flow_location(make_container(2), LeafKey{2, 0}));
    idx = idx.set(slot_c, make_flow_location(make_container(3), LeafKey{3, 0}));

    expect(idx.lookup(slot_a).owner == make_container(1), "page 0 entry");
    expect(idx.lookup(slot_b).owner == make_container(2), "page 1 entry");
    expect(idx.lookup(slot_c).owner == make_container(3), "page 2 entry");
    expect(idx.lookup(slot_of(1)).is_empty(), "未設定的 slot 為 empty");
}

void test_many_slots() {
    auto idx = LocationIndex::empty();
    constexpr std::uint32_t count = 200;

    for (std::uint32_t i = 0; i < count; ++i) {
        idx = idx.set(slot_of(i), make_flow_location(make_container(i + 1), LeafKey{i, 0}));
    }

    bool all_correct = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto entry = idx.lookup(slot_of(i));
        if (!entry.is_flow() || entry.owner != make_container(i + 1) ||
            entry.flow.leaf_key != LeafKey{i, 0}) {
            all_correct = false;
            break;
        }
    }
    expect(all_correct, "200 個 slot 全部正確");
}

// 閘門 7／E1：page-table 必須在 slot 超出當前深度時長高，且舊子樹完全共享。
// fanout=64、page_capacity=64 → 深度 1 只容納 4,096 個 slot。
void test_page_table_grows_beyond_one_level() {
    auto idx = LocationIndex::empty();

    const auto shallow = slot_of(4095);    // 深度 1 邊界
    const auto deep = slot_of(10000);      // 需要深度 2
    const auto deeper = slot_of(300000);   // 需要深度 3

    idx = idx.set(shallow, make_flow_location(make_container(1), LeafKey{1, 0}));
    expect(idx.lookup(shallow).owner == make_container(1), "深度 1 邊界 slot 正確");

    idx = idx.set(deep, make_flow_location(make_container(2), LeafKey{2, 0}));
    expect(idx.lookup(deep).owner == make_container(2), "長高後新 slot 正確");
    expect(idx.lookup(shallow).owner == make_container(1), "長高後舊 slot 仍可讀");

    idx = idx.set(deeper, make_flow_location(make_container(3), LeafKey{3, 0}));
    expect(idx.lookup(deeper).owner == make_container(3), "再次長高後新 slot 正確");
    expect(idx.lookup(shallow).owner == make_container(1), "兩次長高後最舊 slot 仍可讀");
    expect(idx.lookup(deep).owner == make_container(2), "兩次長高後中間 slot 仍可讀");

    expect(idx.lookup(slot_of(deep.value + 1)).is_empty(), "未設定的鄰近 slot 仍為 empty");
    expect(idx.capacity() > deeper.value, "capacity 涵蓋最深的 slot");
}

// 閘門 7／E1：更新單一 slot 不得影響舊版本（短路徑 COW）。
void test_deep_tree_cow_isolation() {
    auto base = LocationIndex::empty();
    for (std::uint32_t i = 0; i < 20; ++i) {
        base =
            base.set(slot_of(i * 1000), make_flow_location(make_container(i + 1), LeafKey{i, 0}));
    }

    auto updated = base.set(slot_of(5000), make_flow_location(make_container(999), LeafKey{99, 0}));

    expect(base.lookup(slot_of(5000)).owner == make_container(6), "舊版本不受更新影響");
    expect(updated.lookup(slot_of(5000)).owner == make_container(999), "新版本已更新");

    bool others_intact = true;
    for (std::uint32_t i = 0; i < 20; ++i) {
        if (i * 1000 == 5000) continue;
        if (updated.lookup(slot_of(i * 1000)).owner != make_container(i + 1)) {
            others_intact = false;
            break;
        }
    }
    expect(others_intact, "未改動的 slot 在新版本中完全一致");
}

// 閘門 7／E1 第二輪：capacity 不得因溢位而迴繞。
//
// `fanout^(depth+1)`：64^11 = 2^66，對 64-bit size_t 取模**恰好等於 0**——
// 迴繞會讓 capacity() 回報 0，比溢位更難察覺。
//
// ObjectSlot 是 32-bit，因此深度有界（最大 slot 2^32-1 → page_index ≤ 2^26 → 深度 ≤ 5），
// 正常路徑不會觸及溢位。本測試驗證**最大 slot 的實際行為**與飽和運算本身。
void test_capacity_does_not_wrap() {
    auto idx = LocationIndex::empty();

    // 可配置的最大 slot（invalid_value 保留為哨兵，因此取其前一個）。
    const auto largest = slot_of(ObjectSlot::invalid_value - 1);
    idx = idx.set(largest, make_flow_location(make_container(1), LeafKey{7, 7}));

    expect(idx.lookup(largest).owner == make_container(1), "最大 slot 可正確讀寫");
    expect(idx.capacity() != 0, "capacity 不得迴繞成 0");
    expect(idx.capacity() > largest.value, "capacity 必須涵蓋最大 slot");

    // 飽和運算本身。
    constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();
    expect(krepis::saturating_mul(max_size, 2) == max_size, "飽和乘法在溢位時回傳上限");
    expect(krepis::saturating_mul(0, 5) == 0, "飽和乘法對 0 正確");
    expect(krepis::saturating_mul(3, 4) == 12, "飽和乘法在未溢位時正常");
}

void test_overwrite() {
    auto idx = LocationIndex::empty()
                   .set(slot_of(5), make_flow_location(make_container(1), LeafKey{1, 0}))
                   .set(slot_of(5), make_flow_location(make_container(2), LeafKey{2, 0}));

    expect(idx.lookup(slot_of(5)).owner == make_container(2), "覆寫後為最新值");
}

}  // namespace

int main() {
    test_empty_index();
    test_invalid_slot_is_rejected();
    test_set_and_lookup();
    test_set_spatial();
    test_clear();
    test_cow_page_sharing();
    test_cross_page_slots();
    test_many_slots();
    test_page_table_grows_beyond_one_level();
    test_deep_tree_cow_isolation();
    test_capacity_does_not_wrap();
    test_overwrite();

    shutdown_default_reclamation_queue();
    return krepis_test::report("krepis.location_index");
}
