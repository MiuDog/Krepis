# 07-binding（`BND`）

## 範圍

FFI 邊界、client 契約、平台外殼介面。

## 什麼該放這裡

**「核心與外殼之間傳什麼、怎麼傳」的決策。** display list 的**格式**屬於 `02-layout`；
它**怎麼跨邊界**屬於這裡。

## 前提（已定，來自 `FND-0001` 與架構總覽）

- **邊界是 display list，不是物件圖。** 外殼不得逐元素查詢位置——那個形狀的成本是
  `O(可見元素數)` 次跨語言資料封送，不可行。
- **client 在結構上無法成為權威**；client 不得是任何規則的唯一實作。

## 決策

- [`BND-0001`](BND-0001-display-list-buffer-command-and-version.md)：固定 binary header、TLV command、
  核心雙緩衝、同步 command、版本與錯誤 fail-closed。

## 尚待量測／定義

- 最終 opcode 與 payload schema。
- BND-5 封送成本、buffer 容量保留及縮減門檻。
