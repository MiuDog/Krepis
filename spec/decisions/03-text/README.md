# 03-text（`TXT`）

## 範圍

文字 shaping、grapheme cluster、字型 fallback、IME 與 composing region。

## 什麼該放這裡

**「一串字元怎麼變成可量測的字形」的決策。** 那些字形**怎麼排**屬於 `02-layout`。

## 決策

- [`TXT-0001`](TXT-0001-text-shaping-fallback-composition-and-cache.md)：HarfBuzz ＋ libunibreak ＋
  SheenBidi、核心 fallback、URL／路徑禁斷、composition overlay 與 shaping cache。

## 尚待量測／平台實作

- Shaping cache 容量與同步路徑占比（TXT-5 benchmark）。
- 各平台 `FontProvider` adapter 與 iPad／ARM 實測。
- 注音輸入仍是第一驗收場景，不是邊緣案例。
