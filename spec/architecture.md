# Krepis 架構總覽

本檔描述**跨能力的邊界與資料流**。單一能力的內部決策寫在該能力的決策目錄，不寫在這裡。

決策索引見 [`index.md`](index.md)。

## 分層

```
┌─ 平台外殼（Flutter，薄）─────────────────────────┐
│  執行 display list                              │
│  鍵盤／IME／指標事件 → command                    │
│  ink 快速路徑的實際繪製                            │
└──────────────┬──────────────────────────────────┘
               │ FFI（BND）：display list 出、command 入
┌──────────────┴─ Krepis 核心（C++，in-process）──┐
│  版面引擎（LAY）：流式 ＋ 空間、增量重算            │
│  文字（TXT）：shaping、grapheme、composing region  │
│  編輯（EDT）：selection、transaction、undo         │
│  文件模型（DOC）：node tree、stable ID、codec      │
│  ink 資料模型（INK）                              │
└──────────────┬──────────────────────────────────┘
               │ IPC：typed transaction 出、事件入
┌──────────────┴─ Authority（C++，out-of-process）┐
│  persistence、identity、permission、digest（ATH） │
└──────────────────────────────────────────────────┘
```

## 三條貫穿全域的約束

這三條不屬於任何單一能力，**任何能力的決策都不得違反**。

### 1. 語言邊界＝權威性邊界

判準是**錯誤會不會靜默**：

| 性質 | 歸屬 |
|---|---|
| 錯誤靜默（codec、狀態轉換、交易順序、規則求值、版面計算） | **C++**，必須可人工審查 |
| 錯誤可見（呈現、樣式、視覺回饋） | 平台外殼 |

### 2. client 在結構上無法成為權威

任何 client 的判斷都要能被 Krepis 否決。**client 不得是任何規則的唯一實作。**

推論：**一條規則只能有一個實作。** 同一條規則若在核心與外殼各有一份，兩者必然靜默分岔——
分岔沒有徵兆，直到資料已經壞掉。

### 3. FFI 邊界是 display list，不是物件圖

外殼**不得**逐元素向核心查詢位置。核心每幀產出一份連續緩衝區形式的 display list，
外殼零複製讀取後直接繪製。

理由：細粒度邊界的成本是 `O(可見元素數)` 次跨語言資料封送，這個形狀不可行。
瀏覽器引擎採 layout → display list → paint 的原因相同。

**推論（架構級，非最佳化）**：版面必須是**增量**的。打一個字重排整份文件是 `O(n)`，
文件一大即失效，因此版面引擎從第一版就必須只重算受影響區域。

## 兩個不可事後補的模型要求

以下兩項若不在第一版的模型裡，之後補會動到文件模型、selection 與 undo 的全部介面。

### composing region 是一級概念

IME 組字中的文字**會佔空間、會影響換行與游標位置**，因此核心必須看得見它。
文件模型天生區分「已提交文字」與「暫定文字」。

配套：**組字中的文字不進 undo stack**；一次確定才是一個 undo 單位。此規則在模型層表達，
不得依賴外殼自律（違反約束 2）。

### 位置的來源有兩種

流式版面的位置是**算出來的**，空間版面的位置是**存起來的**。同一份 ObjectStore 與
`Page → Container → Block` 模型要同時支援兩者，但不要求同一 Block 無條件維護兩個投影。
位置語意屬於擁有該 Block 的 Flow／Spatial Container，Block 本身不得假設位置來源；
跨位置重用以唯讀即時引用表達。具體模型見
[`DOC-0001`](decisions/01-document/DOC-0001-object-tree-stable-id-reference-and-composition.md)。

## 手寫是獨立圖層

自由筆跡**不進統一模型**。畫布（結構化節點的空間排版）與文件共用 selection 與 undo；
自由筆跡是獨立圖層，anchor 到文件位置。

手寫的品質門檻是**意圖捕捉**（求快不求準），不是繪圖產品——不需要平台專屬的低延遲預測 API。

## 平台策略

**先只做 Windows，但所有平台相關能力必須藏在介面後。**

介面的硬條件：**平台實作的輸出必須是值型別，不得洩漏平台 handle。**
例如文字 shaping 第一版用 DirectWrite（其字型 fallback 免費，CJK 場景不可或缺），
但介面回傳 glyph run ＋ metrics ＋ cluster 對應等純資料，使之後可換 HarfBuzz。

換 shaper 時版面輸出會改變（斷行、行高），golden 測試需重新基準化——這是已接受成本。
