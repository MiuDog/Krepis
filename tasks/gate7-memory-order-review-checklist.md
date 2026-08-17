# D17 閘門 7：memory order 人工審查清單

## 這份文件是什麼

LAY-0002 D17 閘門 7 要求「人工逐行審查所有 memory order、owning edge、borrowed pointer
lifetime 與 shutdown drain path」。**這道閘門不接受 AI 自審**——寫程式碼的人不能是簽核的人，
否則同一個錯誤理解會同時出現在實作與審查裡，而且不會有任何徵兆。

因此本文件的形狀是：**AI 提出主張，你負責駁倒或接受。** 每一項都有

- **要審什麼**：具體的程式碼位置
- **我的主張**：為什麼我認為它是對的
- **你要回答的問題**：能真正推翻我的那個問題
- **判定**：由你填

判定欄只有三種值：`接受` / `要求修改` / `待查`。**全部為「接受」之前，閘門 7 不算通過。**

審查範圍：

- `src/reclamation.cpp`（全部）
- `include/krepis/intrusive_ptr.hpp`（`RefCounted`、`IntrusivePtr`）
- `include/krepis/flow_sequence.hpp` 的 `TreeCursor`（borrowed pointer）

---

## 第一輪審查結果與修正（2026-08-18）

第一輪判定：**未通過**。8 項要求修改、4 項接受。全部要求修改已實作，**等待第二輪重審**。

依本檔規則「修完後重審該項，不是直接改判定」，下列項目的判定欄**維持「要求修改」**，
另註明「已實作，待重審」。重審者請對照實作確認，通過後才改判定。

| 項目 | 要求 | 實作摘要 |
|---|---|---|
| 疑點 1 | packed admission gate | `state` 與 producer 計數合併為單一 `GateWord`（高 8 bit 為 state、低 56 bit 為計數），enqueue 以單一 CAS 完成 admission；worker 轉入 `finalizing` 的 CAS 以 `(stopping, 0)` 為 expected，producer 入場會使其失敗 |
| A1 | 防止衍生型別遮蔽計數 | `IntrusivePtr` 改以 `RefCounted::retain()`／`RefCounted::release()` 限定呼叫；新增 `HostileNode` 測試與 raw constructibility 的 `static_assert`；更正 acquire 的說明 |
| A2 | 違約在所有建置終止 | 新增 `require_lifetime()`／`lifetime_violation()`，zero／poison／underflow／overflow 一律 `abort`；更正 poison 說明為「非法 retain 可能在 poison store 進入 modification order 前完成遞增」 |
| B1 | 回收鏈 const-correct | `reclaim_next_` 與 `head_` 改為 `const RefCounted*`，移除 `const_cast` |
| C2 | 喚醒死鎖 | 新增 `worker_should_wake()`，讀 generation **前後都用同一個完整 predicate**；原本讀後只重查 `head_`，會與 `join()` 永久互等 |
| C3 | shutdown 併發冪等 | `shutdown_claimed_`／`shutdown_complete_` 兩個 flag：首位呼叫者完成停止、drain、join 與後置檢查，其餘等待同一次完整結果；四個後置條件改為所有建置執行；destructor 保留並註明 singleton 刻意不解構 |
| D2 | owner pin | `rebalance_children` 先複製 `IntrusivePtr` 為區域 pin，再取 raw pointer |
| E1 | page-table root | `LocationIndex` 改用 `PageTableNode` 樹（fanout 64），更新只複製短路徑；`IdDirectory`／`RecordPage`／`LocationPage`／`PageTableNode` 標為 `final`；`ObjectRecord` 的 owning edge 限制明文化 |

### C2 的死鎖確認

這是本輪最嚴重的發現，已實測確認可重現的推理鏈：

1. worker 檢查 `head_ == nullptr` → 成立，進入等待迴圈
2. worker 檢查 `shutdown_idle` → 此時 state 仍為 `running`，不成立
3. `shutdown()` 將 state CAS 為 `stopping`，bump generation，`notify_one`
4. worker 讀 `observed` = **bump 之後的新值**
5. worker 只重查 `head_`（仍為 null），未重查 state
6. worker 以新值進入 `wait(observed)` → 值相等 → **永久睡眠**
7. `shutdown()` 進入 `join()` → **永久互等**

修正後 `worker_should_wake()` 同時檢查 `head_` 與 `state != running`，步驟 5 會回傳 true 而不進入 wait。

### 新增的回歸測試

- `test_derived_type_cannot_shadow_counter`（A1）—— 惡意衍生型別宣告同名 public `retain`／`release`，驗證計數仍走 `RefCounted` 的唯一實作
- `test_concurrent_shutdown_is_idempotent`（C3）—— 8 個執行緒同時 shutdown，全部正常返回；修正前非首位呼叫者會 `terminate()`
- `test_page_table_grows_beyond_one_level`（E1）—— 跨越深度 1（4,096 slot）與深度 2 邊界
- `test_deep_tree_cow_isolation`（E1）—— 深樹的短路徑 COW 不影響舊版本
- raw constructibility 的 `static_assert`（A1）

全部 11 個測試套件通過；benchmark 無退化。

---

## 先看這兩項：我自己不確定的地方

**這兩項是我在寫這份清單時發現的，不是既有結論。優先審。**

### 疑點 1：`enqueue` 的閘門檢查有沒有誤殺窗口

`src/reclamation.cpp:30-38`

```cpp
active_enqueuers_.fetch_add(1, std::memory_order_acq_rel);
const State state = state_.load(std::memory_order_acquire);
if (state == State::finalizing || state == State::stopped) {
    active_enqueuers_.fetch_sub(1, std::memory_order_release);
    active_enqueuers_.notify_all();
    std::terminate();
}
```

`fetch_add` 與 `state_.load` 是**兩個分開的原子操作**，中間可以被 worker 插入。設想：

1. Producer 執行 `fetch_add` 成功（此時 state 還是 `stopping`）
2. Worker 在 `worker_main` 把 state CAS 成 `finalizing`（`reclamation.cpp:109`）
3. Producer 接著 `load` 到 `finalizing` → **`std::terminate()`**

這個 producer 並沒有違約——它在閘門關閉前就進來了。

**我的主張**：這個窗口在目前的使用契約下不會觸發，因為
(a) 外部 producer 必須在呼叫 `shutdown()` 前 join 完畢（D17 明文）；
(b) `stopping` 之後唯一的 enqueue 來源是 destructor 產生的 child，而 destructor 只在 worker
執行緒上跑，worker 不會同時在 `worker_main` 的 CAS 處。

**你要回答的問題**：
1. (b) 真的成立嗎？去確認 `drain_once()` 的 `delete node`（`reclamation.cpp:66`）確實只由
   worker 執行緒呼叫，沒有任何其他路徑會 delete 節點。
2. 如果 (a) 被將來的使用者違反（例如有人在別的執行緒還活著時呼叫 shutdown），
   結果是 `terminate` 還是靜默錯誤？**`terminate` 可接受嗎？**
3. 若你認為窗口該關掉：正確做法是把「登記 producer」與「檢查 state」合成單一原子操作
   （例如把兩者塞進同一個 word，用 CAS 一次完成）。**這個複雜度值得嗎？**

**判定**：要求修改（2026-08-18）〔**已實作，待重審**〕

**審查意見**：將 state 與 active producer count 合併成單一 atomic word，以 CAS 建立明確的
producer admission 線性化點。修正後必須重審本項；在此之前不得改判為接受。

**實作**：`intrusive_ptr.hpp` 的 `GateWord`／`gate_state`／`gate_count`／`make_gate`；
`reclamation.cpp` 的 `enqueue` admission 迴圈與 worker 的 `(stopping,0) → (finalizing,0)` CAS。
重審要點：(a) 計數欄 56 bit 是否足夠且不會借位到 state；(b) worker 的 finalize CAS 失敗後
是否必然重跑而非漏掉節點；(c) `gate_.fetch_sub(1, release)` 作為離場是否與 admission CAS 相容。

---

### 疑點 2：`TreeCursor` 的 moved-from 狀態有 dangling pointer 〔**已修，改審修法**〕

> **狀態更新（2026-08-17）**：已改為自訂 move constructor／assignment，搬移後把來源的
> `leaf_` 設為 `nullptr`、`local_offset_` 與 `global_position_` 歸零。
> 回歸測試 `test_cursor_move_invalidates_source` 涵蓋兩條路徑。
> **你現在要審的是「這個修法是否正確且充分」，而不是原缺陷。**
> 下面保留原始描述作為脈絡。



`include/krepis/flow_sequence.hpp` 的 `TreeCursor`

```cpp
TreeCursor(TreeCursor&&) noexcept = default;   // ← 預設 move

IntrusivePtr<const FlowSequenceNode> root_;    // move 後變 null
std::vector<Frame> ancestors_;                 // move 後變空
const FlowLeafNode* leaf_ = nullptr;           // ← raw pointer，move 後「被複製」
std::size_t local_offset_ = 0;
std::size_t global_position_ = 0;
```

預設 move 會把 `root_` 搬走（來源變 null），但 `leaf_` 是 raw pointer，**會被複製而不是清空**。
於是 moved-from 的 cursor：

- `is_valid()` 回傳 **true**（因為 `leaf_ != nullptr`）
- 但它已經不持有 `root_`，節點的存活完全依賴 moved-to 那個 cursor
- 若 moved-to 先解構，moved-from 的 `leaf_` 就是 dangling，而 `is_valid()` 仍說有效

**我的主張**：這是缺陷，該修。修法是自訂 move，把來源的 `leaf_` 設為 `nullptr`。

**你要回答的問題**（針對已實作的修法）：
1. 你同意「moved-from 物件的 `is_valid()` 回報 true」本身就是錯的嗎？
   （C++ 慣例：moved-from 物件必須處於**有效但未指定**的狀態；`is_valid()` 說謊會讓
   使用者寫出看似合理卻 UB 的程式碼。）
2. 我選了 (A) 自訂 move。另外兩個選項是
   (B) `= delete` move 讓 cursor 完全不可搬移、
   (C) 不修而在文件註明「moved-from 只能解構」。
   **(B) 比 (A) 保守——你偏好哪個？** 若你要 (B)，說一聲，改動很小。
3. 檢查修法**充分性**：`src/flow_sequence.cpp` 的 move ctor 與 move assignment
   都把來源的 `leaf_`／`local_offset_`／`global_position_` 歸零了嗎？
   `root_` 與 `ancestors_` 靠各自型別的 move 語意歸零——**這個假設對嗎？**
   （`IntrusivePtr` 的 move ctor 會把來源設 null：`intrusive_ptr.hpp:157-159`。
   `std::vector` 的 move 後狀態是「有效但未指定」，標準**不保證**一定是空的——
   但我們沒有讀它，所以無妨。**確認這個推理。**）
4. move assignment 的 self-assignment 防護（`if (this != &other)`）必要嗎？
   自我搬移在標準庫裡是「有效但未指定」，加防護是保守做法。你接受嗎？
5. 這個問題有沒有同型的姊妹問題？全庫搜尋還有哪些型別同時持有
   **owning pointer + 借用 raw pointer** 且使用預設 move。

**判定**：接受（A：保留自訂 move，2026-08-18）

**審查意見**：move constructor／assignment 會把來源的 `leaf_`、offset 與 position 歸零；
`IntrusivePtr` move 會清空來源 owner。`ancestors_` 搬移後雖只保證有效但未指定，無效 cursor 的
操作會先以 `leaf_ == nullptr` 擋下，因此不會讀取該路徑。保留 self-move 防護。

---

## A. retain／release 的 memory order

### A1. `retain()` 為什麼可以用 `relaxed`

`include/krepis/intrusive_ptr.hpp:55-61`

```cpp
const std::uint32_t previous = count_.fetch_add(1, std::memory_order_relaxed);
```

**我的主張**：`relaxed` 足夠，因為 D17 硬性規定「只能從既有 owning reference 複製新
reference」。既有 reference 本身已經保證：物件存活、物件內容對本執行緒可見。因此這次 `+1`
不需要與任何寫入建立 happens-before，它只需要**原子性**（不丟失計數）。

對照：如果允許從未受保護的 raw pointer `retain`（weak / resurrection / pointer promotion），
`relaxed` 就**不夠**了——那時需要 `acquire` 才能保證看到物件的初始化。D17 明文禁止那些路徑，
所以前提成立。

**你要回答的問題**：
1. 「只能從既有 owning reference 複製」這條規則，程式碼是**結構上**保證的，還是只是慣例？
   去看 `IntrusivePtr` 的所有建構子（`intrusive_ptr.hpp:148-173`）與 `AdoptInitialReference`
   （`:237-239`）。**有沒有任何 public 路徑能拿一個 raw pointer 造出 IntrusivePtr？**
2. `make_intrusive`（`:245-252`）是唯一的初始 owner 建立路徑嗎？`friend` 宣告有沒有開後門？
3. 測試 `intrusive_ptr_test.cpp:60-65` 用 concept 驗證 `retain`/`release`/`adopt` 不是 public。
   **這些 static_assert 涵蓋所有逃逸路徑嗎？**

**判定**：要求修改（2026-08-18）〔**已實作，待重審**〕

**審查意見**：`IntrusivePtr` 必須明確呼叫 `RefCounted` 的唯一 counter 實作，不能讓衍生型別以
同名 public `retain`／`release` 遮蔽。新增 hostile derived-type 與 raw constructibility 測試；並更正
「raw pointer promotion 加 acquire 即可」的描述，因為 acquire 本身不能保證遞增前的物件存活。

**實作**：`retain_pointer`／`release_pointer` 以 `static_cast<const RefCounted*>(p)->RefCounted::retain()`
限定呼叫；`test_derived_type_cannot_shadow_counter` 與 `ConstructibleFromRaw`／
`ConstructibleFromMutableRaw` 的 `static_assert`。

**上方「我的主張」已過時，一併更正**：原文寫「允許從 raw pointer retain 時需要 acquire」是錯的。
**acquire 排序不能使物件存活**——若計數已歸零，任何 memory order 都救不了那次遞增。
禁止 raw-pointer retain 的真正理由是**存活性**（必須有既存 owner 保證非零計數），不是可見性。
`relaxed` 足夠的理由因此更強：既存 owner 已同時保證存活與可見。

注意：`IntrusivePtr` 的基底型別 `static_assert` 刻意放在 `retain_pointer`／`release_pointer` 而非
class scope——`IdDirectory` 等型別需要在自身不完整時使用 `IntrusivePtr<const Self>`，
而 `is_base_of` 要求完整型別。**重審請確認這個放寬沒有打開漏洞**
（實質保護仍來自 `static_cast<const RefCounted*>`，非衍生型別無法通過編譯）。

---

### A2. `release()` 的 `release` + 條件式 `acquire` fence

`src/reclamation.cpp:11-28`

```cpp
const std::uint32_t previous = count_.fetch_sub(1, std::memory_order_release);
if (previous != 1) return;
std::atomic_thread_fence(std::memory_order_acquire);
count_.store(poisoned, std::memory_order_relaxed);
default_reclamation_queue().enqueue(this);
```

**我的主張**：這是標準做法（libstdc++／Boost 的 `shared_ptr` 同構）。拆解：

- `fetch_sub(release)`：保證**本執行緒**在放手前對物件的所有存取，排在計數遞減之前。
- `previous == 1` 的那個執行緒是最後一個。它必須看見**其他所有執行緒**先前的存取才能安全銷毀。
  `acquire` fence 與其他執行緒的 `release` fetch_sub 配對，建立 happens-before 邊。
- 為什麼不整個用 `acq_rel`？會讓**每一次** release 都付 acquire 成本，但只有最後一次需要。
  拆成 `release` + 條件式 fence 是這個最佳化的標準形狀。

**你要回答的問題**：
1. 畫出兩執行緒的圖：T1 寫物件 → T1 release；T2 release（拿到 0）→ T2 destruct。
   **T1 的寫入確定對 T2 的 destructor 可見嗎？** 手動追一次 happens-before 鏈，不要接受我的話。
2. `count_.store(poisoned, relaxed)` 在 fence **之後**。此時已是唯一擁有者，所以安全——
   但注意取捨：**若真有非法存取（bug），`relaxed` store 可能不會及時被別的執行緒看到，
   poison 的偵測力就打折。** 這是刻意的（poison 是診斷輔助，不是正確性保證）。你同意嗎？
3. `assert(previous != 0 && previous != poisoned)` 在 `fetch_sub` **之後**才檢查。
   如果 `previous` 真的是 0，那次 `fetch_sub` 已經把計數變成 `0xFFFFFFFF` 了。
   **在 NDEBUG 建置（assert 被移除）下，這會怎麼樣？** 這是可接受的嗎？

**判定**：要求修改（2026-08-18）〔**已實作，待重審**〕

**審查意見**：保留 `fetch_sub(release)` 加最後一次釋放才執行的 acquire fence；將 zero、poison、
underflow 與 overflow 違約改為所有建置都立即終止。更正 poison 說明：問題是非法 retain 可能在
poison store 前進入 modification order，不是 relaxed store 會任意讀到過時值。

**實作**：`require_lifetime()`／`lifetime_violation()`（`intrusive_ptr.hpp`）在 `retain()`、
`release()`、`drain_once()` 與 shutdown 後置條件使用，全部建置生效。
`fetch_sub(release)` ＋ 條件式 acquire fence 依審查意見保留不變。
poison 的註解已改寫為「診斷輔助而非正確性保證」，並寫明真正的保證來自 `IntrusivePtr` 的封裝。

重審要點：熱路徑（`retain`／`release`）新增的分支對效能的影響。分支條件是剛載入的值，
預期可完全預測；若你要求證據，可用 `spike4_intrusive_vs_shared` 對照修正前後。

---

## B. Treiber stack

### B1. `enqueue` 的 CAS 與 `reclaim_next_` 的可見性

`src/reclamation.cpp:44-49`

```cpp
RefCounted* mutable_node = const_cast<RefCounted*>(node);
RefCounted* current_head = head_.load(std::memory_order_relaxed);
do {
    mutable_node->reclaim_next_ = current_head;     // ← 普通寫，非原子
} while (!head_.compare_exchange_weak(current_head, mutable_node,
                                      std::memory_order_release,
                                      std::memory_order_relaxed));
```

**我的主張**：`reclaim_next_` 的普通寫排在 `release` CAS 之前，consumer 用
`head_.exchange(acquire)`（`:59`）讀取，兩者配對，因此 consumer 走串列時必定看得到
`reclaim_next_`。失敗路徑用 `relaxed` 是對的——CAS 失敗時我們只是重讀 `current_head`，
還沒發布任何東西。

**你要回答的問題**：
1. CAS 失敗後迴圈重跑，`reclaim_next_` 被**重寫**。此時該節點還沒進入串列，
   所以沒有別的執行緒在讀它。**確認這一點**——有沒有任何情況下節點會同時在串列裡又被重寫？
2. `const_cast` 去掉 const（`:44`）。`reclaim_next_` 宣告為 `mutable`
   （`intrusive_ptr.hpp:70`），所以其實不需要 const_cast 就能寫。
   **這個 const_cast 是必要的還是殘留？** 若是殘留，移除能讓意圖更清楚。

**判定**：要求修改（2026-08-18）〔**已實作，待重審**〕

**審查意見**：Treiber push 的 release／acquire publication 正確；將 `reclaim_next_` 與 `head_`
統一改為 `const RefCounted*`，使 immutable node 的回收鏈保持 const-correct 並移除 `const_cast`。

**實作**：`RefCounted::reclaim_next_` 與 `ReclamationQueue::head_` 皆為 `const RefCounted*`，
`enqueue` 不再有 `const_cast`。`drain_once` 的 `delete node`（`node` 為 `const RefCounted*`）
合法——標準允許對 const pointer 使用 `delete`。

---

### B2. 為什麼這裡沒有 ABA 問題

`src/reclamation.cpp:59`

```cpp
RefCounted* node = head_.exchange(nullptr, std::memory_order_acquire);
```

**我的主張**：教科書的 Treiber stack pop 有 ABA 問題，因為它做「讀 head → 算 next →
CAS(head, next)」；期間 head 可能被換走又換回，next 已經失效。

**這裡的 consumer 不做 pop，它做 `exchange(nullptr)`——一次取走整條串列。**
沒有「讀了再 CAS 回去」的序列，因此**結構上不可能有 ABA**。這是刻意的設計，不是巧合。

**你要回答的問題**：
1. 確認整份程式碼裡沒有任何地方對 `head_` 做「讀→算→CAS」。（`grep head_`，只有
   `load`、`exchange`、`compare_exchange_weak(在 enqueue，是 push 不是 pop)`。）
2. **若將來要加第二個 worker**，`exchange(nullptr)` 仍然安全（各拿各的批次），
   但 `pending_.fetch_sub(destroyed)`（`:72`）呢？多 worker 下還正確嗎？
   （目前單 worker，所以不是缺陷；但這個結論要寫下來，避免將來有人加 worker 時沒注意。）

**判定**：接受（2026-08-18）

**審查意見**：consumer 只以 `exchange(nullptr)` 取得整批，不做「讀取 next 後再以 CAS 更新 head」，
因此不存在 Treiber pop 的 ABA。多 worker 下，exchange 分批與 atomic pending 扣減本身仍可成立；
但 worker 喚醒、finalization、shutdown 與 join 都是單 worker 契約，若增加 worker 必須重開審查。

---

## C. shutdown drain path（最容易出錯的部分）

狀態機：`running → stopping → finalizing → stopped`，且 `finalizing` **可以退回** `stopping`
（`reclamation.cpp:128`）。

### C1. 「不漏掉節點」的不變條件

**我的主張**：worker 在 `finalizing` 時等 `active_enqueuers_ == 0`（`:112-116`），
再確認 `head_ == nullptr && pending_ == 0`（`:118-119`）。因為 producer 是
**先 `++` 再檢查 state**，任何已越過閘門的 producer 都已被計入，所以 worker 等到 0
就代表沒有「正在發布中」的節點。

若此時仍發現節點（`finalized == false`），就退回 `stopping` 再跑一輪（`:128`）——
這處理「舊 producer 剛發布完」的情況。

**你要回答的問題**：
1. 退回 `stopping` 之後，迴圈重跑。**這個退回會不會無限次發生？**
   論證終止性：什麼保證最終會到達 `finalized == true`？
   （提示：外部 producer 已 join，剩下的只有 destructor 產生的 child，而 DAG 有限深度。）
2. `pending_` 與 `head_` 兩個條件都要檢查——**只檢查其中一個會漏什麼？**
   構造出「head 為空但 pending 非零」的瞬間。
3. 這一項與**疑點 1** 直接相關。先解疑點 1 再回來定這裡。

**判定**：接受（2026-08-18）

**審查意見**：接受有限 DAG、外部 producer 已停止並 join 的前提下，退回 `stopping` 必然終止；
`active`、`pending_` 與 `head_` 分別覆蓋 producer 已入場、已登記未發布、已取批但未刪完等階段，
三者不可互相取代。本項接受的是終止性與防漏條件的論證；疑點 1 已要求的 packed gate 修正仍是
成立前提，實作完成後必須連同 producer admission 線性化點一起重審。

---

### C2. generation counter 的防漏喚醒

`worker_main`（`:89-92`）與 `wait_until_idle`（`:141-147`）都是同一個模式：

```cpp
const std::uint64_t observed = gen.load(acquire);
if (還是忙) {
    gen.wait(observed, acquire);
}
```

**我的主張**：這是標準的防漏喚醒。關鍵是 `std::atomic::wait(old)` 的語意——
**它會先比對目前值與 `old`，若已不同就立刻返回**，不會睡下去。因此「讀 observed 之後、
wait 之前，producer 剛好 bump」不會漏，因為 wait 進去時值已經不等於 observed。

**你要回答的問題**：
1. 確認你真的理解 `atomic::wait` 的這個語意（cppreference：*"if the value is not equal to
   old, returns immediately"*）。**這是整個同步正確性的支點**，弄錯就會偶發死鎖。
2. `notify_one()`（`:54`）vs `notify_all()`（`:104`）：worker 只有一個，
   但 `wait_until_idle` 可能有多個 waiter。**用對了嗎？**
   （`worker_generation_` → `notify_one`；`idle_generation_` → `notify_all`。）
3. `wait_until_idle` 從測試以外的地方被呼叫嗎？如果它是測試專用，
   **它的正確性要求可以放寬嗎？** （我的看法：不行，它進了 public header。）

**判定**：要求修改（2026-08-18）〔**已實作，待重審 —— 本輪最嚴重的發現**〕

**實作**：新增 `ReclamationQueue::worker_should_wake()`，同時檢查 `head_ != nullptr` 與
`state != running`；`worker_main` 在讀 generation 之**前與之後**都呼叫同一個 predicate。
死鎖推理鏈已記錄在本檔開頭的「C2 的死鎖確認」。
重審要點：`worker_should_wake()` 是否涵蓋所有喚醒理由（目前為新節點與停止要求兩類）。

**審查意見**：`wait_until_idle()` 的「讀 generation → 重查完整 predicate → wait」順序正確，
`worker_main()` 卻在讀 generation 前檢查 `shutdown_idle`，讀取後只重查 `head_`。若 shutdown 已
切成 `stopping` 並 bump generation，worker 隨後讀到新 generation，再以同一值進入 wait，便可能
與正在 `join()` 的 shutdown 永久互等。`worker_main()` 必須在讀 generation 後重查完整等待條件；
保留單 worker 的 `notify_one()` 與多個 public waiter 的 `notify_all()`。

---

### C3. `shutdown()` 的重入與併發

`src/reclamation.cpp:151-171`

```cpp
State expected = State::running;
if (!state_.compare_exchange_strong(expected, State::stopping, acq_rel, acquire)) {
    if (expected == State::stopped) return;
    std::terminate();
}
```

**我的主張**：第二次呼叫 `shutdown()` 若已 `stopped` 就安靜返回（冪等）；
若處於 `stopping`／`finalizing`（代表另一執行緒正在 shutdown）就 `terminate`，
因為 D17 說 shutdown 是 runtime 的單一執行緒操作。

**你要回答的問題**：
1. `terminate` 是對的反應嗎？還是應該等待對方完成？
   （我認為 `terminate` 對：併發 shutdown 代表生命週期契約已被破壞，靜默等待會掩蓋 bug。）
2. `~ReclamationQueue()` 也呼叫 `shutdown()`（`:8`）。但 `default_reclamation_queue()`
   **刻意 leak**（`:179` 的 `new`，永不 delete）。**那 destructor 什麼時候跑？**
   如果永遠不跑，`:7-9` 是死碼嗎？留著有害嗎？
3. `:167-170` 的四個 assert 在 NDEBUG 下全部消失。
   **關機證明（「配置數等於釋放數」）在 Release 建置下還存在嗎？**
   若不存在，D17 要求的證明只在 Debug 成立——**這可接受嗎？**

**判定**：要求修改（2026-08-18，依授權採用分析建議）〔**已實作，待重審**〕

**實作**：`shutdown_claimed_`（誰執行）與 `shutdown_complete_`（何時可返回）兩個 flag。
首位呼叫者送出停止要求、`join()`、執行四項後置檢查，最後設 `shutdown_complete_` 並 `notify_all`；
其餘呼叫者在 `shutdown_complete_` 上等待。四項後置條件改用 `require_lifetime`，所有建置生效。
destructor 保留並加註「預設 singleton 刻意不解構，此 destructor 為非 singleton 擁有情境保留」。
回歸測試 `test_concurrent_shutdown_is_idempotent`（8 執行緒同時 shutdown）。

**審查意見**：將 shutdown 定義為併發冪等的冷路徑；唯一執行者完成停止、drain 與 `join()`，
其他同時呼叫者等待同一次完整結果，避免看到 worker 已設 `stopped` 卻在首位 caller 尚未 join 時提早返回。
保留 destructor 作為非 singleton ownership 的防禦性生命週期保證，並註明預設 singleton 刻意不解構。
四個 shutdown postcondition 改為所有建置都執行的 fail-fast 檢查；配置數等於回收數則繼續由
Release CI 的 accounting test 提供證據。late enqueue 仍是生命週期違約並立即終止。

---

## D. borrowed pointer lifetime

### D1. `TreeCursor` 的借用不變條件

**我的主張**：`TreeCursor` 持有 `root_` 的 owning reference，其餘（`ancestors_` 內的
`FlowInternalNode*`、`leaf_`）都是借用。安全前提是「root 可達的所有節點在 root 存活期間
都存活」，這由「節點不可變 + owning edge 只向下形成 DAG」保證。

**你要回答的問題**：
1. 這個前提成立的關鍵是**沒有任何節點在發布後修改它的 owning edge**。
   去確認 `FlowInternalNode` 與 `FlowLeafNode` 都沒有 setter，`children_`／`blocks_`
   都是 private 且只在 constructor 賦值。
2. `TreeCursor` 建構時 `root_(seq.root())`——`FlowSequence::root()` 回傳
   **by value 的 IntrusivePtr**（`flow_sequence.hpp:105`），所以 cursor 有自己的計數。
   **若它回傳的是 reference，會怎麼樣？** 確認目前簽章是 by value。
3. **疑點 2 在這一節。** 先解疑點 2。

**判定**：接受（2026-08-18，依授權採用分析建議）

**審查意見**：`FlowInternalNode::children_` 與 `FlowLeafNode::blocks_` 都是 constructor 建立後
不可修改的 private 成員，沒有 owning-edge setter。`FlowSequence::root()` 確實以值回傳
`IntrusivePtr<const FlowSequenceNode>`，因此 `TreeCursor` 持有獨立 owner；即使原 `FlowSequence`
先解構，ancestor 與 leaf 的 raw pointer 仍由 cursor 的 root 保活。疑點 2 的自訂 move 已另行接受。

---

### D2. `FlowSequence` 內部演算法的借用

`src/flow_sequence.cpp` 的 `insert_into`、`remove_from`、`rebalance_children` 都收
`const FlowSequenceNode*` 這種 raw pointer。

**我的主張**：安全，因為呼叫者（`FlowSequence::insert`／`remove`）在整個呼叫期間持有
`root_`，而所有 raw pointer 都是從該 root 可達的節點。這符合 D17 的
「raw pointer 只能在已有 owning root 保活的詞法範圍內借用」。

**這是最可能藏 use-after-free 的地方**：`rebalance_children` 會把 `children[first]`／
`children[second]` **覆寫**成新節點，覆寫那一刻舊節點的計數可能歸零並被 enqueue。
而 `left_src`／`right_src` 是指向那兩格內容的 raw pointer。

**我追過一次，結論是安全的**，理由：

- Merge 路徑：`combined` 在賦值前就已從兩個 leaf 完整複製；`merged_key = left_src->key()`
  也在 `children[first] = ...` **之前**求值。賦值後不再觸碰 `left_src`／`right_src`。
- Redistribute 路徑：`left_key`／`right_key` 同樣在兩次賦值前就讀出；`left_blocks`／
  `right_blocks` 來自更早複製好的 `combined`。第一次賦值後 `left_src` 失效，
  但後續只用已複製的 `right_key`，沒有再解參照。

**你要回答的問題**：
1. **駁倒或確認上面這個追蹤。** 逐行看一次求值順序——特別是
   `children[first] = {make_intrusive<FlowLeafNode>(merged_key, std::move(combined)), ...}`
   這一行裡，`make_intrusive` 與 `children[first]` 的賦值，**求值順序有保證嗎？**
   （C++17 起賦值運算子的右手邊先於左手邊求值——確認你同意這條適用於此。）
2. 我的追蹤只涵蓋目前的程式碼。**這個安全性是結構保證還是巧合？**
   如果是巧合（只是剛好順序對），將來改動很容易破壞它。
   要不要加註解寫明「此處求值順序有正確性意義」？
3. 不論結論如何，用 ASan 實跑一次 `krepis_flow_sequence_test`（CI 的 `clang-asan` job
   會做）。**人工推理與工具驗證都要有**——這正是閘門 5 與閘門 7 分開列的原因。

**判定**：要求修改（2026-08-18，依授權採用分析建議）〔**已實作，待重審**〕

**實作**：`rebalance_children` 新增 `left_pin`／`right_pin` 兩個區域 `IntrusivePtr`，
raw pointer 一律由 pin 取得。覆寫 `children[first]`／`children[second]` 時舊節點仍被 pin 保活，
因此後續重排不可能造成 use-after-free——保證從「敘述順序正確」升級為「結構上正確」。
ASan 驗證仍待 CI 的 `clang-asan` job（閘門 5）提供。

**審查意見**：目前 merge／redistribute 的讀取順序在 C++17 以上成立，賦值右側先於左側求值，
且程式在覆寫 child owner 前已複製 blocks 與 key，覆寫後不再解參照相應 raw pointer；現況沒有
use-after-free。然而這仍是對目前敘述順序的脆弱保證。`rebalance_children` 應先複製 left／right 的
`IntrusivePtr<const FlowSequenceNode>` 作為局部 owner pin，再從 owner 取得 raw pointer，讓後續重排
不可能破壞 lifetime；並保留 ASan 的 split／merge／redistribute 路徑驗證。

---

## E. owning edge 的形狀

### E1. 沒有 reference cycle

**我的主張**：D17 要求 owning edge 只向下形成 DAG。`make_intrusive` 在 constructor 就接收
已存在的 child owner，因此是 bottom-up 建構——**parent 存在時 child 必定已存在**，
結構上不可能指回尚未存在的 parent，所以不可能成環。

**你要回答的問題**：
1. 這個論證依賴「沒有任何 setter 能在建構後加 owning edge」。全庫確認一次。
2. `FlowSequenceNode`／`FlowLayoutNode`／`LocationPage`／`PageTableNode` 四個 RefCounted
   衍生型別都符合嗎？（`PageTableNode` 有兩個 constructor，注意看。）
3. 新加的 `LocationIndex` 目前用 `std::vector<IntrusivePtr<const LocationPage>>` 直接持有
   pages，**沒有用到 `PageTableNode`**。那 `PageTableNode` 是死碼嗎？
   （若是，該刪還是該接上？死碼會讓後續審查者以為它在用。）

**判定**：要求修改（2026-08-18，依授權採用分析建議）〔**已實作，待重審**〕

**實作**：
1. `LocationIndex` 改以 `PageTableNode` 樹為 root（fanout 64），`set()` 只複製 root 到該 page
   的路徑；樹加高時舊 root 成為新 root 的第 0 個 child，其餘子樹完全共享，加高本身 O(1)。
   先前的扁平 vector 每次更新複製全部 page 指標，10 萬 Block 即 1,563 個——已消除。
2. `IdDirectory`、`RecordPage`、`LocationPage`、`PageTableNode` 標為 `final`。
3. `ObjectRecord` 的 owning edge 限制寫入 header：**衍生 record 不得以 IntrusivePtr 持有其他
   record**，跨記錄關係一律用穩定 ID。理由已寫明——D17 的無循環論證只對樹成立，
   記錄之間是圖，直接持有會形成 reclamation queue 回收不掉且無診斷的循環。
4. 新增 `test_page_table_grows_beyond_one_level` 與 `test_deep_tree_cow_isolation`。

重審要點：`page_span_for_depth()` 的深度／容量換算（`depth d` 容納 `fanout^d` 個 page，
即 `page_span_for_depth(d+1)`），以及 `capacity()` 的回報是否與實際可定址範圍一致。

**審查意見**：現有 FlowSequence／FlowLayout 節點與 `PageTableNode` 都只在 constructor 接收既有
child owner，沒有 setter，已使用向下的 `IntrusivePtr<const T>`；但 `LocationIndex` 實際只持有
`std::vector<IntrusivePtr<const LocationPage>>`，`PageTableNode` 沒有任何建構或讀取點，是會誤導
審查者的死碼，也使每次更新複製整個 pages vector，不符合 D14 的 immutable page-table root 與
短路徑 COW。依全局效能決策，不刪除設計而應將 `PageTableNode` 接成 `LocationIndex` root；同時將
不需擴充的具體 RefCounted 節點標為 `final`，並把可擴充 `ObjectRecord` 的 owning edge 限制明文化：
跨記錄關係以穩定 ID 表示，不得在 record 內形成 reclamation-owned cycle。

---

## 審查完成後

全部判定為「接受」時：

1. 在 `spec/decisions/02-layout/LAY-0002-invalidation-offset-and-viewport-index.md`
   的 D17 閘門清單，把第 7 項標為已完成，並註明審查日期與審查者。
2. 把本檔的判定結果保留（**不要刪**）——它是閘門通過的證據。
3. 若有「要求修改」，修完後**重審該項**，不是直接改判定。

## 審查者

### 第一輪（2026-08-18）

- 日期：2026-08-18
- 結論：**未通過** —— 8 項要求修改、4 項接受
- 最嚴重發現：**C2 的 worker／join 死鎖**（可重現的推理鏈已記錄），
  以及疑點 1 的 producer 誤殺窗口

### 第二輪（待進行）

全部 8 項要求修改已實作並通過 11 個測試套件，等待重審。

- 姓名：_____________
- 日期：_____________
- 結論：_____________

**重審範圍**：只需重審標記〔已實作，待重審〕的 8 項。
第一輪判定為「接受」的 B2、C1、D1 與疑點 2 不需重審，但 **C1 的成立以疑點 1 修正為前提**，
確認疑點 1 後 C1 才算真正閉合。
