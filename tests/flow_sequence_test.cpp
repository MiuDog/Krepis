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
using krepis::TreeCursor;
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

// --- TreeCursor tests ---

void test_cursor_single_element() {
    auto seq = FlowSequence::empty().insert(0, make_block(1));
    auto cursor = TreeCursor(seq, 0);

    expect(cursor.is_valid(), "單元素 cursor 有效");
    expect(cursor.current() == make_block(1), "單元素 cursor 指向正確 block");
    expect(cursor.position() == 0, "單元素 cursor position == 0");
    expect(!cursor.advance(), "單元素 cursor advance 回傳 false");
    expect(!cursor.is_valid(), "advance 後 cursor 無效");
}

void test_cursor_forward_traversal() {
    auto seq = FlowSequence::empty();
    for (std::size_t i = 0; i < 10; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto cursor = TreeCursor(seq, 0);
    bool all_correct = true;
    for (std::size_t i = 0; i < 10; ++i) {
        if (!cursor.is_valid() || cursor.current() != make_block(i + 1) ||
            cursor.position() != i) {
            all_correct = false;
            break;
        }
        if (i < 9) {
            expect(cursor.advance(), "中途 advance 成功");
        }
    }
    expect(all_correct, "前向走訪所有 block 正確");
    expect(!cursor.advance(), "最後一個 block 後 advance 回傳 false");
}

void test_cursor_backward_traversal() {
    auto seq = FlowSequence::empty();
    for (std::size_t i = 0; i < 10; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto cursor = TreeCursor(seq, 9);
    bool all_correct = true;
    for (std::size_t expected = 10; expected >= 1; --expected) {
        if (!cursor.is_valid() || cursor.current() != make_block(expected)) {
            all_correct = false;
            break;
        }
        if (expected > 1) {
            expect(cursor.retreat(), "中途 retreat 成功");
        }
    }
    expect(all_correct, "後向走訪所有 block 正確");
    expect(!cursor.retreat(), "第一個 block 後 retreat 回傳 false");
    expect(cursor.is_valid(), "retreat 失敗後 cursor 仍有效");
    expect(cursor.position() == 0, "retreat 失敗後 position 仍為 0");
}

void test_cursor_cross_leaf_boundaries() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    constexpr std::size_t count = 30;
    for (std::size_t i = 0; i < count; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto cursor = TreeCursor(seq, 0);
    bool forward_ok = true;
    for (std::size_t i = 0; i < count; ++i) {
        if (!cursor.is_valid() || cursor.current() != make_block(i + 1) ||
            cursor.position() != i) {
            forward_ok = false;
            break;
        }
        if (i + 1 < count) {
            cursor.advance();
        }
    }
    expect(forward_ok, "跨 leaf 前向走訪全部正確");

    bool backward_ok = true;
    for (std::size_t i = count; i >= 1; --i) {
        if (!cursor.is_valid() || cursor.current() != make_block(i) ||
            cursor.position() != i - 1) {
            backward_ok = false;
            break;
        }
        if (i > 1) {
            cursor.retreat();
        }
    }
    expect(backward_ok, "跨 leaf 後向走訪全部正確");
}

void test_cursor_seek_middle() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 20; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto cursor = TreeCursor(seq, 10);
    expect(cursor.is_valid(), "中間位置 cursor 有效");
    expect(cursor.current() == make_block(11), "seek 到 position 10 正確");
    expect(cursor.position() == 10, "position 回報 10");

    cursor.advance();
    expect(cursor.current() == make_block(12), "advance 後為 block 12");

    cursor.retreat();
    cursor.retreat();
    expect(cursor.current() == make_block(10), "retreat 兩次後為 block 10");
}

void test_cursor_bidirectional() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 16; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto cursor = TreeCursor(seq, 0);
    for (int i = 0; i < 8; ++i) {
        cursor.advance();
    }
    expect(cursor.position() == 8, "前進 8 步後 position == 8");

    for (int i = 0; i < 3; ++i) {
        cursor.retreat();
    }
    expect(cursor.position() == 5, "後退 3 步後 position == 5");
    expect(cursor.current() == make_block(6), "position 5 指向 block 6");

    for (int i = 0; i < 10; ++i) {
        cursor.advance();
    }
    expect(cursor.position() == 15, "再前進 10 步後 position == 15");
    expect(cursor.current() == make_block(16), "最後一個 block");
    expect(!cursor.advance(), "再 advance 到終點");
}

// --- D16: Merge/Redistribution tests ---

void test_merge_on_underflow() {
    FlowSequenceConfig config;
    config.leaf_capacity = 8;
    config.internal_fanout = 4;
    config.merge_low_water = 3;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 9; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }
    // After 9 inserts with capacity 8: split into two leaves (4+5).
    // Now remove from the smaller leaf until it drops below low_water (3).
    // Remove block at position 0 three times -> leaf goes from 4 to 1, below low_water=3.
    seq = seq.remove(0);  // leaf: 3 blocks (still >= low_water)
    seq = seq.remove(0);  // leaf: 2 blocks (< low_water=3) -> merge/redistribute

    expect(seq.block_count() == 7, "merge 後 count 正確");
    bool all_correct = true;
    for (std::size_t i = 0; i < 7; ++i) {
        if (seq.at(i) != make_block(i + 3)) {
            all_correct = false;
            break;
        }
    }
    expect(all_correct, "merge 後所有位置正確");
}

void test_redistribute_on_underflow() {
    FlowSequenceConfig config;
    config.leaf_capacity = 8;
    config.internal_fanout = 4;
    config.merge_low_water = 3;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 14; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }
    // 14 blocks, capacity 8: split produces leaves. Remove enough from front
    // to trigger underflow, but total with sibling exceeds capacity -> redistribute.
    for (int i = 0; i < 5; ++i) {
        seq = seq.remove(0);
    }

    expect(seq.block_count() == 9, "redistribute 後 count 正確");
    bool all_correct = true;
    for (std::size_t i = 0; i < 9; ++i) {
        if (seq.at(i) != make_block(i + 6)) {
            all_correct = false;
            break;
        }
    }
    expect(all_correct, "redistribute 後所有位置正確");
}

void test_repeated_delete_with_rebalance() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;
    config.merge_low_water = 2;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 30; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    // Remove all from front, one by one. Tests merge cascading up.
    for (std::size_t i = 30; i > 0; --i) {
        expect(seq.block_count() == i, "逐一刪除中 count 正確");
        seq = seq.remove(0);
    }
    expect(seq.is_empty(), "全部刪除後為空");
}

void test_no_rebalance_above_low_water() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;
    config.merge_low_water = 2;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 5; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }
    // Split into 2+3. Remove one from the 3-leaf -> 2+2. Both >= low_water.
    seq = seq.remove(4);  // remove last -> leaves should be 2+2
    expect(seq.block_count() == 4, "刪除後 count == 4");
    for (std::size_t i = 0; i < 4; ++i) {
        expect(seq.at(i) == make_block(i + 1), "高於 low_water 不重平衡，順序不變");
    }
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

    test_merge_on_underflow();
    test_redistribute_on_underflow();
    test_repeated_delete_with_rebalance();
    test_no_rebalance_above_low_water();

    test_cursor_single_element();
    test_cursor_forward_traversal();
    test_cursor_backward_traversal();
    test_cursor_cross_leaf_boundaries();
    test_cursor_seek_middle();
    test_cursor_bidirectional();

    shutdown_default_reclamation_queue();
    return krepis_test::report("krepis.flow_sequence");
}
