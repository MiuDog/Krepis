# DOC-0002：ObjectId 的位元表示、強型別包裝、生成與編碼

## 狀態

**Accepted**

依 [`spec/decisions/README.md`](../README.md) 的決策授權：本題四項全部是純技術選擇，
有可證明的正確性判準，不涉及產品語意、不擴張領域中立範圍、不依賴未取得的實測數字。

## 日期

提出：2026-08-17
接受：2026-08-17（依決策授權自行裁決）

## 背景

[`DOC-0001` D6](DOC-0001-object-tree-stable-id-reference-and-composition.md) 固定
`ObjectId = 128-bit random opaque value`、由核心生成、生成器可替換、載入保留原 ID、
匯入碰撞必須拒絕、刪除後永不重用，並要求 C++ 以強型別包裝同一種底層 ID。

但它把「C ABI 表示、byte order 與文字格式」留給 `BND-*`，並在「尚未決定」列出
「ObjectId 的隨機來源、序列化 byte order 與文字表示」。**這些未定使 ObjectId 無法實作**，
而 ObjectId 是 ObjectStore、LocationIndex、FlowSequence 與所有引用的掛載點。

本決策補上這四項。C ABI 的**傳遞形狀**仍屬 `BND-*`；本決策只固定**值本身的表示**，
兩者不衝突：`BND-*` 決定用什麼參數形式跨邊界，本決策決定那 16 個位元組的意義。

## 專業名詞

| 名詞 | 定義 |
|---|---|
| **ObjectId** | Workspace 全域唯一的 128-bit 永久身分。不含 owner、位置、種類或建立順序 |
| **nil id** | 保留值 `{high=0, low=0}`，表示「沒有物件」。生成器永不產生 |
| **強型別包裝** | 以 tag 型別區分用途的 ObjectId 別名，例如 `BlockId`。底層位元相同，型別不可互換 |
| **正規編碼（canonical encoding）** | 序列化與文字表示使用的唯一位元組順序，見 D4 |
| **RandomSource** | 提供密碼學品質亂數位元組的平台介面 |

## D1：位元表示為兩個 `uint64_t`，比較採 `{high, low}` 無號字典序

### 決策與邊界

```text
ObjectId { uint64_t high; uint64_t low; }
```

- 比較為 `{high, low}` 的 unsigned lexicographic order（與
  [`LAY-0002` D13](../02-layout/LAY-0002-invalidation-offset-and-viewport-index.md) 的 `LeafKey` 同慣例）。
- `{0, 0}` 是保留的 **nil id**，代表「沒有物件」。**生成器永不產生 nil**。
- 排序**沒有文件語意**。ObjectId 的順序只用於容器索引與確定性測試，
  **不得**被解讀為建立順序或文件順序（`DOC-0001` D6）。

理由：MSVC 沒有可依賴的原生 128-bit 整數型別。專案已在 `LeafKey` 採用同一形狀，
保持單一慣例可讓比較、雜湊與除錯輸出共用同一套心智模型。

### Invariant 與拒絕行為

- 生成器產出 nil 時**必須重試**，不得回傳 nil。
- 需要「可能沒有」的欄位一律使用 nil，**不使用額外的 optional 旗標**——
  兩份表示會產生「optional 為空但值非 nil」的靜默不一致。

## D2：強型別包裝以 tag 區分，不可隱式互換

### 決策與邊界

```text
template <typename Tag> class TypedObjectId;

using PageId      = TypedObjectId<PageTag>;
using ContainerId = TypedObjectId<ContainerTag>;
using BlockId     = TypedObjectId<BlockTag>;
using EmbedId     = TypedObjectId<EmbedTag>;
```

- 不同 tag 之間**沒有隱式轉換**，也沒有共同基底類別。
- 轉換必須經由具名函式明確表達意圖，且該函式必須出現在審查清單上。
- 底層位元與 nil 語意完全相同；型別只存在於編譯期。

理由：`DOC-0001` D6 要求共用同一 namespace 但以強型別包裝。把 `BlockId` 誤傳成
`ContainerId` 是典型的靜默錯誤——查找會失敗或查到別的物件，而兩者都不會有徵兆。
編譯期攔截的成本為零。

### 具體例子

```cpp
BlockId block = store.create_block();
container.append(block);        // 正確
flow_index.rank(block);         // 正確
store.find_container(block);    // 編譯失敗：BlockId 不是 ContainerId
```

## D3：生成介面可替換，預設為平台 CSPRNG

### 決策與邊界

```text
class RandomSource { virtual void fill(std::span<std::byte>) = 0; };
class IdGenerator  { virtual ObjectId next() = 0; };
```

- `RandomIdGenerator` 由 `RandomSource` 取 16 個位元組；**產出 nil 時重試**。
- `SequentialIdGenerator` 供測試使用，產生確定性遞增序列，且**跳過 nil**。
- Windows 的 `RandomSource` 實作使用 `BCryptGenRandom`。
  這是**系統 API 而非第三方依賴**，不受 `FND-0003` 約束。
- 平台實作藏在介面後（`spec/architecture.md` 的平台策略），輸出是純位元組，
  **不洩漏任何平台 handle**。

### 為何要求密碼學品質

ObjectId **不是安全 token**，可預測性本身不是威脅模型的一部分。選 CSPRNG 的理由是：

1. 它同時給出足夠均勻的分佈，使 128-bit 空間的碰撞機率由生日界主導
   （約 `2^64` 個 ID 才達到顯著碰撞機率）。
2. 若 ID 未來出現在匯出檔或 URL 中，不會反推出建立時間或數量。
3. 自行以 `mt19937_64` 拼裝需要自證分佈與播種品質，成本高於直接用平台 API。

碰撞的**正確性**保障不依賴機率：`DOC-0001` D6 已要求匯入碰撞必須拒絕。
機率只決定該拒絕路徑多久被觸發一次。

### 演算法流程

正常路徑、邊界路徑與失敗路徑：

1. 向 `RandomSource` 要 16 位元組。
2. 以 D4 的正規順序組成 `{high, low}`。
3. 結果為 nil → **回到步驟 1 重試**（邊界路徑，機率為 `2^-128`）。
4. `RandomSource` 失敗 → 依 `FND-0002` D5，這是不可恢復狀況，**終止**。
   身分無法生成時繼續執行只會產出損壞的文件。

### 複雜度

`next()` 為 `O(1)`；成本由 `RandomSource` 決定，與 Workspace 大小無關。

## D4：正規編碼為 16 位元組 big-endian，文字為 32 個小寫十六進位字元

### 決策與邊界

```text
bytes[0..7]  = high，big-endian
bytes[8..15] = low， big-endian
文字          = 32 個小寫 hex 字元，無分隔符
```

- 序列化、雜湊輸入與文字表示**共用同一個位元組順序**。
- 文字解析**只接受**恰好 32 個十六進位字元；**拒絕**大寫、分隔符、`0x` 前綴與任何長度偏差。
- 記憶體中的 `{high, low}` 是主機位元組序；**跨越儲存或邊界時一律轉為正規編碼**。

### 為何不使用 UUID 的破折號格式

ObjectId **不是 UUID**：它沒有 version 與 variant 位元，也不承諾 RFC 4122 的任何語意。
借用 `8-4-4-4-12` 的外觀會讓讀者（與未來的工具）誤以為可以套用 UUID 的解析與假設，
那是會被沿用很久的誤導。固定 32 字元、無分隔符，使「這不是 UUID」在肉眼層級即可辨識。

### 為何是 big-endian

序列化格式與文字表示的位元組順序一致，使十六進位字串與二進位傾印可以直接肉眼比對；
除錯時不需要在腦中翻轉位元組。寫入與讀取各一次位元組序轉換，成本可忽略。

### 具體例子

```text
high = 0x0123456789ABCDEF
low  = 0xFEDCBA9876543210

bytes = 01 23 45 67 89 AB CD EF FE DC BA 98 76 54 32 10
text  = "0123456789abcdeffedcba9876543210"
```

邊界例子（必須被拒絕）：

```text
"0123456789ABCDEFFEDCBA9876543210"      → 拒絕：大寫
"01234567-89ab-cdef-fedc-ba9876543210"  → 拒絕：含分隔符
"0x0123456789abcdeffedcba9876543210"    → 拒絕：含前綴
"0123456789abcdeffedcba987654321"       → 拒絕：長度 31
```

nil 的文字表示為 32 個 `0`。

### Invariant 與拒絕行為

- `decode(encode(id)) == id` 對所有 ObjectId 成立（含 nil）。
- `parse(to_text(id)) == id` 對所有 ObjectId 成立（含 nil）。
- 解析失敗回傳 `Error{ErrorCode::invalid_argument}`，**不得**回傳 nil ——
  以 nil 表示解析失敗會讓「無物件」與「輸入損壞」無法區分，是靜默錯誤。

## 後果

### 正面

- ObjectStore、LocationIndex、FlowSequence 與所有引用取得可實作的掛載點。
- 型別錯誤在編譯期攔截，成本為零。
- 文字與二進位表示一致，除錯可直接比對。

### 負面

- 每個 ID 佔 16 位元組。以每個 Block 若干個 ID 計，百萬 Block 的身分成本在數十 MB 量級——
  **這是 128-bit 全域唯一身分的固有代價，不是本決策引入的**。
- 強型別包裝使需要泛型處理任意物件的程式碼必須明確轉換。
- Windows 的 `RandomSource` 需連結 `bcrypt`；其他平台的實作在對應平台工作展開時補上。

### 驗證責任

- round-trip property test（encode／decode、to_text／parse），涵蓋 nil、全 1、隨機值。
- 解析拒絕測試，逐項涵蓋上方四種邊界例子。
- 生成器測試：`SequentialIdGenerator` 的確定性與跳過 nil；大量生成無重複。
- 型別安全以**編譯失敗**驗證，不以執行期斷言驗證。

## 未決參數

無。本決策不含需要 benchmark 或平台證據的數值。

## 相關決策

- **本決策依賴**：[`DOC-0001`](DOC-0001-object-tree-stable-id-reference-and-composition.md) D6、
  [`FND-0002`](../00-foundation/FND-0002-c-abi-error-memory-and-threading.md)（錯誤模型、D5 終止語意）。
- **依賴本決策**：`DOC-*` 後續（ObjectStore、LocationIndex）、
  [`LAY-0002`](../02-layout/LAY-0002-invalidation-offset-and-viewport-index.md)（D10、D14 以 ObjectId 解析 slot）、
  `BND-*`（C ABI 的傳遞形狀，須沿用本決策的正規編碼）、`EDT-*`、`INK-*`。

## 聚焦架構文件

生成、編碼與拒絕路徑的資料流見
[`docs/architecture/object-id-generation-and-encoding.md`](../../../docs/architecture/object-id-generation-and-encoding.md)。
