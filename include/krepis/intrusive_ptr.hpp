#pragma once

// 依 LAY-0002 D17：immutable snapshot、B+ tree node、page-table node 與共享 page
// 使用封裝後的 intrusive atomic reference count。
//
// **業務程式碼不得直接操作計數值**（D17）。唯一的 owning edge 型別是 IntrusivePtr。
//
// 本檔於 2026-08-18 依閘門 7 人工審查意見修訂，主要變更：
//   - 生命週期違約（zero／poison／underflow／overflow）在**所有建置**立即終止，不再只是 assert。
//   - producer admission 與 state 檢查合併為單一 atomic word 的 CAS，消除誤殺窗口。
//   - retain／release 一律以 `RefCounted::` 限定呼叫，衍生型別無法以同名成員遮蔽。
//   - 回收鏈改為 const-correct，移除 const_cast。

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>

namespace krepis {

class ReclamationQueue;
template <typename T>
class IntrusivePtr;
template <typename T, typename... Args>
[[nodiscard]] IntrusivePtr<const T> make_intrusive(Args&&... args);

// 生命週期違約的統一處置。
//
// **在所有建置都生效**（閘門 7／A2）。以 assert 表達會使 Release 版本失去保護，
// 而引用計數的違約一旦發生就是 use-after-free 或 double free——
// 那類錯誤靜默發生時無法診斷，因此必須立即終止而非繼續執行。
[[noreturn]] inline void lifetime_violation(const char* what) noexcept {
    std::fprintf(stderr, "krepis 生命週期違約：%s\n", what);
    std::abort();
}

inline void require_lifetime(bool condition, const char* what) noexcept {
    if (!condition) [[unlikely]] {
        lifetime_violation(what);
    }
}

// 具備 intrusive reference count 的節點基底。
//
// 責任：保存計數，並在最後一次 release 時把銷毀責任交給 reclamation queue。
// 不負責：決定何時被 retain —— 只能從既有 owning reference 複製（D17 禁止由
//         未受保護的 raw pointer retain，因此沒有 weak、resurrection 或 pointer promotion）。
// 維持的不變條件：計數自 1 起算；歸零後不得再被 retain。
// 擁有哪些資源：自身；子節點的 owning edge 由衍生型別以 IntrusivePtr 持有。
// 生命週期：最後一次 release 之後，原執行緒不得再存取本物件。
// 錯誤語意：underflow、overflow 與 double release **在所有建置立即終止**。
// 執行緒安全程度：retain／release 可由多執行緒併發呼叫；**內容發布後必須不可變**。
// 可否複製／移動：不可複製、不可移動 —— 計數與位址綁定。
class RefCounted {
public:
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    // 僅供診斷與測試。**不得用於控制流程** —— 讀到的值在回傳當下即可能過期。
    [[nodiscard]] std::uint32_t use_count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

protected:
    RefCounted() noexcept = default;
    virtual ~RefCounted() = default;

private:
    friend class ReclamationQueue;
    template <typename>
    friend class IntrusivePtr;

    void retain() const noexcept {
        const std::uint32_t previous = count_.fetch_add(1, std::memory_order_relaxed);
        // 從已歸零或已毒化的物件 retain，代表 ownership 封裝已被破壞。
        require_lifetime(previous != 0, "從計數為零的節點 retain");
        require_lifetime(previous != poisoned, "從已釋放（毒化）的節點 retain");
        require_lifetime(previous < poisoned - 1, "reference count 溢位");
    }

    void release() const noexcept;

    // 歸零後寫入的毒化值，使 double release 與 use-after-free 被攔截。
    //
    // **這是診斷輔助，不是正確性保證。** 非法 retain 有可能在 poison store
    // 進入 modification order **之前**就完成遞增，因而讀到舊值而未被攔截。
    // 真正的保證來自封裝：只能從既有 owning reference 複製（見 IntrusivePtr）。
    static constexpr std::uint32_t poisoned = 0xDEAD0000u;

    mutable std::atomic<std::uint32_t> count_{1};
    // 供 reclamation queue 串接，使最後一次 release 的路徑不需配置記憶體。
    // 型別為 const pointer：節點發布後不可變，回收鏈也不應成為破壞 const 的藉口（閘門 7／B1）。
    mutable const RefCounted* reclaim_next_ = nullptr;
};

// 待回收節點的佇列。
//
// 責任：接收最後一次 release 交出的節點，並由背景 worker 分批執行銷毀。
// 不負責：由呼叫者銷毀節點 —— destructor 只能由內部背景 worker 執行。
// 維持的不變條件：enqueue 不配置記憶體、不拋例外；每個節點恰好被銷毀一次。
// 生命週期：process-wide 單例，必須以 shutdown_default_reclamation_queue() 明確停止。
// 執行緒安全程度：enqueue 可由任意執行緒併發呼叫；只有內部 worker 能 drain。
//
// **單 worker 契約**：worker 喚醒、finalization、shutdown 與 join 都假設只有一個 worker。
// 若將來增加 worker，必須重開閘門 7 審查（閘門 7／B2）。
class ReclamationQueue {
public:
    ReclamationQueue(const ReclamationQueue&) = delete;
    ReclamationQueue& operator=(const ReclamationQueue&) = delete;

    // 等待背景 worker 銷毀目前所有 outstanding node；不停止 worker。
    void wait_until_idle() noexcept;

    [[nodiscard]] std::size_t pending() const noexcept {
        return pending_.load(std::memory_order_relaxed);
    }

    // 自建立以來累計銷毀的節點數。供「配置數等於釋放數」的關機證明使用。
    [[nodiscard]] std::size_t total_reclaimed() const noexcept {
        return total_reclaimed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_shutdown() const noexcept;

private:
    friend class RefCounted;
    friend ReclamationQueue& default_reclamation_queue() noexcept;
    friend void shutdown_default_reclamation_queue() noexcept;

    enum class State : std::uint8_t {
        running,
        stopping,
        finalizing,
        stopped,
    };

    // --- Admission gate（閘門 7／疑點 1）---
    //
    // state 與 in-flight producer 數**放在同一個 atomic word**。
    // 原本兩者是分開的原子操作，producer 可能在「已登記」之後才讀到 finalizing
    // 而被誤殺——它並沒有違約。合併之後，「檢查 state」與「登記 producer」
    // 是單一 CAS，producer admission 有明確的線性化點。
    //
    // 佈局：高 8 bit 為 State，低 56 bit 為 producer 計數。
    using GateWord = std::uint64_t;
    static constexpr unsigned gate_state_shift = 56;
    static constexpr GateWord gate_count_mask = (GateWord{1} << gate_state_shift) - 1;

    [[nodiscard]] static constexpr State gate_state(GateWord word) noexcept {
        return static_cast<State>(word >> gate_state_shift);
    }
    [[nodiscard]] static constexpr GateWord gate_count(GateWord word) noexcept {
        return word & gate_count_mask;
    }
    [[nodiscard]] static constexpr GateWord make_gate(State state, GateWord count) noexcept {
        return (static_cast<GateWord>(state) << gate_state_shift) | (count & gate_count_mask);
    }

    ReclamationQueue();
    ~ReclamationQueue();

    // 由最後一次 release 呼叫。以單一 CAS 完成 admission，再以 Treiber stack 發布 node。
    void enqueue(const RefCounted* node) noexcept;
    [[nodiscard]] std::size_t drain_once() noexcept;
    void worker_main() noexcept;
    void shutdown() noexcept;

    // worker 是否應該停止等待（喚醒條件）。**讀 generation 前後都必須用同一個 predicate**，
    // 否則會與 join() 互等（閘門 7／C2）。
    [[nodiscard]] bool worker_should_wake() const noexcept;

    std::atomic<const RefCounted*> head_{nullptr};
    std::atomic<std::size_t> pending_{0};
    std::atomic<std::size_t> total_reclaimed_{0};
    std::atomic<GateWord> gate_{make_gate(State::running, 0)};
    std::atomic<std::uint64_t> worker_generation_{0};
    std::atomic<std::uint64_t> idle_generation_{0};
    // shutdown 是併發冪等的冷路徑：第一個進入者執行完整流程，其餘等待同一次結果。
    std::atomic<bool> shutdown_claimed_{false};
    std::atomic<bool> shutdown_complete_{false};
    std::thread worker_;
};

// Process-wide 的預設佇列。物件刻意存活到 process 結束，避免 static destruction order 問題；
// runtime 必須在所有外部 owner 釋放後明確 shutdown。
[[nodiscard]] ReclamationQueue& default_reclamation_queue() noexcept;
void shutdown_default_reclamation_queue() noexcept;

// 唯一合法的 owning edge。
//
// 責任：以 RAII 維護 retain／release 配對。
// 不負責：處理循環 —— owning edge 只向下形成 DAG（D17），本型別不偵測循環。
// 維持的不變條件：非空時恰好持有一個計數。
// 錯誤語意：不失敗；違約由 RefCounted 的 fail-fast 攔截。
// 執行緒安全程度：同一個 IntrusivePtr 物件不可由多執行緒同時修改；
//                 不同 IntrusivePtr 指向同一節點可併發使用。
template <typename T>
class IntrusivePtr {
public:
    // 只在此處檢查 const —— 不檢查基底型別。
    // IntrusivePtr<const X> 必須能在 X 尚未完整定義時使用（例如 X 的成員自我引用），
    // 而 is_base_of 要求完整型別。基底檢查改在 retain_pointer／release_pointer，
    // 那裡本來就需要完整型別。
    static_assert(std::is_const_v<T>, "owning edge 必須使用 IntrusivePtr<const T>");

    constexpr IntrusivePtr() noexcept = default;
    constexpr IntrusivePtr(std::nullptr_t) noexcept {}

    IntrusivePtr(const IntrusivePtr& other) noexcept : pointer_(other.pointer_) {
        retain_pointer(pointer_);
    }

    IntrusivePtr(IntrusivePtr&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    // 跨型別轉換（複製）：衍生 → 基底。只在可隱式轉換時成立。
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    IntrusivePtr(const IntrusivePtr<U>& other) noexcept : pointer_(other.get()) {
        retain_pointer(pointer_);
    }

    // 跨型別轉換（搬移）：竊取計數，不做 retain／release。
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    IntrusivePtr(IntrusivePtr<U>&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    IntrusivePtr& operator=(const IntrusivePtr& other) noexcept {
        // 先 retain 再 release，使 self-assignment 與「other 由 this 保活」都安全。
        T* incoming = other.pointer_;
        retain_pointer(incoming);
        T* outgoing = pointer_;
        pointer_ = incoming;
        release_pointer(outgoing);
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept {
        if (this != &other) {
            T* outgoing = pointer_;
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
            release_pointer(outgoing);
        }
        return *this;
    }

    ~IntrusivePtr() { release_pointer(pointer_); }

    [[nodiscard]] T* get() const noexcept { return pointer_; }
    [[nodiscard]] T& operator*() const noexcept {
        assert(pointer_ != nullptr && "解參照空的 IntrusivePtr");
        return *pointer_;
    }
    [[nodiscard]] T* operator->() const noexcept {
        assert(pointer_ != nullptr && "解參照空的 IntrusivePtr");
        return pointer_;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return pointer_ != nullptr; }

    void reset() noexcept {
        T* outgoing = pointer_;
        pointer_ = nullptr;
        release_pointer(outgoing);
    }

    friend bool operator==(const IntrusivePtr& a, const IntrusivePtr& b) noexcept {
        return a.pointer_ == b.pointer_;
    }

private:
    template <typename>
    friend class IntrusivePtr;

    template <typename U, typename... Args>
    friend IntrusivePtr<const U> make_intrusive(Args&&... args);

    // **一律以 `RefCounted::` 限定呼叫**（閘門 7／A1）。
    // 若寫成 `p->retain()`，衍生型別宣告同名 public 成員時會以 name hiding 遮蔽，
    // 使計數走到錯誤的實作而不產生任何診斷。
    static void retain_pointer(T* pointer) noexcept {
        static_assert(std::is_base_of_v<RefCounted, std::remove_const_t<T>>,
                      "IntrusivePtr 只能指向 RefCounted 衍生型別");
        if (pointer != nullptr) {
            static_cast<const RefCounted*>(pointer)->RefCounted::retain();
        }
    }

    static void release_pointer(T* pointer) noexcept {
        static_assert(std::is_base_of_v<RefCounted, std::remove_const_t<T>>,
                      "IntrusivePtr 只能指向 RefCounted 衍生型別");
        if (pointer != nullptr) {
            static_cast<const RefCounted*>(pointer)->RefCounted::release();
        }
    }

    struct AdoptInitialReference {};

    explicit IntrusivePtr(T* raw, AdoptInitialReference) noexcept : pointer_(raw) {}

    T* pointer_ = nullptr;
};

// 唯一公開的初始 owner 建立路徑：建構完整 immutable node，並只發布 const owning pointer。
template <typename T, typename... Args>
[[nodiscard]] IntrusivePtr<const T> make_intrusive(Args&&... args) {
    static_assert(!std::is_const_v<T>, "factory 的 T 不需包含 const");
    static_assert(std::is_base_of_v<RefCounted, T>, "factory 只能建立 RefCounted 衍生型別");

    using Pointer = IntrusivePtr<const T>;
    return Pointer(new T(std::forward<Args>(args)...), typename Pointer::AdoptInitialReference{});
}

}  // namespace krepis
