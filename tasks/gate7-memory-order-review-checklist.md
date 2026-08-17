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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

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

**判定**：_____________

---

## 審查完成後

全部判定為「接受」時：

1. 在 `spec/decisions/02-layout/LAY-0002-invalidation-offset-and-viewport-index.md`
   的 D17 閘門清單，把第 7 項標為已完成，並註明審查日期與審查者。
2. 把本檔的判定結果保留（**不要刪**）——它是閘門通過的證據。
3. 若有「要求修改」，修完後**重審該項**，不是直接改判定。

## 審查者

- 姓名：_____________
- 日期：_____________
- 結論：`通過` / `未通過`
