# Krepis

> κρηπίς —— 希臘神廟的階狀基座，柱子站立其上的承重平台。

**結構化筆記的領域中立基座。** 提供多平台、本地優先的資料與版面核心，
**不含任何特定產品的語意**。

## 範圍

### 進庫

- **authority**：identity、session、permission、persistence、protocol、event digest
- **文件模型**：node tree、stable ID、schema、codec
- **版面引擎**：流式 layout ＋ 空間 layout
- **selection 模型、undo、typed transaction**
- **ink 資料模型**（不含繪製）

### 不進庫

計畫語意、repo binding 與 code locator、workflow 匯出、governance bundle 格式、
導覽與一切產品外殼決策。規格語意層（SpecFacet／SpecRelation／conformance／WorldAnchor）
為**庫之上的獨立包**，不屬核心。

### 判準

> 一個刻意平凡的筆記 app 需不需要這個決策？
> 需要 → 進庫。只有特定產品需要 → 留在該產品。

## 消費者

| 專案 | 角色 |
|---|---|
| **Jotist** | 刻意平凡的筆記 app；本庫的第一個消費者與介面逼迫函數 |
| Planist | 規格工程（擱置中） |

產品一律以 `-ist` 命名；**基座不使用 `-ist`**，命名層級即區分基座與產品。

## 語言邊界

判準是**錯誤會不會靜默**：

| 性質 | 歸屬 |
|---|---|
| 錯誤靜默（codec、anchor 狀態、交易順序、規則求值） | **C++**，必須可人工審查 |
| 錯誤可見（UI、排版呈現、樣式） | 各平台外殼 |

**client 必須在結構上無法成為權威**：任何 client 判斷都要能被 authority 否決；
client 不得是任何規則的唯一實作。**一條規則只能有一個實作。**

## 建置

### 前置需求

需要 Visual Studio 2022 BuildTools（含「使用 C++ 的桌面開發」工作負載）。本機的 CMake 與
CTest 隨 BuildTools 內建，但不在 `PATH` 上；請在專案根目錄以 PowerShell 執行以下指令。

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'

& $cmake --preset msvc-x64
& $cmake --build build/msvc-x64 --config Debug
& $ctest --test-dir build/msvc-x64 -C Debug --output-on-failure
```

VS Code 使用者也可以執行 `Tasks: Run Task` → `Verify`，一次完成相同的設定、編譯與測試流程。

設定完成時會顯示 `Build files have been written to`；編譯完成後會產生
`build/msvc-x64/Debug/krepis.lib`，測試結果應為 `100% tests passed`。

### 編輯器診斷

專案同時提供 Microsoft C/C++ 與 clangd 所需的 C++20、`include/` 搜尋路徑設定。
若新增或修改設定後紅線未立即消失，請在 VS Code 執行 `clangd: Restart language server`。

### 建置排錯

| 訊息 | 處理方式 |
|---|---|
| 找不到 `cmake` 或 `ctest` | 使用上方完整路徑指令，不要假設工具已加入 `PATH`。 |
| `MSB6001`，並提到重複的 `Path`／`PATH` | 關閉目前終端機，從開始功能表重新開啟 **Developer PowerShell for VS 2022**，再執行上方指令；不要修改系統 `PATH`。 |
| clangd 顯示找不到 `krepis/version.hpp` 或只使用 C++14 | 確認從專案根目錄開啟 VS Code，並重新啟動 clangd；clangd 會讀取根目錄的 `compile_flags.txt`。 |

## 原始碼慣例

- 原始碼為**無 BOM 的 UTF-8**；**註解使用繁體中文**，identifier 與測試名稱維持英文。
- MSVC 必須帶 `/utf-8`（已於 `CMakeLists.txt` 全域設定）。**移除它會使中文註解在繁中
  Windows 上被當成 CP950／Big5 解讀，行尾 `0x5C` 會被當成續行符並靜默吞掉下一行程式碼。**
- C++20，`/W4 /permissive-`。

## 狀態

`0.0.1` —— 骨架階段。公開介面尚未穩定。

進入實作前必須完成的 spike：

1. **C++ 文字處理依賴選型**（text shaping、雙向文字、grapheme 邊界；候選 HarfBuzz／ICU）
2. **FFI 邊界延遲實測**（C++ 版面引擎 ↔ Flutter 每次按鍵往返）。**此項不通過則整個架構不成立。**
3. **in-process／out-of-process 提交點**：一次編輯何時從 in-process 狀態提交為 authority transaction。
