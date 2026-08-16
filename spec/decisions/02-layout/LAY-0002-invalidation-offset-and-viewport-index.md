# LAY-0002：失效傳播、偏移與可見範圍索引

## 狀態

**Proposed**（D1–D2 已接受；資料結構與完整失效規則待決）

## 日期

提出：2026-08-16  
D1–D2 接受：2026-08-16

## 背景

P0 已證明若每次編輯都重寫全部後續 Block 的絕對 `y`，工作量會隨文件長度成長。P1 必須以
累積高度索引從 viewport 找出可見 Block，並讓單一 Block 高度改變只更新聚合路徑。

本檔只記錄已核准的模型邊界；Fenwick tree、order-statistic tree、延遲物化與完整失效規則
尚未裁決，整份 `LAY-0002` 尚未完成。

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

## 尚未決定

- 正式資料結構：Fenwick、order-statistic tree 或其他方案。
- 未量測 Block 的估計高度與第一次開啟超長文件的物化策略。
- 高度修正時的 scroll anchoring。
- overscan 的單位、範圍與調整策略。
- shaping、line breaking、extent、paint 與 hit-test index 的失效傳播。
- 巢狀容器的失效停止條件與 reference view 的 cache key。

## 被淘汰的方案

| 方案 | 理由 |
|---|---|
| Block 保存永久 `height` | 同一內容在不同寬度、字型、DPI 或 composition 下高度不同 |
| Block 保存永久絕對 `y` | 前方高度變化會迫使所有後方 Block 做 `O(n)` 更新 |
| ObjectStore 實體順序兼作文檔順序 | map／arena 重排會改變文件語意，且無法支援巢狀 Container |

## 相關決策

- **本決策依賴**：[`DOC-0001`](../01-document/DOC-0001-object-tree-stable-id-reference-and-composition.md)、
  [`LAY-0001`](LAY-0001-sync-background-split-and-frame-budget.md)。
- **依賴本決策**：P1 增量 layout、viewport virtualization 與 display list 產出。

