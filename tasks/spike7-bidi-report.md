# Spike 7：雙向文字（SheenBidi）—— 文字堆疊完成

## 判定

**SheenBidi 可取代 ICU 的 bidi。文字堆疊全部驗證完畢，總成本 913 KB，不含 ICU。**

| 元件 | 責任 | 連結後成本 | Spike |
|---|---|---:|---|
| HarfBuzz 10.1.0 | shaping（GSUB／GPOS、kerning、連字） | 771 KB | 5 |
| libunibreak 6.1 | UAX #14 斷行、UAX #29 grapheme／word 分群 | 94 KB | 6 |
| SheenBidi 2.8 | UAX #9 雙向文字重排 | **48 KB** | 7 |
| **合計** | | **913 KB** | |

對照 ICU 完整版 30 MB 以上——**約 33 倍的差距**。三個 spike 的結論一致：
跨平台文字堆疊不需要 ICU。

量測方式為連結後實際差值（`spike7_bidi.exe` 86 KB − 無文字依賴的對照 38 KB）；
`sheenbidi.lib` 靜態封存 145 KB 含未使用 section，不是實際成本。

---

## 正確性驗證

| 測試 | 輸入 | 結果 | 判讀 |
|---|---|---|---|
| 純 LTR | `Hello world` | 1 run，level 0 | ✓ |
| 純阿拉伯（自動偵測） | `مرحبا` | 基準 level 1，1 run | 自動偵測 RTL ✓ |
| 英文夾阿拉伯 | `The word مرحبا means hello` | 3 run：LTR/RTL/LTR | ✓ |
| **阿拉伯夾英文** | `مرحبا Krepis مرحبا` | run offset **17 → 11 → 0** | **回傳的是視覺順序**，見下 ✓ |
| **阿拉伯中的歐洲數字** | `مرحبا 2026 مرحبا` | `2026` 為 **level 2** | **最關鍵的一項** ✓ |
| 中性字元 | `abc مرحبا, def` | 逗號與空白正確歸屬 | ✓ |
| 無強字元 ＋ RTL fallback | `123 456.` | 基準 level 1 | fallback 正確生效 ✓ |
| 明確 level 1 | `Hello world` | 基準 1，內容 level 2 | 強制 RTL 正確 ✓ |

### 為什麼數字那一項最關鍵

歐洲數字在阿拉伯文脈絡中，依 UAX #9 必須取得 base+2 的 level，
也就是**在 RTL 段落中維持 LTR**。SheenBidi 回報 `2026` 為 level 2 —— 正確。

若這裡算成 level 1（跟著 RTL 走），畫面上會顯示成 `6202`。
**這種錯誤是靜默的**：文字仍然顯示、沒有例外、沒有警告，而寫程式的人若不讀阿拉伯文
根本不會察覺。這正是 FND-0001「錯誤會靜默的東西進 C++」判準所指的情況。

### run 陣列已是視覺順序

`مرحبا Krepis مرحبا` 的 run offset 為 **17 → 11 → 0**（遞減）。
SheenBidi 回傳的 run 已經完成重排，**渲染時依序擺放即可，不需自行反轉**。
這消除了一整類「自己實作重排」會產生的錯誤。

---

## 我的預期錯了一次（記錄下來）

原本測試案例寫著「`SBLevelDefaultRTL` → 強制 RTL 基準」，實測卻得到 level 0。
查 header 後確認**是我的預期錯了，不是函式庫錯了**：

```c
/** A value specifying to set base level to one (right-to-left)
    if there is no strong character. */
#define SBLevelDefaultRTL  0xFD
```

`SBLevelDefault*` 是「**若無強方向字元**才採用的 fallback」，不是強制。
`Hello world` 含強 LTR 字元，自動偵測勝出為 LTR —— 這是正確的 UAX #9 行為。
真正要強制 RTL 必須直接傳明確 level `1`。

測試已改為涵蓋三種情況（fallback 未生效、fallback 生效、明確強制），全部通過。

---

## 整合成本

兩項都不大，但都必須寫進決策而非只留在 CMake 註解：

### 1. 沒有 CMakeLists（與 libunibreak 相同）

SheenBidi 提供 Makefile 與 meson，沒有 CMake。需 `FetchContent_Populate` ＋ 自寫 target。

**但比 libunibreak 好**：SheenBidi 提供 unity build，只需編譯單一檔案：

```cmake
add_library(sheenbidi STATIC ${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c)
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(sheenbidi PUBLIC ${sheenbidi_SOURCE_DIR}/Headers)
target_include_directories(sheenbidi PRIVATE ${sheenbidi_SOURCE_DIR}/Source)
```

因此**升級時不需要人工同步原始碼清單**——libunibreak 的那項維護負擔在這裡不存在。

### 2. Header 沒有 `extern "C"` 護欄

從 C++ 引入會使符號被 C++ mangling，連結期才報「找不到外部符號」。必須自行包裝：

```cpp
extern "C" {
#include <SheenBidi.h>
}
```

漏掉不會在編譯期報錯，只會在連結期出現一整串 unresolved symbol，
**訊息不會指向真正原因**。這一點值得寫進整合說明。

---

## 文字堆疊的完整形狀

三個 spike 完成後，形狀可以定案：

```
輸入 UTF-8
   │
   ├─ SheenBidi ─────→ 視覺順序的 run（含 embedding level）
   │
   ├─ libunibreak ───→ UAX #14 候選斷點、UAX #29 grapheme 邊界
   │                     ↓
   │                   Krepis 政策層（C++，唯一實作）
   │                     · 抑制 URL 內部斷行
   │                     · 數字與單位不斷
   │                     · 使用者自訂不斷點
   │
   └─ HarfBuzz ──────→ 每個 run 的 glyph 與位置
                         ↑
                   FontProvider（平台層提供字型清單）
                   ＋ fallback 策略（C++，唯一實作）
```

**兩個「唯一實作」是刻意的**：斷行政策與 font fallback 策略都會靜默出錯，
依 FND-0001 必須留在 C++ 且不得有第二份實作。平台只提供資料（字型清單），不做判斷。

## 可以寫 TXT 決策了

前兩個 spike 之後我都寫了「尚不足以定案」，理由是剩餘風險未量測。**現在三塊都量完了**：

- shaping：HarfBuzz，771 KB ✓
- 斷行／分群：libunibreak，94 KB ✓
- bidi：SheenBidi，48 KB ✓

剩下的未知只有 **font fallback**，但它不是選型問題（沒有第三方庫可選，
必然是平台層 ＋ 自寫策略），因此不阻擋文字堆疊的選型定案。

建議寫成 `TXT-0001：跨平台文字堆疊選型`，內容涵蓋三個元件、拒絕 ICU 的理由、
兩項整合成本，以及兩個「唯一實作」的邊界。font fallback 另立 `TXT-0002`。

## 尚未涵蓋

- **ARM（iPad）與 Linux 未實測**——三個 spike 都只在 Windows/MSVC 驗證，
  而跨平台正是整個選型的前提。應併入 CI（見同批的 CI 工作項）。
- 未測效能：shaping 與斷行的延遲尚未進入 frame budget 的核算。
- 未測記憶體：三個函式庫的 Unicode 資料表常駐大小未量。

## 環境

- Windows 11、MSVC Release
- SheenBidi 2.8（`v2.8` tag）
- 量測日期：2026-08-18
