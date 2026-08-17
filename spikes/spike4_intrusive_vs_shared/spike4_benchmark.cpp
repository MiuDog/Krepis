// Spike 4：IntrusivePtr vs std::shared_ptr benchmark
//
// LAY-0002 D17 閘門 6：若 IntrusivePtr 沒有可重現的整體優勢，D17 必須重開。
// 五個 workload：
//   1. retain／release 迴圈（單執行緒高頻 copy/destroy）
//   2. COW edit（模擬 B+ tree path copy）
//   3. 跨執行緒 handoff（producer 建立，consumer 銷毀）
//   4. 深 DAG 回收（chain 釋放 root）
//   5. 多執行緒併發 retain/release

#include "krepis/intrusive_ptr.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

// ─── 測試用節點 ───

class INode : public krepis::RefCounted {
public:
    explicit INode(int v, krepis::IntrusivePtr<const INode> child = nullptr) noexcept
        : value_(v), child_(std::move(child)) {}
    [[nodiscard]] int value() const noexcept { return value_; }
    [[nodiscard]] const krepis::IntrusivePtr<const INode>& child() const noexcept { return child_; }

private:
    const int value_;
    const krepis::IntrusivePtr<const INode> child_;
};

struct SNode {
    explicit SNode(int v) noexcept : value(v) {}
    int value;
    std::shared_ptr<SNode> child;
};

// ─── 工具 ───

double micros(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

void print_result(const char* name, double intrusive_us, double shared_us) {
    double ratio = shared_us / intrusive_us;
    std::printf("  %-35s  intrusive: %10.1f us  shared: %10.1f us  ratio: %.2fx\n",
                name, intrusive_us, shared_us, ratio);
}

[[nodiscard]] krepis::IntrusivePtr<const INode> copy_incremented(const INode* node) {
    if (node == nullptr) return nullptr;

    auto child = copy_incremented(node->child().get());
    return krepis::make_intrusive<INode>(node->value() + 1, std::move(child));
}

// 跑 measure_runs 次取最小值
template <typename F>
double best_of(F fn, int warmup = 3, int measure = 5) {
    for (int i = 0; i < warmup; ++i) fn();
    double best = 1e18;
    for (int i = 0; i < measure; ++i) {
        double t = fn();
        if (t < best) best = t;
    }
    return best;
}

// ─── Workload 1：retain／release 迴圈 ───

void bench_retain_release(int iterations) {
    auto& queue = krepis::default_reclamation_queue();

    double ti = best_of([&] {
        auto root = krepis::make_intrusive<INode>(42);
        auto start = Clock::now();
        for (int i = 0; i < iterations; ++i) {
            auto copy = root;
            (void)copy;
        }
        double elapsed = micros(start);
        root.reset();
        queue.wait_until_idle();
        return elapsed;
    });

    double ts = best_of([&] {
        auto root = std::make_shared<SNode>(42);
        auto start = Clock::now();
        for (int i = 0; i < iterations; ++i) {
            auto copy = root;
            (void)copy;
        }
        return micros(start);
    });

    char label[64];
    std::snprintf(label, sizeof(label), "%dM ops", iterations / 1'000'000);
    print_result(label, ti, ts);
}

// ─── Workload 2：COW edit（模擬 B+ tree path copy）───

void bench_cow_edit(int tree_depth, int edits) {
    auto& queue = krepis::default_reclamation_queue();

    double ti = best_of([&] {
        // 建立初始 chain
        auto current_i = krepis::make_intrusive<INode>(0);
        for (int d = 1; d < tree_depth; ++d) {
            current_i = krepis::make_intrusive<INode>(d, std::move(current_i));
        }

        auto start = Clock::now();
        for (int e = 0; e < edits; ++e) {
            // 從 root 到 leaf 複製每個節點（線性 chain = 整條路徑）
            current_i = copy_incremented(current_i.get());
        }
        double elapsed = micros(start);
        current_i.reset();
        queue.wait_until_idle();
        return elapsed;
    });

    double ts = best_of([&] {
        auto current_s = std::make_shared<SNode>(0);
        for (int d = 1; d < tree_depth; ++d) {
            auto parent = std::make_shared<SNode>(d);
            parent->child = std::move(current_s);
            current_s = std::move(parent);
        }

        auto start = Clock::now();
        for (int e = 0; e < edits; ++e) {
            std::shared_ptr<SNode> new_root;
            std::shared_ptr<SNode>* attach = nullptr;
            const SNode* old = current_s.get();

            while (old != nullptr) {
                auto copy = std::make_shared<SNode>(old->value + 1);
                if (attach == nullptr) {
                    new_root = std::move(copy);
                    attach = &new_root->child;
                } else {
                    *attach = std::move(copy);
                    attach = &(*attach)->child;
                }
                old = old->child.get();
            }
            current_s = std::move(new_root);
        }
        return micros(start);
    });

    char label[64];
    std::snprintf(label, sizeof(label), "depth=%d, %dk edits", tree_depth, edits / 1000);
    print_result(label, ti, ts);
}

// ─── Workload 3：跨執行緒 handoff ───

void bench_cross_thread_handoff(int count) {
    auto& queue = krepis::default_reclamation_queue();

    double ti = best_of([&] {
        std::vector<krepis::IntrusivePtr<const INode>> items(count);
        for (int i = 0; i < count; ++i)
            items[i] = krepis::make_intrusive<INode>(i);

        std::vector<krepis::IntrusivePtr<const INode>> received(count);
        auto start = Clock::now();
        std::thread consumer([&] {
            for (int i = 0; i < count; ++i)
                received[i] = std::move(items[i]);
            received.clear();
            queue.wait_until_idle();
        });
        consumer.join();
        return micros(start);
    });

    double ts = best_of([&] {
        std::vector<std::shared_ptr<SNode>> items(count);
        for (int i = 0; i < count; ++i)
            items[i] = std::make_shared<SNode>(i);

        std::vector<std::shared_ptr<SNode>> received(count);
        auto start = Clock::now();
        std::thread consumer([&] {
            for (int i = 0; i < count; ++i)
                received[i] = std::move(items[i]);
            received.clear();
        });
        consumer.join();
        return micros(start);
    });

    char label[64];
    std::snprintf(label, sizeof(label), "%dk nodes", count / 1000);
    print_result(label, ti, ts);
}

// ─── Workload 4：深 DAG 回收 ───
// shared_ptr 在深度太高時會 stack overflow（遞迴析構），因此限制深度。

void bench_deep_dag_reclaim(int depth) {
    auto& queue = krepis::default_reclamation_queue();

    double ti = best_of([&] {
        auto current = krepis::make_intrusive<INode>(0);
        for (int d = 1; d < depth; ++d) {
            current = krepis::make_intrusive<INode>(d, std::move(current));
        }
        auto start = Clock::now();
        current.reset();
        queue.wait_until_idle();
        return micros(start);
    });

    double ts = best_of([&] {
        // shared_ptr：用迭代方式拆鏈以避免 stack overflow
        auto current = std::make_shared<SNode>(0);
        for (int d = 1; d < depth; ++d) {
            auto parent = std::make_shared<SNode>(d);
            parent->child = std::move(current);
            current = std::move(parent);
        }
        auto start = Clock::now();
        // 迭代釋放以避免遞迴析構 stack overflow
        while (current) {
            auto next = std::move(current->child);
            current.reset();
            current = std::move(next);
        }
        return micros(start);
    });

    char label[64];
    std::snprintf(label, sizeof(label), "depth=%d", depth);
    print_result(label, ti, ts);
}

// ─── Workload 5：多執行緒併發 retain/release ───

void bench_concurrent_retain_release(int threads, int ops_per_thread) {
    auto& queue = krepis::default_reclamation_queue();

    double ti = best_of([&] {
        auto root = krepis::make_intrusive<INode>(99);
        auto start = Clock::now();
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&root, ops_per_thread] {
                for (int i = 0; i < ops_per_thread; ++i) {
                    auto copy = root;
                    (void)copy;
                }
            });
        }
        for (auto& w : workers) w.join();
        double elapsed = micros(start);
        root.reset();
        queue.wait_until_idle();
        return elapsed;
    });

    double ts = best_of([&] {
        auto root = std::make_shared<SNode>(99);
        auto start = Clock::now();
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&root, ops_per_thread] {
                for (int i = 0; i < ops_per_thread; ++i) {
                    auto copy = root;
                    (void)copy;
                }
            });
        }
        for (auto& w : workers) w.join();
        return micros(start);
    });

    char label[64];
    std::snprintf(label, sizeof(label), "%dt x %dM ops", threads, ops_per_thread / 1'000'000);
    print_result(label, ti, ts);
}

int main() {
    std::printf("=== Spike 4: IntrusivePtr vs std::shared_ptr benchmark ===\n");
    std::printf("LAY-0002 D17 gate 6\n\n");

    std::printf("[Memory footprint]\n");
    std::printf("  sizeof(INode)                 = %zu bytes\n", sizeof(INode));
    std::printf("  sizeof(SNode)                 = %zu bytes\n", sizeof(SNode));
    std::printf("  sizeof(IntrusivePtr<const INode>) = %zu bytes\n",
                sizeof(krepis::IntrusivePtr<const INode>));
    std::printf("  sizeof(shared_ptr<SNode>)      = %zu bytes\n",
                sizeof(std::shared_ptr<SNode>));

    std::printf("\n[Workload 1: retain/release loop]\n");
    bench_retain_release(1'000'000);
    bench_retain_release(10'000'000);

    std::printf("\n[Workload 2: COW edit (path copy)]\n");
    bench_cow_edit(8, 100'000);
    bench_cow_edit(16, 100'000);
    bench_cow_edit(32, 10'000);

    std::printf("\n[Workload 3: cross-thread handoff]\n");
    bench_cross_thread_handoff(100'000);
    bench_cross_thread_handoff(1'000'000);

    std::printf("\n[Workload 4: deep DAG reclaim]\n");
    bench_deep_dag_reclaim(1'000);
    bench_deep_dag_reclaim(10'000);
    bench_deep_dag_reclaim(100'000);

    std::printf("\n[Workload 5: concurrent retain/release]\n");
    bench_concurrent_retain_release(8, 1'000'000);
    bench_concurrent_retain_release(16, 1'000'000);

    krepis::shutdown_default_reclamation_queue();
    std::printf("\n=== done ===\n");
    return 0;
}
