#include "krepis/document_revision.hpp"

#include "krepis/embed_record.hpp"

#include <cassert>
#include <unordered_set>
#include <utility>

namespace krepis {

DocumentRevision::DocumentRevision(SnapshotId snapshot_id,
                                   IntrusivePtr<const IdDirectory> directory,
                                   ObjectStoreSnapshot store, LocationIndex locations,
	                               std::vector<FlowRootEntry> flow_roots,
	                               std::vector<SpatialRootEntry> spatial_roots,
	                               IntrusivePtr<const ReferenceIndex> references) noexcept
    : snapshot_id_(snapshot_id),
      directory_(std::move(directory)),
      store_(std::move(store)),
      locations_(std::move(locations)),
      flow_roots_(std::move(flow_roots)),
	  spatial_roots_(std::move(spatial_roots)),
	  references_(std::move(references)) {}

DocumentRevision DocumentRevision::initial() {
    return DocumentRevision(SnapshotId{0, 0}, IdDirectory::empty(), ObjectStoreSnapshot::empty(),
                            LocationIndex::empty(), {}, {}, ReferenceIndex::empty());
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

const SpatialContainer* DocumentRevision::spatial_root(ContainerId container) const {
	for (const auto& entry : spatial_roots_) {
		if (entry.first == container) return &entry.second;
	}
	return nullptr;
}

ContainerId DocumentRevision::container_id_at(std::size_t index) const {
	assert(index < flow_roots_.size());
	return flow_roots_[index].first;
}

ContainerId DocumentRevision::spatial_container_id_at(std::size_t index) const {
	assert(index < spatial_roots_.size());
	return spatial_roots_[index].first;
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
	auto references = references_;
	if (const auto* prior = dynamic_cast<const EmbedRecord*>(store_.get(allocated.slot).get())) {
		references = references->with_removed(prior->source_container(), block);
	}
	if (const auto* embed = dynamic_cast<const EmbedRecord*>(record.get())) {
		references = references->with_added(embed->source_container(), block);
	}
    auto new_store = store_.with_record(allocated.slot, std::move(record));

    return DocumentRevision(next_content_revision(), std::move(allocated.directory),
                            std::move(new_store), locations_, flow_roots_, spatial_roots_,
	                        std::move(references));
}

DocumentRevision DocumentRevision::with_updated_record(
    BlockId block, IntrusivePtr<const ObjectRecord> record) const {
    const auto slot = resolve(block);
    assert(slot.is_valid() && "修改記錄前必須先配置 slot");

	auto references = references_;
	if (const auto* prior = dynamic_cast<const EmbedRecord*>(store_.get(slot).get())) {
		references = references->with_removed(prior->source_container(), block);
	}
	if (const auto* embed = dynamic_cast<const EmbedRecord*>(record.get())) {
		references = references->with_added(embed->source_container(), block);
	}
    auto new_store = store_.with_record(slot, std::move(record));

    return DocumentRevision(next_content_revision(), directory_, std::move(new_store), locations_,
                            flow_roots_, spatial_roots_, std::move(references));
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
	auto references = references_;
	for (const auto& update : updates) {
		const auto slot = resolve(update.block);
		if (const auto* prior = dynamic_cast<const EmbedRecord*>(new_store.get(slot).get())) {
			references = references->with_removed(prior->source_container(), update.block);
		}
		if (const auto* embed = dynamic_cast<const EmbedRecord*>(update.record.get())) {
			references = references->with_added(embed->source_container(), update.block);
		}
		new_store = new_store.with_record(resolve(update.block), update.record);
	}

	return DocumentRevision(
		next_content_revision(),
		directory_,
		std::move(new_store),
		locations_,
		flow_roots_,
		spatial_roots_,
		std::move(references)
	);
}

Result<DocumentRevision> DocumentRevision::with_atomic_flow_edit(
	ContainerId container,
	FlowSequence sequence,
	std::span<const FlowRecordMutation> mutations
) const {
	if (flow_root(container) == nullptr) {
		return Error{ErrorCode::not_found, "Flow edit 的 Container 不存在"};
	}

	// 步驟 1：驗證 mutation 不重複，且 tombstone 只指向已存在物件。
	std::unordered_set<ObjectId> mutation_ids;
	mutation_ids.reserve(mutations.size());
	for (const auto& mutation : mutations) {
		if (!mutation_ids.insert(mutation.block.raw()).second ||
		    (!mutation.tombstone && mutation.record == nullptr)) {
			return Error{ErrorCode::invalid_argument, "Flow edit mutation 重複或 record 為 null"};
		}
		if (mutation.tombstone && !resolve(mutation.block).is_valid()) {
			return Error{ErrorCode::not_found, "Flow edit tombstone 目標不存在"};
		}
	}

	// 步驟 2：在未發布的 store 中一次建立全部 record 變更。
	auto new_directory = directory_;
	auto new_store = store_;
	auto new_references = references_;
	for (const auto& mutation : mutations) {
		auto allocated = new_directory->allocate(mutation.block.raw());
		new_directory = std::move(allocated.directory);
		if (const auto* prior = dynamic_cast<const EmbedRecord*>(new_store.get(allocated.slot).get())) {
			new_references = new_references->with_removed(
				prior->source_container(),
				mutation.block
			);
		}
		if (mutation.tombstone) {
			new_store = new_store.with_tombstone(allocated.slot);
			continue;
		}
		if (const auto* embed = dynamic_cast<const EmbedRecord*>(mutation.record.get())) {
			new_references = new_references->with_added(embed->source_container(), mutation.block);
		}
		new_store = new_store.with_record(allocated.slot, mutation.record);
	}

	// 步驟 3：驗證新順序的每個 Block 都有可見 record，且沒有偷走其他 owner 的內容。
	for (std::size_t position = 0; position < sequence.block_count(); ++position) {
		const auto block = sequence.at(position);
		const auto slot = new_directory->resolve(block.raw());
		if (!slot.is_valid() || !new_store.contains(slot)) {
			return Error{ErrorCode::invalid_state, "Flow edit 順序指向無可見 record"};
		}
		const auto prior_location = locations_.lookup(slot);
		if (!prior_location.is_empty() && prior_location.owner != container) {
			return Error{ErrorCode::invalid_state, "Flow edit 不得改變其他 Container 的 ownership"};
		}
	}

	// 步驟 4：依新 FlowSequence 重建該 owner 的 locator，並與 store 同時發布。
	auto new_locations = locations_;
	const auto* prior_root = flow_root(container);
	for (std::size_t position = 0; position < prior_root->block_count(); ++position) {
		const auto slot = new_directory->resolve(prior_root->at(position).raw());
		const auto location = new_locations.lookup(slot);
		if (location.is_flow() && location.owner == container) {
			new_locations = new_locations.clear(slot);
		}
	}
	for (std::size_t position = 0; position < sequence.block_count(); ++position) {
		const auto block = sequence.at(position);
		const auto slot = new_directory->resolve(block.raw());
		new_locations = new_locations.set(
			slot,
			make_flow_location(container, sequence.leaf_key_at(position))
		);
	}

	auto roots = flow_roots_;
	for (auto& entry : roots) {
		if (entry.first == container) {
			entry.second = std::move(sequence);
			break;
		}
	}
	return DocumentRevision(
		next_content_revision(),
		std::move(new_directory),
		std::move(new_store),
		std::move(new_locations),
		std::move(roots),
		spatial_roots_,
		std::move(new_references)
	);
}

DocumentRevision DocumentRevision::with_deleted_object(BlockId block) const {
    const auto slot = resolve(block);
    assert(slot.is_valid() && "刪除前必須先配置 slot");

    // Tombstone 保留在 slot 上；directory 不移除映射，
    // 因為 slot 在同一世代內不得重用（D10）。
    auto new_store = store_.with_tombstone(slot);
    auto new_locations = locations_.clear(slot);
	auto references = references_;
	if (const auto* prior = dynamic_cast<const EmbedRecord*>(store_.get(slot).get())) {
		references = references->with_removed(prior->source_container(), block);
	}

    return DocumentRevision(next_content_revision(), directory_, std::move(new_store),
                            std::move(new_locations), flow_roots_, spatial_roots_,
	                        std::move(references));
}

DocumentRevision DocumentRevision::with_flow_root(ContainerId container,
                                                  FlowSequence sequence) const {
    // 順序與位置索引必須在同一交易內一起更新（D12）——
    // 因此本方法同時重建受影響 Container 的所有 LocationIndex entry。
    auto new_directory = directory_;
    auto new_locations = locations_;
	if (const auto* prior = flow_root(container); prior != nullptr) {
		for (std::size_t i = 0; i < prior->block_count(); ++i) {
			const auto slot = new_directory->resolve(prior->at(i).raw());
			const auto location = new_locations.lookup(slot);
			if (location.is_flow() && location.owner == container) {
				new_locations = new_locations.clear(slot);
			}
		}
	}

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
                            std::move(new_locations), std::move(new_roots), spatial_roots_,
	                        references_);
}

DocumentRevision DocumentRevision::with_spatial_root(
	ContainerId container,
	SpatialContainer spatial
) const {
	auto new_directory = directory_;
	auto new_locations = locations_;
	if (const auto* prior = spatial_root(container); prior != nullptr) {
		for (std::size_t i = 0; i < prior->placement_count(); ++i) {
			const auto slot = new_directory->resolve(prior->placement_at(i).child.raw());
			const auto location = new_locations.lookup(slot);
			if (location.is_spatial() && location.owner == container) {
				new_locations = new_locations.clear(slot);
			}
		}
	}
	for (std::size_t i = 0; i < spatial.placement_count(); ++i) {
		const auto& placement = spatial.placement_at(i);
		auto allocated = new_directory->allocate(placement.child.raw());
		new_directory = std::move(allocated.directory);
		new_locations = new_locations.set(
			allocated.slot,
			make_spatial_location(container, placement.placement_key)
		);
	}
	auto roots = spatial_roots_;
	bool replaced = false;
	for (auto& entry : roots) {
		if (entry.first == container) {
			entry.second = std::move(spatial);
			replaced = true;
			break;
		}
	}
	if (!replaced) roots.emplace_back(container, std::move(spatial));
	return DocumentRevision(
		next_content_revision(),
		std::move(new_directory),
		store_,
		std::move(new_locations),
		flow_roots_,
		std::move(roots),
		references_
	);
}

Result<DocumentRevision> DocumentRevision::with_flow_block_removal(
	ContainerId container,
	BlockId block,
	bool delete_record
) const {
	const auto* prior = flow_root(container);
	if (prior == nullptr) {
		return Error{ErrorCode::not_found, "Flow removal 的 Container 不存在"};
	}
	const auto slot = resolve(block);
	if (!slot.is_valid()) return Error{ErrorCode::not_found, "Flow removal 的 Block 不存在"};
	const auto location = locations_.lookup(slot);
	if (!location.is_flow() || location.owner != container) {
		return Error{ErrorCode::invalid_state, "Block 不直接屬於指定 FlowContainer"};
	}
	const auto rank = prior->find_block_in_leaf(location.flow.leaf_key, block);
	if (!rank.has_value()) {
		return Error{ErrorCode::invalid_state, "Flow locator 無法解析到 Block"};
	}

	const auto next_content_revision = snapshot_id_.content_revision + 1;
	auto new_store = store_;
	for (const auto embed_block : references_->referencing(container)) {
		auto record = store_.get(resolve(embed_block));
		const auto* embed = dynamic_cast<const EmbedRecord*>(record.get());
		if (embed == nullptr) {
			return Error{ErrorCode::invalid_state, "ReferenceIndex 指向非 EmbedRecord"};
		}
		const auto* target = std::get_if<FlowRangeTarget>(&embed->target());
		if (target == nullptr ||
		    (target->anchor_a != block && target->anchor_b != block)) {
			continue;
		}
		auto repaired = repair_flow_range_after_removal(*target, *prior, block);
		if (!repaired.is_ok()) return repaired.error();
		if (repaired.value() == *target) continue;
		auto replacement = EmbedRecord::create(
			next_content_revision,
			repaired.value()
		);
		if (!replacement.is_ok()) return replacement.error();
		new_store = new_store.with_record(resolve(embed_block), std::move(replacement).take());
	}

	auto references = references_;
	if (delete_record) {
		if (const auto* removed_embed = dynamic_cast<const EmbedRecord*>(store_.get(slot).get())) {
			references = references->with_removed(removed_embed->source_container(), block);
		}
		new_store = new_store.with_tombstone(slot);
	}
	auto new_locations = locations_;
	for (std::size_t i = 0; i < prior->block_count(); ++i) {
		const auto prior_slot = resolve(prior->at(i));
		const auto prior_location = new_locations.lookup(prior_slot);
		if (prior_location.is_flow() && prior_location.owner == container) {
			new_locations = new_locations.clear(prior_slot);
		}
	}
	auto next_sequence = prior->remove(*rank);
	for (std::size_t i = 0; i < next_sequence.block_count(); ++i) {
		const auto child_slot = resolve(next_sequence.at(i));
		new_locations = new_locations.set(
			child_slot,
			make_flow_location(container, next_sequence.leaf_key_at(i))
		);
	}
	auto roots = flow_roots_;
	for (auto& root : roots) {
		if (root.first == container) {
			root.second = std::move(next_sequence);
			break;
		}
	}
	DocumentRevision result(
		SnapshotId{next_content_revision, snapshot_id_.storage_generation},
		directory_,
		std::move(new_store),
		std::move(new_locations),
		std::move(roots),
		spatial_roots_,
		std::move(references)
	);
	if (!result.validate().ok()) {
		return Error{ErrorCode::invalid_state, "Flow removal 產生不一致 revision"};
	}
	return result;
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
                            std::move(new_locations), std::move(new_roots), spatial_roots_,
	                        references_);
}

DocumentRevision DocumentRevision::with_storage_rebuild() const {
    // 只遞增 storage_generation：內容不變，但持有內部 handle 的工作必須失效（D18）。
    const SnapshotId rebuilt{snapshot_id_.content_revision, snapshot_id_.storage_generation + 1};
    return DocumentRevision(
		rebuilt,
		directory_,
		store_,
		locations_,
		flow_roots_,
		spatial_roots_,
		references_
	);
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
	for (const auto& [container, spatial] : spatial_roots_) {
		for (std::size_t i = 0; i < spatial.placement_count(); ++i) {
			const auto& placement = spatial.placement_at(i);
			const auto slot = directory_->resolve(placement.child.raw());
			if (!slot.is_valid()) {
				return {RevisionValidation::Failure::unresolved_block_id,
				        placement.child, container};
			}
			const auto location = locations_.lookup(slot);
			if (!location.is_spatial()) {
				return {RevisionValidation::Failure::missing_location_entry,
				        placement.child, container};
			}
			if (location.owner != container) {
				return {RevisionValidation::Failure::owner_mismatch,
				        placement.child, container};
			}
			if (location.spatial.placement_key != placement.placement_key) {
				return {RevisionValidation::Failure::spatial_placement_mismatch,
				        placement.child, container};
			}
		}
	}
    return {};
}

}  // namespace krepis
