# 05-ink（`INK`）

## 決策流程例外

本領域不套用 [`spec/decisions/README.md`](../README.md) 的 AI 自動選擇全局最佳方案與強制文件展開
流程。Ink 資料模型、anchor、reflow 與輸入手感由人類主導；AI 可以整理選項、實作或依明確要求補
架構圖，但不得因其他決策領域的授權自行核准 Ink 語意。

此例外不取消 Krepis 的領域中立、單一權威、證據、測試與繁體中文文件規則。

## 範圍

ink 資料模型、圖層語意、anchor 到文件位置。**不含繪製**——實際畫出來屬於平台外殼。

## 什麼該放這裡

**「一筆手寫是什麼、它跟文件的關係是什麼」的決策。**

## 前提（已定）

- **自由筆跡是獨立圖層，不進統一模型。** 畫布（結構化節點的空間排版）與文件共用
  selection／undo；自由筆跡不共用。
- **品質門檻是意圖捕捉，不是繪圖產品。** 求快不求準，不需要平台專屬的低延遲預測 API，
  不需要筆鋒漸細與向量重編輯。此前提若改變，成本會回到繪圖產品的量級。

## 已定方向（人類決定）

### Ink 越界行為（2026-08-17 人類決定）

筆跡 anchor 到所屬 Block，但允許超出 Block 邊界繪製：

- **落點歸屬**：若筆跡跨越多個 Block 邊界，以 **ink 中心點**計算落點，歸屬到中心所在的
  Block。
- **頁面邊界裁切**：
  - 非全寬模式下的有效區域 = Block 寬度 + 互動區。超出此範圍的部分裁切。
  - 超出頁面高度的部分裁切。
- **一般／全寬切換**：切換時**水平方向等比拉伸** ink 座標，使筆跡隨寬度縮放保持相對位置。
  垂直方向不變。
- reflow 後筆跡隨所屬 Block 頂部移動（anchor 是 Block 級），Block 內座標不變。
  這意味著 reflow 可能造成筆跡與文字內容錯位——這是已接受的取捨，不自動修復。

### 取樣、快速路徑與工具語意（2026-08-17～2026-08-21 人類決定）

- 一個 sample 使用 9 bytes：正規化 `x`、相對 Block 頂部的 `y`、pressure、tilt 與時間差。
- tilt azimuth 使用 `uint8`；只有真實 Apple Pencil fixture 顯示可見角度階梯時，才能重開為
  `uint16`／10 bytes sample。
- Flutter 外殼持有尚未提交的 in-progress stroke；pen-up 時才以一個 transaction 提交到 Krepis。
- 繪製中與提交後的筆畫外框都必須呼叫 C++ 的同一份 outline 實作。不得在 Flutter 複製近似版。
  每幀 FFI 成本是強制 benchmark；超出 frame budget 時重開方案，不以雙實作規避。
- Ink lasso 是受限 selection：第一版只允許選取、移動與刪除整筆，不取得文字 caret／range 語意，
  不擴張 EDT-1 的兩種主要 selection。
- Ink undo 使用記憶體預算，不使用固定筆數；初始上限為 256 MiB，超過時淘汰最舊交易。

### 階段邊界（2026-08-21 人類決定）

- P1 保留每個 Block 可擁有 `InkOverlay` 的模型能力與既有 Ink 資料型別，但不把手寫輸入、擦除、
  lasso 或 Ink undo 納入 P1 驗收。
- Ink 整合留在 P3；Apple Pencil 手感與延遲仍在 iPad 平台路徑驗收。
- P1.5 codec 只有在最小 `ParagraphRecord` 與原子 `Transaction` 完成後才實作；若 dogfood 納入
  手寫，格式必須保存 `InkStroke` 與 `BrushStyle`，並明文標示之後會被丟棄。

## 尚未決定／尚待量測

- anchor 的正式內部型別仍須在 Paragraph／Block schema 定案後寫入 ADR。
- C++ outline 的每幀 FFI 成本尚未量測。
- 256 MiB undo 預算的交易計費、共享資料去重與淘汰行為尚未實作及驗證。
- tilt azimuth 精度尚未以真實 Apple Pencil fixture 驗證。
