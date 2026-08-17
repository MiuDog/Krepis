# Krepis 階段規劃

本檔是**排程與範圍**，會隨實測改變。架構決策在 [`spec/`](../spec/index.md)，兩者不要混。

## 排序原則

**先做會殺死架構的未知，不是先做地基。**

| 未知 | 會殺死架構嗎 | 有沒有做過 |
|---|---|---|
| C++ 版面 ↔ 外殼的每幀成本 | **會** | ❌ |
| C++ 的文字 shaping／grapheme | **會** | ❌ |
| 流式 ＋ 空間共用同一份模型 | **會** | ❌ 只有設計 |
| IME 跨邊界 | **會** | ❌ |
| authority／persistence／identity | ❌ | ✅ 既有專案已實作 74,016 行 |

authority 是**已知可解的工作量**，不是未知。先做它意味著數月後才會知道架構成不成立。
因此它排在最後。

## 階段

### P0 —— spike（可丟棄）

回答兩個會殺死架構的問題，**不寫正式程式碼**。

1. **文字 shaping 選型實測**：DirectWrite 的 glyph run／metrics／cluster 對應能否完整取出為
   純值型別（不洩漏平台 handle）。
2. **display list 零複製實測**：C++ 寫入連續緩衝區、Dart 以 `Pointer` ＋ typed data view 讀取，
   量測每幀成本與 GC 行為。

另需量 `LAY-0001` 列出的五項，其中**捲動的持續表現優先於打字**——版面不是每幀都跑，
但捲動時是（120 次／秒），那才是持續吞吐的情況。

**一幀預算**：目標裝置 120Hz，總預算 **8.33ms**。Krepis 只佔其中一段，各段佔比必須實測。
主要 gate 為端到端的「不掉幀」，不依賴猜測的拆分。

**結束條件**：`LAY-0001` 的五項各有實測數字，且 shaping 與零複製兩項可行性有結論。
**不過的話**：回到 `spec/architecture.md` 重新評估「FFI 邊界是 display list」這條約束，
或回到 `LAY-0001` 改採方案 B。

### P1 —— 垂直切片：一個模型的兩種可組合布局

**這是會證明或殺死整個架構的階段。** 只做 Windows。

**核心（C++）**
- node tree，單一 block 型別（純文字段落）
- **流式版面**，增量重算
- **空間版面**，位置為儲存值
- selection、undo／redo
- composing region（一級概念）
- display list 產出

**外殼（Notist）**
- 流式頁面與空間頁面並列，各自執行 display list
- 流式頁面唯讀引用空間 viewport；空間頁面唯讀引用流式 Block 區間
- 送鍵盤／IME／指標事件
- 不做其他任何事

**刻意不做**：平移／縮放、z-order、群組、連接線、多種節點型別、持久化、authority、
identity、權限、ink、多平台。

#### 驗收條件（全部可機器判定）

1. 流式與空間 Container 共用同一 ObjectStore；編輯來源 Block 時，其唯讀即時引用在同一幀反映。
2. 注音輸入的組字、選字、確定、倒退在流式視圖全部正確；**組字中的文字影響換行與游標位置**。
3. undo 的單位是「一次確定」而非一個 rune，且在兩個視圖產生一致結果。
4. N 個段落的文件，打字時每幀的 layout ＋ display list 產出時間 ≤ P0 定下的預算。
5. **打一個字時重算的節點數不隨文件長度成長。**
6. Intrusive reference-count 壓力測試結束並 drain reclamation queue 後，所有 node 都恰好銷毀一次，
   且配置總數等於釋放總數。
7. UI 執行緒釋放最後一個 snapshot reference 時，node destructor 不得在 UI 執行緒執行；測試必須
   記錄並斷言實際 destructor thread。
8. 舊 snapshot 背景走訪與新 revision 高頻發布並行時，舊 snapshot 的內容 hash 全程不變，且不得
   出現 use-after-free、double release、reference-count underflow／overflow 或 data race 證據。
9. Intrusive counter 與 `std::shared_ptr<const Node>` 基準實作必須在相同 retain／release、COW edit、
   跨執行緒 handoff 與深 DAG 回收 workload 下比較時間與峰值記憶體。測試前先登記容許回歸範圍；
   intrusive 方案若沒有可重現的整體優勢，就重開 `LAY-0002` D17。

第 5 條把「增量」從宣稱變成可測。**沒有這一條，前四條都可能在小文件上假性通過。**

第 6–9 條是 [`LAY-0002` D17](../spec/decisions/02-layout/LAY-0002-invalidation-offset-and-viewport-index.md)
偏離原建議、直接採 intrusive atomic reference count 所增加的嚴格閘門。除了機器測試，人類核准者
還必須逐行讀完：

- 所有 owning edge 是否只使用 `IntrusivePtr<const T>`。
- 是否有人從未保護 raw pointer 建立新 owner。
- `retain`／`release` memory order 是否只由單一封裝實作。
- Snapshot DAG 是否可能形成 owning cycle。
- Reclamation queue 的 `noexcept` transfer、drain 與 shutdown path 是否完整。

Race detector 若在主要 MSVC 工具鏈不可用，必須增加具備相應能力的第二工具鏈或等價競態證據；
不得把「本機多跑幾次沒有失敗」算成通過。

> ⚠️ **P0 spike 並未滿足第 5 條。** 其 `layout()` 對全部段落重算 `y_offset`，
> 實測為 $O(N)$（$N=10^6$ 時 p99 4.89ms，佔預算 59%；
> 見 `tasks/p0-spike-report.md` 第六節）。
> **P1 必須真正實作前綴和／延遲物化，不得沿用 spike 的做法。**
>
> 驗收時複雜度必須測到**常數項被超越為止**——三個數量級以內的平坦曲線量到的是常數項，
> 不是複雜度。

#### LAY-0002 D17 的強制閘門：目前狀態

D17 因偏離原建議（未採 `std::shared_ptr`）而升級了七項強制閘門。
基礎實作已完成，**四項達成**（含第 6 項），其餘在 P1 結束前必須補齊：

| # | 閘門 | 狀態 |
|---|---|---|
| 1 | `IntrusivePtr` copy／move／self-assignment／跨型別轉換的精確 retain／release 測試 | ✅ `tests/intrusive_ptr_test.cpp` |
| 2 | 多執行緒隨機複製／釋放；drain 後每個 node 恰好銷毀一次 | ✅ 8 執行緒 × 2000 次 |
| 3 | 舊 snapshot 背景走訪與新 revision 連續發布並行時，舊內容 hash 保持不變 | ❌ **需先有 FlowSequence 與 snapshot 發布** |
| 4 | 最後 reference 於 UI 執行緒釋放時，destructor 在 reclamation worker 執行 | ⚠️ **部分**：延後銷毀已驗證，但背景 worker 尚未實作，目前由測試手動 `drain()` |
| 5 | AddressSanitizer 與 race detector／第二工具鏈 | ❌ **未做**。MSVC 無可假定的 ThreadSanitizer，不得以「本機沒報錯」代替競態證據 |
| 6 | 與 `std::shared_ptr` 基準比較 | ✅ **Spike 4 完成（2026-08-17）**。IntrusivePtr 無效能優勢；偏離理由已改為架構需求（延後銷毀），不再以效能為正當性 |
| 7 | 人工逐行審查所有 memory order、owning edge、borrowed pointer lifetime 與 shutdown drain path | ❌ **需人類**。此項不可由 AI 自我核可 |

**第 6 項已完成但結果翻轉**：benchmark 證明效能不是理由。D17 已改寫偏離理由為
「延後銷毀是架構需求，shared_ptr 無法在保留 make_shared 合併配置的前提下提供 custom deleter」。
新的重開條件見 LAY-0002 D17。

### P1.5 —— 能存檔（刻意可丟棄）

最簡單的檔案讀寫，**不是 authority**。目的只有一個：**讓 Notist 能開始每天被使用。**

dogfood 是整個計畫的驗證機制——抽象設計的錯誤只有在被實際使用後才會暴露。
沒有存檔就沒有 dogfood。

**此格式會在 P4 被丟棄，必須寫進決策**，否則會被誤當成正式格式沿用。

### P2 —— 文件模型完整化

多種 block 型別、stable ID、codec、schema 版本與遷移。

### P3 —— ink layer

獨立圖層、快速路徑、anchor 到文件位置、reflow 時的行為。

#### 相依風險：筆在 iPad 上，開發在 Windows

2026-08-16 確認：日常觸控筆為 **Apple Pencil**，即裝置是 iPad。而 P1–P4 全部只做 Windows。
**因此本階段必須拆成兩半，不能整塊放在 P3。**

| 部分 | 需要真筆嗎 | 排在哪 |
|---|---|---|
| **Krepis 的 ink 資料模型**（圖層語意、anchor、reflow 行為、取樣點儲存） | ❌ 不需要 | **維持 P3**，以合成筆跡與錄製軌跡驗證 |
| **Notist 的擷取路徑、快速路徑繪製、手感與延遲** | ✅ **需要 iPad** | **與 iOS 平台支援綁定，落在 P5** |

這個拆法成立的理由：`spec/decisions/05-ink/` 的範圍本來就明寫「**不含繪製**」。
需要真筆的部分不在 Krepis 裡。

**但風險仍在**：從未摸過真實輸入路徑就設計資料模型，可能把錯誤假設寫死
（取樣率、coalesced 事件、預測點的處理）。

**降低方式**：在 P3 之前，先在 iPad 上錄一批**真實的 Apple Pencil 軌跡**（時間戳、壓力、傾斜），
存成 fixture 供 Windows 端重播。這把「有真實資料」與「有整合好的平台」分開——
前者便宜，後者昂貴。

**未解的前置**：在 iPad 上跑任何自製 app 需要 **macOS ＋ Xcode**。是否具備尚未確認，
見「待確認」。

### P4 —— authority

out-of-process、persistence、identity、permission、protocol、digest。丟棄 P1.5 的暫用格式。

### P5 —— 多平台

Linux、macOS、Android、iOS。此時文字 shaping 可能需要從 DirectWrite 換到 HarfBuzz——
**版面輸出會改變（斷行、行高），golden 測試需重新基準化，這是已接受成本。**

## 待確認

- ~~一幀預算的目標數字~~ → 已定：120Hz，8.33ms 總預算（2026-08-16）。Krepis 的佔比待 P0 實測校正
- **ink 的驗收裝置**：筆為 Apple Pencil（iPad），開發機為 Windows。Krepis 的 ink 資料模型可在
  Windows 以合成／錄製軌跡驗證，但手感與延遲的驗收與 iOS 平台支援綁定。**見 P3 相依風險。**
- ~~是否具備 macOS ＋ Xcode~~ → 已確認：**具備**（2026-08-17）。P3 之前可在 iPad 上錄製
  Apple Pencil 軌跡作為 fixture。
- **依賴管理方式**：引入任何第三方庫前必須先定（見 `spec/decisions/00-foundation/README.md`）
