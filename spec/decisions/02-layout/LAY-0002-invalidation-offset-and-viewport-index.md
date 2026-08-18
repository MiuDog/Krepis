# LAY-0002：失效傳播、偏移與可見範圍索引

## 狀態

**Proposed**（D1–D20 已接受；完整失效規則待決）

> **D17 的強制閘門尚未全部關閉**：閘門 5（ASan／TSan）未執行、閘門 7（人工審查）未通過。
> 見本檔 D17 的「閘門狀態」節。**依賴 D17 的實作可以繼續，但 D17 的偏離尚未完成驗證。**

## 日期

- 提出：2026-08-16
- D1–D11 接受：2026-08-16
- D12–D19 接受：2026-08-17
- D20（分塊參數，由 benchmark 定案）接受：2026-08-17

## 背景

P0 已證明若每次編輯都重寫全部後續 Block 的絕對 `y`，工作量會隨文件長度成長。P1 必須以
累積高度索引從 viewport 找出可見 Block，並讓單一 Block 高度改變只更新聚合路徑。

本檔已選定 Chunked B+ tree／Block rope 作為正式方向；分塊參數、延遲物化與完整失效規則
尚未裁決，因此整份 `LAY-0002` 尚未完成。

## D1：高度是 layout cache，不是 Block 內容

Block 不保存權威高度或永久絕對 `y`。高度由特定 LayoutContext 產生，至少受可用寬度、
文字樣式 revision、字型環境 revision 與 scale 影響。同一 Block 可以在不同 LayoutContext
擁有不同 cache；cache 可失效、可重建，不得成為 ObjectStore 的第二份權威內容。

```text
LayoutEntry {
    block_id,
    content_revision,
    measured_height,
    measurement_status
}
```

## D2：Flow 以累積高度索引解析 viewport

每個 FlowContainer 依 LayoutContext 維護自己的累積高度索引，至少支援：

```text
update_extent(block_id, new_height)
insert(block_id, position, height)
remove(block_id)
move(block_id, new_position)
prefix_extent(position)
lower_bound_extent(y)
rank(block_id)
```

渲染器以 viewport 起訖座標執行 `lower_bound_extent`，只對可見範圍及 overscan 內的 Block
產生 display list。高度改變只更新該項與聚合路徑；後方 Block 的絕對 `y` 在查詢時由
prefix extent 求出，不逐項重寫。

巢狀 FlowContainer 向父容器只回報自己的總 extent。SpatialContainer 沒有線性前綴順序，
其 viewport 查詢另用矩形交集或空間索引。

## D3：FlowRangeEmbed 沿用來源布局，不依引用框重新換行

FlowRangeEmbed 先以 Flow 的一般（非全寬）模式產生來源布局；放入 SpatialContainer
後，不依 Spatial frame 的寬度重新換行。

Flow 只有兩種寬度模式：**一般**與**全寬**。Embed 一律使用一般寬度的來源布局，與使用者在
流式頁面看到的結果一致。

- Spatial Embed 不因自己的 frame width 建立新的文字 LayoutContext。
- Embed 保留來源斷行、Block extent 比例與整體長寬比。
- 來源內容或一般模式的布局參數改變時，先更新來源布局，再讓 Embed 顯示更新後的結果。
- **Spatial frame 寬度填滿來源布局寬度；高度方向若超出 frame 則裁切並允許垂直捲動。**
  Frame 不做水平裁切或等比例整體縮放。
- Viewport 只保存顯示範圍與垂直捲動偏移，不做非等比拉伸。

這使 FlowSequence 與主要 FlowLayoutIndex 不必為每個 Spatial 引用框複製一份不同寬度的高度
索引；在空間頁面中，文字以來源寬度呈現，長文件以垂直捲動瀏覽。

## D4：Flow 使用專用 Chunked B+ tree／Block rope

FlowContainer 的權威 child sequence 使用專用 Chunked B+ tree（亦可描述為 Block rope），不採
扁平 vector 加 Fenwick，也不採每個 Block 一個節點的二元 order-statistic tree。

```text
FlowSequence
├─ InternalNode
│  ├─ children
│  └─ subtree_block_counts
└─ LeafNode
   └─ entries: Sequence<BlockId>
```

FlowSequence 只保存權威順序。每個有效 LayoutContext 使用相同分塊思路建立可重建的聚合索引：

```text
FlowLayoutIndex
├─ source_sequence_revision
├─ InternalNode { subtree_counts, subtree_extents }
└─ ExtentLeaf { BlockId, measured_extent, status }
```

- Sequence 與 LayoutIndex 是不同責任，不共用一份權威高度。
- LayoutIndex 只能依 FlowSequence transaction result 更新順序，不自行猜測或維護第二套規則。
- 插入、刪除、移動、rank、prefix extent 與 `y → Block` 必須維持對數或以固定 leaf 容量為界
  的攤銷成本。
- Viewport 連續走訪以 leaf 內連續記憶體為主，避免每個 Block 一次指標跳轉。
- ObjectStore 仍保存 BlockRecord；B+ tree leaf 只保存 BlockId 與該索引責任所需資料。

Leaf 容量、internal fanout 與 split／merge／redistribution 門檻見 **D20**（已定案）。

## D5：動態頁是 ownership 邊界，固定頁是 layout fragment

Flow 支援動態頁長與固定頁長，但兩者不共用相同的資料語意。

### 動態頁長

每個動態頁是具有 stable ID 的實際 FlowContainer。頂層 Page 可以指向該 Container；巢狀動態
頁則以 `OwnedContainerBody` 存在於父 Container。其高度隨內容成長，頁面邊界是 ownership
邊界，不因內容高度自動把 Block 搬到相鄰動態頁。

### 固定頁長

固定頁由 Viewport 的 pagination policy 對同一 FlowSequence 產生衍生 PageFragment：

```text
FixedPagination { page_height, margins }

FlowSequence [A, B, C, D, E]
    -> Fragment 1 [A, B]
    -> Fragment 2 [C, D]
    -> Fragment 3 [E]
```

- PageFragment 沒有 PageId、ContainerId 或 ownership。
- 自動換頁不得產生 Block move transaction。
- Viewport、字型或 Block extent 改變時可以重新計算 fragment。
- 不適合剩餘空間的 Block 由 pagination policy 排到下一個 fragment。
- 使用者指定的強制換頁以具有 BlockId 的 `PageBreak` 布局控制 Block 表達。
- PageBreak 是權威 FlowSequence 內容；自動產生的 fragment 不是。

固定頁邊界使用 FlowLayoutIndex 的 subtree extent 與 `lower_bound_extent` 尋找，不另建一份
扁平頁面索引作為權威資料。

## D6：固定分頁由 Block body 宣告可分割能力

固定分頁不把所有 Block 一律視為不可分割。每一種 Block body 必須宣告自己是否支援分割，
以及允許的分割邊界。

- Paragraph 可在排版後的行邊界分割。
- 同一個 Paragraph 可以在相鄰固定頁上產生多個暫時 `PageFragment`。
- 每個片段只記錄同一個 `BlockId` 與該片段涵蓋的內部範圍；不配置新的 `ObjectId`、不取得
  ownership，也不形成獨立 undo 單位。
- selection 可以跨越多個片段，但語意上仍位於同一個 Block。
- 寬度、字型或內容變更造成重新排版時，舊片段直接失效並重新產生。
- 不支援分割的圖片或 embed 保持原子性；剩餘頁面放不下時，整個移到下一頁。

## D7：過高的不可分割 Block 採 body-specific policy

不可分割 Block 高於整頁可用高度時，layout core 不推測內容是否適合縮放：

- 核心預設保留原尺寸並允許 overflow，避免文字或互動內容被靜默縮成不可用狀態。
- Block body 可以明確宣告 `ScaleToFit`；縮放必須保持比例，而且只縮小、不放大。
- 圖片類 body 可使用 `ScaleToFit`；其他 body 是否支援縮放由各自契約決定。
- client 只能提交 viewport 與顯示意圖，不能自行覆寫 policy；最終結果由 layout authority 裁決。
- overflow 與縮放結果都屬 layout output，不改寫 Block 內容或 ownership。

## D8：authority 發布不可變 revision snapshot

每次成功交易都由 authority 一次發布新的 `DocumentRevision`。背景排版取得的是不可變 snapshot
handle，而不是仍會被 UI 執行緒修改的 live object。

```text
DocumentRevision {
    snapshot_id { content_revision, storage_generation }
    object_store_snapshot
    flow_sequence_roots
}
```

- `DocumentRevision` 必須涵蓋 ObjectStore 內容與所有 FlowSequence root，避免背景工作讀到「新順序配
  舊內容」或「舊順序配新內容」的混合狀態。
- FlowSequence 節點發布後不可再原地修改。交易只複製 root 到受影響 leaf 的路徑；未受影響的
  subtree 由新舊 revision 共用。
- Block 內容修改也必須產生屬於新 revision 的 immutable record view；ObjectStore 依 D10 採分頁
  copy-on-write。
- authority 完成所有變更後才原子發布新的 snapshot handle。讀者只能取得完整的舊 revision 或完整的
  新 revision。
- 背景 layout job 與結果都攜帶來源 `SnapshotId`。P1 先採保守規則：結果回來時若不等於目前
  `SnapshotId`，整份結果丟棄；未來只有在具備可驗證 dependency key 後，才允許部分沿用。
- 舊 revision 的節點在最後一個編輯、排版或渲染讀者釋放 snapshot 後，依 D9 移交背景回收。

資料流與 copy-on-write 範例見
[`docs/architecture/layout-revision-snapshot-data-flow.md`](../../../docs/architecture/layout-revision-snapshot-data-flow.md)。

## D9：引用計數保活，最後釋放移交背景回收

P1 使用引用計數管理 immutable snapshot 與共享節點的生命週期：

- 編輯、排版或渲染工作持有 snapshot root 時，該 root 可到達的全部節點都必須保持有效。
- 樹內共享 subtree 也由引用計數保護；新舊 revision 可以安全共用同一批 immutable 節點。
- 最後一個 root／node reference 被放開時，不在 UI 執行緒同步遞迴銷毀大型 subtree，而是把回收工作
  移交背景 reclamation queue。
- P1 不採 epoch reclamation，避免 reader registration 遺漏造成靜默 use-after-free。
- 計數器依 D17 使用封裝後的 intrusive atomic reference count；不得在業務程式碼直接操作計數值。
- 背景回收不得改變 revision 可見內容，也不得把釋放責任交給 client。

## D10：ObjectStore 使用穩定 slot 與分頁 copy-on-write

authority 內部以 `IdDirectory` 將不透明 `ObjectId` 解析成 `ObjectSlot`，再由當前 snapshot 的
page-table root 找到 immutable record：

```text
ObjectId -> IdDirectoryGeneration -> ObjectSlot
ObjectStoreSnapshot(page_table_root) -> RecordPage -> RecordPtr
```

- `ObjectSlot` 在同一個 directory generation 內一經配置便不改變，也不因刪除立即重用。
- 修改內容時建立新的 immutable record，複製包含該 `RecordPtr` 的 page，再複製 page-table root
  到該 page 的短路徑；其他 page 與 record 由新舊 revision 共用。
- 刪除在新 revision 對應的 slot 寫入 tombstone。舊 snapshot 仍能從自己的 page root 讀取舊 record。
- `IdDirectory` 是 authority-owned、可由持久資料重建的內部索引，不是 client 可提交或覆寫的權威資料。
- 一般新增期間 directory 只追加映射。舊 snapshot 即使能解析後來新增的 slot，也會因自己的 page
  table 沒有該 record 而得到 `NotFound`。
- Compact 若要重新配置 slot，必須建立新的 `IdDirectoryGeneration` 與對應 page-table root，並在同一個
  `DocumentRevision` 原子發布；舊 snapshot 繼續持有舊 generation，直到依 D9 安全回收。
- Record page 容量、page-table fanout 與 compact 門檻屬 benchmark 參數，本決策不先固定數值。

## D11：Flow LocationIndex 保存 owner 與 leaf key，不保存 posIndex

每個 revision 的 Flow entry 保存：

```text
LocationIndex[BlockId] = { owner_container_id, FlowLocator { leaf_key } }
```

- `leaf_key` 指向 FlowSequence 中的一個邏輯 chunk，由 authority 管理；它不是 `ObjectId`，也不是
  client 可提交的權威位置。
- 查找位置時先由 owner 取得 sequence root，再以 `leaf_key` 找到當前 revision 的 leaf，最後在固定
  容量 leaf 內掃描 `BlockId`。
- `posIndex` 由 leaf 前方的 subtree block count 加上 leaf 內 offset 即時計算，不保存為權威欄位。
- 在同一 leaf 內插入或刪除時，其他 Block 的 LocationIndex 不需更新。
- Leaf split／merge 時，只更新實際移到其他 `leaf_key` 的 Block；更新量受 leaf 容量限制，不隨整份
  文件長度成長。
- LocationIndex 與 FlowSequence 必須在同一個 authority transaction、同一個 revision 原子發布。
- Compact／重建可以改變 `leaf_key`，但不能改變 `BlockId`；selection、reference 與 undo 不得以
  `leaf_key` 作為持久錨點。
- Spatial entry 使用 `SpatialLocator` 指向該 owner 的 placement；它不具有 `leaf_key` 或 `posIndex`。

## D12：FlowSequence 是唯一位置權威，LocationIndex 是可重建索引

Block 的 owner 與順序只由 FlowSequence／Spatial placement 表達。`LocationIndex` 是 authority-owned
的加速索引，不是第二份權威資料：

- 交易先在新的 immutable FlowSequence root 上完成結構修改，再由同一個 transaction builder 更新
  受影響的 LocationIndex entries。
- 發布前至少驗證所有受影響 leaf：leaf 中每個 `BlockId` 必須恰有一筆相符 owner／leaf key 的 entry；
  不允許一個 Block 同時出現在兩個 ownership 位置。
- 驗證失敗時拒絕發布整個新 revision，不能選一邊覆寫另一邊，也不能由 client 修補。
- 載入或診斷時可以完全從 ownership tree 重建 LocationIndex。
- Runtime query 發現不一致時回報 invariant failure；不得靜默全表掃描後繼續，避免掩蓋交易 bug。

具體例子：若 FlowSequence 是 `[A, B, C]`，但 `LocationIndex[B]` 指向另一個 Container，正確結果是
拒絕該 snapshot，而不是把 B 搬到索引所說的位置。

## D13：LeafKey 使用 128-bit 稀疏排序標籤與局部重新編號

每個邏輯 leaf 具有 authority-owned `LeafKey`。MSVC 沒有可依賴的原生 `uint128`，因此值型別以
兩個 `uint64_t` 組成，按 `{high, low}` 做 unsigned lexicographic comparison。

一般 split 使用 midpoint insertion：

```text
left  = 1000
right = 2000
new   = midpoint(left, right) = 1500
```

- 若左右 key 間至少有一個可用整數，配置中點為 `O(1)`。
- 若反覆在同一區間 split 導致沒有間隙，選取附近 leaf window，以 128-bit 空間重新均勻編號。
- Window 先從固定小範圍開始；空間仍不足時幾何擴張，直到能留下足夠間距。初始 window 與目標間距
  是 benchmark 參數。
- Relabel 與所有受影響 LocationIndex entries 在同一 transaction 發布。
- LeafKey 可以在 compact／重建時改變，不能序列化為 selection、reference 或 undo anchor。

固定 16-byte key 使比較與儲存有上限；代價是極端密集 split 會觸發低頻局部 relabel。

## D14：LocationIndex 使用 ObjectSlot-indexed paged COW table

LocationIndex 與 ObjectStore 共用 `ObjectId -> ObjectSlot` 的解析結果，但擁有獨立的 immutable
page-table root：

```text
BlockId -> ObjectSlot -> LocationPage
    -> optional { owner_container_id, FlowLocator { leaf_key } | SpatialLocator { placement_key } }
```

- Lookup 先做一次 IdDirectory resolve，再以 slot 的 page number／offset 直接定位，接近 `O(1)`。
- 更新 entry 只複製所在 LocationPage 與 page-table 的短路徑。
- Detached、已刪除或不是 ownership child 的 ObjectSlot 保存 empty entry。
- Leaf split／merge 可能更新多個 entry，但數量受 leaf 容量限制；新舊 revision 共用未改動 page。
- LocationPage 容量與 page-table fanout 由 benchmark 決定，不預先寫死。

具體例子：`ObjectSlot 520` 若落在 page 2 的 offset 8，移動該 Block 只建立新版 page 2；page 0、1、3
仍由前一個 revision 共用。

## D15：跨 leaf 走訪使用 snapshot-bound TreeCursor

FlowSequence leaf 不保存 `next`／`previous` sibling pointer。每次走訪建立只屬於單一 snapshot 的
`TreeCursor`：

```text
TreeCursor {
    snapshot_handle
    ancestor_frames[] { internal_node, child_index }
    current_leaf
    local_offset
}
```

- 以 LeafKey seek 時建立 root-to-leaf ancestor stack，成本為 `O(log L)`，其中 `L` 是 leaf 數量。
- Leaf 內逐 Block 前進為 `O(1)`。
- Leaf 結尾時向上回退到仍有下一個 child 的 frame，再下降到該 subtree 最左 leaf；連續掃描的
  leaf 切換為攤銷 `O(1)`。
- Cursor 持有 snapshot root，內部 node pointer 只作借用；cursor 不得跨 revision 使用或持久化。
- 不維護 sibling linked list，避免 persistent split 迫使前方 leaf 連鎖 copy-on-write。

## D16：Split／merge 採有遲滯區間的延遲重平衡

結構規則先固定，數值由 benchmark 決定：

- Leaf 寫滿後才 split，內容約平均分配。
- 刪除後低於一半不立即 merge；只有低於較低 low-water mark 才啟動重平衡。
- 重平衡先向相鄰 leaf redistribution；鄰居也無法維持政策時才 merge。
- Split threshold、low-water mark 與 redistribution window 必須有明顯遲滯，避免 delete／undo 在
  同一邊界反覆 split 與 merge。
- Internal node 使用相同原則；root 可以是例外並在只剩單一 child 時降高。

例示容量若為 8，插入第 9 筆可 split 成 4＋5；刪到 3 筆不必立即 merge。此處的 8、4、5、3
只說明演算法，不是正式參數。

## D17：使用 intrusive atomic reference count

P1 的 immutable snapshot、B+ tree node、page-table node 與共享 page 使用封裝後的
`IntrusivePtr<const T>`。Owning edge 必須使用此型別；raw pointer 只能在已有 owning root 保活的
詞法範圍內借用。

計數與釋放流程：

```text
retain(existing owner): fetch_add(1, relaxed)
release():
    if fetch_sub(1, release) == 1:
        acquire fence
        transfer node to reclamation queue
```

- 只能從既有 owning reference 複製新 reference；禁止由未保護 raw pointer `retain`，因此 P1 沒有
  weak acquire、resurrection 或 lock-free pointer promotion。
- 不使用 macro 建立或保護 owner。Macro 不能限制 access、constness 或 lifetime；硬邊界由 private
  `retain`／`release`／adopt path、`IntrusivePtr<const T>` 與唯一公開 factory 建立。
- `make_intrusive<T>(...)` 一次完成 immutable node 建構並回傳 `IntrusivePtr<const T>`。發布後沒有
  owning-edge setter；新 node 只能在 constructor 接收已存在的 child owner，以 bottom-up 建構保證
  owning edge 不可能指回尚未存在的 parent。
- Reference count 從 1 開始；underflow、overflow、double release 在測試與診斷建置必須立即失敗。
- Node 發布後完全 immutable；owning edge 只向下形成 DAG，不得形成 reference cycle。
- 最後一次 release 把唯一銷毀責任以 `noexcept` 路徑交給 reclamation queue；之後原執行緒不得再存取。
- Reclamation queue 由單一背景 worker 銷毀 node；任意呼叫者不得直接執行 destructor。
- Enqueue 必須先登記 outstanding node，再發布 Treiber-stack head，避免 consumer 先扣除造成計數下溢。
- Shutdown 對外依 `Running → Stopping → Draining → Stopped` 執行：先停止並 join 外部 producer、
  允許 destructor 產生的 child enqueue，再反覆 drain 到 idle。實作必須在 `Stopped` 前使用內部
  `Finalizing` 閘門：先禁止新 producer、等待已進入 enqueue 的 producer 歸零，再同時確認 head 與
  outstanding count 都是零；若仍找到已發布 node，就退回 `Draining`。
- Worker 進入 `Stopped` 後才可被 shutdown 呼叫者 join，最後必須證明配置數等於釋放數。
- `Finalizing` 或 `Stopped` 後的晚到 enqueue 是生命週期違約，必須立即終止，不能靜默洩漏。

### 偏離原建議：未採用 `std::shared_ptr<const Node>`

原建議先使用標準 shared ownership，再以 benchmark 決定是否替換。本決策改選 intrusive counter。

#### 原理由與實測結果

原理由為「優先取得全局效能、配置控制與資料區域性」。

Spike 4（2026-08-17，MSVC 19.44.35222 Release）在五類 workload 上比較 IntrusivePtr 與
`std::shared_ptr`，**IntrusivePtr 在所有 workload 上均無可重現的整體效能優勢**：

| Workload | intrusive | shared | ratio |
|---|---|---|---|
| retain/release 10M | 27,367 us | 26,639 us | 0.97x |
| COW depth=16, 100k edits | 74,939 us | 44,319 us | **0.59x** |
| 跨執行緒 handoff 1M | 50,977 us | 26,482 us | **0.52x** |
| 深 DAG 回收 100k | 2,204 us | 1,200 us | **0.54x** |
| 8t×1M 併發 retain/release | 104,427 us | 89,378 us | **0.86x** |

ratio > 1 = intrusive 較快。完整報告見 `tasks/spike4-intrusive-vs-shared-report.md`。

原因：`RefCounted` 基底需要 vtable（virtual destructor）＋ atomic 計數 ＋ reclaim 串接指標，
使節點比 `std::shared_ptr` + `make_shared` 合併配置的版本大 67%（40 vs 24 bytes）；
reclamation queue 的 Treiber CAS 加上後續 drain 的成本也超過 shared_ptr 的就地 delete。

**效能不再是偏離理由。**

#### 保留的理由：延後銷毀是架構需求

`std::shared_ptr` 在最後一個擁有者釋放時**就地同步遞迴銷毀**。若 UI 執行緒是最後釋放者，
深 snapshot DAG 的銷毀會卡住畫幀。IntrusivePtr + reclamation queue 把銷毀推到背景執行緒，
使 UI 執行緒的 release 成本為 O(1)（一次 atomic decrement ＋ 一次 Treiber push）。

要讓 `std::shared_ptr` 做到延後銷毀，需要 custom deleter。但 `std::make_shared` 不支援
custom deleter——使用 custom deleter 必須改為 `std::shared_ptr<T>(new T(...), deleter)`，
**每個節點變成兩次配置（物件 ＋ control block）**，配置數翻倍、cache locality 更差。
這在 P1 的高頻 COW edit 路徑上不可接受。

因此保留 IntrusivePtr 的理由是：

1. **延後銷毀**——UI 執行緒不執行 destructor，這是架構級的 frame budget 保護。
2. **shared_ptr 沒有低成本替代**——custom deleter 與 make_shared 互斥，配置翻倍。
3. **整合度**——reclamation queue 已是 D9 的核心元件，intrusive counter 直接串接。

#### 重開條件

1. 若找到使 `std::shared_ptr` + 延後銷毀共存且不犧牲合併配置的方案（例如未來標準提案或
   平台擴充），此決策可重開。
2. 若 P1 實測延後銷毀在 frame budget 中佔比不顯著（即就地銷毀也能在 8.33ms 內完成），
   此決策可重開。

因此下列項目升級為 P1 強制閘門：

1. `IntrusivePtr` copy、move、self-assignment、exception path 與跨型別轉換的精確 retain／release 測試。
2. 多執行緒隨機複製／釋放同一 snapshot；queue drain 後每個 node 恰好銷毀一次。
3. 舊 snapshot 背景走訪與新 revision 連續發布並行時，舊內容 hash 必須保持不變。
4. 最後一個 reference 在 UI 執行緒釋放時，實際 destructor 必須在 reclamation worker 執行。
5. AddressSanitizer 與可用的 race detector／第二工具鏈測試；目前 MSVC 沒有可假定存在的
   ThreadSanitizer，因此不得以「本機沒有報錯」代替競態證據。
6. ~~與 `std::shared_ptr` 基準比較~~ → **已完成**（Spike 4, 2026-08-17）。效能無優勢，
   理由已改為架構需求。
7. 人工逐行審查所有 memory order、owning edge、borrowed pointer lifetime 與 shutdown drain path。

### 閘門狀態（2026-08-18）

| 閘門 | 狀態 | 證據 |
|---|---|---|
| 1 | 通過 | `krepis.intrusive_ptr` |
| 2 | 通過 | `test_concurrent_retain_release`、`test_concurrent_enqueue_and_background_drain` |
| 3 | 通過 | `test_snapshot_parallel_traversal` |
| 4 | 通過 | `test_destruction_runs_on_reclamation_worker` |
| 5 | **未執行** | CI 已配置 MSVC ＋ Clang（ASan／UBSan／TSan）於 `.github/workflows/ci.yml`，但**尚未實跑過**。2026-08-18 查核本機：clang 未安裝、MSVC ASan runtime 亦不存在，**無法本機執行**。關閉此閘門需推上 GitHub 讓 CI 跑一次。依本閘門自身的規定，未取得競態證據前不得視為通過 |
| 6 | 通過 | Spike 4（2026-08-17） |
| 7 | **未通過（要求修改）** | 見下 |

**閘門 7 目前為「未通過，要求修改」。**

三輪人工審查（`tasks/gate7-memory-order-review-checklist.md`）共 12 項，
第三輪 11 項接受、1 項（C3）仍要求修改：shutdown 的併發回歸測試缺少一條必要的同步邊。
該項的修正已實作並提交（`shutdown_waiters()` 使「已進入等待路徑」成為可觀察事實），
但**尚未經第四輪重審**，因此閘門維持未通過。

審查過程中修正的實質缺陷包括：worker 與 `join()` 的永久互等（C2）、
producer admission 的誤殺窗口（疑點 1）、`TreeCursor` moved-from 的 dangling pointer（疑點 2）、
`LocationIndex` 的 `ObjectSlot` 型別邊界與 `capacity()` 溢位歸零（E1）。

**這對 D17 的效力**：D17 選用 intrusive refcount 的**決策本身**維持成立，
但它是對原建議（`std::shared_ptr`）的偏離，而閘門正是該偏離的驗證條件。
在閘門 5 與 7 關閉之前，**此偏離屬於條件性接受，不得視為已完成驗證**。

使用者於 2026-08-18 行政放行後續工作（不阻擋依賴此程式碼的開發），
**但放行不等於通過**——閘門狀態不因放行而改變。

## D18：SnapshotId 分離 content revision 與 storage generation

```text
SnapshotId {
    content_revision
    storage_generation
}
```

- 使用者可觀察的內容或 ownership transaction 增加 `content_revision`。
- Slot compact、LeafKey 全域重建或其他不改內容的內部重排增加 `storage_generation`。
- Collaboration transaction 的 base revision 只以 `content_revision` 判斷語意先後，避免 maintenance
  被誤認成使用者衝突。
- 含 ObjectSlot、LeafKey、node pointer 或其他內部 handle 的工作與 cache 必須核對完整 `SnapshotId`。
- P1 layout result 採保守 exact-match：任一軸不同就丟棄。未來只有正式定義 dependency key 後才能
  放寬。
- 同一 publication 若同時包含內容交易與 storage rebuild，兩軸都增加，且仍只原子發布一次。

具體例子：文字輸入使 `{42, 3} -> {43, 3}`；不改文字的 compact 使 `{43, 3} -> {43, 4}`。
Worker 若仍持有 `{43, 3}` 的 LeafKey／slot cache，其結果不能套到 `{43, 4}`。

D12–D16 的資料結構與演算法圖見
[`flow-sequence-location-and-traversal.md`](../../../docs/architecture/flow-sequence-location-and-traversal.md)；
D17–D18 的生命週期與 revision 流程見
[`layout-revision-snapshot-data-flow.md`](../../../docs/architecture/layout-revision-snapshot-data-flow.md)。

## D19：高度修正時的 scroll anchoring

背景排版修正可見範圍之前的 Block 高度時，採 **anchor-to-first-visible-block** 策略：

- 目前可見範圍內的第一個 Block 保持螢幕位置不變。
- Scrollbar thumb 位置因總高改變而即時更新。
- 可見 Block 之前的高度修正只影響 scrollbar，不造成畫面跳動。
- 可見 Block 自身或之後的高度修正由 viewport 按既有流程更新，不需 anchoring。
- Viewport 無法見到任何 Block（例如空文件）時 anchoring 不適用。

此行為與 Chrome 的 overflow-anchor: auto 一致。

## D20：分塊參數定案

依 2026-08-17 的 benchmark（`tasks/lay-0002-chunking-parameters-report.md`）：

```text
leaf_capacity   = 64
internal_fanout = 32
merge_low_water = 24
```

量測的**主要產出不是這三個數字，而是「這三個數字不是瓶頸」**：全部候選設定都通過
所有硬門檻，最緊的一項仍有 12 倍餘裕，最鬆的有 37 倍。因此後續若出現 frame budget
問題，**不應回頭調整這三個數字**。

定案依據（延遲無法區分，改用記憶體放大）：

- `leaf ≥ 128` 被 `leaf = 64` 嚴格支配：每次編輯的節點配置數相同（3），但 leaf 大 2–4 倍。
- `leaf ≤ 32` 每次編輯多配置 33% 的節點（4 vs 3）。
- `merge_low_water` 必須 `< leaf_capacity / 2`；等於一半時 split／merge 會在同一邊界震盪。
  取上界內最大值 24，兩個方向各留約 10 次操作的遲滯。

同時確立：文件長度 1,000 → 50,000（50 倍）時打字 p99 僅增為 1.25 倍，
**證實 FlowSequence 沒有隱藏的線性路徑**。這是 D4 選擇 Chunked B+ tree 而非
Vector＋Fenwick 的核心主張，現由實測支持而非僅靠設計論證。

未涵蓋：ARM（iPad）平台、長期編輯後的樹稀疏化、併發讀寫。`internal_fanout`
在 8–32 之間依原則而非量測決定（長文件的深度優勢在 10,000 blocks 時看不出來）。

## 尚未決定

- LeafKey 的 relabel window 初始大小與目標間距。
- ObjectStore 的 Record page 容量、page-table fanout 與 compact 門檻。
- 未量測 Block 的估計高度與第一次開啟超長文件的物化策略。
- overscan 的單位、範圍與調整策略。
- shaping、line breaking、extent、paint 與 hit-test index 的失效傳播。
- 巢狀容器的失效停止條件與 reference view 的 cache key。

## 被淘汰的方案

| 方案 | 理由 |
|---|---|
| Block 保存永久 `height` | 同一內容在不同寬度、字型、DPI 或 composition 下高度不同 |
| Block 保存永久絕對 `y` | 前方高度變化會迫使所有後方 Block 做 `O(n)` 更新 |
| ObjectStore 實體順序兼作文檔順序 | map／arena 重排會改變文件語意，且無法支援巢狀 Container |
| Vector＋Fenwick 作為正式順序結構 | 中間插入、刪除與移動仍為 `O(n)`，且順序與高度索引可能靜默分岔 |
| 每 Block 一節點的 order-statistic tree | 動態操作正確，但大量 Block 的節點開銷與指標跳轉不如分塊葉節點 |

## 相關決策

- **本決策依賴**：[`DOC-0001`](../01-document/DOC-0001-object-tree-stable-id-reference-and-composition.md)、
  [`LAY-0001`](LAY-0001-sync-background-split-and-frame-budget.md)。
- **依賴本決策**：P1 增量 layout、viewport virtualization 與 display list 產出。
