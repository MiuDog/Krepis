# Krepis — Agent 作業入口（AGENTS.md）

Krepis 是**結構化筆記的領域中立基座庫**，以 C++20 撰寫，用 CMake 建置。
公開標頭在 `include/krepis/`，實作在 `src/`，測試在 `tests/`（CTest），決策在 `spec/decisions/`。
**不含任何特定產品的語意**——範圍與拒絕清單見 `README.md`。

本檔為通用入口（Codex 等工具原生讀取；CLAUDE.md / GEMINI.md 內容應與本檔一致或直接指向本檔）。

## 你必須遵守的規範（按順序讀）

1. `.agents/skills/agent-entry/SKILL.md` — 入口：角色判定、任務分型、硬規則、衝突裁決（最先讀）。
2. `.agents/skills/work-protocol/SKILL.md` — 執行紀律與回報合約（所有任務必讀）。
3. 依任務型態加讀：`task-planning` / `task-development` / `task-delivery`；需求模糊先讀 `human-intent`；
   可派 subagent 的指揮側加讀 `model-dispatch`。

## 硬規則摘要（完整版在上述技能）

- 完成＝證據（測試輸出尾行、exit code、來源引用）；「應該可以」不是完成。
- 同一錯誤最多修兩次，第三次帶完整失敗軌跡回報或升級。
- 大檔（>200 行或大小不明）先內容搜尋定位再分段讀，不整檔讀。
- 查不到的事實標「未查證」，嚴禁編造 API、路徑、來源。

### 本專案特有硬規則

- **`CMakeLists.txt` 的 `add_compile_options(/utf-8)` 不得移除。** 見「已知陷阱」。
- **註解一律繁體中文**，identifier、測試名稱、`assert` 訊息維持英文。
- **一條規則只能有一個實作。** 同一條規則若在 Krepis 與 client 各有一份，兩者必然靜默分岔。
- **client 必須在結構上無法成為權威。** 任何 client 判斷都要能被 authority 否決。
- **拒絕清單優先於便利。** 產品語意（計畫、repo binding、workflow、治理格式、導覽）
  一律不得進入本庫，即使「放這裡比較方便」。判準：**Jotist 需不需要？**
- 公開介面尚未穩定（`0.0.1`），但**新增公開標頭前必須先確認它通過上述判準**。
- 除 `spec/decisions/05-ink/` 外，後續決策文件必須遵守
  [`spec/decisions/README.md`](spec/decisions/README.md) 的全局最佳解、聚焦架構圖、具體例子與
  偏離建議加嚴規則。`05-ink` 保持人類主導。

## 環境事實

實測於 2026-08-16，Windows 11 Home 26200，繁體中文語系。**只增不猜。**

| 項目 | 值 |
|---|---|
| 編譯器 | MSVC `14.44.35207`（Visual Studio 2022 **BuildTools**，非完整 VS） |
| cmake | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`（3.31.6-msvc6） |
| ctest | 同上目錄的 `ctest.exe` |
| Ninja | 同上層 `CMake` 目錄旁的 `Ninja`（隨 BuildTools 內建） |
| **cmake／ctest 不在 PATH 上** | 必須用完整路徑，或先跑 VS 開發者環境 |
| vcpkg | **未安裝**。引入 HarfBuzz／ICU 前需先決定依賴管理方式 |

建置與測試（已實測通過）：

```
cmake --preset msvc-x64
cmake --build build/msvc-x64 --config Debug
ctest --test-dir build/msvc-x64 -C Debug --output-on-failure
```

### 已知陷阱

**MSVC 在繁體中文 Windows 上會把無 BOM 的 UTF-8 原始碼當 CP950（Big5）解讀。**
Big5 有字元的第二個位元組為 `0x5C`（反斜線），使中文 `//` 註解的行尾被當成續行符，
**靜默吞掉下一行程式碼**——症狀是「namespace 存在但成員全部找不到」，而編譯器不會指向註解。

2026-08-16 本專案第一次建置即踩到此坑。解法為 `CMakeLists.txt` 的全域 `add_compile_options(/utf-8)`。
**不要改用加 BOM 的方式繞過**，那會讓其他平台的工具鏈出問題。

## 規則衝突時

依 `agent-entry` 的「規則衝突裁決」節（使用者當下指示 > 本檔 > agent-entry 與 work-protocol
> 各任務型 skill > 模板）；裁決不了就用 human-intent 的批次提問格式問人類。
