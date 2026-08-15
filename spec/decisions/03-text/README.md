# 03-text（`TXT`）

## 範圍

文字 shaping、grapheme cluster、字型 fallback、IME 與 composing region。

## 什麼該放這裡

**「一串字元怎麼變成可量測的字形」的決策。** 那些字形**怎麼排**屬於 `02-layout`。

## 決策

（尚無）

## 待決問題

- **shaping 介面的值型別定義**：第一版以 DirectWrite 實作（其字型 fallback 免費，
  CJK 場景不可或缺），但介面**必須回傳純資料**（glyph run ＋ metrics ＋ cluster 對應），
  **不得洩漏平台 handle**——洩漏了就永遠換不掉。
- **換 shaper 的已接受成本**：改用 HarfBuzz 時版面輸出會變（斷行、行高），
  golden 測試需重新基準化。此成本落在多平台階段。
- **grapheme cluster 的來源**：自實作、ICU、或平台 API？影響游標移動與刪除的正確性。
- **composing region 的跨邊界契約**：IME 狀態由外殼擁有，但組字文字會佔空間、影響換行與
  游標位置，因此核心必須看得見。此契約與 `01-document`、`04-editing`、`07-binding` 都相關。
- **注音輸入是第一驗收場景**，不是邊緣案例。
