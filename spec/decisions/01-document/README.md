# 01-document（`DOC`）

## 範圍

node tree 的形狀、stable ID、schema 與其版本遷移、序列化 codec。

## 什麼該放這裡

**「文件是什麼」的決策。** 「文件長什麼樣」屬於 `02-layout`；「怎麼改文件」屬於 `04-editing`。

## 決策

- [`DOC-0001`](DOC-0001-object-tree-stable-id-reference-and-composition.md)：物件樹、stable ID、
  即時引用與 composing region 的模型位置（**Accepted**）。

## 待決問題

- **schema 版本策略**：第一版即需決定「舊檔怎麼讀」，否則 P1.5 開始 dogfood 後的資料無法演進。
- **第一版 codec 是刻意可丟棄的**：P1.5 只需要能存檔以支撐 dogfood，正式格式待 `ATH-*`。
  這一點必須在決策裡寫明，否則會被誤當成正式格式沿用。
