# Krepis 規格索引

本目錄是 Krepis 的架構真相。任何與程式碼衝突的地方，**以本目錄為準，並修正程式碼或開新決策**。

- 架構總覽：[`architecture.md`](architecture.md)
- 決策依能力分區存放於 [`decisions/`](decisions/)，**一份決策一個檔案，不合併**。

## 能力分區

決策**依能力切分目錄**，目錄本身即是分類。每份決策恰好屬於一個能力；跨能力的決策代表它混合了
層次，**應拆分**。

| 目錄 | 代碼 | 範圍 |
|---|---|---|
| [`00-foundation/`](decisions/00-foundation/) | `FND` | 專案級：範圍與拒絕清單、語言邊界、命名、原始碼慣例、依賴政策 |
| [`01-document/`](decisions/01-document/) | `DOC` | node tree、stable ID、schema、codec、版本遷移 |
| [`02-layout/`](decisions/02-layout/) | `LAY` | 流式版面、空間版面、增量重算、display list |
| [`03-text/`](decisions/03-text/) | `TXT` | shaping、grapheme、字型 fallback、IME 與 composing region |
| [`04-editing/`](decisions/04-editing/) | `EDT` | selection 模型、typed transaction、undo／redo |
| [`05-ink/`](decisions/05-ink/) | `INK` | ink 資料模型、圖層、anchor 到文件位置 |
| [`06-authority/`](decisions/06-authority/) | `ATH` | persistence、identity、permission、protocol、digest |
| [`07-binding/`](decisions/07-binding/) | `BND` | FFI 邊界、client 契約、平台外殼介面 |

### 放哪一格判斷不出來時

代表這份決策同時決定了兩件事。**拆成兩份**，各自放進所屬能力，並以「相關決策」互相引用。
不要為了省事新增第九個目錄。

## 檔名與編號

```
decisions/<NN-能力>/<代碼>-NNNN-kebab-case-主題.md
例：decisions/02-layout/LAY-0001-incremental-layout-and-display-list.md
```

編號**每個能力獨立遞增**，不共用全域序號。引用時一律寫完整代碼（`LAY-0001`），
代碼本身即說明它屬於哪個能力。

## 狀態詞彙

**五種狀態從第一天就全部可用。** 一個只能增加承諾、不能減少承諾的制度會讓待辦單調成長、
完成度停滯——這是有前例的失敗模式，不重蹈。

| 狀態 | 意思 | 可否作為新實作依據 |
|---|---|---|
| `Accepted` | 決策成立且在當期路徑上 | ✅ |
| `Deferred` | 決策仍然正確，但不在當期範圍內 | ❌ |
| `Superseded` | 已由新決策取代 | ❌ |
| `Proposed` | 尚未核准 | ❌ |
| `Rejected` | 明確否決 | ❌ |

`Deferred` 不表示決策有錯，只表示停止投入。被取代的決策**保留檔案**，由新決策標記關係。

## 兩條硬規則

### 1. 每份決策必須說出它關閉哪一道閘門

`Accepted` 的前提是回答這一句：

> **這份決策成立之後，哪一件原本做不了的事變成做得了？**

答不出來的決策**不得 Accepted**。它可以是 `Proposed` 或 `Deferred`，但不能算數。
規格的用途是關閘門，不是累積承諾。

### 2. 跨能力的相依必須雙向登記

一份決策若依賴另一個能力的決策，**兩份檔案都要寫**。單向引用會產生「A 改了、B 不知道」的
靜默矛盾——這種矛盾不會有任何徵兆，直到有人照舊決策實作。

每份決策的「相關決策」節必須列出：它依賴誰、誰依賴它。

## 決策紀錄

| 決策 | 狀態 | 主題 |
|---|---|---|
| [FND-0001](decisions/00-foundation/FND-0001-scope-language-boundary-and-rejections.md) | Accepted | Krepis 的範圍、語言邊界判準與拒絕清單 |
| [FND-0002](decisions/00-foundation/FND-0002-c-abi-error-memory-and-threading.md) | Accepted | C ABI 邊界、錯誤模型、arena 與釋放契約、執行緒模型（分工線除外） |
| [FND-0003](decisions/00-foundation/FND-0003-dependency-management.md) | Accepted | 第三方依賴以 CMake FetchContent 取得；淘汰 vcpkg |
| [DOC-0001](decisions/01-document/DOC-0001-object-tree-stable-id-reference-and-composition.md) | Accepted | 物件樹、stable ID、即時引用與組字疊加層 |
| [DOC-0002](decisions/01-document/DOC-0002-object-id-representation-and-generation.md) | Accepted | ObjectId 的位元表示、強型別包裝、生成與正規編碼 |
| [LAY-0001](decisions/02-layout/LAY-0001-sync-background-split-and-frame-budget.md) | Accepted | 同步／背景的分工線與一幀預算；P0 實測通過 |
| [LAY-0002](decisions/02-layout/LAY-0002-invalidation-offset-and-viewport-index.md) | Accepted | 失效傳播、偏移與可見範圍索引；D17 七道閘門與 D21 完整失效規則已通過 |
| [TXT-0001](decisions/03-text/TXT-0001-text-shaping-fallback-composition-and-cache.md) | Accepted | 跨平台 shaping、核心 fallback、URL／路徑禁斷、composition overlay 與 cache key |
| [EDT-0001](decisions/04-editing/EDT-0001-selection-transaction-and-undo.md) | Accepted | 兩種主要 selection、單層原子 Transaction、command merge 與全域 undo |
| [BND-0001](decisions/07-binding/BND-0001-display-list-buffer-command-and-version.md) | Accepted | Display list binary format、雙緩衝、同步 command、版本與錯誤拒絕策略 |
