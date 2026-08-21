# 04-editing（`EDT`）

## 範圍

selection 模型、typed transaction、undo／redo。

## 什麼該放這裡

**「怎麼改文件、以及怎麼還原」的決策。** 文件的形狀屬於 `01-document`。

## 決策

- [`EDT-0001`](EDT-0001-selection-transaction-and-undo.md)：兩種主要 selection、單層原子
  Transaction、command-specific merge 與全域 undo。

## 尚待量測

- **文字 undo 合併採固定時間窗**：連續打字若在閾值時間內，合併為同一個 undo 步。
  閾值為 benchmark 參數，初始值參考 VS Code（約 1 秒打字暫停）。IME 一次確定已由
  [`DOC-0001` D10](../01-document/DOC-0001-object-tree-stable-id-reference-and-composition.md)
  定為一個 undo 單位，此時間窗只影響確定後的連續非 IME 打字。

時間窗只影響已確定的一般文字輸入；實際 EDT-5 閾值待真實 workload benchmark。
