# P0 測試架構：模組依賴與驗證邊界

## 範圍

- 核心問題：本專案在 P0 階段如何透過 Spike 測試集與單元測試驗證文字 Shaping、FFI 零複製 Display List 延遲與交易提交邊界？
- 納入：P0 三大 Spike 測試執行器（Dart Benchmark、Native/Scaling Test、DirectWrite 測試、Authority 模擬）、CTest 基礎測試、C ABI 邊界、C++ 核心排版與 Arena 記憶體管理、底層 DirectWrite 與 OS 邊界。
- 不納入：未進入實作的 P1 完整 Node Tree、Selection、Undo/Redo、Flutter 完整視圖與實際跨行程 Authority IPC 實作。

## 架構圖

### 主要視角：模組依賴與驗證架構

```mermaid
flowchart TD
    subgraph test_runners["測試與基準執行層 (Test Runners)"]
        dart_bench["spike2_benchmark.dart<br/>(120Hz 捲動/打字基準)"]
        cxx_tests["spike2_native_test<br/>spike2_scaling_test"]
        spike1_runner["spike1_directwrite<br/>(Shaper 效能基準)"]
        spike3_runner["spike3_submission_boundary<br/>(交易提交策略評估)"]
        ctest_runner["version_test<br/>(CTest 基礎檢驗)"]
    end

    subgraph c_abi_boundary["C ABI / FFI 邊界 (krepis_c_abi.dll)"]
        abi_interface["engine_abi.h<br/>(純 C 函式 / 不透明 Handle)"]
    end

    subgraph cpp_core["C++ 核心邏輯 (In-Process)"]
        engine_impl["EngineImpl<br/>(段落儲存 / 視窗裁剪 / 排版排程)"]
        dl_builder["DisplayListBuilder<br/>(二進位指令封裝)"]
        arena_alloc["Arena (Bump Allocator)<br/>(512KB 雙緩衝區)"]
        dw_shaper["DirectWriteShaper<br/>(純值型別轉換)"]
        version_mod["version.hpp"]
    end

    subgraph platform_boundary["平台與外部邊界 (Platform Boundary)"]
        dwrite_com["DirectWrite (dwrite.lib)<br/>(IDWriteFactory / TextLayout)"]
        os_memory["OS Memory<br/>(malloc / free / realloc)"]
    end

    %% 測試依賴與呼叫關係
    dart_bench -->|dart:ffi 呼叫| abi_interface
    cxx_tests -->|鏈結與呼叫| abi_interface
    abi_interface -->|轉發呼叫| engine_impl
    spike1_runner -->|直接呼叫| dw_shaper
    ctest_runner -->|驗證版本| version_mod

    %% 核心內部協同
    engine_impl -->|呼叫文字排版| dw_shaper
    engine_impl -->|建構視窗繪製指令| dl_builder
    dl_builder -->|配置指令記憶體| arena_alloc
    dw_shaper -->|呼叫原生 API| dwrite_com
    arena_alloc -->|整塊記憶體配置| os_memory

    %% Spike 3 獨立邊界
    spike3_runner -.->|模擬 WAL 與非同步 Flush| engine_impl
```

### 輔助視角：Spike 2 Display List 零複製互動時序

```mermaid
sequenceDiagram
    participant Runner as 外殼 / 測試端<br/>(Dart FFI / Native Test)
    participant ABI as C ABI 邊界<br/>(engine_abi)
    participant Engine as EngineImpl
    participant Arena as Dual-buffered Arena
    participant Shaper as DirectWriteShaper

    Runner->>ABI: krepis_engine_set_viewport(w, h, scroll_y)
    ABI->>Engine: set_viewport(...)
    
    opt 段落髒污時 (打字/編輯)
        Runner->>ABI: krepis_engine_edit_paragraph(index, utf8_text)
        ABI->>Engine: edit_paragraph(...)
        Runner->>ABI: krepis_engine_layout()
        ABI->>Engine: layout()
        Engine->>Shaper: shape(text, "Segoe UI", 16.0f)
        Shaper-->>Engine: ShapedParagraph (純值型別)
        Note over Engine: 增量重算高度與累加 y 座標
    end

    Runner->>ABI: krepis_engine_acquire_display_list(&ptr, &size, &handle)
    ABI->>Engine: build_display_list(arena_index)
    Engine->>Arena: reset() (單向 Bump 清零)
    Engine->>Arena: allocate(...) 寫入 DisplayListHeader 與指令集
    Engine-->>ABI: 回傳連續記憶體指標 (ptr, size)
    ABI-->>Runner: KREPIS_OK (指標與大小)

    Note over Runner: Dart 透過 ByteData.view(ptr, size) 零複製直接解析指令

    Runner->>ABI: krepis_display_list_release(handle)
    ABI->>Engine: 切換/釋放雙緩衝區 Handle
    ABI-->>Runner: KREPIS_OK
```

## 證據

- [`tests/CMakeLists.txt`](file:///c:/Projects/Krepis/tests/CMakeLists.txt#L1-L12) 與 [`tests/version_test.cpp`](file:///c:/Projects/Krepis/tests/version_test.cpp#L1-L29)：定義 CTest 測試執行檔 `krepis_tests` 與 `version.hpp` 驗證。
- [`spikes/CMakeLists.txt`](file:///c:/Projects/Krepis/spikes/CMakeLists.txt#L1-L87)：定義 Spike 1 (`spike1_directwrite`)、Spike 2 C ABI 共享庫 (`krepis_c_abi`)、C++ 原生測試 (`spike2_native_test`)、規模擴展測試 (`spike2_scaling_test`) 與 Spike 3 交易評估 (`spike3_submission_boundary`)。
- [`spikes/spike1_directwrite/spike1_shaper.hpp`](file:///c:/Projects/Krepis/spikes/spike1_directwrite/spike1_shaper.hpp#L11-L65)：定義 `GlyphInfo`、`ShapedRun`、`ShapedParagraph` 純值型別與 `DirectWriteShaper` 抽象介面。
- [`spikes/spike2_ffi_displaylist/engine_abi.h`](file:///c:/Projects/Krepis/spikes/spike2_ffi_displaylist/engine_abi.h#L16-L84)：定義 C ABI 介面（含 `krepis_engine_acquire_display_list`、`krepis_display_list_release`、Opaque Handle）。
- [`spikes/spike2_ffi_displaylist/display_list.hpp`](file:///c:/Projects/Krepis/spikes/spike2_ffi_displaylist/display_list.hpp#L12-L191)：定義二進位指令結構體（`DrawRectCommand`、`DrawGlyphRunHeader`、`DisplayListHeader`）以及 512KB Bump Allocator `Arena` 與 `DisplayListBuilder`。
- [`spikes/spike2_ffi_displaylist/engine_abi.cpp`](file:///c:/Projects/Krepis/spikes/spike2_ffi_displaylist/engine_abi.cpp#L21-L135)：`EngineImpl` 整合 `DirectWriteShaper`、雙 `Arena`、視窗裁剪過濾以及版面計算邏輯。
- [`spikes/spike2_ffi_displaylist/spike2_benchmark.dart`](file:///c:/Projects/Krepis/spikes/spike2_ffi_displaylist/spike2_benchmark.dart#L1-L311)：Dart 端透過 `dart:ffi` 呼叫 C ABI，並使用 `ByteData.view` 零複製解析 Display List 驗證 120Hz 捲動與打字延遲。
- [`spikes/spike2_ffi_displaylist/spike2_scaling_test.cpp`](file:///c:/Projects/Krepis/spikes/spike2_ffi_displaylist/spike2_scaling_test.cpp#L27-L80)：對 $N \in [10^3, 10^6]$ 段落進行極限規模壓測，揭露 $O(N)$ 座標累加缺口。
- [`spikes/spike3_submission_boundary/spike3_main.cpp`](file:///c:/Projects/Krepis/spikes/spike3_submission_boundary/spike3_main.cpp#L24-L115)：評估 Per-keystroke、300ms Debounce、以及 In-Process Lock-free WAL + 異步背景批次 Flush（策略 C）之延遲與資料安全視窗。
- [`tasks/p0-spike-report.md`](file:///c:/Projects/Krepis/tasks/p0-spike-report.md#L1-L158)：彙整三大 Spike 實測數據、生死門檻判定與事後複驗更正記錄。

## 閱讀說明

1. **零複製生命週期保證**：`Arena` 採 Bump Allocator 設計，在 `krepis_engine_acquire_display_list` 到 `krepis_display_list_release` 期間提供保證有效的連續二進位緩衝區，使 Dart 端能在不觸發 GC 與記憶體複製的前提下完成 120Hz 渲染。
2. **值型別隔離**：`DirectWriteShaper` 將 Windows 原生 COM 物件（`IDWriteFactory`、`IDWriteTextLayout`）封裝於實作內部，回傳純值型別 `ShapedParagraph`，確保核心資料結構不依賴特定 OS API Handle。
3. **已知設計缺口標記**：`spike2_scaling_test` 實測發現 `EngineImpl::layout()` 在 $N=10^6$ 時的 y 座標累加為 $O(N)$，已記錄於架構報告並作為 P1 垂直切片的重點改造項目（改用前綴和或延遲物化）。
