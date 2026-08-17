#include "krepis/object_store.hpp"

#include <cassert>
#include <utility>

namespace krepis {

// --- IdDirectory ---

IdDirectory::IdDirectory(std::vector<ObjectId> slot_to_id,
                         std::unordered_map<ObjectId, std::uint32_t> id_to_slot,
                         IdDirectoryGeneration generation)
    : slot_to_id_(std::move(slot_to_id)),
      id_to_slot_(std::move(id_to_slot)),
      generation_(generation) {}

IntrusivePtr<const IdDirectory> IdDirectory::empty() {
    return make_intrusive<IdDirectory>(std::vector<ObjectId>{},
                                       std::unordered_map<ObjectId, std::uint32_t>{},
                                       IdDirectoryGeneration{0});
}

ObjectSlot IdDirectory::resolve(const ObjectId& id) const {
    const auto it = id_to_slot_.find(id);
    if (it == id_to_slot_.end()) {
        return invalid_object_slot;
    }
    return ObjectSlot{it->second};
}

ObjectId IdDirectory::id_for(ObjectSlot slot) const {
    if (!slot.is_valid() || slot.value >= slot_to_id_.size()) {
        return nil_object_id;
    }
    return slot_to_id_[slot.value];
}

IdDirectory::AllocateResult IdDirectory::allocate(const ObjectId& id) const {
    assert(!id.is_nil() && "不得為 nil ObjectId 配置 slot");

    if (const auto existing = resolve(id); existing.is_valid()) {
        // 已配置過。directory 不變——slot 在同一世代內穩定（D10）。
        return {IntrusivePtr<const IdDirectory>(
                    make_intrusive<IdDirectory>(slot_to_id_, id_to_slot_, generation_)),
                existing};
    }

    auto next_slot = static_cast<std::uint32_t>(slot_to_id_.size());
    assert(next_slot != ObjectSlot::invalid_value && "slot 空間耗盡");

    auto new_slot_to_id = slot_to_id_;
    auto new_id_to_slot = id_to_slot_;
    new_slot_to_id.push_back(id);
    new_id_to_slot.emplace(id, next_slot);

    return {make_intrusive<IdDirectory>(std::move(new_slot_to_id), std::move(new_id_to_slot),
                                        generation_),
            ObjectSlot{next_slot}};
}

// --- RecordPage ---

RecordPage::RecordPage(std::vector<Entry> entries) noexcept : entries_(std::move(entries)) {
    assert(entries_.size() == page_capacity && "RecordPage 大小必須固定");
}

const RecordPage::Entry& RecordPage::at(std::size_t offset) const {
    assert(offset < entries_.size());
    return entries_[offset];
}

// --- ObjectStoreSnapshot ---

namespace {

struct SlotAddress {
    std::size_t page_index;
    std::size_t offset;
};

SlotAddress address_of(ObjectSlot slot) noexcept {
    return {slot.value / RecordPage::page_capacity, slot.value % RecordPage::page_capacity};
}

}  // anonymous namespace

ObjectStoreSnapshot::ObjectStoreSnapshot(
    std::vector<IntrusivePtr<const RecordPage>> pages) noexcept
    : pages_(std::move(pages)) {}

ObjectStoreSnapshot ObjectStoreSnapshot::empty() {
    return ObjectStoreSnapshot({});
}

IntrusivePtr<const ObjectRecord> ObjectStoreSnapshot::get(ObjectSlot slot) const {
    if (!slot.is_valid()) {
        return nullptr;
    }
    const auto addr = address_of(slot);
    if (addr.page_index >= pages_.size() || !pages_[addr.page_index]) {
        return nullptr;
    }
    return pages_[addr.page_index]->at(addr.offset).record;
}

bool ObjectStoreSnapshot::contains(ObjectSlot slot) const {
    return get(slot) != nullptr;
}

bool ObjectStoreSnapshot::is_tombstoned(ObjectSlot slot) const {
    if (!slot.is_valid()) {
        return false;
    }
    const auto addr = address_of(slot);
    if (addr.page_index >= pages_.size() || !pages_[addr.page_index]) {
        return false;
    }
    return pages_[addr.page_index]->at(addr.offset).tombstoned;
}

ObjectStoreSnapshot ObjectStoreSnapshot::with_record(
    ObjectSlot slot, IntrusivePtr<const ObjectRecord> record) const {
    assert(slot.is_valid() && "不得對無效 slot 寫入");
    const auto addr = address_of(slot);

    auto new_pages = pages_;
    while (new_pages.size() <= addr.page_index) {
        new_pages.push_back(nullptr);
    }

    // COW：只複製受影響的 page（D10）。
    std::vector<RecordPage::Entry> entries;
    if (new_pages[addr.page_index]) {
        entries.reserve(RecordPage::page_capacity);
        for (std::size_t i = 0; i < RecordPage::page_capacity; ++i) {
            entries.push_back(new_pages[addr.page_index]->at(i));
        }
    } else {
        entries.resize(RecordPage::page_capacity);
    }

    entries[addr.offset].record = std::move(record);
    entries[addr.offset].tombstoned = false;
    new_pages[addr.page_index] = make_intrusive<RecordPage>(std::move(entries));

    return ObjectStoreSnapshot(std::move(new_pages));
}

ObjectStoreSnapshot ObjectStoreSnapshot::with_tombstone(ObjectSlot slot) const {
    assert(slot.is_valid() && "不得對無效 slot 寫入 tombstone");
    const auto addr = address_of(slot);

    auto new_pages = pages_;
    while (new_pages.size() <= addr.page_index) {
        new_pages.push_back(nullptr);
    }

    std::vector<RecordPage::Entry> entries;
    if (new_pages[addr.page_index]) {
        entries.reserve(RecordPage::page_capacity);
        for (std::size_t i = 0; i < RecordPage::page_capacity; ++i) {
            entries.push_back(new_pages[addr.page_index]->at(i));
        }
    } else {
        entries.resize(RecordPage::page_capacity);
    }

    entries[addr.offset].record = nullptr;
    entries[addr.offset].tombstoned = true;
    new_pages[addr.page_index] = make_intrusive<RecordPage>(std::move(entries));

    return ObjectStoreSnapshot(std::move(new_pages));
}

}  // namespace krepis
