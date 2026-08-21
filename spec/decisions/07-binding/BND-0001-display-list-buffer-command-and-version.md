# BND-0001：Display list 格式、雙緩衝、同步命令與版本拒絕

## 狀態

**Accepted**

## 日期

- 提出並接受：2026-08-18（使用者逐題裁決 BND-1～BND-4）
- 格式正式化：2026-08-21（依技術決策授權）

## 背景

`FND-0002` 固定 C ABI 只使用 C 型別、status code 與明確所有權；P0 又證明邊界必須是一段 display
list，而不是外殼逐物件查詢。尚未固定的部分是 byte format、buffer lifetime、command 時序與版本
不符行為。這些若含糊，外殼可能讀到被覆寫的 frame 或把未知 opcode 當成舊命令。

資料流見
[`docs/architecture/display-list-boundary-data-flow.md`](../../../docs/architecture/display-list-boundary-data-flow.md)。

## 專業名詞

| 名詞 | 定義 |
|---|---|
| display list | 核心產生、按繪製順序排列的 binary command stream |
| frame token | 單調遞增的 frame 身分，用來辨識 span 是否仍屬預期畫面 |
| double buffer | 核心交替使用 front／back 兩段儲存，使產生下一幀時不覆寫目前 front |
| read-only span | C ABI 的 `{const uint8_t* data, size_t size, uint64_t frame_token}` 借用視圖 |
| TLV command | 固定 command header 加指定長度 payload；長度使 decoder 能做邊界檢查 |
| fail closed | 版本、長度或 opcode 不合法時整幀拒絕，不畫部分結果 |

## D1：固定 little-endian header 與 8-byte 對齊 command

所有多位元組整數與 IEEE-754 float 使用 little-endian。Frame header 固定 32 bytes：

| Offset | 型別 | 欄位 | 契約 |
|---:|---|---|---|
| 0 | `uint32` | magic | ASCII `KRDL` 的 little-endian 值 |
| 4 | `uint16` | major | 不相容格式版本 |
| 6 | `uint16` | minor | 同 major 下的向後相容擴充版本 |
| 8 | `uint32` | header_size | 第一版固定 32，decoder 仍需核對 |
| 12 | `uint32` | byte_size | 含 header 的完整長度 |
| 16 | `uint32` | command_count | command 數量上限檢查依據 |
| 20 | `uint32` | flags | 第一版必須為 0；未知 bit 拒絕 |
| 24 | `uint64` | frame_token | 每次成功產出遞增 |

每個 command header 固定 8 bytes：`{uint16 opcode, uint16 flags, uint32 byte_size}`。`byte_size`
包含 command header，至少為 8 且向上對齊 8 bytes；padding 必須為 0。Payload 只含固定寬度值、
offset 與 count，不含 native pointer、`size_t`、C++ enum layout 或平台 handle。

### Decoder 演算法

1. 先以 checked arithmetic 驗證 frame header、`byte_size` 與可用 span 完全一致。
2. 核對版本、flags 與 command_count 合理上限。
3. 逐 command 驗證 header、對齊、payload 長度、offset／count 與 opcode-specific invariant。
4. 全部通過後才提交給 renderer；任一失敗整幀拒絕。

Decoder 時間為 `O(B + C)`，`B` 是 byte size、`C` 是 command count；額外空間第一版為 `O(1)`
加 renderer 自身資料，不需要複製整段 buffer。

## D2：核心擁有雙緩衝，外殼以顯式 lease 借用 front span

核心在 back buffer 完整 encode 並自我驗證後，才交換 front／back 並增加 frame token。失敗時 front
與 token 都不變，外殼可以繼續顯示上一個有效 frame。Span 在對應 lease 明確 release 前有效；外殼
不得直接釋放或修改 buffer。兩個 slot 都仍被 lease 時，下一次 `begin_frame` 回 `invalid_state`，不得
覆寫舊 pointer。

這裡修正本文件原先「下一次 publish 即失效」的較弱文字，以符合 `FND-0002 D3` 已接受的顯式
release 契約。代價是外殼漏 release 會造成 backpressure；配套測試比原建議更嚴格：同時持有兩幀、
確認第三幀被拒絕、釋放後恢復，並要求 acquire count 恰等於 release count。

Buffer 容量可保留並成長；當單一 slot 容量超過 8 MiB、當幀使用量不到容量 25%，且該
slot 連續 60 次 publish 都符合低使用時，核心才在它成為未借用 back slot 時縮容。任一次
回到高使用即將計數歸零。外殼不能提供容量或要求核心在容量不足時截斷命令。

這個遲滯策略的目的是同時排除兩種靜默劣化：單次異常大頁不會永久占著高水位，而大／小頁
面交替時也不會在每幀重複釋放與重新配置。

## D5：P1 opcode 與 payload

| Opcode | 值 | Payload | 驗證 |
|---|---:|---|---|
| `DrawRect` | 1 | `float32 x,y,w,h`、`u32 RGBA` | finite、非負尺寸；command 32 bytes |
| `DrawGlyphRun` | 2 | font ID、26.6 baseline/font size、color、direction、glyph array | 非空、合法方向；每 glyph 固定 24 bytes |
| `PushClip` / `PopClip` | 3 / 4 | rect / 無 | clip stack 不可 underflow，frame 結束必須歸零 |
| `PushTransform` / `PopTransform` | 5 / 6 | 六個 affine `float32` / 無 | finite；transform stack 必須平衡 |

`DrawGlyphRun` 的 glyph entry 是 `u32 glyph_id`、五個 26.6／offset `i32`（其中 cluster offset 為
`u32`），不傳 native font pointer。P1 C ABI 的 `KrepisGlyph` 固定 24 bytes，並以 C 與 C++ 編譯測試
共同鎖定。

Release benchmark（WSL，2026-08-21）以 4,000 幀、每幀 200 個 20-glyph runs，加背景與 clip，
同時計入 encode、publish 自驗、consumer 再驗與 lease：

| commands/frame | bytes/frame | 兩 slot retained | p50 | p99 | max |
|---:|---:|---:|---:|---:|---:|
| 203 | 104,096 | 266,624 | 29.713 µs | 65.193 µs | 160.632 µs |

p99 只有 3 ms gate 的 2.2%，同步通道不需重開。4,200 acquire／release 全數對帳。Peak→steady
workload 將 9,600,104 bytes retained 降到 128 bytes，縮容花費 1,075.04 µs，仍低於 3 ms。

## D3：Command 通道同步，p99 超標即重開

鍵盤、IME 與核心需要立即回應的指標命令使用同步 C ABI：呼叫返回時 transaction 已成功／失敗，
並可取得對應的新 frame token。背景 shaping／layout 可以使用 immutable snapshot，但同步路徑只能
等待 P1 明定的必要工作。若正式 workload 的同步路徑 p99 超過 3ms，必須重開 BND-2，不能由
Flutter 複製規則做 optimistic rendering。

## D4：版本與錯誤分級

- `major` 不相同：`version_mismatch`，整幀拒絕。
- 相同 major、producer minor 大於 decoder 支援值：`version_mismatch`。
- producer minor 較舊：只要所有 opcode／size 都合法即可讀取。
- 未知 opcode、未知 flags、長度錯誤或算術溢位：不可恢復的 frame decode error，整幀拒絕並要求
  重建／升級，不跳過未知命令。
- 可恢復命令錯誤（例如 stale selection）回傳 status 並保留舊 frame；invariant 破壞、OOM 與核心
  programmer error 依 `FND-0002` 終止，不偽裝成一般狀態碼。

## 具體例子

Producer 產生版本 1.2、`byte_size=96`、兩個 command。支援 1.3 的外殼可以讀取；支援 1.1 的外殼
必須回 `version_mismatch`。若第二個 command 宣告 40 bytes 但剩餘 span 只有 32 bytes，即使第一個
command 完全合法，也不能先畫第一個再報錯，必須保留上一幀。

另一例是使用者曾開啟一個形成 9.6 MB display list 的超大空間頁，之後回到只有幾個區塊的
流式頁。核心不在第一個小 frame 立即縮容；只有當同一 slot 連續 60 次都低於 25% 使用率，
才在它未被 lease 的時候釋放尖峰容量。

## Invariant 與拒絕行為

- Publish 前必須完整 encode 與驗證，front buffer 永遠是一個完整有效 frame。
- 任何 offset＋length 都使用 checked arithmetic，禁止 unsigned wraparound。
- 外殼不能根據 opcode 重新解釋文件規則，只能畫核心提供的值。
- ABI struct 必須有固定寬度欄位與 size／version；不得跨邊界傳 C++ container 或 ownership pointer。

## 後果與驗證

- TLV 與明確版本增加少量 header bytes，換得可驗證的 skip boundary；第一版仍對未知 opcode fail
  closed，以免舊外殼靜默漏畫必要命令。
- 雙緩衝使用約兩倍峰值 display-list 記憶體，避免每幀配置與讀寫競態。
- Property test 必須覆蓋 round-trip、截斷每一 byte、錯誤 size、未知 opcode／flags、版本矩陣與
  frame-token lifetime。
