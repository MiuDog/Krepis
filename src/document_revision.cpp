#include "krepis/document_revision.hpp"

#include <cassert>
#include <utility>

namespace krepis {

DocumentRevision::DocumentRevision(SnapshotId snapshot_id,
                                   IntrusivePtr<const IdDirectory> directory,
                                   ObjectStoreSnapshot store, LocationIndex locations,
                                   std::vector<FlowRootEntry> flow_roots) noexcept
    : snapshot_id_(snapshot_id),
      directory_(std::move(directory)),
      store_(std::move(store)),
      locations_(std::move(locations)),
      flow_roots_(std::move(flow_roots)) {}

DocumentRevision DocumentRevision::initial() {
    return DocumentRevision(SnapshotId{0, 0}, IdDirectory::empty(), ObjectStoreSnapshot::empty(),
                            LocationIndex::empty(), {});
}

SnapshotId DocumentRevision::next_content_revision() const noexcept {
    return SnapshotId{snapshot_id_.content_revision + 1, snapshot_id_.storage_generation};
}

const FlowSequence* DocumentRevision::flow_root(ContainerId container) const {
    for (const auto& entry : flow_roots_) {
        if (entry.first == container) {
            return &entry.second;
        }
    }
    return nullptr;
}

ContainerId DocumentRevision::container_id_at(std::size_t index) const {
	assert(index < flow_roots_.size());
	return flow_roots_[index].first;
}

ObjectSlot DocumentRevision::resolve(BlockId block) const {
    return directory_->resolve(block.raw());
}

IntrusivePtr<const ObjectRecord> DocumentRevision::record_for(BlockId block) const {
    return store_.get(resolve(block));
}

DocumentRevision DocumentRevision::with_new_object(
    BlockId block, IntrusivePtr<const ObjectRecord> record) const {
    auto allocated = directory_->allocate(block.raw());
    auto new_store = store_.with_record(allocated.slot, std::move(record));

    return DocumentRevision(next_content_revision(), std::move(allocated.directory),
                            std::move(new_store), locations_, flow_roots_);
}

DocumentRevision DocumentRevision::with_updated_record(
    BlockId block, IntrusivePtr<const ObjectRecord> record) const {
    const auto slot = resolve(block);
    assert(slot.is_valid() && "修改記錄前必須先配置 slot");

    auto new_store = store_.with_record(slot, std::move(record));

    return DocumentRevision(next_content_revision(), directory_, std::move(new_store), locations_,
                            flow_roots_);
}

Result<DocumentRevision> DocumentRevision::with_updated_records(
	std::span<const RecordUpdate> updates
) const {
	// 步驟 1：在建立任何新 store path 前驗證所有目標。
	for (const auto& update : updates) {
		if (update.record == nullptr) {
			return Error{ErrorCode::invalid_argument, "批次 record 更新不得包含 null"};
		}

		const auto slot = resolve(update.block);
		if (!slot.is_valid() || !store_.contains(slot)) {
			return Error{ErrorCode::not_found, "批次 record 更新的 Block 不存在"};
		}
	}

	// 步驟 2：只在全部驗證成功後建立新的 COW store，最後增加一次 revision。
	auto new_store = store_;
	for (const auto& update : updates) {
		new_store = new_store.with_record(resolve(update.block), update.record);
	}

	return DocumentRevision(
		next_content_revision(),
		directory_,
		std::move(new_store),
		locations_,
		flow_roots_
	);
}

DocumentRevision DocumentRevision::with_deleted_object(BlockId block) const {
    const auto slot = resolve(block);
    assert(slot.is_valid() && "刪除前必須先配置 slot");

    // Tombstone 保留在 slot 上；directory 不移除映射，
    // 因為 slot 在同一世代內不得重用（D10）。
    auto new_store = store_.with_tombstone(slot);
    auto new_locations = locations_.clear(slot);

    return DocumentRevision(next_content_revision(), directory_, std::move(new_store),
                            std::move(new_locations), flow_roots_);
}

DocumentRevision DocumentRevision::with_flow_root(ContainerId container,
                                                  FlowSequence sequence) const {
    // 順序與位置索引必須在同一交易內一起更新（D12）——
    // 因此本方法同時重建受影響 Container 的所有 LocationIndex entry。
    auto new_directory = directory_;
    auto new_locations = locations_;

    for (std::size_t i = 0; i < sequence.block_count(); ++i) {
        const auto block = sequence.at(i);
        const auto leaf_key = sequence.leaf_key_at(i);

        auto allocated = new_directory->allocate(block.raw());
        new_directory = std::move(allocated.directory);

        new_locations =
            new_locations.set(allocated.slot, make_flow_location(container, leaf_key));
    }

    auto new_roots = flow_roots_;
    bool replaced = false;
    for (auto& entry : new_roots) {
        if (entry.first == container) {
            entry.second = std::move(sequence);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        new_roots.emplace_back(container, std::move(sequence));
    }

    return DocumentRevision(next_content_revision(), std::move(new_directory), store_,
                            std::move(new_locations), std::move(new_roots));
}

Result<DocumentRevision> DocumentRevision::with_flow_insert(
    ContainerId container, const FlowSequenceInsertResult& edit) const {
    const auto* current = flow_root(container);
    const auto current_root = current ? current->root() : nullptr;
    if (current_root.get() != edit.source_root().get()) {
        return Error{ErrorCode::revision_conflict,
                     "Flow insert 的來源 root 已不是目前 revision"};
    }

    auto new_directory = directory_;
    auto new_locations = locations_;
    for (const auto& update : edit.locator_updates()) {
        auto allocated = new_directory->allocate(update.block.raw());
        new_directory = std::move(allocated.directory);
        new_locations = new_locations.set(
            allocated.slot, make_flow_location(container, update.leaf_key));
    }

    auto new_roots = flow_roots_;
    bool replaced = false;
    for (auto& entry : new_roots) {
        if (entry.first == container) {
            entry.second = edit.sequence();
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        new_roots.emplace_back(container, edit.sequence());
    }

    const SnapshotId next{
        snapshot_id_.content_revision + 1,
        snapshot_id_.storage_generation + (edit.diagnostics().global_rebuild ? 1u : 0u),
    };
    return DocumentRevision(next, std::move(new_directory), store_,
                            std::move(new_locations), std::move(new_roots));
}

DocumentRevision DocumentRevision::with_storage_rebuild() const {
    // 只遞增 storage_generation：內容不變，但持有內部 handle 的工作必須失效（D18）。
    const SnapshotId rebuilt{snapshot_id_.content_revision, snapshot_id_.storage_generation + 1};
    return DocumentRevision(rebuilt, directory_, store_, locations_, flow_roots_);
}

RevisionValidation DocumentRevision::validate() const {
    for (const auto& [container, sequence] : flow_roots_) {
        for (std::size_t i = 0; i < sequence.block_count(); ++i) {
            const auto block = sequence.at(i);

            const auto slot = directory_->resolve(block.raw());
            if (!slot.is_valid()) {
                return {RevisionValidation::Failure::unresolved_block_id, block, container};
            }

            const auto entry = locations_.lookup(slot);
            if (entry.is_empty()) {
                return {RevisionValidation::Failure::missing_location_entry, block, container};
            }
            if (entry.owner != container) {
                return {RevisionValidation::Failure::owner_mismatch, block, container};
            }
            if (!entry.is_flow()) {
                return {RevisionValidation::Failure::missing_location_entry, block, container};
            }
            if (entry.flow.leaf_key != sequence.leaf_key_at(i)) {
                return {RevisionValidation::Failure::leaf_key_mismatch, block, container};
            }
        }
    }
    return {};
}

}  // namespace krepis
