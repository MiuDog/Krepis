#include "krepis/flow_layout_index.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "test_support.hpp"

#include <cmath>
#include <vector>

using krepis::BlockId;
using krepis::FlowLayoutConfig;
using krepis::FlowLayoutIndex;
using krepis::LayoutEntry;
using krepis::ObjectId;
using krepis::default_reclamation_queue;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

BlockId make_block(std::uint64_t n) {
    return BlockId{ObjectId{0, n}};
}

bool approx(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

void test_empty_index() {
    auto idx = FlowLayoutIndex::empty();
    expect(idx.is_empty(), "空索引 is_empty");
    expect(idx.block_count() == 0, "空索引 block_count == 0");
    expect(approx(idx.total_extent(), 0.0), "空索引 total_extent == 0");
    expect(approx(idx.prefix_extent(0), 0.0), "空索引 prefix_extent(0) == 0");
    expect(idx.lower_bound_extent(0.0) == 0, "空索引 lower_bound_extent(0) == 0");
}

void test_single_insert() {
    auto idx = FlowLayoutIndex::empty()
        .insert(0, {make_block(1), 30.0});

    expect(idx.block_count() == 1, "插入後 count == 1");
    expect(approx(idx.total_extent(), 30.0), "總高 == 30");
    expect(approx(idx.prefix_extent(0), 0.0), "prefix_extent(0) == 0");
    expect(approx(idx.prefix_extent(1), 30.0), "prefix_extent(1) == 30");
    expect(idx.at(0).block_id == make_block(1), "at(0) 正確");
}

void test_multiple_inserts_and_prefix() {
    auto idx = FlowLayoutIndex::empty()
        .insert(0, {make_block(1), 10.0})
        .insert(1, {make_block(2), 20.0})
        .insert(2, {make_block(3), 30.0});

    expect(idx.block_count() == 3, "count == 3");
    expect(approx(idx.total_extent(), 60.0), "總高 == 60");
    expect(approx(idx.prefix_extent(0), 0.0), "prefix(0)");
    expect(approx(idx.prefix_extent(1), 10.0), "prefix(1)");
    expect(approx(idx.prefix_extent(2), 30.0), "prefix(2)");
    expect(approx(idx.prefix_extent(3), 60.0), "prefix(3)");
}

void test_lower_bound_extent() {
    auto idx = FlowLayoutIndex::empty()
        .insert(0, {make_block(1), 10.0})
        .insert(1, {make_block(2), 20.0})
        .insert(2, {make_block(3), 30.0});

    expect(idx.lower_bound_extent(0.0) == 0, "y=0 → position 0");
    expect(idx.lower_bound_extent(5.0) == 0, "y=5 → position 0 (在第一個 block 內)");
    expect(idx.lower_bound_extent(10.0) == 1, "y=10 → position 1 (第一個 block 結束)");
    expect(idx.lower_bound_extent(25.0) == 1, "y=25 → position 1 (在第二個 block 內)");
    expect(idx.lower_bound_extent(30.0) == 2, "y=30 → position 2");
    expect(idx.lower_bound_extent(60.0) == 3, "y=60 → past-end");
    expect(idx.lower_bound_extent(100.0) == 3, "y>total → past-end");
}

void test_update_extent() {
    auto idx = FlowLayoutIndex::empty()
        .insert(0, {make_block(1), 10.0})
        .insert(1, {make_block(2), 20.0})
        .insert(2, {make_block(3), 30.0});

    auto updated = idx.update_extent(1, 50.0);

    expect(approx(idx.total_extent(), 60.0), "原索引不變");
    expect(approx(updated.total_extent(), 90.0), "更新後總高 == 90");
    expect(approx(updated.prefix_extent(2), 60.0), "更新後 prefix(2) == 60");
    expect(updated.at(1).measured_height == 50.0, "更新後高度 == 50");
}

void test_remove() {
    auto idx = FlowLayoutIndex::empty()
        .insert(0, {make_block(1), 10.0})
        .insert(1, {make_block(2), 20.0})
        .insert(2, {make_block(3), 30.0});

    auto without_middle = idx.remove(1);

    expect(without_middle.block_count() == 2, "移除後 count == 2");
    expect(approx(without_middle.total_extent(), 40.0), "移除後總高 == 40");
    expect(without_middle.at(0).block_id == make_block(1), "position 0 不變");
    expect(without_middle.at(1).block_id == make_block(3), "position 1 為原 block 3");
}

void test_cow_sharing() {
    auto idx1 = FlowLayoutIndex::empty()
        .insert(0, {make_block(1), 10.0})
        .insert(1, {make_block(2), 20.0});

    auto idx2 = idx1.update_extent(0, 100.0);

    expect(approx(idx1.total_extent(), 30.0), "原索引不變");
    expect(approx(idx2.total_extent(), 120.0), "更新的索引正確");
}

void test_large_index_with_splits() {
    FlowLayoutConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto idx = FlowLayoutIndex::empty(config);
    constexpr std::size_t count = 50;
    double expected_total = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        double height = static_cast<double>(i + 1) * 10.0;
        idx = idx.insert(i, {make_block(i + 1), height});
        expected_total += height;
    }

    expect(idx.block_count() == count, "大量插入後 count 正確");
    expect(approx(idx.total_extent(), expected_total), "大量插入後總高正確");

    // Verify prefix_extent at each position.
    double running = 0.0;
    bool prefix_ok = true;
    for (std::size_t i = 0; i <= count; ++i) {
        if (!approx(idx.prefix_extent(i), running)) {
            prefix_ok = false;
            break;
        }
        if (i < count) {
            running += static_cast<double>(i + 1) * 10.0;
        }
    }
    expect(prefix_ok, "所有 prefix_extent 正確");
}

void test_lower_bound_across_leaves() {
    FlowLayoutConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto idx = FlowLayoutIndex::empty(config);
    for (std::size_t i = 0; i < 20; ++i) {
        idx = idx.insert(i, {make_block(i + 1), 10.0});
    }

    // Each block is 10.0 high. Total = 200.0.
    expect(idx.lower_bound_extent(0.0) == 0, "y=0 → 0");
    expect(idx.lower_bound_extent(15.0) == 1, "y=15 → 1 (inside block 1)");
    expect(idx.lower_bound_extent(50.0) == 5, "y=50 → 5");
    expect(idx.lower_bound_extent(199.0) == 19, "y=199 → 19");
    expect(idx.lower_bound_extent(200.0) == 20, "y=200 → past-end");
}

}  // namespace

int main() {
    test_empty_index();
    test_single_insert();
    test_multiple_inserts_and_prefix();
    test_lower_bound_extent();
    test_update_extent();
    test_remove();
    test_cow_sharing();
    test_large_index_with_splits();
    test_lower_bound_across_leaves();

    shutdown_default_reclamation_queue();
    return krepis_test::report("krepis.flow_layout_index");
}
