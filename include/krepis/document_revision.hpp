#pragma once

// 依 LAY-0002 D8：authority 發布不可變 revision snapshot。
//
//   DocumentRevision {
//       snapshot_id { content_revision, storage_generation }
//       object_store_snapshot
//       flow_sequence_roots
//   }
//
// **必須同時涵蓋 ObjectStore 內容與所有 FlowSequence root**，否則背景工作會讀到
// 「新順序配舊內容」或「舊順序配新內容」的混合狀態（D8）。

#include "krepis/flow_sequence.hpp"
#include "krepis/error.hpp"
#include "krepis/intrusive_ptr.hpp"
#include "krepis/location_index.hpp"
#include "krepis/object_id.hpp"
#include "krepis/object_store.hpp"
#include "krepis/reference_index.hpp"
#include "krepis/snapshot_id.hpp"
#include "krepis/spatial_container.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace krepis {

// D12 的發布前驗證結果。
struct RevisionValidation {
    enum class Failure : std::uint8_t {
        none,
        // FlowSequence 中的 Block 在 LocationIndex 沒有對應 entry。
        missing_location_entry,
        // LocationIndex 記載的 owner 與該 Block 實際所在的 Container 不符。
        owner_mismatch,
        // LocationIndex 記載的 leaf key 與該 Block 實際所在的 leaf 不符。
        leaf_key_mismatch,
		// SpatialLocator 的 placement key 與 owner root 不一致。
		spatial_placement_mismatch,
        // Block 的 ObjectId 無法解析為 slot。
        unresolved_block_id,
    };

    Failure failure = Failure::none;
    BlockId offending_block{};
    ContainerId offending_container{};

    [[nodiscard]] bool ok() const noexcept { return failure == Failure::none; }
};

struct RecordUpdate {
	BlockId block;
	IntrusivePtr<const ObjectRecord> record;
};

struct FlowRecordMutation {
	BlockId block;
	IntrusivePtr<const ObjectRecord> record;
	bool tombstone = false;
};

// 一次 authority 發布的完整、不可變文件狀態。
//
// 責任：把 SnapshotId、IdDirectory、記錄內容、位置索引與所有 FlowSequence root
//       綁成單一原子單位，使讀者只能看到完整的舊 revision 或完整的新 revision。
// 不負責：決定何時發布 —— 那是 authority／transaction builder 的責任。
// 維持的不變條件：`validate()` 通過前不得發布（D12）。
// 擁有哪些資源：透過 IntrusivePtr 與值型別 COW 結構共享子樹。
// 生命週期：值型別語意；複製只增加共享計數，不深拷貝內容。
// 執行緒安全程度：不可變，可跨執行緒共享；同一實例不可併發修改。
class DocumentRevision {
public:
    [[nodiscard]] static DocumentRevision initial();

    [[nodiscard]] const SnapshotId& snapshot_id() const noexcept { return snapshot_id_; }
    [[nodiscard]] const IdDirectory& directory() const noexcept { return *directory_; }
    [[nodiscard]] const ObjectStoreSnapshot& store() const noexcept { return store_; }
    [[nodiscard]] const LocationIndex& locations() const noexcept { return locations_; }
	[[nodiscard]] const ReferenceIndex& references() const noexcept { return *references_; }

    // 取得某個 Container 的 FlowSequence。不存在時回傳 nullptr。
    [[nodiscard]] const FlowSequence* flow_root(ContainerId container) const;
	[[nodiscard]] const SpatialContainer* spatial_root(ContainerId container) const;

    [[nodiscard]] std::size_t container_count() const noexcept { return flow_roots_.size(); }
	// 前置條件：index < container_count()。只供序列化與診斷列舉，不具有順序語意。
	[[nodiscard]] ContainerId container_id_at(std::size_t index) const;
	[[nodiscard]] std::size_t spatial_container_count() const noexcept {
		return spatial_roots_.size();
	}
	[[nodiscard]] ContainerId spatial_container_id_at(std::size_t index) const;

    // 解析 BlockId 為 slot。找不到回傳 invalid_object_slot。
    [[nodiscard]] ObjectSlot resolve(BlockId block) const;

    // 取得 Block 的記錄。不存在或已刪除時回傳 null。
    [[nodiscard]] IntrusivePtr<const ObjectRecord> record_for(BlockId block) const;

    // --- 衍生新 revision ---
    //
    // 每個方法都回傳新的 DocumentRevision，且**只遞增 content_revision**。
    // storage_generation 只在 compact／重建時遞增（D18），由 with_storage_rebuild 處理。

    // 建立新物件：配置 slot 並寫入記錄。
    [[nodiscard]] DocumentRevision with_new_object(BlockId block,
                                                   IntrusivePtr<const ObjectRecord> record) const;

    // 修改既有物件的記錄。前置條件：block 已配置 slot。
    [[nodiscard]] DocumentRevision with_updated_record(
        BlockId block, IntrusivePtr<const ObjectRecord> record) const;

	// 先驗證全部目標，再以單一 content revision 發布所有 record 更新。
	// 任一目標不存在或 record 為 null 時不產生部分 revision。
	[[nodiscard]] Result<DocumentRevision> with_updated_records(
		std::span<const RecordUpdate> updates
	) const;
	// 在一個 content revision 內同時更新 Flow 順序、record 與 LocationIndex。
	[[nodiscard]] Result<DocumentRevision> with_atomic_flow_edit(
		ContainerId container,
		FlowSequence sequence,
		std::span<const FlowRecordMutation> mutations
	) const;

    // 刪除物件：寫入 tombstone 並清除位置索引。
    [[nodiscard]] DocumentRevision with_deleted_object(BlockId block) const;

    // 設定某個 Container 的 FlowSequence，並同步更新受影響 Block 的 LocationIndex。
    // 這是唯一會同時改動順序與位置索引的入口——D12 要求兩者在同一交易內更新。
    [[nodiscard]] DocumentRevision with_flow_root(ContainerId container,
                                                  FlowSequence sequence) const;
	[[nodiscard]] DocumentRevision with_spatial_root(
		ContainerId container,
		SpatialContainer spatial
	) const;
	// 移出 Flow ownership，並在同一 revision 修復所有引用該來源的 range endpoints。
	// delete_record 為 true 時同時把被移除 Block 寫成 tombstone。
	[[nodiscard]] Result<DocumentRevision> with_flow_block_removal(
		ContainerId container,
		BlockId block,
		bool delete_record
	) const;

    // D22：只套用 typed edit 明列的 locator 更新。來源 root 不符時整筆拒絕。
    [[nodiscard]] Result<DocumentRevision> with_flow_insert(
        ContainerId container, const FlowSequenceInsertResult& edit) const;

    // 不改內容的內部重排（compact／LeafKey 全域重建）：只遞增 storage_generation。
    [[nodiscard]] DocumentRevision with_storage_rebuild() const;

    // D12：發布前驗證。FlowSequence 與 LocationIndex 必須一致。
    //
    // **驗證失敗時必須拒絕整個新 revision**，不得選一邊覆寫另一邊，
    // 也不得由 client 修補。
    [[nodiscard]] RevisionValidation validate() const;

private:
    using FlowRootEntry = std::pair<ContainerId, FlowSequence>;
	using SpatialRootEntry = std::pair<ContainerId, SpatialContainer>;

    DocumentRevision(SnapshotId snapshot_id, IntrusivePtr<const IdDirectory> directory,
                     ObjectStoreSnapshot store, LocationIndex locations,
                     std::vector<FlowRootEntry> flow_roots,
	                 std::vector<SpatialRootEntry> spatial_roots,
	                 IntrusivePtr<const ReferenceIndex> references) noexcept;

    [[nodiscard]] SnapshotId next_content_revision() const noexcept;

    SnapshotId snapshot_id_;
    IntrusivePtr<const IdDirectory> directory_;
    ObjectStoreSnapshot store_;
    LocationIndex locations_;
    // Container 數量遠少於 Block 數量，線性搜尋足夠；
    // 若容器數成長到需要索引，應以量測驅動而非預先最佳化。
    std::vector<FlowRootEntry> flow_roots_;
	std::vector<SpatialRootEntry> spatial_roots_;
	IntrusivePtr<const ReferenceIndex> references_;
};

}  // namespace krepis
