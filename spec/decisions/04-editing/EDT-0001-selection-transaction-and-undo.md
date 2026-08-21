# EDT-0001：Selection、原子 Transaction 與全域 Undo

## 狀態

**Accepted**

## 日期

- 提出並接受：2026-08-18（使用者逐題裁決 EDT-1～EDT-4）
- 正式化：2026-08-21

## 背景

Flow 文字與 Spatial 節點的選取語意不同，但跨容器操作必須是一個不可分割的修改。Composition、
一般打字與 Ink 的 undo 單位也不同。若外殼自行判斷 transaction 或 merge 邊界，核心與不同 client
會靜默分岔。

資料與失效流程見
[`docs/architecture/edit-transaction-and-layout-invalidation.md`](../../../docs/architecture/edit-transaction-and-layout-invalidation.md)。

## 專業名詞

| 名詞 | 定義 |
|---|---|
| TextSelection | 同一文字 authority 中具有 anchor／focus 的有向文字範圍 |
| SpatialSelection | 同一 SpatialContainer 中以 stable ID 表達的無序節點集合 |
| typed command | 以具體型別描述前置條件、套用、反向操作與 merge 規則的修改命令 |
| Transaction | 一組 typed command 的單層原子邊界；全成功或全拒絕 |
| undo entry | 一次成功 Transaction 的反向資料與 merge metadata |
| base revision | Transaction 建立時依據的 `content_revision`，用於拒絕 stale commit |

## D1：P1 只有兩種主要 Selection

P1 的公開 selection variant 只有 `TextSelection` 與 `SpatialSelection`。TextSelection 有順序與方向；
SpatialSelection 是 stable node ID 集合，不假造 caret 或連續區間。Ink lasso 是受限工具狀態，只能
移動／刪除整筆，不加入所有編輯 command 的主要 selection variant。

加入第三種主要 selection 的條件是：至少有一項除移動／刪除外的跨工具操作需要一致語意，且 ADR
逐一列出所有既有 command 如何處理新型別。單純需要 lasso 不足以重開。

## D2：單層 Transaction 先驗證全部命令，再一次發布

`Transaction` 保存 base content revision 與 ordered typed commands。Commit 依序執行：

1. 核對 base revision。
2. 驗證全部 stable ID、record kind、owner、範圍與 command-specific 前置條件。
3. 在未發布的局部值上建立所有 COW 變更。
4. 執行 `DocumentRevision::validate()` 與 transaction-specific invariant。
5. 只產生一個 content revision 並回傳 `CommitResult`。

任一步失敗只回傳 `Error`；沒有可取得的半成品 revision，也不由 client 修補。第一條實作路徑只含
`ReplaceParagraphText`，之後新增 command 不改變原子邊界。

### 複雜度

令 `K` 為命令數、`P` 為 ObjectStore page-table 深度、`H` 為受影響樹高。驗證與 COW 成本為
`O(K × (P + H))` 加上實際修改內容大小；不得因只修改一個 Paragraph 掃描全部 `N` 個 Block。

## D3：Undo merge 規則由 command 型別宣告

全域 undo stack 不根據時間窗猜 command 語意。每個 command 必須提供 merge key、是否可合併與合併
方法：

- 連續一般文字輸入可在 EDT-5 的時間窗內合併，但跨 Paragraph、selection 改變或非文字 command
  會關閉 merge。
- IME composing overlay 不進 undo；一次確定產生一個不可再拆的文字 command。
- Spatial move／resize 第一版不與其他 transaction 合併。
- 一筆 Ink commit 是一個單位；Ink undo 另受 256 MiB session 記憶體預算限制。

## D4：整份文件共用一條全域 Undo 序列

一次跨容器 Transaction 只產生一個 undo entry，反向操作也以一個 Transaction 全部成功或全部拒絕。
不得按 Container 拆成多條序列；否則「從 Flow 移到 Spatial」可能只還原一半。Undo entry 可以記錄
受影響 ContainerId 供 UI 提示，但提示不改變序列或 authority。

## Composition 邊界

Composition overlay 不是 Transaction。確定時核心把 overlay 內容與當時 anchor 轉成一個 typed
command；Block 已刪除時取消，Block 移動時仍依 stable ID 提交。其他協作者修改正式文字時依
`DOC-0001` 的 anchor 重定位規則處理，不把未授權 overlay 傳給對方。

## 具體例子

Transaction 同時把 Paragraph A 改成 `AB頁`，並移動 Spatial node B：兩個 command 都先驗證。若 B
已被刪除，A 不能獨自變成 `AB頁`；commit 回傳錯誤且 base revision 仍是唯一可見狀態。若都成功，
content revision 從 41 直接變 42，不是 43，undo 也只有一項。

## Invariant 與拒絕行為

- Stale base、重複目標、找不到 stable ID、record kind 不符或 revision validation 失敗皆 fail closed。
- Command 不得保存 borrowed node pointer；跨 snapshot 資料只保存 stable ID 與值。
- Client 不得自行發布 `DocumentRevision`、拆分 Transaction 或合併 undo entry。
- 成功 CommitResult 的 invalidation set 必須由實際 command 產生，layout 不從 diff 猜測。

## 後果與驗證

- 單一全域序列使跨容器 undo 語意完整，代價是 UI 必須提示 undo 可能影響目前看不到的容器。
- Typed command 增加每種操作的明確實作，但把 merge 與反向規則集中在唯一位置。
- 測試必須涵蓋全部成功、任一失敗無部分結果、stale base、舊 snapshot 不變與一次 revision 增量。

## 尚未決定

- 一般文字輸入 merge 的 EDT-5 時間窗；初始約一秒，但必須以真實 workload 量測。
