# Spike 5：跨平台 text shaping 選型報告

## 觸發這個 spike 的決定

使用者於 2026-08-17 確認平台需求：

> **Windows + Linux + iPad 一定是有的，其次會延伸到 macOS、Android。**

這句話直接決定了選型，而且**排除了原本看似最省事的方案**。

## 為什麼「各平台用原生 API」出局

原本的折衷構想是：Windows 用 DirectWrite（spike 1 已驗過）、iPad／macOS 用 CoreText、
Linux／Android 用其他。但攤開平台清單後這條路是：

| 平台 | 原生 shaping API |
|---|---|
| Windows | DirectWrite |
| iPad / macOS | CoreText |
| Linux | 無統一原生方案（Pango／FreeType＋HarfBuzz） |
| Android | Minikin（內部使用 HarfBuzz） |

**至少三套獨立實作。** FND-0001 明文：一條規則只能有一個實作，雙實作必然靜默分岔。
Shaping 的分岔形式特別惡劣——同一段文字在兩個平台上斷行位置不同、游標命中位置不同，
而**兩邊都不會報錯**。這正是 FND-0001 的語言邊界判準要擋的東西（「錯誤會靜默的進 C++」）。

因此結論不是「HarfBuzz 比較好」，而是**跨平台需求把選項壓到只剩單一 stack**。

## 本 spike 驗證什麼

只驗最大的一塊：**HarfBuzz 負責 shaping**。bidi 與 grapheme 分段是第二階段。

要回答四個問題：

1. FetchContent 能不能取得並建置？（FND-0003 已定案用 FetchContent）
2. 二進位成本多少？
3. shaping 結果正確嗎？
4. API 複雜度如何？

---

## 結果

### 1. FetchContent：可行

```cmake
FetchContent_Declare(harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG 10.1.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(harfbuzz)
```

| 項目 | 數值 |
|---|---:|
| Configure（含 shallow clone） | 31 秒 |
| Build（Release，單一 target） | 9.8 秒 |
| 原始碼樹 | 92 MB |

HarfBuzz 的 CMake 支援完整，關掉不需要的元件即可：

```cmake
set(HB_BUILD_SUBSET  OFF)   # 字型子集化，排版不需要
set(HB_BUILD_TESTS   OFF)
set(HB_BUILD_UTILS   OFF)
set(HB_HAVE_FREETYPE OFF)   # 我們自己讀字型檔
set(HB_HAVE_GLIB     OFF)
set(HB_HAVE_ICU      OFF)   # 關鍵：不拖進 ICU
```

**`HB_HAVE_ICU OFF` 是重點**——HarfBuzz 可以獨立運作，不強制拖進 ICU 那幾十 MB。

Spike 刻意設為 opt-in（`-DKREPIS_BUILD_TEXT_SPIKE=ON`），使一般 configure 不受影響。

### 2. 二進位成本：772 KB

| 產物 | 大小 |
|---|---:|
| `harfbuzz.lib`（靜態封存） | 14.54 MB |
| `spike5_text_shaping.exe`（已連結） | 809 KB |
| `spike4_intrusive_vs_shared.exe`（無 HarfBuzz 對照） | 37 KB |
| **HarfBuzz 的實際連結成本** | **≈ 772 KB** |

**不要看 14.54 MB 那個數字。** 靜態封存含大量未使用的 section 與符號表，
連結器只取用到的部分。772 KB 才是實際付出的代價，對排版引擎而言完全可接受
（對照：ICU 完整版動輒 30 MB 以上）。

### 3. Shaping 正確性：通過

字型為 Segoe UI（Windows 內建，959 KB）。

| 測試 | 輸入 | 結果 | 判讀 |
|---|---|---|---|
| Kerning | `AVATAR To Yo` | 同一 gid=36（A）出現三次，advance 分別為 **2362 / 2348 / 2642** | **kerning 生效**——同字形因鄰居不同而調整前進量 |
| 組合字 | `cafe` + U+0301 | 5 codepoints → **4 glyphs**，末字 gid=112 | **正確合成**——e＋銳音符合成單一 é 字形 |
| 阿拉伯文 RTL | `مرحبا` | 5 glyphs，cluster 為 **8,6,4,2,0（遞減）** | **RTL 排序正確**，且套用了脈絡字形 |
| 拉丁 | `Waffle office` | 13 → 13 glyphs | 此字型未提供 ffl 合字，非 HarfBuzz 問題 |
| CJK 繁中 | `結構化筆記基座` | **7 glyphs 全為 .notdef** | Segoe UI 不含 CJK——**見下方重要發現** |

Kerning 與組合字這兩項是最有力的證據：它們證明 HarfBuzz 真的在執行 OpenType
的 GPOS／GSUB 表，而不只是把字元對應成字形。

### 4. API 複雜度：低

核心呼叫序列不到 10 行：

```cpp
hb_blob_t*   blob   = hb_blob_create(font_data, size, HB_MEMORY_MODE_READONLY, ...);
hb_face_t*   face   = hb_face_create(blob, 0);
hb_font_t*   font   = hb_font_create(face);
hb_font_set_scale(font, 64 * 64, 64 * 64);

hb_buffer_t* buffer = hb_buffer_create();
hb_buffer_add_utf8(buffer, utf8, -1, 0, -1);
hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
hb_shape(font, buffer, nullptr, 0);
// -> hb_buffer_get_glyph_infos / hb_buffer_get_glyph_positions
```

純 C API、生命週期以 `_destroy` 明確配對，包成 RAII 很直接。

---

## 重要發現：HarfBuzz 不做 font fallback

CJK 測試全部 `.notdef`，這**不是 HarfBuzz 的缺陷**——Segoe UI 本來就不含 CJK 字形。
但它暴露了一個必須在架構上處理的責任：

> **HarfBuzz 只對「一個字型 + 一段同書寫系統的文字」做 shaping。
> 決定「哪個字元該用哪個字型」不在它的範圍內。**

Font fallback 需要：列舉系統字型、查詢字型的 cmap 涵蓋範圍、按書寫系統挑選候選、
處理缺字。**這一層無法跨平台統一**，因為字型清單本身就是平台資源。

這對 FND-0001 的語言邊界產生一個新問題：

- Fallback 的**策略**（挑選順序、缺字如何降級）會靜默出錯 → 應在 C++
- Fallback 的**資料來源**（列舉系統字型）必然是平台專屬

建議形狀：C++ 定義 `FontProvider` 介面（列舉字型、查 cmap），各平台實作薄薄一層；
**挑選邏輯留在 C++ 且只有一份**。這樣分岔風險限縮在「有哪些字型可用」，
而不是「同一組字型會選出不同結果」。

**此事應另立決策（TXT-0001），不在本 spike 裁決。**

---

## 尚未驗證（第二階段）

本 spike 只證明 shaping 可行。完整的文字堆疊還缺三塊：

| 責任 | 候選 | 風險 |
|---|---|---|
| Bidi（雙向文字） | SheenBidi（小、專一）／ICU | 低——SheenBidi 約 100 KB |
| Grapheme 分群 | utf8proc（小）／ICU | 低——游標移動要用，錯了使用者看得見 |
| 斷行（line breaking） | ICU BreakIterator／自行實作 UAX #14 | **中高**——CJK 與泰文的斷行規則複雜 |
| Font fallback | 平台層 ＋ C++ 策略 | **中**——見上方 |

**斷行是下一個該做的 spike**，因為它是唯一可能需要拖進 ICU 的項目，
而 ICU 的體積會推翻「772 KB 可接受」這個結論。

> **更新（2026-08-18）：該閘門已通過。** Spike 6 證實 libunibreak（94 KB）
> 可完全取代 ICU 的 BreakIterator，涵蓋 UAX #14 斷行與 UAX #29 grapheme 分群，
> CJK 行首禁則正確。**本報告的體積結論維持成立。**
> 見 [`spike6-line-breaking-report.md`](spike6-line-breaking-report.md)。

## 判定

**HarfBuzz 作為 shaping 層：通過，建議採用。**

- 跨平台需求已排除原生 API 方案，這不是偏好而是約束推導的結果。
- 成本 772 KB，可接受。
- 正確性經 kerning、組合字、RTL 三項驗證。
- 不強制依賴 ICU。

**尚不足以定案整個文字堆疊。** 正式的 TXT 決策應等斷行 spike 完成後再寫，
否則會在不知道 ICU 是否必要的情況下承諾一個可能站不住的體積結論。

## 環境

- Windows 11、MSVC Release
- HarfBuzz 10.1.0
- 量測日期：2026-08-17
- ~~未在 Linux 上驗證~~ → **已驗證（2026-08-18）**：CI 的 `text-spikes-linux` job
  以 DejaVu 字型執行通過。**iPad／ARM 仍未驗證**——那是目前跨平台前提中唯一剩下的缺口。
