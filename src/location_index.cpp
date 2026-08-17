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

PageTableNode::PageTableNode(std::vector<PageTableEntry> children) noexcept
    : leaf_level_(true), page_children_(std::move(children)) {}

PageTableNode::PageTableNode(std::vector<IntrusivePtr<const PageTableNode>> internal_children) noexcept
    : leaf_level_(false), internal_children_(std::move(internal_children)) {}

bool PageTableNode::is_leaf_level() const noexcept { return leaf_level_; }

std::span<const PageTableEntry> PageTableNode::page_children() const noexcept {
    assert(leaf_level_);
    return page_children_;
}

std::span<const IntrusivePtr<const PageTableNode>> PageTableNode::internal_children() const noexcept {
    assert(!leaf_level_);
    return internal_children_;
}

// --- LocationIndex ---

namespace {

IntrusivePtr<const LocationPage> make_empty_page() {
    std::vector<LocationEntry> entries(LocationPage::page_capacity);
    return make_intrusive<LocationPage>(std::move(entries));
}

}  // anonymous namespace

LocationIndex::LocationIndex(std::vector<IntrusivePtr<const LocationPage>> pages) noexcept
    : pages_(std::move(pages)) {}

LocationIndex LocationIndex::empty() {
    return LocationIndex({});
}

LocationEntry LocationIndex::lookup(std::size_t slot) const {
    std::size_t page_idx = slot / LocationPage::page_capacity;
    std::size_t offset = slot % LocationPage::page_capacity;

    if (page_idx >= pages_.size()) {
        return {};
    }
    if (!pages_[page_idx]) {
        return {};
    }
    auto entries = pages_[page_idx]->entries();
    if (offset >= entries.size()) {
        return {};
    }
    return entries[offset];
}

LocationIndex LocationIndex::set(std::size_t slot, LocationEntry entry) const {
    std::size_t page_idx = slot / LocationPage::page_capacity;
    std::size_t offset = slot % LocationPage::page_capacity;

    auto new_pages = pages_;

    // Extend if needed.
    while (new_pages.size() <= page_idx) {
        new_pages.push_back(nullptr);
    }

    // COW: copy the affected page.
    IntrusivePtr<const LocationPage> old_page = new_pages[page_idx];
    std::vector<LocationEntry> entries;
    if (old_page) {
        entries = std::vector<LocationEntry>(old_page->entries().begin(), old_page->entries().end());
    } else {
        entries.resize(LocationPage::page_capacity);
    }

    entries[offset] = std::move(entry);
    new_pages[page_idx] = make_intrusive<LocationPage>(std::move(entries));

    return LocationIndex(std::move(new_pages));
}

LocationIndex LocationIndex::clear(std::size_t slot) const {
    return set(slot, LocationEntry{});
}

std::size_t LocationIndex::capacity() const noexcept {
    return pages_.size() * LocationPage::page_capacity;
}

}  // namespace krepis
