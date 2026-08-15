# Mermaid 架構圖樣式

只讀取與已選架構視角相符的章節。所有名稱換成程式中的真實符號，不照抄範例名稱。

## 類別架構

```mermaid
classDiagram
  class DocumentService {
    +open(documentId)
  }
  class DocumentRepository {
    <<interface>>
    +find(documentId)
  }
  class SqlDocumentRepository

  DocumentService --> DocumentRepository : calls
  DocumentRepository <|.. SqlDocumentRepository : implements
```

只列出能解釋關係的關鍵成員，不把完整 API 複製進圖。

## 模組與依賴架構

```mermaid
flowchart TD
  adapter["DocumentAdapter"] -->|calls| core["DocumentService"]
  core -->|depends on| port["DocumentRepository"]
  infra["SqlDocumentRepository"] -->|implements| port
```

箭頭方向表示程式依賴方向；若要表達資料流，另畫資料流圖。

## 呼叫鏈與互動時序

```mermaid
sequenceDiagram
  participant Caller
  participant DocumentService
  participant DocumentRepository

  Caller->>DocumentService: open(documentId)
  DocumentService->>DocumentRepository: find(documentId)
  DocumentRepository-->>DocumentService: Document
  DocumentService-->>Caller: OpenResult
```

同步呼叫使用實線箭頭，回傳使用虛線箭頭；條件分支使用 `alt`，避免把每個小函式都列為 participant。

## 資料流架構

```mermaid
flowchart LR
  input["EncodedDocument"] -->|decode| codec["DocumentCodec"]
  codec -->|Document| validator["DocumentValidator"]
  validator -->|validated Document| store[("DocumentStore")]
```

節點表示處理階段或儲存邊界，箭頭標示實際資料形狀或轉換。

## 事件架構

```mermaid
flowchart LR
  editor["Editor"] -->|publishes DocumentChanged| bus["EventBus"]
  bus -->|notifies| indexer["SearchIndexer"]
  bus -->|notifies| persistence["PersistenceWorker"]
```

若重點是發送、排程、重試與回覆的先後順序，改用 `sequenceDiagram`。

## 狀態架構

```mermaid
stateDiagram-v2
  [*] --> Idle
  Idle --> Loading : open
  Loading --> Ready : loaded
  Loading --> Failed : error
  Ready --> Idle : close
  Failed --> Loading : retry
```

轉移標籤使用真實事件或條件；不要把一般函式呼叫誤畫成狀態。

## 並行與非同步架構

```mermaid
sequenceDiagram
  participant UIThread
  participant WorkerQueue
  participant Authority

  UIThread->>WorkerQueue: submit(transaction)
  WorkerQueue->>Authority: validateAndCommit(transaction)
  Authority-->>WorkerQueue: CommitResult
  WorkerQueue-->>UIThread: notify(result)
```

participant 使用真實執行緒、queue、actor 或程序名稱；用 `Note over` 標示執行緒切換或排程保證。

## 執行期與部署架構

```mermaid
flowchart TD
  subgraph client_process["Client Process"]
    shell["Application Shell"]
    library["Core Library"]
    shell -->|calls| library
  end

  subgraph authority_process["Authority Process"]
    authority["Authority Service"]
    store[("Persistent Store")]
    authority -->|writes| store
  end

  library -->|protocol request| authority
```

`subgraph` 表示程序、主機或信任邊界；不要混用成一般視覺分組。

## 資料模型架構

```mermaid
erDiagram
  DOCUMENT ||--o{ NODE : contains
  NODE ||--o{ NODE : has_children
  DOCUMENT {
    string id
  }
  NODE {
    string id
    string parent_id
  }
```

只有資料關聯是核心問題時才使用 `erDiagram`；行為與依賴改用類別圖或模組圖。

## 共通語法規則

- 使用 ASCII 節點 ID，例如 `document_service`；標籤可使用中文、空格與真實符號名。
- 標籤含括號、冒號、斜線或標點時使用雙引號。
- 同一張圖維持單一主方向；避免交叉箭頭與無意義的雙向線。
- 實作與繼承使用 Mermaid 對應關係；呼叫、事件與資料流使用有動詞的邊標籤。
- 省略與核心問題無關的欄位、工具函式、框架內部與第三方細節。
