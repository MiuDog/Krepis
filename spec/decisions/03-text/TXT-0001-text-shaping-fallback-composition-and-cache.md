# TXT-0001：文字 shaping、fallback、斷行政策、composition 與快取

## 狀態

**Accepted**

## 日期

- 提出：2026-08-18
- 接受：2026-08-18（使用者逐題裁決 TXT-1～TXT-4）
- 正式化：2026-08-21

## 背景

P1 必須正確處理注音 composition、CJK fallback、雙向文字與 URL。Spike 5～7 已驗證
HarfBuzz、libunibreak 與 SheenBidi 可用；也證實 HarfBuzz 不會替缺字自動選 fallback 字型。
若 fallback 或斷行政策散落在平台外殼，Windows、Linux 與 iPad 會形成三份靜默分岔的規則。

詳細資料流見
[`docs/architecture/text-shaping-data-flow.md`](../../../docs/architecture/text-shaping-data-flow.md)。

## 專業名詞

| 名詞 | 定義 |
|---|---|
| shaping | 把 Unicode 文字與字型轉為 glyph id、位置、advance 與 cluster 對應 |
| grapheme cluster | 使用者視為一個可移動／刪除單位的一組 code point |
| fallback | 首選字型缺 glyph 時，依同一政策選擇能覆蓋該文字的字型 |
| composing overlay | 核心持有但不進 ObjectStore／undo 的暫時文字覆蓋層 |
| script | 文字使用的書寫系統，例如 Latin、Han、Arabic |
| font-set revision | 平台可用字型集合改變時遞增的版本，屬 shaping cache key |

## D1：跨平台文字堆疊與純值輸出

核心使用 HarfBuzz shaping、libunibreak UAX #14 候選斷點及 SheenBidi 雙向分析。平台只透過
`FontProvider` 提供可用字型的純資料與字型 bytes；挑選 fallback、分段與政策過濾只在 C++ 實作。
輸出是 glyph run、metrics 與 cluster mapping，不得洩漏 DirectWrite、CoreText 或其他平台 handle。

### 複雜度

令 `T` 為段落 code point 數、`F` 為候選 fallback 字型數。Bidi、候選斷點與 shaping 對文字長度
線性；fallback 最壞為 `O(T × F)`，實作必須按 script／coverage 索引候選集合，不能每個字元掃描
全部系統字型。

## D2：斷行採 UAX #14，再抑制 URL／路徑內部候選點

libunibreak 先產生候選斷點，核心再辨識 URL 與檔案路徑 token，移除 token 內部候選點。NBSP 等
Unicode 原生規則仍由 UAX #14 處理；第一版不加入數字＋單位或使用者自訂禁斷屬性。

例：`請看 https://example.com/path 然後繼續` 可以在 URL 前後斷行，但不能在 `/path` 內斷開。

## D3：composition 是核心的非權威覆蓋層

Composition 保存 `{owner session, BlockId, TextAnchor, utf8 text, phase}`，參與 shaping、line breaking、
extent、paint 與 caret hit-test，但不寫入 `ObjectStoreSnapshot`、不進 undo、不持久化，也不讓其他
協作者看見。確定時才轉成一個 `Transaction`；取消時直接移除 overlay。

被編輯 Block 移動時，overlay 依 stable `BlockId` 保留；Block 被刪除時取消。其他人的正式修改不得
把 overlay 變成權威文字，重新定位規則依 `DOC-0001` D10～D15 執行。

注音輸入的 phonetic phase 與已有候選中文字但尚未確定的 candidate phase 都使用同一 overlay
資料結構；phase 只影響 IME 呈現與事件，不改變權威性。

## D4：shaping cache 使用完整 dependency key 與 LRU

鍵至少包含 `{text hash, font-set revision, font size, script, language, direction, feature set}`。
Composition 必須把 overlay 後的文字與其 session-local revision 納入鍵，不能誤用正式內容的 cache。
值只含純資料 glyph run。容量與淘汰門檻屬 TXT-5 benchmark 參數，不在本 ADR 猜測。

### Invariant 與拒絕行為

- UTF-8 不合法、cluster mapping 越界或缺少必要字型資料時回傳明確錯誤，不產出部分 run。
- 平台不能提交「已挑好的 fallback 結果」覆寫核心政策。
- Cache miss 只能重建，不能退回平台 shaping。
- Font-set revision 改變時，舊 key 自然不可命中，不需原地修改既有 immutable 結果。

## 後果與驗證

- 三平台共用一份 fallback 與 URL 禁斷規則，代價是每平台仍需薄的字型列舉 adapter。
- P1 的 composition 路徑必須同步完成並守住 p99 ≤ 3ms；實際 shaping 占比由 TXT-5 benchmark 決定。
- 測試必須包含注音、多 code-point grapheme、缺字 fallback、RTL 與 URL／Windows path。
- 換文字堆疊或字型版本可能改變 glyph metrics；視覺 baseline 變更必須明文說明。

## 尚未決定

- Shaping cache 容量、實際記憶體預算與同步路徑占比（TXT-5）。
- 各平台 FontProvider 的列舉 API 與 iPad／ARM 實測數字。
