# Spike 4：IntrusivePtr vs std::shared_ptr benchmark 報告

## 目的

LAY-0002 D17 閘門 6：若 IntrusivePtr 沒有可重現的整體優勢，D17 必須重開。

## 環境

- MSVC 19.44.35222, Release (`/O2`), Windows 11 Home 26200
- 2026-08-17 依 D17 最終介面重跑：`IntrusivePtr<const T>`、bottom-up immutable node、
  單一背景 reclamation worker；兩次執行的勝負方向一致（下方取第二次）

## 記憶體佔用

| 項目 | IntrusivePtr 方案 | shared_ptr 方案 |
|---|---|---|
| 節點大小 | **40 bytes**（含 vtable 8 + atomic count 4 + reclaim_next 8） | **24 bytes**（control block 由 make_shared 合併配置，但不計入 sizeof） |
| 指標大小 | **8 bytes** | **16 bytes**（raw ptr + control block ptr） |

INode 比 SNode 大 67%，因為 RefCounted 基底需要 vtable pointer（virtual destructor）、
atomic 計數與 reclamation 串接指標。shared_ptr 的 control block 也有這些，但被 make_shared
隱藏在同一塊配置中，不影響物件 sizeof。

## 結果

| Workload | intrusive (us) | shared (us) | ratio (shared/intrusive) |
|---|---|---|---|
| **1. retain/release 1M** | 2,716 | 2,737 | 1.01x |
| **1. retain/release 10M** | 27,367 | 26,639 | 0.97x |
| **2. COW depth=8, 100k** | 41,622 | 19,053 | **0.46x** ← shared 勝 |
| **2. COW depth=16, 100k** | 74,939 | 44,319 | **0.59x** ← shared 勝 |
| **2. COW depth=32, 10k** | 15,784 | 9,017 | **0.57x** ← shared 勝 |
| **3. handoff 100k** | 2,486 | 1,624 | **0.65x** ← shared 勝 |
| **3. handoff 1M** | 50,977 | 26,482 | **0.52x** ← shared 勝 |
| **4. reclaim depth=1k** | 37.4 | 12.5 | **0.33x** |
| **4. reclaim depth=10k** | 230 | 115 | **0.50x** |
| **4. reclaim depth=100k** | 2,204 | 1,200 | **0.54x** |
| **5. 8t×1M concurrent** | 104,427 | 89,378 | **0.86x** |
| **5. 16t×1M concurrent** | 288,714 | 283,920 | 0.98x |

ratio > 1 = intrusive 較快，ratio < 1 = shared 較快。

## 分析

### 為什麼 IntrusivePtr 在 MSVC 上沒有優勢

1. **INode 40 bytes vs SNode 24 bytes**：RefCounted 強制加入 vtable、計數、串接指標。
   make_shared 把 control block 與物件合併配置，所以 SNode 的真正配置也只多了 ~16 bytes
   （ref count + weak count），但 SNode 本身更小，cache line 裝得更多。

2. **Reclamation queue 開銷**：IntrusivePtr 在最後 release 時要做 Treiber stack CAS，背景 worker
   之後再遍歷刪除。shared_ptr 直接在最後 release 的執行緒上 delete，少一層交接。
   這正是跨執行緒 handoff 與 reclaim 慢的原因，也是把 destructor 移出 UI 執行緒所付的成本。

3. **MSVC 的 shared_ptr 實作品質**：微軟大量投資在 STL 的 atomic 實作上，make_shared 的
   combined allocation 與計數操作高度最佳化。

### 但架構需求仍然存在

D17 選 IntrusivePtr 的**原始理由寫的是效能**——benchmark 證明這個理由不成立。

但**延後銷毀（deferred destruction）的架構需求是真的**：
- shared_ptr 在最後 release 的執行緒上同步遞迴銷毀——若 UI 執行緒是最後釋放者，
  深 DAG 會卡住 UI。
- IntrusivePtr + reclamation queue 把銷毀推到背景，正是 D17 要解決的問題。
- 用 shared_ptr + custom deleter 也能達成延後銷毀，但 deleter 必須放在額外 control block；
  `make_shared` 的合併配置不支援 custom deleter，因此每個節點會變成物件與 control block 兩次配置。

## 結論：D17 的效能正當性不成立，但架構正當性仍在

**D17 必須重開（依照閘門 6 的條款）。** 重開不代表要放棄 IntrusivePtr——但必須把理由從
「效能優勢」改寫為「延後銷毀是架構需求，shared_ptr 無法在不犧牲 make_shared 合併配置的
前提下提供 custom deleter」。

新的偏離建議段落應記錄：
1. 效能不是理由——benchmark 證明在 MSVC 上沒有可重現的整體優勢。
2. 保留的理由是架構：延後銷毀 + reclamation queue 的整合。
3. 若未來找到使 shared_ptr + 延後銷毀共存的低成本方案，此決策可再次重開。

## 人類決策（已完成）

接受以架構理由保留 IntrusivePtr；效能優勢不再作為理由。D17 的額外嚴格閘門是：const-only
owning edge、唯一 factory、private retain／release／adopt、單一背景 worker，以及顯式 shutdown 證明。
