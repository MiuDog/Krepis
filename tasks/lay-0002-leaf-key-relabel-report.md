# LAY-0002 LeafKey relabel window 實測

## 問題

`LeafKey` 是 128-bit 稀疏排序標籤。連續在同一側 split 會逐步耗盡中點；D22 已固定使用局部
relabel，但初始 window 大小必須量測，不能憑維護成本或直覺選擇。

## 先寫下的判準

在讀數字前固定以下順序：

1. 任一候選若無法完成 workload、產生 global rebuild，或破壞順序，直接淘汰。
2. 主要指標為同一輪 50,000 次頭插、50,000 次尾插、50,000 次中間插入的總時間中位數；候選各跑
   三輪。這三種模式分別覆蓋左側密集、右側密集與內部密集 split。
3. 若候選與最快值相差不超過 5%，選擇 `locator_updates` 較少者，因為正式 authority 還要為每個
   update 複製 LocationIndex path；單測 FlowSequence 時尚未包含這筆成本。
4. `max_window` 與 relabel event 數是診斷，不得以減少 event 為理由換取更多總 locator 更新。

## 環境與 workload

- 日期：2026-08-21
- 平台：Windows 11 上的 WSL2，Linux `6.6.87.2-microsoft-standard-WSL2`。
- CPU：AMD Ryzen 7 7700 8-Core Processor。
- 編譯器：GCC `13.3.0`。
- 組態：Release。
- 候選 initial window：2、4、8、16、32、64。
- 每個候選三輪；每輪合計 150,000 次插入。
- 測量範圍包含 immutable COW insert、局部 relabel 與 typed locator update 建立，不包含
  `DocumentRevision` 發布或 snapshot 最終銷毀。
- 每個頭／尾／中間 pattern 計時前 drain reclamation queue，避免上一段 workload 的背景銷毀與
  下一段計時重疊。

## 結果

| initial window | median total ms | relabel events | locator updates | max window | global rebuilds | valid |
|---:|---:|---:|---:|---:|---:|:---:|
| 2 | 2231.726 | 1537 | 1334444 | 1024 | 0 | yes |
| 4 | 2208.273 | 1583 | 1268418 | 512 | 0 | yes |
| 8 | 3714.249 | 1377 | 1075614 | 512 | 0 | yes |
| 16 | 2605.258 | 1126 | 1140896 | 512 | 0 | yes |
| 32 | 2710.892 | 959 | 1414288 | 512 | 0 | yes |
| **64** | **743.988** | **126** | **265078** | **64** | **0** | **yes** |

## 裁決

選擇 **initial relabel window = 64 leaves**。

它不是因為維護方便而選：相較次快的 window 4，總時間降低約 66.3%；locator updates 也降低約
79.1%。window 2–32 都會在某些密集區域擴張到 512 或 1024，代表小起點沒有換來較少工作，反而
重複付出鄰近 leaf 搜尋與重新編號成本。window 64 在本 workload 沒有擴張，也沒有 global rebuild。

這項選擇偏離實作前的暫定值 8，因此增加下列嚴格驗證：

- 日常測試仍以 window 8 強迫走「小 window 與擴張」路徑，避免預設 64 掩蓋演算法缺陷。
- 正式預設 64 的 100,000 次頭插、尾插與中間插入大型 gate 必須保持。
- 測試必須斷言舊 snapshot 不變、locator update 可回查、stale source root 被拒絕。
- 未來 Block 大小、leaf capacity 或 LocationIndex 實作改變時需重跑本 benchmark，不得沿用本數字。
