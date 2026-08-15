# P0 Spike 驗證報告與架構裁決

日期：2026-08-16  
測試環境：Windows 11 Home (Build 26200), MSVC C++20, Dart SDK 3.12.2, DirectWrite, 120Hz 顯示預算基準 (8.33ms)。

---

## 一、 執行摘要

本報告記錄 Krepis 專案進入正式實作前必須完成的 **P0 三大 Spike** 實測結果：
1. **Spike 1（文字 Shaping 選型）**：DirectWrite 純值型別抽取與多語系/Bidi/Fallback 實測。
2. **Spike 2（FFI 與 Display List 零複製）**：C++ ↔ Dart FFI 在 120Hz 連續捲動與打字增量排版下的每幀往返延遲（**生死門檻**）。
3. **Spike 3（交易邊界）**：In-Process 記憶體狀態與 Out-of-Process Authority 提交點評估。

### 核心結論：**架構成立，方案 A 正式通過**

- **捲動每幀延遲**：在 $N = 10,000$ 段落的超長文件下，Dart FFI + C++ 視窗裁剪 + Display List 零複製讀取的 **p99 延遲僅 0.42ms**（佔 120Hz 總預算 8.33ms 的 **5.04%**）。
- **打字增量延遲**：單段修改觸發 DirectWrite Shaping + 增量版面重排 + Display List 產出之 **p99 延遲僅 0.72ms**，且耗時為 $O(1)$，完全不隨文件總長度 $N$ 增長。
- **純值型別隔離**：成功將 Windows DirectWrite 的 GlyphRun、Metrics、ClusterMap、BidiLevels 完整取出為純 POD 值型別，完全無平台 Handle 洩漏。

---

## 二、 Spike 1：文字 Shaping 與 DirectWrite 選型實測

### 1. 驗證項目
- [x] 多語系與繁體中文（Segoe UI / JhengHei Fallback）、Emoji、Bidi 阿拉伯文混排正確性。
- [x] Glyph Index、Advance X/Y、Offset X/Y、Cluster Map、Bidi Level、Ascent/Descent 純值型別提取。
- [x] 不洩漏任何 COM / Windows 平台 Handle。

### 2. 實測數據 (Benchmark)
| 測試情境 | 長度 | 平均耗時 | p50 | p90 | p99 | Max |
|---|---|---|---|---|---|---|
| **單行短句 (10 字)** | 36 bytes | **25.3 μs** | 24.5 μs | 25.3 μs | 41.4 μs | 76.3 μs |
| **單個段落 (60 字)** | 161 bytes | **136.8 μs** | 131.4 μs | 144.9 μs | 216.4 μs | 289.1 μs |
| **中長段落 (300 字)** | 425 bytes | **455.7 μs** | 439.8 μs | 482.9 μs | 710.2 μs | 895.6 μs |

### 3. 裁決
- 單段 60 字的 Shaping 僅需 ~0.13ms，完全具備即時打字重排的餘裕。
- 介面成功封裝為值型別，未來可直接平移抽換為 HarfBuzz / ICU。

---

## 三、 Spike 2：FFI 邊界延遲與 Display List 零複製實測 (生死門檻)

### 1. 實測架構
- **C++ 核心**：Dual-buffered Arena (512KB Bump Allocator)、Frustum/Viewport 視窗裁剪、增量段落排版。
- **C ABI**：純 C 函式介面、Opaque Handle、Exception-safe。
- **Dart 外殼**：`dart:ffi`、`ByteData.view` 零複製解析 Display List 二進位指令。

### 2. 實測數據矩陣

#### A. 120Hz 持續捲動表現 (1,000 幀)
| 文件大小 ($N$ 段落) | 總高度 (px) | 平均耗時 | p50 | p90 | p99 | Max | 佔 120Hz 預算 (8.33ms) |
|---|---|---|---|---|---|---|---|
| **$N = 100$** | 2,800 px | 278.3 μs | 272.0 μs | 288.0 μs | **397.0 μs** | 1667.0 μs | **4.76%** |
| **$N = 1,000$** | 28,000 px | 278.5 μs | 275.0 μs | 289.0 μs | **405.0 μs** | 510.0 μs | **4.86%** |
| **$N = 10,000$** | 280,000 px | 298.0 μs | 295.0 μs | 310.0 μs | **420.0 μs** | 516.0 μs | **5.04%** |

#### B. 打字增量編輯往返表現 (100 次按鍵，含 Shaping + Layout + FFI + Zero-copy)
| 文件大小 ($N$ 段落) | 編輯位置 | 平均耗時 | p50 | p90 | p99 | 複雜度趨勢 |
|---|---|---|---|---|---|---|
| **$N = 100$** | 中間段落 | 438.1 μs | 403.0 μs | 649.0 μs | **713.0 μs** | $O(1)$ |
| **$N = 1,000$** | 中間段落 | 411.1 μs | 397.0 μs | 442.0 μs | **675.0 μs** | $O(1)$ |
| **$N = 10,000$** | 中間段落 | 445.9 μs | 437.0 μs | 466.0 μs | **718.0 μs** | $O(1)$ |

### 3. 對 `LAY-0001` 的裁決
- **方案 A（以視窗劃分）正式成立**：
  視窗內的增量重排與 Display List 產出穩定在 **0.3ms ~ 0.7ms** 完成，遠低於預算上限（8.33ms），保留了 90% 以上的時間給 Flutter UI 執行緒進行事件分派與 GPU 繪製。
- **增量性驗證通過**：耗時隨 $N$ 從 100 增長至 10,000 完全無退化現象（始終穩定在 ~0.4ms）。

---

## 四、 Spike 3：In-Process ↔ Out-of-Process 交易提交點評估

| 策略 | UI 阻塞延遲 (p99) | 磁碟/IPC 開銷 | Crash 遺失視窗 | 建議結論 |
|---|---|---|---|---|
| **策略 A (Per-keystroke 同步)** | 1.5ms ~ 3ms (易卡頓) | 極高 | 0ms | 淘汰 (高頻輸入掉幀) |
| **策略 B (300ms 閒置防抖)** | 0.05ms | 低 | 3~5 秒 / 整段未送出的字 | 備選 |
| **策略 C (記憶體環形 WAL + 異步背景批次 Flush)** | **< 0.01ms** | 批次合併，極低 | **最大 100ms / 10 字元** | **選定方案** |

**決策結論**：採用策略 C。UI 執行緒將 Micro-mutation 寫入 In-process Lock-free 環形日誌（耗時 < 10μs），背景工作執行緒每 100ms 或累積批次非同步 Flush 至 Out-of-process Authority，兼顧極致響應與資料安全性。

---

## 五、 後續行動建議

1. **核准 `LAY-0001`**（狀態從 `Proposed` 改為 `Accepted`，選定方案 A）。
2. **正式進入 P1 垂直切片**：
   - 核心（C++）：Node Tree、Single Block、增量流式/空間排版、Composing Region、Selection、Undo/Redo。
   - 外殼（Jotist）：Flutter 雙視圖並列 Display List 渲染與鍵盤/IME 串接。

---

## 六、事後複驗：增量性主張的更正（2026-08-16）

### 觸發原因

第三節的結論宣稱打字增量重排「耗時為 $O(1)$，完全不隨文件總長度 $N$ 增長」。
該結論由 $N \in \{100, 1000, 10000\}$ 三點推得，**測試範圍不足以分辨 $O(1)$ 與
「$O(N)$ 但常數項很小」**——單段 DirectWrite shaping 約 130μs，會蓋住 $N \le 10^4$ 時
只有數十微秒的線性項。

### 複驗方法

新增 `spikes/spike2_ffi_displaylist/spike2_scaling_test.cpp`（原生 C++，Release），
沿用同一個 C ABI，把 $N$ 拉到百萬級，量測「編輯中間段落 → `krepis_engine_layout`」往返。

### 實測結果

| $N$ | avg | p50 | p99 | 相對 $N=1{,}000$ |
|---|---|---|---|---|
| 1,000 | 48.7 μs | 47.3 μs | 80.5 μs | 1× |
| 10,000 | 59.7 μs | 58.0 μs | 89.0 μs | 1.2× |
| 100,000 | 145.6 μs | 136.8 μs | 387.8 μs | **3×** |
| 1,000,000 | **4,074.9 μs** | 4,073.5 μs | **4,888.3 μs** | **84×** |

$N$ 由 $10^5$ 增至 $10^6$（10 倍）時耗時增為 **28 倍**——線性再疊加快取失效。

### 根因

`spikes/spike2_ffi_displaylist/engine_abi.cpp` 的 `layout()`：

```cpp
for (auto& item : m_paragraphs) {      // ← 全掃
    if (item.is_dirty) { /* shaping 才是增量的 */ }
    item.y_offset = current_y;          // ← 每個段落都重算
    current_y += item.height + 4.0f;
}
```

**shaping 是增量的，但 y 座標累加是每次全掃。**

### 對既有結論的影響

| 原結論 | 複驗後 |
|---|---|
| 方案 A（以視窗劃分）成立 | ✅ **維持成立**。捲動不觸發 `layout()`（`m_layout_dirty` 為 false 即返回），捲動確為 $O(\text{可見})$，第三節 A 表的數據有效 |
| 打字增量重排為 $O(1)$ | ❌ **更正為 $O(N)$**，常數項小。$N \le 10^4$ 時可用，$N = 10^6$ 時 layout 單獨佔滿 8.33ms 預算的 59% |

### 實務影響評估

單一筆記文件通常不會到 $10^6$ 段落；$N = 10^5$ 時 p99 為 388μs，仍有餘裕。
**因此這不是架構否決，而是一個已知的、必須在 P1 解決的設計缺口。**

### 必須採取的行動

1. `tasks/roadmap.md` 的 P1 驗收條件 #5（「重算的節點數不隨文件長度成長」）
   **現行實作並未滿足**，P1 必須真正實作它，不得沿用 spike 的做法。
2. 正解方向：**不儲存絕對 `y_offset`**。改為只存高度，配合前綴和結構
   （Fenwick／order-statistic tree）或「偏移有效至索引 $k$」的延遲物化，
   使編輯第 $k$ 段只需 $O(\log N)$ 或 $O(\text{可見})$ 即可得出視窗內座標。
3. 此缺口已登記於 `spec/decisions/02-layout/README.md` 的待決問題。

### 方法論教訓

**三個數量級以內的平坦曲線不足以宣告 $O(1)$。** 複雜度主張必須測到常數項被超越為止，
否則量到的是常數項而不是複雜度。本專案後續所有複雜度主張皆適用此規則。
