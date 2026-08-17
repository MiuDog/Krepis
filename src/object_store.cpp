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

// --- RecordPageTableNode ---

RecordPageTableNode::RecordPageTableNode(
    std::vector<IntrusivePtr<const RecordPage>> pages) noexcept
    : leaf_level_(true), pages_(std::move(pages)) {
    assert(pages_.size() <= fanout && "葉層 children 不得超過 fanout");
}

RecordPageTableNode::RecordPageTableNode(
    std::vector<IntrusivePtr<const RecordPageTableNode>> children) noexcept
    : leaf_level_(false), children_(std::move(children)) {
    assert(children_.size() <= fanout && "內層 children 不得超過 fanout");
}

std::span<const IntrusivePtr<const RecordPage>> RecordPageTableNode::pages() const noexcept {
    assert(leaf_level_);
    return pages_;
}

std::span<const IntrusivePtr<const RecordPageTableNode>> RecordPageTableNode::children()
    const noexcept {
    assert(!leaf_level_);
    return children_;
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

// depth 層的樹中，**單一 child** 涵蓋多少個 page。採飽和運算避免溢位迴繞。
std::size_t page_span_for_depth(std::size_t depth) noexcept {
    std::size_t span = 1;
    for (std::size_t i = 1; i < depth; ++i) {
        span = saturating_mul(span, RecordPageTableNode::fanout);
    }
    return span;
}

IntrusivePtr<const RecordPage> make_page_with(const RecordPage* existing, std::size_t offset,
                                              RecordPage::Entry entry) {
    std::vector<RecordPage::Entry> entries;
    entries.reserve(RecordPage::page_capacity);
    if (existing != nullptr) {
        for (std::size_t i = 0; i < RecordPage::page_capacity; ++i) {
            entries.push_back(existing->at(i));
        }
    } else {
        entries.resize(RecordPage::page_capacity);
    }
    entries[offset] = std::move(entry);
    return make_intrusive<RecordPage>(std::move(entries));
}

// 在 depth 層的子樹中設定 page_index 對應的 entry，**只複製受影響的路徑**（D10）。
IntrusivePtr<const RecordPageTableNode> set_in_subtree(const RecordPageTableNode* node,
                                                       std::size_t depth, std::size_t page_index,
                                                       std::size_t offset,
                                                       RecordPage::Entry entry) {
    if (depth == 1) {
        std::vector<IntrusivePtr<const RecordPage>> pages;
        if (node != nullptr) {
            pages.assign(node->pages().begin(), node->pages().end());
        }
        while (pages.size() <= page_index) {
            pages.push_back(nullptr);
        }
        pages[page_index] = make_page_with(pages[page_index].get(), offset, std::move(entry));
        return make_intrusive<RecordPageTableNode>(std::move(pages));
    }

    const std::size_t child_span = page_span_for_depth(depth);
    const std::size_t child_index = page_index / child_span;
    const std::size_t inner_index = page_index % child_span;

    std::vector<IntrusivePtr<const RecordPageTableNode>> children;
    if (node != nullptr) {
        children.assign(node->children().begin(), node->children().end());
    }
    while (children.size() <= child_index) {
        children.push_back(nullptr);
    }

    children[child_index] = set_in_subtree(children[child_index].get(), depth - 1, inner_index,
                                           offset, std::move(entry));

    return make_intrusive<RecordPageTableNode>(std::move(children));
}

const RecordPage* find_page(const RecordPageTableNode* node, std::size_t depth,
                            std::size_t page_index) noexcept {
    while (node != nullptr && depth > 1) {
        const std::size_t child_span = page_span_for_depth(depth);
        const std::size_t child_index = page_index / child_span;
        auto children = node->children();
        if (child_index >= children.size()) {
            return nullptr;
        }
        node = children[child_index].get();
        page_index %= child_span;
        --depth;
    }
    if (node == nullptr) {
        return nullptr;
    }
    auto pages = node->pages();
    if (page_index >= pages.size()) {
        return nullptr;
    }
    return pages[page_index].get();
}

}  // anonymous namespace

ObjectStoreSnapshot::ObjectStoreSnapshot(IntrusivePtr<const RecordPageTableNode> root,
                                          std::size_t depth) noexcept
    : root_(std::move(root)), depth_(depth) {}

ObjectStoreSnapshot ObjectStoreSnapshot::empty() {
    return ObjectStoreSnapshot(nullptr, 0);
}

IntrusivePtr<const ObjectRecord> ObjectStoreSnapshot::get(ObjectSlot slot) const {
    if (!slot.is_valid() || !root_) {
        return nullptr;
    }
    const auto addr = address_of(slot);
    const RecordPage* page = find_page(root_.get(), depth_, addr.page_index);
    if (page == nullptr) {
        return nullptr;
    }
    return page->at(addr.offset).record;
}

bool ObjectStoreSnapshot::contains(ObjectSlot slot) const {
    return get(slot) != nullptr;
}

bool ObjectStoreSnapshot::is_tombstoned(ObjectSlot slot) const {
    if (!slot.is_valid() || !root_) {
        return false;
    }
    const auto addr = address_of(slot);
    const RecordPage* page = find_page(root_.get(), depth_, addr.page_index);
    if (page == nullptr) {
        return false;
    }
    return page->at(addr.offset).tombstoned;
}

ObjectStoreSnapshot ObjectStoreSnapshot::with_entry(ObjectSlot slot,
                                                    RecordPage::Entry entry) const {
    assert(slot.is_valid() && "不得對無效 slot 寫入");
    const auto addr = address_of(slot);

    // 樹加高：舊 root 成為新 root 的第 0 個 child，其餘子樹完全共享，加高本身 O(1)。
    std::size_t depth = (depth_ == 0) ? 1 : depth_;
    IntrusivePtr<const RecordPageTableNode> root = root_;
    while (addr.page_index >= page_span_for_depth(depth + 1)) {
        std::vector<IntrusivePtr<const RecordPageTableNode>> children;
        if (root) {
            children.push_back(root);
        }
        root = make_intrusive<RecordPageTableNode>(std::move(children));
        ++depth;
    }

    auto new_root =
        set_in_subtree(root.get(), depth, addr.page_index, addr.offset, std::move(entry));
    return ObjectStoreSnapshot(std::move(new_root), depth);
}

ObjectStoreSnapshot ObjectStoreSnapshot::with_record(
    ObjectSlot slot, IntrusivePtr<const ObjectRecord> record) const {
    RecordPage::Entry entry;
    entry.record = std::move(record);
    entry.tombstoned = false;
    return with_entry(slot, std::move(entry));
}

ObjectStoreSnapshot ObjectStoreSnapshot::with_tombstone(ObjectSlot slot) const {
    RecordPage::Entry entry;
    entry.record = nullptr;
    entry.tombstoned = true;
    return with_entry(slot, std::move(entry));
}

std::size_t ObjectStoreSnapshot::capacity() const noexcept {
    if (depth_ == 0) {
        return 0;
    }
    // 與 LocationIndex 同樣採飽和運算：迴繞後的 `64^11 = 2^66` 恰好為 0（閘門 7／E1 第二輪）。
    return saturating_mul(page_span_for_depth(depth_ + 1), RecordPage::page_capacity);
}

}  // namespace krepis
