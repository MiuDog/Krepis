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

需要 Visual Studio 2022 BuildTools（含 C++ 工作負載）。CMake 與 Ninja 隨其內建。

```
cmake --preset msvc-x64
cmake --build build/msvc-x64 --config Debug
ctest --test-dir build/msvc-x64 -C Debug --output-on-failure
```

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
