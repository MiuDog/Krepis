#include "krepis/intrusive_ptr.hpp"
#include "krepis/snapshot_id.hpp"

#include "test_support.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

using krepis::IntrusivePtr;
using krepis::RefCounted;
using krepis::SnapshotId;
using krepis::default_reclamation_queue;
using krepis::make_intrusive;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

std::atomic<int> live_nodes{0};
std::mutex destruction_thread_mutex;
std::thread::id last_destruction_thread;

// 測試用的不可變節點。發布後不再修改，owning edge 只向下形成 DAG（LAY-0002 D17）。
class Node : public RefCounted {
public:
    explicit Node(int value, IntrusivePtr<const Node> child = nullptr) noexcept
        : value_(value), child_(std::move(child)) {
        live_nodes.fetch_add(1);
    }
    ~Node() override {
        {
            std::lock_guard lock(destruction_thread_mutex);
            last_destruction_thread = std::this_thread::get_id();
        }
        live_nodes.fetch_sub(1);
    }

    [[nodiscard]] int value() const noexcept { return value_; }
    [[nodiscard]] const IntrusivePtr<const Node>& child() const noexcept { return child_; }

private:
    const int value_;
    const IntrusivePtr<const Node> child_;
};

class DerivedNode final : public Node {
public:
    explicit DerivedNode(int value) noexcept : Node(value) {}
};

template <typename T>
concept HasPublicRetain = requires(const T& value) { value.retain(); };

template <typename T>
concept HasPublicRelease = requires(const T& value) { value.release(); };

template <typename T>
concept HasPublicAdopt = requires(const T* value) { IntrusivePtr<const T>::adopt(value); };

static_assert(!HasPublicRetain<Node>, "retain 必須由 IntrusivePtr 私下呼叫");
static_assert(!HasPublicRelease<Node>, "release 必須由 IntrusivePtr 私下呼叫");
static_assert(!HasPublicAdopt<Node>, "raw pointer adopt path 不得公開");
static_assert(std::is_same_v<decltype(make_intrusive<Node>(1)), IntrusivePtr<const Node>>,
              "factory 必須只發布 const owning pointer");

void wait_until_reclaimed() {
    default_reclamation_queue().wait_until_idle();
}

[[nodiscard]] std::thread::id read_last_destruction_thread() {
    std::lock_guard lock(destruction_thread_mutex);
    return last_destruction_thread;
}

void test_initial_count_is_one() {
    auto node = make_intrusive<Node>(1);
    expect(node->use_count() == 1, "reference count 從 1 起算");
    node.reset();
    wait_until_reclaimed();
}

// LAY-0002 D17 閘門 1：copy、move、self-assignment 的精確 retain／release。
void test_copy_and_move_semantics() {
    auto a = make_intrusive<Node>(2);
    {
        IntrusivePtr<const Node> b = a;
        expect(a->use_count() == 2, "複製後計數為 2");

        IntrusivePtr<const Node> c = std::move(b);
        expect(a->use_count() == 2, "移動不改變計數");
        expect(!b, "被移出的指標為空");
        expect(c->value() == 2, "移動後仍可存取");
    }
    expect(a->use_count() == 1, "作用域結束後計數回到 1");

    a.reset();
    wait_until_reclaimed();
}

void test_self_assignment_is_safe() {
    auto a = make_intrusive<Node>(3);
    const IntrusivePtr<const Node>& alias = a;
    a = alias;  // self-assignment
    expect(a->use_count() == 1, "self-assignment 不改變計數");
    expect(a->value() == 3, "self-assignment 後物件仍有效");

    a.reset();
    wait_until_reclaimed();
}

// 跨型別轉換：衍生 → 基底、T → const T。
void test_cross_type_conversion() {
    IntrusivePtr<const DerivedNode> derived = make_intrusive<DerivedNode>(4);
    IntrusivePtr<const Node> base = derived;
    expect(derived->use_count() == 2, "轉換為基底後計數增加");
    expect(base->value() == 4, "基底指標可存取");

    derived.reset();
    base.reset();
    wait_until_reclaimed();
}

// D17：最後一次 release 不在原執行緒同步銷毀，而是由 reclamation worker 執行。
void test_destruction_runs_on_reclamation_worker() {
    wait_until_reclaimed();
    const int before = live_nodes.load();
    const std::thread::id releasing_thread = std::this_thread::get_id();

    {
        auto node = make_intrusive<Node>(5);
        (void)node;
    }  // 最後一次 release 於此發生

    wait_until_reclaimed();
    expect(live_nodes.load() == before, "背景 worker 完成銷毀");
    expect(read_last_destruction_thread() != releasing_thread, "destructor 不在 release 執行緒執行");
}

// 子節點的釋放發生在父節點被銷毀時，因此需要第二次 drain。
void test_chain_is_reclaimed_completely() {
    wait_until_reclaimed();
    const int before = live_nodes.load();

    {
        auto grandchild = make_intrusive<Node>(12);
        auto child = make_intrusive<Node>(11, grandchild);
        auto parent = make_intrusive<Node>(10, child);
        expect(parent->child()->child()->value() == 12, "bottom-up immutable DAG 可走訪");
    }

    wait_until_reclaimed();
    expect(live_nodes.load() == before, "整條 owning chain 全部回收");
}

// D17 閘門 2：多執行緒隨機複製／釋放同一 snapshot；drain 後每個節點恰好銷毀一次。
void test_concurrent_retain_release() {
    wait_until_reclaimed();
    const int before = live_nodes.load();

    auto shared = make_intrusive<Node>(20);

    constexpr int thread_count = 8;
    constexpr int iterations = 2000;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&shared] {
            for (int i = 0; i < iterations; ++i) {
                IntrusivePtr<const Node> local = shared;  // retain
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
    wait_until_reclaimed();
    expect(live_nodes.load() == before, "最終恰好銷毀一次");
}

// D17：producer enqueue 與背景 consumer drain 必須可真正並行，且 outstanding 計數不得下溢。
void test_concurrent_enqueue_and_background_drain() {
    wait_until_reclaimed();
    const int before = live_nodes.load();
    const std::size_t reclaimed_before = default_reclamation_queue().total_reclaimed();

    constexpr int thread_count = 8;
    constexpr int nodes_per_thread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (int thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([] {
            for (int value = 0; value < nodes_per_thread; ++value) {
                auto node = make_intrusive<Node>(value);
                (void)node;
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    wait_until_reclaimed();
    const std::size_t expected = thread_count * nodes_per_thread;
    expect(default_reclamation_queue().pending() == 0, "背景 drain 後 outstanding 為零");
    expect(default_reclamation_queue().total_reclaimed() - reclaimed_before == expected,
           "並行 enqueue 的每個 node 恰好回收一次");
    expect(live_nodes.load() == before, "並行 enqueue／drain 後沒有存活 node");
}

// D17：關機必須 drain queue，並證明配置數等於釋放數。
void test_allocation_matches_reclamation() {
    wait_until_reclaimed();
    const int before = live_nodes.load();
    const std::size_t reclaimed_before = default_reclamation_queue().total_reclaimed();

    constexpr int node_count = 500;
    {
        std::vector<IntrusivePtr<const Node>> nodes;
        nodes.reserve(node_count);
        for (int i = 0; i < node_count; ++i) {
            nodes.push_back(make_intrusive<Node>(i));
        }
    }
    wait_until_reclaimed();

    expect(live_nodes.load() == before, "存活節點數回到起點");
    expect(default_reclamation_queue().total_reclaimed() - reclaimed_before == node_count,
           "回收數等於配置數");
}

// D17：關機只發出停止要求並 join worker；呼叫者不得直接執行 destructor。
void test_shutdown_drains_and_joins_worker() {
    wait_until_reclaimed();
    const int before = live_nodes.load();

    {
        auto node = make_intrusive<Node>(99);
        (void)node;
    }

    shutdown_default_reclamation_queue();
    expect(default_reclamation_queue().is_shutdown(), "reclamation queue 進入 Stopped");
    expect(default_reclamation_queue().pending() == 0, "shutdown 完成後沒有 outstanding node");
    expect(live_nodes.load() == before, "shutdown 已完成所有背景銷毀");
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
    test_destruction_runs_on_reclamation_worker();
    test_chain_is_reclaimed_completely();
    test_concurrent_retain_release();
    test_concurrent_enqueue_and_background_drain();
    test_allocation_matches_reclamation();
    test_snapshot_id_axes();
    test_shutdown_drains_and_joins_worker();
    return krepis_test::report("krepis.intrusive_ptr");
}
