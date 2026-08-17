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

DocumentRevision DocumentRevision::with_deleted_object(BlockId block) const {
    const auto slot = resolve(block);
    assert(slot.is_valid() && "刪除前必須先配置 slot");

    // Tombstone 保留在 slot 上；directory 不移除映射，
    // 因為 slot 在同一世代內不得重用（D10）。
    auto new_store = store_.with_tombstone(slot);
    auto new_locations = locations_.clear(slot.value);

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
            new_locations.set(allocated.slot.value, make_flow_location(container, leaf_key));
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

            const auto entry = locations_.lookup(slot.value);
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
