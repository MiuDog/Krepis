#include "krepis/flow_sequence.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "test_support.hpp"

#include <vector>

using krepis::BlockId;
using krepis::FlowInternalNode;
using krepis::FlowLeafNode;
using krepis::FlowSequence;
using krepis::FlowSequenceConfig;
using krepis::FlowSequenceNode;
using krepis::ObjectId;
using krepis::default_reclamation_queue;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

BlockId make_block(std::uint64_t n) {
    return BlockId{ObjectId{0, n}};
}

void test_empty_sequence() {
    auto seq = FlowSequence::empty();
    expect(seq.is_empty(), "空序列 is_empty");
    expect(seq.block_count() == 0, "空序列 block_count == 0");
    expect(!seq.root(), "空序列 root 為 null");
}

void test_single_insert() {
    auto seq = FlowSequence::empty();
    auto seq2 = seq.insert(0, make_block(1));

    expect(seq.is_empty(), "insert 不改變原序列");
    expect(seq2.block_count() == 1, "插入後 block_count == 1");
    expect(seq2.at(0) == make_block(1), "插入的 block 可取回");
}

void test_insert_at_beginning() {
    auto seq = FlowSequence::empty()
                   .insert(0, make_block(2))
                   .insert(0, make_block(1));

    expect(seq.block_count() == 2, "兩次插入後 count == 2");
    expect(seq.at(0) == make_block(1), "開頭插入的 block 在 position 0");
    expect(seq.at(1) == make_block(2), "原本的 block 被推到 position 1");
}

void test_insert_at_end() {
    auto seq = FlowSequence::empty()
                   .insert(0, make_block(1))
                   .insert(1, make_block(2))
                   .insert(2, make_block(3));

    expect(seq.block_count() == 3, "三次尾端插入後 count == 3");
    for (std::size_t i = 0; i < 3; ++i) {
        expect(seq.at(i) == make_block(i + 1), "尾端插入的順序正確");
    }
}

void test_insert_at_middle() {
    auto seq = FlowSequence::empty()
                   .insert(0, make_block(1))
                   .insert(1, make_block(3))
                   .insert(1, make_block(2));

    expect(seq.block_count() == 3, "中間插入後 count == 3");
    expect(seq.at(0) == make_block(1), "position 0 不變");
    expect(seq.at(1) == make_block(2), "中間插入在 position 1");
    expect(seq.at(2) == make_block(3), "原 position 1 推到 2");
}

void test_remove_single() {
    auto seq = FlowSequence::empty().insert(0, make_block(1));
    auto seq2 = seq.remove(0);

    expect(seq.block_count() == 1, "remove 不改變原序列");
    expect(seq2.is_empty(), "移除唯一元素後為空");
}

void test_remove_preserves_order() {
    auto seq = FlowSequence::empty()
                   .insert(0, make_block(1))
                   .insert(1, make_block(2))
                   .insert(2, make_block(3));

    auto without_middle = seq.remove(1);
    expect(without_middle.block_count() == 2, "移除中間後 count == 2");
    expect(without_middle.at(0) == make_block(1), "移除後 position 0 不變");
    expect(without_middle.at(1) == make_block(3), "移除後原 position 2 變為 1");
}

void test_cow_sharing() {
    auto seq1 = FlowSequence::empty()
                    .insert(0, make_block(1))
                    .insert(1, make_block(2));

    auto seq2 = seq1.insert(2, make_block(3));
    auto seq3 = seq1.insert(2, make_block(4));

    expect(seq1.block_count() == 2, "原序列不變");
    expect(seq2.block_count() == 3, "分支 A 有 3 個");
    expect(seq3.block_count() == 3, "分支 B 有 3 個");
    expect(seq2.at(2) == make_block(3), "分支 A 尾端為 3");
    expect(seq3.at(2) == make_block(4), "分支 B 尾端為 4");
}

void test_split_on_overflow() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 5; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    expect(seq.block_count() == 5, "超過 leaf 容量後 count 正確");
    expect(seq.root() && !seq.root()->is_leaf(),
           "split 後 root 是 internal node");

    for (std::size_t i = 0; i < 5; ++i) {
        expect(seq.at(i) == make_block(i + 1), "split 後每個位置正確");
    }
}

void test_multiple_splits() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    constexpr std::size_t count = 50;
    for (std::size_t i = 0; i < count; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    expect(seq.block_count() == count, "大量插入後 count 正確");
    bool all_correct = true;
    for (std::size_t i = 0; i < count; ++i) {
        if (seq.at(i) != make_block(i + 1)) {
            all_correct = false;
            break;
        }
    }
    expect(all_correct, "大量插入後所有位置正確");
}

void test_remove_across_leaves() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 20; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    for (std::size_t i = 0; i < 20; ++i) {
        seq = seq.remove(0);
    }
    expect(seq.is_empty(), "逐一移除後為空");
}

void test_interleaved_insert_remove() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);

    for (std::size_t i = 0; i < 10; ++i) {
        seq = seq.insert(seq.block_count(), make_block(i + 1));
    }
    expect(seq.block_count() == 10, "初始插入 10 個");

    seq = seq.remove(0);
    seq = seq.remove(0);
    expect(seq.block_count() == 8, "移除兩個後剩 8");
    expect(seq.at(0) == make_block(3), "移除後第一個是 block 3");

    seq = seq.insert(0, make_block(100));
    expect(seq.block_count() == 9, "再插入一個後為 9");
    expect(seq.at(0) == make_block(100), "新插入在開頭");
}

void test_large_sequence() {
    auto seq = FlowSequence::empty();
    constexpr std::size_t count = 1000;
    for (std::size_t i = 0; i < count; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    expect(seq.block_count() == count, "1000 個 block 插入後 count 正確");

    bool all_correct = true;
    for (std::size_t i = 0; i < count; ++i) {
        if (seq.at(i) != make_block(i + 1)) {
            all_correct = false;
            break;
        }
    }
    expect(all_correct, "1000 個 block 全部可正確取回");
}

void test_reclamation_accounting() {
    auto& queue = default_reclamation_queue();
    auto baseline = queue.total_reclaimed();

    {
        FlowSequenceConfig config;
        config.leaf_capacity = 4;
        config.internal_fanout = 4;

        auto seq = FlowSequence::empty(config);
        for (std::size_t i = 0; i < 20; ++i) {
            seq = seq.insert(i, make_block(i + 1));
        }
    }

    queue.wait_until_idle();
    expect(queue.total_reclaimed() > baseline,
           "FlowSequence 釋放後 reclamation queue 有回收節點");
    expect(queue.pending() == 0, "等待後無待回收節點");
}

}  // namespace

int main() {
    test_empty_sequence();
    test_single_insert();
    test_insert_at_beginning();
    test_insert_at_end();
    test_insert_at_middle();
    test_remove_single();
    test_remove_preserves_order();
    test_cow_sharing();
    test_split_on_overflow();
    test_multiple_splits();
    test_remove_across_leaves();
    test_interleaved_insert_remove();
    test_large_sequence();
    test_reclamation_accounting();

    shutdown_default_reclamation_queue();
    return krepis_test::report("krepis.flow_sequence");
}
