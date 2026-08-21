# 01-document（`DOC`）

## 範圍

node tree 的形狀、stable ID、schema 與其版本遷移、序列化 codec。

## 什麼該放這裡

**「文件是什麼」的決策。** 「文件長什麼樣」屬於 `02-layout`；「怎麼改文件」屬於 `04-editing`。

## 決策

- [`DOC-0001`](DOC-0001-object-tree-stable-id-reference-and-composition.md)：物件樹、stable ID、
  即時引用與 composing region 的模型位置（**Accepted**）。
- [`DOC-0002`](DOC-0002-object-id-representation-and-generation.md)：ObjectId 的位元表示、
  強型別包裝、生成與正規編碼（**Accepted**）。補上 DOC-0001 留給後續的四項，使 ObjectId 可實作。
- [`DOC-0003`](DOC-0003-disposable-dogfood-file.md)：Q1=A 的可丟棄本機檔案、重建邊界與原子替換
  （**Accepted**；P4 必須丟棄）。

## 待決問題

- 正式 schema 與 migration 策略仍待 `ATH-*`；DOC-0003 明確不建立 migration chain。
