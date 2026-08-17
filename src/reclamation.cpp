#include "krepis/intrusive_ptr.hpp"

namespace krepis {

ReclamationQueue::ReclamationQueue() : worker_([this] { worker_main(); }) {}

ReclamationQueue::~ReclamationQueue() {
    // 預設 singleton 刻意不解構（見 default_reclamation_queue）；
    // 此 destructor 是為**非 singleton 擁有情境**保留的防禦性生命週期保證，不是死碼。
    shutdown();
}

void RefCounted::release() const noexcept {
    const std::uint32_t previous = count_.fetch_sub(1, std::memory_order_release);
    require_lifetime(previous != 0, "對計數為零的節點 release（underflow）");
    require_lifetime(previous != poisoned, "對已釋放（毒化）的節點再次 release");

    if (previous != 1) {
        return;
    }

    // 此時本執行緒是唯一擁有者。acquire fence 使先前所有執行緒對本物件的寫入
    // 在銷毀前對本執行緒可見（D17 的 release／acquire 配對）。
    std::atomic_thread_fence(std::memory_order_acquire);

    // 毒化計數。**這是診斷輔助而非正確性保證**：非法 retain 仍可能在本 store
    // 進入 modification order 之前完成遞增。真正的保證來自 IntrusivePtr 的封裝。
    count_.store(poisoned, std::memory_order_relaxed);

    // 不在此處同步遞迴銷毀大型 subtree（D17）；把責任交給 reclamation queue。
    default_reclamation_queue().enqueue(this);
}

void ReclamationQueue::enqueue(const RefCounted* node) noexcept {
    // Admission：以單一 CAS 同時檢查 state 與登記 producer（閘門 7／疑點 1）。
    //
    // 兩者合併之後，不可能出現「已登記卻讀到 finalizing 而被誤殺」的窗口：
    // 若 CAS 成功，代表當下 state 允許 enqueue 且本 producer 已計入；
    // 若 worker 同時要轉入 finalizing，它的 CAS 會因計數已改變而失敗。
    GateWord current = gate_.load(std::memory_order_acquire);
    for (;;) {
        const State state = gate_state(current);
        if (state == State::finalizing || state == State::stopped) {
            // 晚到的 enqueue 是生命週期違約，不得靜默洩漏（D17）。
            lifetime_violation("reclamation queue 已關閉後仍有 enqueue");
        }
        const GateWord desired = make_gate(state, gate_count(current) + 1);
        if (gate_.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            break;
        }
    }

    // Outstanding 必須先增加；consumer 不可先銷毀 node，再從零扣除 pending。
    pending_.fetch_add(1, std::memory_order_relaxed);

    // Treiber stack：不配置記憶體，串接指標就在節點本身。
    // 節點尚未進入串列前沒有其他執行緒可讀，因此 CAS 失敗時重寫 reclaim_next_ 是安全的。
    const RefCounted* current_head = head_.load(std::memory_order_relaxed);
    do {
        node->reclaim_next_ = current_head;
    } while (!head_.compare_exchange_weak(current_head, node, std::memory_order_release,
                                          std::memory_order_relaxed));

    // 離場：只遞減計數，state 不變。計數必定 >= 1，不會借位到 state 欄位。
    gate_.fetch_sub(1, std::memory_order_release);
    gate_.notify_all();

    worker_generation_.fetch_add(1, std::memory_order_release);
    worker_generation_.notify_one();
}

std::size_t ReclamationQueue::drain_once() noexcept {
    // 一次取走整條串列，之後的銷毀不再與 enqueue 競爭。
    // **不是 Treiber pop**（沒有「讀 next 再 CAS head」），因此結構上沒有 ABA。
    const RefCounted* node = head_.exchange(nullptr, std::memory_order_acquire);

    std::size_t destroyed = 0;
    while (node != nullptr) {
        const RefCounted* next = node->reclaim_next_;
        // 銷毀可能使子節點的 IntrusivePtr 釋放，進而 enqueue 更多節點；
        // 那些會在下一次 drain 處理，因此本迴圈不會無界遞迴。
        delete node;
        node = next;
        ++destroyed;
    }

    if (destroyed != 0) {
        const std::size_t previous = pending_.fetch_sub(destroyed, std::memory_order_acq_rel);
        require_lifetime(previous >= destroyed, "reclamation pending count 下溢");
        total_reclaimed_.fetch_add(destroyed, std::memory_order_relaxed);
    }
    return destroyed;
}

bool ReclamationQueue::worker_should_wake() const noexcept {
    if (head_.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    // 停止要求也是喚醒理由。**漏掉這一項會與 join() 永久互等**（閘門 7／C2）。
    const GateWord gate = gate_.load(std::memory_order_acquire);
    const State state = gate_state(gate);
    return state != State::running;
}

void ReclamationQueue::worker_main() noexcept {
    for (;;) {
        // Atomic generation 避免在 producer 不持鎖時遺失喚醒。
        //
        // **關鍵**：讀 generation 之後必須重查**完整**的喚醒條件，不能只重查 head_。
        // 若只查 head_，shutdown 在讀 generation 之前送出停止要求並 bump generation 時，
        // worker 會以新 generation 進入 wait 而永遠不被喚醒，與 join() 互等。
        while (!worker_should_wake()) {
            const std::uint64_t observed = worker_generation_.load(std::memory_order_acquire);
            if (!worker_should_wake()) {
                worker_generation_.wait(observed, std::memory_order_acquire);
            }
        }

        // Parent destructor 可能 enqueue child；持續取批次，直到本輪真正清空。
        while (drain_once() != 0) {
        }

        const GateWord gate = gate_.load(std::memory_order_acquire);
        const bool quiet = head_.load(std::memory_order_acquire) == nullptr &&
                           gate_count(gate) == 0 &&
                           pending_.load(std::memory_order_acquire) == 0;

        if (quiet) {
            idle_generation_.fetch_add(1, std::memory_order_release);
            idle_generation_.notify_all();
        }

        if (quiet && gate_state(gate) == State::stopping) {
            // 轉入 finalizing 必須連同「沒有 in-flight producer」一起原子成立。
            // CAS 的 expected 是 (stopping, 0)：若期間有 producer 入場，計數改變、CAS 失敗，
            // 於是重跑迴圈——不會關在半路。
            GateWord expected = make_gate(State::stopping, 0);
            if (gate_.compare_exchange_strong(expected, make_gate(State::finalizing, 0),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                // 此刻起新 producer 一律 terminate，且沒有 producer 在途中。
                const bool finalized = head_.load(std::memory_order_acquire) == nullptr &&
                                       pending_.load(std::memory_order_acquire) == 0;
                if (finalized) {
                    gate_.store(make_gate(State::stopped, 0), std::memory_order_release);
                    gate_.notify_all();
                    idle_generation_.fetch_add(1, std::memory_order_release);
                    idle_generation_.notify_all();
                    return;
                }

                // 舊 producer 已發布 node；重新開放 child enqueue，讓 worker 完成最後批次。
                // 終止性：外部 producer 已停止，剩下的只有 destructor 產生的 child，
                // 而 owning edge 形成有限深度的 DAG，因此退回次數有界。
                gate_.store(make_gate(State::stopping, 0), std::memory_order_release);
                gate_.notify_all();
            }
        }
    }
}

void ReclamationQueue::wait_until_idle() noexcept {
    for (;;) {
        const GateWord gate = gate_.load(std::memory_order_acquire);
        const bool idle = head_.load(std::memory_order_acquire) == nullptr &&
                          gate_count(gate) == 0 &&
                          pending_.load(std::memory_order_acquire) == 0;
        if (idle) return;

        const std::uint64_t observed = idle_generation_.load(std::memory_order_acquire);

        const GateWord recheck = gate_.load(std::memory_order_acquire);
        const bool still_busy = head_.load(std::memory_order_acquire) != nullptr ||
                                gate_count(recheck) != 0 ||
                                pending_.load(std::memory_order_acquire) != 0;
        if (still_busy) {
            idle_generation_.wait(observed, std::memory_order_acquire);
        }
    }
}

void ReclamationQueue::shutdown() noexcept {
    // Shutdown 是**併發冪等的冷路徑**（閘門 7／C3）：
    // 第一個進入者執行完整的停止、drain 與 join；其餘呼叫者等待同一次完整結果。
    //
    // 不可只看 state == stopped 就返回——worker 可能已自行設成 stopped，
    // 但首位呼叫者尚未 join()，此時提早返回會讓呼叫者以為 worker 已結束。
    bool expected_claim = false;
    if (!shutdown_claimed_.compare_exchange_strong(expected_claim, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
        // 登記「已進入等待路徑」**在檢查完成旗標之前**——
        // 順序相反的話，計數只能證明「即將等待」，建立不起同步邊（閘門 7／C3 第三輪）。
        shutdown_waiters_.fetch_add(1, std::memory_order_acq_rel);
        shutdown_waiters_.notify_all();

        while (!shutdown_complete_.load(std::memory_order_acquire)) {
            shutdown_complete_.wait(false, std::memory_order_acquire);
        }

        shutdown_waiters_.fetch_sub(1, std::memory_order_acq_rel);
        shutdown_waiters_.notify_all();
        return;
    }

    // 送出停止要求：只改 state 欄位，保留當下的 producer 計數。
    GateWord current = gate_.load(std::memory_order_acquire);
    for (;;) {
        const State state = gate_state(current);
        if (state != State::running) {
            break;  // worker 已在停止流程中
        }
        const GateWord desired = make_gate(State::stopping, gate_count(current));
        if (gate_.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            break;
        }
    }

    worker_generation_.fetch_add(1, std::memory_order_release);
    worker_generation_.notify_one();

    if (worker_.joinable()) {
        worker_.join();
    }

    // 關機後置條件：**在所有建置執行**（閘門 7／C3）。
    // 「配置數等於回收數」則由 Release CI 的 accounting test 提供證據。
    const GateWord final_gate = gate_.load(std::memory_order_acquire);
    require_lifetime(gate_state(final_gate) == State::stopped, "worker 未完成 shutdown");
    require_lifetime(gate_count(final_gate) == 0, "shutdown 後仍有 in-flight producer");
    require_lifetime(head_.load(std::memory_order_acquire) == nullptr, "shutdown 後仍有 queue node");
    require_lifetime(pending_.load(std::memory_order_acquire) == 0,
                     "shutdown 後仍有 outstanding node");

    shutdown_complete_.store(true, std::memory_order_release);
    shutdown_complete_.notify_all();
}

bool ReclamationQueue::is_shutdown() const noexcept {
    return gate_state(gate_.load(std::memory_order_acquire)) == State::stopped;
}

ReclamationQueue& default_reclamation_queue() noexcept {
    // 刻意讓 queue 物件存活到 process 結束，避免其他 static owner 較晚解構時存取已銷毀的 queue。
    // 因此本 singleton 的 destructor **永不執行**——這是設計，不是洩漏。
    static ReclamationQueue* queue = new ReclamationQueue();
    return *queue;
}

void shutdown_default_reclamation_queue() noexcept {
    default_reclamation_queue().shutdown();
}

}  // namespace krepis
