#include "krepis/location_index.hpp"

#include <cassert>
#include <utility>

namespace krepis {

LocationEntry make_flow_location(ContainerId owner, LeafKey key) {
    LocationEntry entry;
    entry.owner = owner;
    entry.kind = LocationEntry::Kind::flow;
    entry.flow = FlowLocator{key};
    return entry;
}

LocationEntry make_spatial_location(ContainerId owner, std::uint64_t placement_key) {
    LocationEntry entry;
    entry.owner = owner;
    entry.kind = LocationEntry::Kind::spatial;
    entry.spatial = SpatialLocator{placement_key};
    return entry;
}

// --- LocationPage ---

LocationPage::LocationPage(std::vector<LocationEntry> entries) noexcept
    : entries_(std::move(entries)) {}

std::span<const LocationEntry> LocationPage::entries() const noexcept { return entries_; }
std::size_t LocationPage::size() const noexcept { return entries_.size(); }

// --- PageTableNode ---

PageTableNode::PageTableNode(std::vector<IntrusivePtr<const LocationPage>> pages) noexcept
    : leaf_level_(true), pages_(std::move(pages)) {
    assert(pages_.size() <= fanout && "葉層 children 不得超過 fanout");
}

PageTableNode::PageTableNode(std::vector<IntrusivePtr<const PageTableNode>> children) noexcept
    : leaf_level_(false), children_(std::move(children)) {
    assert(children_.size() <= fanout && "內層 children 不得超過 fanout");
}

std::span<const IntrusivePtr<const LocationPage>> PageTableNode::pages() const noexcept {
    assert(leaf_level_);
    return pages_;
}

std::span<const IntrusivePtr<const PageTableNode>> PageTableNode::children() const noexcept {
    assert(!leaf_level_);
    return children_;
}

// --- LocationIndex ---

namespace {

// depth 層的樹能容納多少個 page。
std::size_t page_span_for_depth(std::size_t depth) noexcept {
    std::size_t span = 1;
    for (std::size_t i = 1; i < depth; ++i) {
        span *= PageTableNode::fanout;
    }
    return span;
}

IntrusivePtr<const LocationPage> make_page_with(const LocationPage* existing, std::size_t offset,
                                                LocationEntry entry) {
    std::vector<LocationEntry> entries;
    if (existing != nullptr) {
        entries.assign(existing->entries().begin(), existing->entries().end());
    } else {
        entries.resize(LocationPage::page_capacity);
    }
    entries[offset] = std::move(entry);
    return make_intrusive<LocationPage>(std::move(entries));
}

// 在 depth 層的子樹中設定 page_index 對應的 entry，只複製受影響的路徑。
// node 可為 null，代表該子樹尚未物化。
IntrusivePtr<const PageTableNode> set_in_subtree(const PageTableNode* node, std::size_t depth,
                                                 std::size_t page_index, std::size_t offset,
                                                 LocationEntry entry) {
    if (depth == 1) {
        std::vector<IntrusivePtr<const LocationPage>> pages;
        if (node != nullptr) {
            pages.assign(node->pages().begin(), node->pages().end());
        }
        while (pages.size() <= page_index) {
            pages.push_back(nullptr);
        }
        pages[page_index] = make_page_with(pages[page_index].get(), offset, std::move(entry));
        return make_intrusive<PageTableNode>(std::move(pages));
    }

    const std::size_t child_span = page_span_for_depth(depth);
    const std::size_t child_index = page_index / child_span;
    const std::size_t inner_index = page_index % child_span;

    std::vector<IntrusivePtr<const PageTableNode>> children;
    if (node != nullptr) {
        children.assign(node->children().begin(), node->children().end());
    }
    while (children.size() <= child_index) {
        children.push_back(nullptr);
    }

    // 只有這一條路徑被複製；其餘 children 與其整棵子樹由新舊版本共享。
    children[child_index] =
        set_in_subtree(children[child_index].get(), depth - 1, inner_index, offset,
                       std::move(entry));

    return make_intrusive<PageTableNode>(std::move(children));
}

const LocationPage* find_page(const PageTableNode* node, std::size_t depth,
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

LocationIndex::LocationIndex(IntrusivePtr<const PageTableNode> root, std::size_t depth) noexcept
    : root_(std::move(root)), depth_(depth) {}

LocationIndex LocationIndex::empty() {
    return LocationIndex(nullptr, 0);
}

LocationEntry LocationIndex::lookup(std::size_t slot) const {
    if (!root_) {
        return {};
    }
    const std::size_t page_index = slot / LocationPage::page_capacity;
    const std::size_t offset = slot % LocationPage::page_capacity;

    const LocationPage* page = find_page(root_.get(), depth_, page_index);
    if (page == nullptr) {
        return {};
    }
    auto entries = page->entries();
    if (offset >= entries.size()) {
        return {};
    }
    return entries[offset];
}

LocationIndex LocationIndex::set(std::size_t slot, LocationEntry entry) const {
    const std::size_t page_index = slot / LocationPage::page_capacity;
    const std::size_t offset = slot % LocationPage::page_capacity;

    // 先把樹長高到足以容納 page_index。
    //
    // 深度 d 的樹可容納 fanout^d 個 page，即 page_span_for_depth(d + 1)。
    // 加高只是包一層新 root，舊 root 成為新 root 的第 0 個 child，
    // **其餘子樹完全共享**，因此加高本身是 O(1)。
    std::size_t depth = (depth_ == 0) ? 1 : depth_;
    IntrusivePtr<const PageTableNode> root = root_;
    while (page_index >= page_span_for_depth(depth + 1)) {
        std::vector<IntrusivePtr<const PageTableNode>> children;
        if (root) {
            children.push_back(root);
        }
        root = make_intrusive<PageTableNode>(std::move(children));
        ++depth;
    }

    auto new_root = set_in_subtree(root.get(), depth, page_index, offset, std::move(entry));
    return LocationIndex(std::move(new_root), depth);
}

LocationIndex LocationIndex::clear(std::size_t slot) const {
    return set(slot, LocationEntry{});
}

std::size_t LocationIndex::capacity() const noexcept {
    if (depth_ == 0) {
        return 0;
    }
    return page_span_for_depth(depth_ + 1) * LocationPage::page_capacity;
}

}  // namespace krepis
