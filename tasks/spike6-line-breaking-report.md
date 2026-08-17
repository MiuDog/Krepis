# Spike 6：不引入 ICU 的斷行與分群

## 這個 spike 為什麼存在

Spike 5 得出「HarfBuzz 772 KB 可接受」，但那只涵蓋 shaping。該報告自己寫下：

> **斷行是下一個該做的 spike**，因為它是唯一可能需要拖進 ICU 的項目，
> 而 ICU 的體積會推翻「772 KB 可接受」這個結論。

**本 spike 就是那道閘門。** 結果：**閘門通過，Spike 5 的結論維持成立。**

## 判定

**libunibreak 可完全取代 ICU BreakIterator。全文字堆疊不需要 ICU。**

| 元件 | 責任 | 連結後成本 |
|---|---|---:|
| HarfBuzz 10.1.0 | shaping（GSUB／GPOS） | 771 KB |
| libunibreak 6.1 | UAX #14 斷行、UAX #29 grapheme／word 分群 | **94 KB** |
| SheenBidi（尚未驗證） | UAX #9 雙向文字 | 約 100 KB（估） |
| **合計（估）** | | **約 1 MB** |

對照：ICU 完整版 30 MB 以上。**避免了約 30 倍的體積差。**

量測方式為連結後的實際差值，非靜態封存大小：

| 產物 | 大小 |
|---|---:|
| `spike6_line_breaking.exe` | 132 KB |
| `spike4_intrusive_vs_shared.exe`（無文字依賴的對照） | 38 KB |
| **libunibreak 實際成本** | **94 KB** |
| `unibreak.lib`（靜態封存，含未使用 section） | 189 KB |
| libunibreak 原始碼樹 | 2.3 MB |

---

## 正確性驗證

### UAX #14 斷行

| 測試 | 輸入 | 結果 | 判讀 |
|---|---|---|---|
| 英文空白 | `The quick brown fox jumps` | 4 個斷點 | 對應 4 個空白 ✓ |
| 連字號 | `well-known state-of-the-art` | 5 個斷點（含各 `-` 之後） | UAX #14 的 hyphen 規則生效 ✓ |
| **CJK 禁則** | `結構化筆記基座，是一個實驗。` | 11 個斷點 | **見下方詳析** ✓ |
| **CJK 禁則** | `測試。測試` | 斷點在 2、8、11，**不在 5** | 句號前不可斷 ✓ |
| 中英混排 | `使用 HarfBuzz 做 shaping` | 4 個斷點 | 字／詞邊界皆正確 ✓ |
| URL | `https://example.com/path` | 2 個斷點（`//` 與 `/` 之後） | UAX #14 正確，但**需要政策層**，見下 |

**CJK 禁則詳析**（每個漢字 3 bytes）：

```
結(0) 構(3) 化(6) 筆(9) 記(12) 基(15) 座(18) ，(21) 是(24) 一(27) 個(30) 實(33) 驗(36) 。(39)
斷點： 2     5     8     11    14     17     ✗      23     26     29     32     35     ✗
                                            ↑                                        ↑
                                    位置 20 無斷點                            位置 38 無斷點
```

位置 20（`座` 之後、`，` 之前）與位置 38（`驗` 之後、`。` 之前）**都沒有斷點**——
標點不得出現在行首，這正是 CJK 排版的行首禁則。**libunibreak 正確實作了禁則規則**，
這是最關鍵的 CJK 正確性訊號，也是自行實作 UAX #14 最容易做錯的部分。

### UAX #29 grapheme 分群（游標移動用）

| 輸入 | bytes | cluster 數 | 判讀 |
|---|---:|---:|---|
| `café`（e ＋ U+0301 組合） | 6 | 4 | 組合序列算一個 cluster ✓ |
| 👨‍👩‍👦（ZWJ 家族序列） | 18 | **1** | **游標不會走進 emoji 內部** ✓ |
| `क्ष`（天城文組合） | 9 | 1 | 印度系文字的組合字正確 ✓ |

Emoji ZWJ 那一項最重要：若回報 3 個 cluster，使用者按一次方向鍵就會把家庭 emoji 拆開，
是**立刻看得見**的錯誤。libunibreak 正確回報 1。

---

## 整合成本：需要自寫建置膠水

**libunibreak 沒有 `CMakeLists.txt`**——它是 autotools 專案。
因此 `FetchContent_MakeAvailable()` 只會下載原始碼，不會產生任何 target，
而且**不會報錯**（`target_link_libraries` 會把名稱當成裸函式庫名，延後到連結期才失敗）。

正確做法是 `FetchContent_Populate()` 之後自行宣告 target：

```cmake
FetchContent_Populate(libunibreak)
add_library(unibreak STATIC
    ${libunibreak_SOURCE_DIR}/src/linebreak.c
    ${libunibreak_SOURCE_DIR}/src/linebreakdata.c
    ${libunibreak_SOURCE_DIR}/src/graphemebreak.c
    # ...（共 14 個 .c，排除 tests.c）
)
target_include_directories(unibreak PUBLIC ${libunibreak_SOURCE_DIR}/src)
```

成本約 20 行，一次性。**但這是維護負擔**：libunibreak 新增原始碼檔時，
我們的清單不會自動更新，且**不會有編譯錯誤**——只會缺少符號或缺少某些 Unicode 資料。
升級版本時必須人工比對 `Makefile.am` 的檔案清單。

這一點應寫進未來的 FND 或 TXT 決策，不要只留在 CMake 註解裡。

---

## 尚未涵蓋

| 責任 | 狀態 |
|---|---|
| UAX #9 雙向文字（bidi） | **未驗證**——SheenBidi 是候選，體積估 100 KB |
| Font fallback | 未驗證；Spike 5 已確認不屬於 HarfBuzz，需平台層 ＋ C++ 策略 |
| 斷行政策層 | 見下 |
| ARM（iPad）平台 | 兩個 spike 都只在 Windows/MSVC 驗證 |

### 斷行需要一層政策，不只是演算法

URL 測試顯示 libunibreak 在 `//` 與 `/` 之後給出斷點。**這符合 UAX #14**，
但多數編輯器會抑制 URL 內部斷行。同類的還有：

- 數字與單位之間（`100 kg`）通常不斷
- 使用者自訂的不斷空白
- 專有名詞

因此正確的分層是：**libunibreak 提供 UAX #14 的候選斷點，Krepis 在其上套用政策過濾**。
政策屬於會靜默出錯的部分（斷錯了使用者未必立刻察覺，但排版就是錯的），
依 FND-0001 應留在 C++ 且只有一份實作。

---

## 對決策的影響

1. **Spike 5 的體積結論維持成立。** 原本標記為「可能推翻」的風險已解除。
2. 文字堆疊的形狀可以定案為：**HarfBuzz ＋ libunibreak ＋ SheenBidi，不含 ICU**。
3. 仍**不建議現在就寫 TXT 決策**——bidi 尚未驗證。雖然剩餘風險已從「可能 30 MB」
   降到「可能多 100 KB」，但依「未量測不承諾」的原則，等 SheenBidi spike 完成再定案。
4. 新增一項必須寫進決策的維護負擔：**libunibreak 的原始碼清單需人工同步**。

## 環境

- Windows 11、MSVC Release
- libunibreak 6.1（`libunibreak_6_1` tag）
- 量測日期：2026-08-18
- 未在 Linux／iPad 驗證——應併入 CI 的 Linux job
