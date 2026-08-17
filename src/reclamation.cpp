#include "krepis/intrusive_ptr.hpp"

namespace krepis {

ReclamationQueue::ReclamationQueue() : worker_([this] { worker_main(); }) {}

ReclamationQueue::~ReclamationQueue() {
    shutdown();
}

void RefCounted::release() const noexcept {
    const std::uint32_t previous = count_.fetch_sub(1, std::memory_order_release);
    assert(previous != 0 && previous != poisoned && "對已釋放的節點再次 release");

    if (previous != 1) {
        return;
    }

    // 此時本執行緒是唯一擁有者。acquire fence 使先前所有執行緒對本物件的寫入
    // 在銷毀前對本執行緒可見（D17 的 release／acquire 配對）。
    std::atomic_thread_fence(std::memory_order_acquire);

    // 毒化計數，使任何殘留的 raw pointer 在 retain／release 時立即被 assert 攔截。
    count_.store(poisoned, std::memory_order_relaxed);

    // 不在此處同步遞迴銷毀大型 subtree（D17）；把責任交給 reclamation queue。
    default_reclamation_queue().enqueue(this);
}

void ReclamationQueue::enqueue(const RefCounted* node) noexcept {
    // 先登記正在 enqueue 的 producer，避免 shutdown 在 node 尚未發布時誤判為 idle。
    active_enqueuers_.fetch_add(1, std::memory_order_acq_rel);
    const State state = state_.load(std::memory_order_acquire);
    if (state == State::finalizing || state == State::stopped) {
        active_enqueuers_.fetch_sub(1, std::memory_order_release);
        active_enqueuers_.notify_all();
        std::terminate();
    }

    // Outstanding 必須先增加；consumer 不可先銷毀 node，再從零扣除 pending。
    pending_.fetch_add(1, std::memory_order_relaxed);

    // Treiber stack：不配置記憶體，串接指標就在節點本身。
    RefCounted* mutable_node = const_cast<RefCounted*>(node);
    RefCounted* current_head = head_.load(std::memory_order_relaxed);
    do {
        mutable_node->reclaim_next_ = current_head;
    } while (!head_.compare_exchange_weak(current_head, mutable_node, std::memory_order_release,
                                           std::memory_order_relaxed));

    active_enqueuers_.fetch_sub(1, std::memory_order_release);
    active_enqueuers_.notify_all();
    worker_generation_.fetch_add(1, std::memory_order_release);
    worker_generation_.notify_one();
}

std::size_t ReclamationQueue::drain_once() noexcept {
    // 一次取走整條串列，之後的銷毀不再與 enqueue 競爭。
    RefCounted* node = head_.exchange(nullptr, std::memory_order_acquire);

    std::size_t destroyed = 0;
    while (node != nullptr) {
        RefCounted* next = node->reclaim_next_;
        // 銷毀可能使子節點的 IntrusivePtr 釋放，進而 enqueue 更多節點；
        // 那些會在下一次 drain 處理，因此本迴圈不會無界遞迴。
        delete node;
        node = next;
        ++destroyed;
    }

    if (destroyed != 0) {
        const std::size_t previous = pending_.fetch_sub(destroyed, std::memory_order_acq_rel);
        assert(previous >= destroyed && "reclamation pending count 下溢");
        (void)previous;
        total_reclaimed_.fetch_add(destroyed, std::memory_order_relaxed);
    }
    return destroyed;
}

void ReclamationQueue::worker_main() noexcept {
    for (;;) {
        // Atomic generation 避免 condition variable 在 producer 不持鎖時遺失喚醒。
        while (head_.load(std::memory_order_acquire) == nullptr) {
            const bool shutdown_idle = state_.load(std::memory_order_acquire) == State::stopping &&
                                       active_enqueuers_.load(std::memory_order_acquire) == 0 &&
                                       pending_.load(std::memory_order_acquire) == 0;
            if (shutdown_idle) break;

            const std::uint64_t observed = worker_generation_.load(std::memory_order_acquire);
            if (head_.load(std::memory_order_acquire) == nullptr) {
                worker_generation_.wait(observed, std::memory_order_acquire);
            }
        }

        // Parent destructor 可能 enqueue child；持續取批次，直到本輪真正清空。
        while (drain_once() != 0) {
        }

        const bool idle = head_.load(std::memory_order_acquire) == nullptr &&
                          active_enqueuers_.load(std::memory_order_acquire) == 0 &&
                          pending_.load(std::memory_order_acquire) == 0;
        if (idle) {
            idle_generation_.fetch_add(1, std::memory_order_release);
            idle_generation_.notify_all();
        }

        if (idle && state_.load(std::memory_order_acquire) == State::stopping) {
            State expected = State::stopping;
            if (state_.compare_exchange_strong(expected, State::finalizing, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                // 阻擋新 producer，並等待已越過入口閘門的 producer 完成發布。
                std::size_t active = active_enqueuers_.load(std::memory_order_acquire);
                while (active != 0) {
                    active_enqueuers_.wait(active, std::memory_order_acquire);
                    active = active_enqueuers_.load(std::memory_order_acquire);
                }

                const bool finalized = head_.load(std::memory_order_acquire) == nullptr &&
                                       pending_.load(std::memory_order_acquire) == 0;
                if (finalized) {
                    state_.store(State::stopped, std::memory_order_release);
                    idle_generation_.fetch_add(1, std::memory_order_release);
                    idle_generation_.notify_all();
                    return;
                }

                // 舊 producer 已發布 node；重新開放 child enqueue，讓 worker 完成最後批次。
                state_.store(State::stopping, std::memory_order_release);
            }
        }
    }
}

void ReclamationQueue::wait_until_idle() noexcept {
    for (;;) {
        const bool idle = head_.load(std::memory_order_acquire) == nullptr &&
                          active_enqueuers_.load(std::memory_order_acquire) == 0 &&
                          pending_.load(std::memory_order_acquire) == 0;
        if (idle) return;

        const std::uint64_t observed = idle_generation_.load(std::memory_order_acquire);
        const bool still_busy = head_.load(std::memory_order_acquire) != nullptr ||
                                active_enqueuers_.load(std::memory_order_acquire) != 0 ||
                                pending_.load(std::memory_order_acquire) != 0;
        if (still_busy) {
            idle_generation_.wait(observed, std::memory_order_acquire);
        }
    }
}

void ReclamationQueue::shutdown() noexcept {
    State expected = State::running;
    if (!state_.compare_exchange_strong(expected, State::stopping, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
        if (expected == State::stopped) return;

        // Shutdown 是 runtime 的單一執行緒操作；同時呼叫代表生命週期契約被破壞。
        std::terminate();
    }

    worker_generation_.fetch_add(1, std::memory_order_release);
    worker_generation_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }

    assert(state_.load(std::memory_order_acquire) == State::stopped && "worker 未完成 shutdown");
    assert(head_.load(std::memory_order_acquire) == nullptr && "shutdown 後仍有 queue node");
    assert(active_enqueuers_.load(std::memory_order_acquire) == 0 && "shutdown 後仍有 producer");
    assert(pending_.load(std::memory_order_acquire) == 0 && "shutdown 後仍有 outstanding node");
}

bool ReclamationQueue::is_shutdown() const noexcept {
    return state_.load(std::memory_order_acquire) == State::stopped;
}

ReclamationQueue& default_reclamation_queue() noexcept {
    // 刻意讓 queue 物件存活到 process 結束，避免其他 static owner 較晚解構時存取已銷毀的 queue。
    static ReclamationQueue* queue = new ReclamationQueue();
    return *queue;
}

void shutdown_default_reclamation_queue() noexcept {
    default_reclamation_queue().shutdown();
}

}  // namespace krepis
