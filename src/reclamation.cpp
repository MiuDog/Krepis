#include "krepis/intrusive_ptr.hpp"

namespace krepis {

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
    // Treiber stack：不配置記憶體，串接指標就在節點本身。
    RefCounted* mutable_node = const_cast<RefCounted*>(node);
    RefCounted* current_head = head_.load(std::memory_order_relaxed);
    do {
        mutable_node->reclaim_next_ = current_head;
    } while (!head_.compare_exchange_weak(current_head, mutable_node, std::memory_order_release,
                                          std::memory_order_relaxed));
    pending_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t ReclamationQueue::drain() noexcept {
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
        pending_.fetch_sub(destroyed, std::memory_order_relaxed);
        total_reclaimed_.fetch_add(destroyed, std::memory_order_relaxed);
    }
    return destroyed;
}

ReclamationQueue& default_reclamation_queue() noexcept {
    static ReclamationQueue queue;
    return queue;
}

}  // namespace krepis
