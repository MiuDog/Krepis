# 00-foundation（`FND`）

## 範圍

專案級、不屬於任何單一能力的決策：範圍與拒絕清單、語言邊界、命名、原始碼慣例、依賴政策、
測試策略。

## 什麼該放這裡

**會約束其他所有能力的決策。** 如果一份決策只影響一個能力，它屬於該能力的目錄，不屬於這裡。

## 決策

| 決策 | 狀態 | 主題 |
|---|---|---|
| [FND-0001](FND-0001-scope-language-boundary-and-rejections.md) | Accepted | 範圍、語言邊界判準與拒絕清單 |

## 待決問題

- **依賴管理方式**：vcpkg／CMake `FetchContent`／git submodule。引入任何第三方庫前必須先定。
  目前本機未安裝 vcpkg。
- **測試框架**：目前以退出碼＋CTest，無框架依賴。引入 Catch2／GoogleTest 需先解上一條。
