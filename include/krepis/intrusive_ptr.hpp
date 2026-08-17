#pragma once

// 依 LAY-0002 D17：immutable snapshot、B+ tree node、page-table node 與共享 page
// 使用封裝後的 intrusive atomic reference count。
//
// **業務程式碼不得直接操作計數值**（D17）。唯一的 owning edge 型別是 IntrusivePtr。

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
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

// 具備 intrusive reference count 的節點基底。
//
// 責任：保存計數，並在最後一次 release 時把銷毀責任交給 reclamation queue。
// 不負責：決定何時被 retain —— 只能從既有 owning reference 複製（D17 禁止由
//         未受保護的 raw pointer retain，因此沒有 weak、resurrection 或 pointer promotion）。
// 維持的不變條件：計數自 1 起算；歸零後不得再被 retain。
// 擁有哪些資源：自身；子節點的 owning edge 由衍生型別以 IntrusivePtr 持有。
// 生命週期：最後一次 release 之後，原執行緒不得再存取本物件。
// 錯誤語意：underflow、overflow 與 double release 在有 assert 的建置立即失敗。
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
        assert(previous != 0 && previous != poisoned && "從已釋放的節點 retain");
        assert(previous < poisoned - 1 && "reference count 溢位");
        (void)previous;
    }

    void release() const noexcept;

    // 歸零後寫入的毒化值，使 double release 與 use-after-free 立即被 assert 攔截。
    static constexpr std::uint32_t poisoned = 0xDEAD0000u;

    mutable std::atomic<std::uint32_t> count_{1};
    // 供 reclamation queue 串接，使最後一次 release 的路徑不需配置記憶體。
    mutable RefCounted* reclaim_next_ = nullptr;
};

// 待回收節點的佇列。
//
// 責任：接收最後一次 release 交出的節點，並由背景 worker 分批執行銷毀。
// 不負責：由呼叫者銷毀節點 —— destructor 只能由內部背景 worker 執行。
// 維持的不變條件：enqueue 不配置記憶體、不拋例外；每個節點恰好被銷毀一次。
// 生命週期：process-wide 單例，必須以 shutdown_default_reclamation_queue() 明確停止。
// 執行緒安全程度：enqueue 可由任意執行緒併發呼叫；只有內部 worker 能 drain。
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

    ReclamationQueue();
    ~ReclamationQueue();

    // 由最後一次 release 呼叫。先登記 outstanding，再以 Treiber stack 發布 node。
    void enqueue(const RefCounted* node) noexcept;
    [[nodiscard]] std::size_t drain_once() noexcept;
    void worker_main() noexcept;
    void shutdown() noexcept;

    std::atomic<RefCounted*> head_{nullptr};
    std::atomic<std::size_t> pending_{0};
    std::atomic<std::size_t> total_reclaimed_{0};
    std::atomic<std::size_t> active_enqueuers_{0};
    std::atomic<State> state_{State::running};
    std::atomic<std::uint64_t> worker_generation_{0};
    std::atomic<std::uint64_t> idle_generation_{0};
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
// 錯誤語意：不失敗。
// 執行緒安全程度：同一個 IntrusivePtr 物件不可由多執行緒同時修改；
//                 不同 IntrusivePtr 指向同一節點可併發使用。
template <typename T>
class IntrusivePtr {
public:
    static_assert(std::is_const_v<T>, "owning edge 必須使用 IntrusivePtr<const T>");

    constexpr IntrusivePtr() noexcept = default;
    constexpr IntrusivePtr(std::nullptr_t) noexcept {}

    IntrusivePtr(const IntrusivePtr& other) noexcept : pointer_(other.pointer_) {
        if (pointer_ != nullptr) {
            pointer_->retain();
        }
    }

    IntrusivePtr(IntrusivePtr&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    // 跨型別轉換（複製）：衍生 → 基底。只在可隱式轉換時成立。
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    IntrusivePtr(const IntrusivePtr<U>& other) noexcept : pointer_(other.get()) {
        if (pointer_ != nullptr) {
            pointer_->retain();
        }
    }

    // 跨型別轉換（搬移）：竊取計數，不做 retain／release。
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    IntrusivePtr(IntrusivePtr<U>&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }

    IntrusivePtr& operator=(const IntrusivePtr& other) noexcept {
        // 先 retain 再 release，使 self-assignment 與「other 由 this 保活」都安全。
        T* incoming = other.pointer_;
        if (incoming != nullptr) {
            incoming->retain();
        }
        T* outgoing = pointer_;
        pointer_ = incoming;
        if (outgoing != nullptr) {
            outgoing->release();
        }
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept {
        if (this != &other) {
            T* outgoing = pointer_;
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
            if (outgoing != nullptr) {
                outgoing->release();
            }
        }
        return *this;
    }

    ~IntrusivePtr() {
        if (pointer_ != nullptr) {
            pointer_->release();
        }
    }

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
        if (outgoing != nullptr) {
            outgoing->release();
        }
    }

    friend bool operator==(const IntrusivePtr& a, const IntrusivePtr& b) noexcept {
        return a.pointer_ == b.pointer_;
    }

private:
    template <typename>
    friend class IntrusivePtr;

    template <typename U, typename... Args>
    friend IntrusivePtr<const U> make_intrusive(Args&&... args);

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
