# 基座抽取計畫與 Planist 擱置紀錄（2026-08-16）

## 這份文件是什麼

2026-08-15～16 與使用者的方向討論定案紀錄。**Planist 開發擱置，改為先抽出獨立基座庫。**

本文件是給下一個 session 的執行入口。**不是 ADR** —— 定案的架構決策待新 repo 建立後另立
ADR，避免在擱置中的專案繼續累積承諾（該模式的成因見
[`planist-completion-bottleneck`](../../.claude/projects/) 與 `spec/decisions/index.md` 的狀態詞彙段）。

## 定案（使用者明確決定）

| 項目 | 決定 |
|---|---|
| Planist | **擱置**，核心需求尚未成熟 |
| 抽取 | **立即抽出獨立基座庫**，各 `-ist` 為獨立 app |
| 抽取範圍 | 基座 ＋ **全部筆記類型決策** |
| 基座庫名稱 | **`Krepis`**（κρηπίς＝希臘神廟的階狀基座，字面即「基座」） |
| 第一個消費者 | **`Jotist`** —— 一個刻意平凡的筆記 app |
| 版面引擎歸屬 | **C++**；Flutter 只負責畫結果與輸入事件 |
| 目的 | 學習與走對的路，**明確非經濟考量** |

## 決定的推導依據（避免下次重新爭論）

1. **手寫是獨立 layer，不進統一模型。** 使用者原始預期即為「輸入是資料之主，手寫只是不同
   layer」。此立場與 ADR-0064 §4–5 一致（手寫草圖是意圖、求快不求準、自由繪製方案 F 已淘汰），
   但與 ADR-0022「文件與畫布必須共用同一套 selection／undo／資料模型」及 ADR-0023 Migration
   第 3 步（「移除 ink 私有 selection」）衝突。**此矛盾必須解掉，方向為手寫分層。**
2. **畫布 ≠ 手寫。** 畫布是結構化節點的空間排版，**仍然**要與文件共用模型；只有自由筆跡分層。
3. **手寫品質門檻低兩個量級。** 對照 Concepts（TopHatch）：其手寫即產品本身，需個位數 ms 延遲與
   平台專屬路徑（PencilKit／Metal），且該團隊至今未達成跨平台功能對等。本專案手寫為意圖捕捉，
   20–30ms 可接受，不需平台特有預測 API。
4. **語言分界應與權威性分界重合。** 判準是「錯誤會不會靜默」：
   - 靜默出錯（codec、anchor 狀態、交易順序、規則求值）→ 必須可審查 → C++
   - 錯誤可見（UI、排版呈現、樣式）→ 不需審查 → Flutter
   使用者可審查語言為 C／C++／C#／Java，**不含 Dart 與 TypeScript**。
5. **client 必須在結構上無法成為權威。** 任何 client 判斷都要能被 authority 否決；
   client 不得是任何規則的唯一實作。**一條規則只能有一個實作**——雙實作必然靜默分岔。

## 庫的邊界

### 範圍陳述（草案，需定稿）

> 提供多平台、本地優先的結構化筆記核心：權威狀態、文件模型、版面、選取、undo 與 ink 資料模型。
> **不含任何特定產品的語意。**

### 進庫

- authority：identity／session／permission／persistence／protocol／event digest
- 文件模型：node tree、stable ID、schema、codec
- **版面引擎：流式 layout ＋ 空間 layout**（最具原創性的部分）
- selection 模型、undo、typed transaction
- ink **資料模型**（不含繪製）

### 不進庫（拒絕清單）

- `PlanCommitment` 與一切計畫語意（ADR-0064）
- repo binding 與 code locator（ADR-0051）
- n8n workflow 匯出（ADR-0067）
- governance bundle 格式（ADR-0052）
- 導覽、入口與一切產品外殼決策

### 灰色地帶：判為「第二個包」，不進核心

`SpecFacet`／`SpecRelation`／conformance／`WorldAnchor`（ADR-0049／0050/0056／0061）。
領域中立性已實測 29／30 成立，但**一個平凡的筆記 app 不需要它們**——依第一消費者判準，
它們是庫之上的獨立包，不是核心的一部分。

### 判準

> 這個決策，那個刻意平凡的筆記 app 需不需要？
> 需要 → 進庫。只有 Planist 需要 → 留在 Planist。

## 部署分界（最重要的單一設計決定）

| 模式 | 內容 | 理由 |
|---|---|---|
| **in-process（FFI）** | 文件模型、版面、selection、undo、ink 資料 | 每次按鍵都要跑，IPC 往返會毀掉輸入延遲 |
| **out-of-process（IPC）** | authority、persistence、permission、digest、session | 需隔離、跨 client、crash recovery（沿用 ADR-0041／0044 拓撲） |

**未決（必須在寫第一行程式碼前定案）**：一次編輯在什麼時間點從 in-process 狀態提交為
out-of-process transaction？太早則輸入卡頓，太晚則 crash 掉資料。

## 必須早做的 spike（風險最高的未知）

1. **C++ 文字處理依賴選型**：text shaping、雙向文字、grapheme 邊界。Dart 側現由 `characters`
   套件承擔（ADR-0028），C++ 側通常意味著引入 HarfBuzz／ICU。**這是 C++ 版面引擎最實在的一筆成本。**
2. **FFI 邊界延遲實測**：C++ 版面引擎 ↔ Flutter 的每次按鍵往返延遲。**此項不通過則整個架構不成立**，
   應最先做。
3. **in／out-of-process 提交點**：以實測決定，不以推理決定。

## 執行順序

### A. 舊 repo 收尾（便宜，先做）

1. 處理 258 個未提交檔案——分類為完成／半成品／丟棄，該提交的提交。**未做此步則三個月後
   無法判斷手上有什麼。**
2. 於本檔補記擱置時的實際狀態（commit hash、最後可運行版本）。
3. **ADR-0071 改寫或標記 `Rejected`。** 其第 6 節「抽取觸發條件」要求 Planist 驗證完成且第二個
   `-ist` 確定才可抽取，已被本決定推翻。留著會讓下個 session 照它擋住工作。
4. **解掉 ADR-0022／0023 與 ADR-0064 的矛盾**，方向為「手寫是獨立 layer」。ADR-0023 Migration
   第 3 步須撤銷或改寫。**此項未做，下個 session 會繼續把 ink selection 併進統一模型。**

### B. 新庫前置（依序）

5. 定庫名與範圍陳述定稿。**庫不得使用 `-ist` 命名**——`-ist` 保留給產品，命名層級的區分是防止
   產品語意漏進庫的第一道防線。
6. 跑 spike 1 與 spike 2。**spike 2 不過則回到本文件重新評估架構。**
7. 依 spike 結果定 in／out-of-process 分界。
8. 開新 repo，建立第一份 ADR。

### C. 移植原則

**不搬程式碼，照規格重寫。**

複製會把 `lib/product` 的 617 個 class、48 個 `abstract interface class` 與五層 clean architecture
一併帶過去，而那正是「程式碼讀不懂」最可能的主因之一（AI 產出的 clean architecture 會忠實補齊
每一層，任何行為都要跨 4–5 個檔案才追得完）。

規格在 `spec/`。**這同時是對 ADR-0068 核心命題的第一次真實檢驗**——若規格足以決定產出，
重寫應比搬運快；若很貴，代表規格其實不足以決定產出。**兩種結果都是決定性的資訊。**

## 待處理的未決項

- [ ] 庫名（不得為 `-ist`）
- [ ] `notist` 是否定為第一消費者的正式名稱（`notist` 優於 `noteist`：`-eist` 讀音卡頓，
      且 note 接後綴本來就吃掉 e）
- [ ] in／out-of-process 提交點的精確定義
- [ ] C++ 文字處理依賴選型
- [ ] 第二個包（spec／conformance／anchor）的邊界與命名

## 已知風險（使用者已知情並接受）

**只有一個消費者時，庫的介面有一部分會是猜的。** ADR-0071 當初延後抽取即為此因，該理由未消失；
使用者的決定改變的是「是否承受」，不是「是否存在」。

降低方式即為第一消費者策略：`notist` 刻意平凡、刻意不含 spec engineering，作為介面的逼迫函數與
每日可用的驗證載體。**它會被完成，而 Planist 短期不會**——而使用者從系統設計中學到的東西，有一半
來自抽象被實際使用之後才暴露的錯誤。
