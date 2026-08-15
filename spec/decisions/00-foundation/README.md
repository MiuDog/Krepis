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
| [FND-0002](FND-0002-c-abi-error-memory-and-threading.md) | **Proposed** | C ABI 邊界、錯誤模型、arena 記憶體、同步／背景執行緒分工 |

## 待決問題

### 已由 FND-0002 涵蓋（待核准）

交付形式與 ABI、例外策略、記憶體策略、並行模型四項已合併於
[FND-0002](FND-0002-c-abi-error-memory-and-threading.md)，狀態 `Proposed`。
**該決策核准前不得撰寫公開標頭。**

### 可延後

- **依賴管理方式**：vcpkg／CMake `FetchContent`／git submodule。引入任何第三方庫前必須先定。
  目前本機未安裝 vcpkg。
- **測試框架**：目前以退出碼＋CTest，無框架依賴。引入 Catch2／GoogleTest 需先解上一條。

## 公開型別的設計慣例

新增**公開**型別前，於決策或標頭註解回答（僅限公開型別，內部型別不需要，
避免制度變成官僚）：

```text
責任：
不負責：
維持的不變條件：
擁有哪些資源：
生命週期：
錯誤語意：
執行緒安全程度：
可否複製／移動：
```
