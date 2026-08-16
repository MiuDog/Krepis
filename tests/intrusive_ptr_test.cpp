#include "krepis/intrusive_ptr.hpp"
#include "krepis/snapshot_id.hpp"

#include "test_support.hpp"

#include <atomic>
#include <thread>
#include <vector>

using krepis::IntrusivePtr;
using krepis::RefCounted;
using krepis::SnapshotId;
using krepis::default_reclamation_queue;
using krepis::make_intrusive;
using krepis_test::expect;

namespace {

std::atomic<int> live_nodes{0};

// 測試用的不可變節點。發布後不再修改，owning edge 只向下形成 DAG（LAY-0002 D17）。
class Node : public RefCounted {
public:
    explicit Node(int value) noexcept : value_(value) { live_nodes.fetch_add(1); }
    ~Node() override { live_nodes.fetch_sub(1); }

    [[nodiscard]] int value() const noexcept { return value_; }

    // 子節點以 owning edge 持有；新舊 revision 可安全共用同一批 immutable 節點。
    void set_child(IntrusivePtr<Node> child) noexcept { child_ = std::move(child); }
    [[nodiscard]] const IntrusivePtr<Node>& child() const noexcept { return child_; }

private:
    int value_;
    IntrusivePtr<Node> child_;
};

class DerivedNode final : public Node {
public:
    explicit DerivedNode(int value) noexcept : Node(value) {}
};

void test_initial_count_is_one() {
    auto node = make_intrusive<Node>(1);
    expect(node->use_count() == 1, "reference count 從 1 起算");
    node.reset();
    default_reclamation_queue().drain();
}

// LAY-0002 D17 閘門 1：copy、move、self-assignment 的精確 retain／release。
void test_copy_and_move_semantics() {
    auto a = make_intrusive<Node>(2);
    {
        IntrusivePtr<Node> b = a;
        expect(a->use_count() == 2, "複製後計數為 2");

        IntrusivePtr<Node> c = std::move(b);
        expect(a->use_count() == 2, "移動不改變計數");
        expect(!b, "被移出的指標為空");
        expect(c->value() == 2, "移動後仍可存取");
    }
    expect(a->use_count() == 1, "作用域結束後計數回到 1");

    a.reset();
    default_reclamation_queue().drain();
}

void test_self_assignment_is_safe() {
    auto a = make_intrusive<Node>(3);
    const IntrusivePtr<Node>& alias = a;
    a = alias;  // self-assignment
    expect(a->use_count() == 1, "self-assignment 不改變計數");
    expect(a->value() == 3, "self-assignment 後物件仍有效");

    a.reset();
    default_reclamation_queue().drain();
}

// 跨型別轉換：衍生 → 基底、T → const T。
void test_cross_type_conversion() {
    IntrusivePtr<DerivedNode> derived = make_intrusive<DerivedNode>(4);
    IntrusivePtr<Node> base = derived;
    expect(derived->use_count() == 2, "轉換為基底後計數增加");
    expect(base->value() == 4, "基底指標可存取");

    IntrusivePtr<const Node> readonly = base;
    expect(readonly->use_count() == 3, "轉為 const 後計數增加");

    derived.reset();
    base.reset();
    readonly.reset();
    default_reclamation_queue().drain();
}

// D17：最後一次 release 不在原執行緒同步銷毀，而是交給 reclamation queue。
void test_destruction_is_deferred_to_queue() {
    default_reclamation_queue().drain();
    const int before = live_nodes.load();

    {
        auto node = make_intrusive<Node>(5);
        (void)node;
    }  // 最後一次 release 於此發生

    expect(live_nodes.load() == before + 1, "release 當下尚未銷毀");
    expect(default_reclamation_queue().pending() >= 1, "節點已進入 reclamation queue");

    default_reclamation_queue().drain();
    expect(live_nodes.load() == before, "drain 後才真正銷毀");
}

// 子節點的釋放發生在父節點被銷毀時，因此需要第二次 drain。
void test_chain_is_reclaimed_completely() {
    default_reclamation_queue().drain();
    const int before = live_nodes.load();

    {
        auto parent = make_intrusive<Node>(10);
        auto child = make_intrusive<Node>(11);
        auto grandchild = make_intrusive<Node>(12);
        child->set_child(grandchild);
        parent->set_child(child);
    }

    // 反覆 drain 直到佇列清空：每一層銷毀會使下一層進入佇列。
    while (default_reclamation_queue().drain() != 0) {
    }
    expect(live_nodes.load() == before, "整條 owning chain 全部回收");
}

// D17 閘門 2：多執行緒隨機複製／釋放同一 snapshot；drain 後每個節點恰好銷毀一次。
void test_concurrent_retain_release() {
    default_reclamation_queue().drain();
    const int before = live_nodes.load();

    auto shared = make_intrusive<Node>(20);

    constexpr int thread_count = 8;
    constexpr int iterations = 2000;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&shared] {
            for (int i = 0; i < iterations; ++i) {
                IntrusivePtr<Node> local = shared;  // retain
                if (local->value() != 20) {
                    krepis_test::expect(false, "併發存取讀到錯誤內容");
                }
            }  // release
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    expect(shared->use_count() == 1, "所有 worker 結束後計數回到 1");
    expect(live_nodes.load() == before + 1, "併發期間未被提前銷毀");

    shared.reset();
    while (default_reclamation_queue().drain() != 0) {
    }
    expect(live_nodes.load() == before, "最終恰好銷毀一次");
}

// D17：關機必須 drain queue，並證明配置數等於釋放數。
void test_allocation_matches_reclamation() {
    while (default_reclamation_queue().drain() != 0) {
    }
    const int before = live_nodes.load();
    const std::size_t reclaimed_before = default_reclamation_queue().total_reclaimed();

    constexpr int node_count = 500;
    {
        std::vector<IntrusivePtr<Node>> nodes;
        nodes.reserve(node_count);
        for (int i = 0; i < node_count; ++i) {
            nodes.push_back(make_intrusive<Node>(i));
        }
    }
    while (default_reclamation_queue().drain() != 0) {
    }

    expect(live_nodes.load() == before, "存活節點數回到起點");
    expect(default_reclamation_queue().total_reclaimed() - reclaimed_before == node_count,
           "回收數等於配置數");
}

// LAY-0002 D18：持有內部 handle 的工作必須核對完整 SnapshotId。
void test_snapshot_id_axes() {
    const SnapshotId base{42, 3};
    const SnapshotId after_edit{43, 3};
    const SnapshotId after_compact{43, 4};

    expect(base.content_precedes(after_edit), "文字輸入使 content_revision 遞增");
    expect(!after_edit.content_precedes(after_compact), "compact 不改變內容先後");
    expect(!after_edit.handles_valid_for(after_compact),
           "storage_generation 改變即使 handle 失效");
    expect(after_edit.handles_valid_for(SnapshotId{43, 3}), "兩軸相同時 handle 有效");
}

}  // namespace

int main() {
    test_initial_count_is_one();
    test_copy_and_move_semantics();
    test_self_assignment_is_safe();
    test_cross_type_conversion();
    test_destruction_is_deferred_to_queue();
    test_chain_is_reclaimed_completely();
    test_concurrent_retain_release();
    test_allocation_matches_reclamation();
    test_snapshot_id_axes();
    return krepis_test::report("krepis.intrusive_ptr");
}
