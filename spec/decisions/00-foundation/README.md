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
| [FND-0002](FND-0002-c-abi-error-memory-and-threading.md) | Accepted | C ABI 邊界、錯誤模型、arena 與釋放契約、執行緒模型（**分工線除外**） |
| [FND-0003](FND-0003-dependency-management.md) | **Proposed** | 第三方依賴以 CMake FetchContent 取得；淘汰 vcpkg |

FND-0002 原含的「同步／背景分工線」（D1）已分離至
[`LAY-0001`](../02-layout/LAY-0001-sync-background-split-and-frame-budget.md)。
該決策刻意停在 `Proposed` 直到 P0 實測完成，**已於 2026-08-16 通過並核准**——
其正確性只能量、不能推，而實測也確實推翻了報告中一項未經充分測試的複雜度主張。

## 待決問題

### 已由 FND-0002 解決

交付形式與 ABI、例外策略、記憶體策略、執行緒模型（不含分工線）已 `Accepted`。
**公開標頭的形狀已可確定，撰寫不再被阻擋。**

### 可延後

- ~~依賴管理方式~~ → 分析與建議見 FND-0003（Proposed）。目前不需核准：TXT 第一版採 DirectWrite，P0–P4 不引入任何第三方庫。
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
