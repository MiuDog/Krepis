#include "krepis/flow_layout_index.hpp"

#include <cassert>
#include <iterator>
#include <utility>

namespace krepis {

// --- FlowLayoutLeaf ---

FlowLayoutLeaf::FlowLayoutLeaf(std::vector<LayoutEntry> entries) noexcept
    : entries_(std::move(entries)), total_extent_(0.0) {
    assert(!entries_.empty());
    for (const auto& e : entries_) {
        total_extent_ += e.measured_height;
    }
}

bool FlowLayoutLeaf::is_leaf() const noexcept { return true; }
std::size_t FlowLayoutLeaf::block_count() const noexcept { return entries_.size(); }
double FlowLayoutLeaf::subtree_extent() const noexcept { return total_extent_; }
std::span<const LayoutEntry> FlowLayoutLeaf::entries() const noexcept { return entries_; }

// --- FlowLayoutInternal ---

FlowLayoutInternal::FlowLayoutInternal(std::vector<LayoutChildEntry> children) noexcept
    : children_(std::move(children)), total_block_count_(0), total_extent_(0.0) {
    assert(children_.size() >= 2);
    for (const auto& c : children_) {
        assert(c.child);
        total_block_count_ += c.subtree_block_count;
        total_extent_ += c.subtree_extent;
    }
}

bool FlowLayoutInternal::is_leaf() const noexcept { return false; }
std::size_t FlowLayoutInternal::block_count() const noexcept { return total_block_count_; }
double FlowLayoutInternal::subtree_extent() const noexcept { return total_extent_; }
std::span<const LayoutChildEntry> FlowLayoutInternal::children() const noexcept { return children_; }

// --- Internal helpers ---

namespace {

LayoutChildEntry make_layout_child(IntrusivePtr<const FlowLayoutNode> child) {
    return {child, child->block_count(), child->subtree_extent()};
}

struct LayoutInsertResult {
    IntrusivePtr<const FlowLayoutNode> left;
    IntrusivePtr<const FlowLayoutNode> right;
};

std::size_t find_child_for_position(std::span<const LayoutChildEntry> children,
                                    std::size_t& remaining) {
    for (std::size_t i = 0; i + 1 < children.size(); ++i) {
        if (remaining < children[i].subtree_block_count) {
            return i;
        }
        remaining -= children[i].subtree_block_count;
    }
    return children.size() - 1;
}

LayoutInsertResult insert_into_leaf(const FlowLayoutLeaf* leaf, std::size_t position,
                                    LayoutEntry entry, const FlowLayoutConfig& config) {
    auto entries = std::vector<LayoutEntry>(leaf->entries().begin(), leaf->entries().end());
    assert(position <= entries.size());
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(position), std::move(entry));

    if (entries.size() <= config.leaf_capacity) {
        return {make_intrusive<FlowLayoutLeaf>(std::move(entries)), nullptr};
    }

    auto mid = static_cast<std::ptrdiff_t>(entries.size() / 2);
    auto left_entries = std::vector<LayoutEntry>(entries.begin(), entries.begin() + mid);
    auto right_entries = std::vector<LayoutEntry>(entries.begin() + mid, entries.end());
    return {
        make_intrusive<FlowLayoutLeaf>(std::move(left_entries)),
        make_intrusive<FlowLayoutLeaf>(std::move(right_entries)),
    };
}

LayoutInsertResult insert_into(const FlowLayoutNode* node, std::size_t position,
                               LayoutEntry entry, const FlowLayoutConfig& config) {
    if (node->is_leaf()) {
        return insert_into_leaf(static_cast<const FlowLayoutLeaf*>(node),
                                position, std::move(entry), config);
    }

    const auto* internal = static_cast<const FlowLayoutInternal*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

    auto child_result = insert_into(children[child_idx].child.get(),
                                    remaining, std::move(entry), config);

    std::vector<LayoutChildEntry> new_children;
    new_children.reserve(children.size() + (child_result.right ? 1 : 0));
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i == child_idx) {
            new_children.push_back(make_layout_child(child_result.left));
            if (child_result.right) {
                new_children.push_back(make_layout_child(child_result.right));
            }
        } else {
            new_children.push_back({children[i].child, children[i].subtree_block_count,
                                    children[i].subtree_extent});
        }
    }

    if (new_children.size() <= config.internal_fanout) {
        return {make_intrusive<FlowLayoutInternal>(std::move(new_children)), nullptr};
    }

    auto mid = static_cast<std::ptrdiff_t>(new_children.size() / 2);
    auto left = std::vector<LayoutChildEntry>(
        std::make_move_iterator(new_children.begin()),
        std::make_move_iterator(new_children.begin() + mid));
    auto right = std::vector<LayoutChildEntry>(
        std::make_move_iterator(new_children.begin() + mid),
        std::make_move_iterator(new_children.end()));
    return {
        make_intrusive<FlowLayoutInternal>(std::move(left)),
        make_intrusive<FlowLayoutInternal>(std::move(right)),
    };
}

IntrusivePtr<const FlowLayoutNode> remove_from(const FlowLayoutNode* node,
                                                std::size_t position) {
    if (node->is_leaf()) {
        const auto* leaf = static_cast<const FlowLayoutLeaf*>(node);
        auto entries = std::vector<LayoutEntry>(leaf->entries().begin(), leaf->entries().end());
        assert(position < entries.size());
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(position));

        if (entries.empty()) {
            return nullptr;
        }
        return make_intrusive<FlowLayoutLeaf>(std::move(entries));
    }

    const auto* internal = static_cast<const FlowLayoutInternal*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

    auto new_child = remove_from(children[child_idx].child.get(), remaining);

    std::vector<LayoutChildEntry> new_children;
    new_children.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i == child_idx) {
            if (new_child) {
                new_children.push_back(make_layout_child(std::move(new_child)));
            }
        } else {
            new_children.push_back({children[i].child, children[i].subtree_block_count,
                                    children[i].subtree_extent});
        }
    }

    if (new_children.empty()) {
        return nullptr;
    }
    if (new_children.size() == 1) {
        return std::move(new_children[0].child);
    }
    return make_intrusive<FlowLayoutInternal>(std::move(new_children));
}

IntrusivePtr<const FlowLayoutNode> update_entry(
	const FlowLayoutNode* node,
	std::size_t position,
	LayoutEntry replacement
) {
    if (node->is_leaf()) {
        const auto* leaf = static_cast<const FlowLayoutLeaf*>(node);
        auto entries = std::vector<LayoutEntry>(leaf->entries().begin(), leaf->entries().end());
        assert(position < entries.size());
		entries[position] = std::move(replacement);
        return make_intrusive<FlowLayoutLeaf>(std::move(entries));
    }

    const auto* internal = static_cast<const FlowLayoutInternal*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

	auto new_child = update_entry(
		children[child_idx].child.get(),
		remaining,
		std::move(replacement)
	);

    std::vector<LayoutChildEntry> new_children;
    new_children.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i == child_idx) {
            new_children.push_back(make_layout_child(std::move(new_child)));
        } else {
            new_children.push_back({children[i].child, children[i].subtree_block_count,
                                    children[i].subtree_extent});
        }
    }
    return make_intrusive<FlowLayoutInternal>(std::move(new_children));
}

}  // anonymous namespace

// --- FlowLayoutIndex ---

FlowLayoutIndex::FlowLayoutIndex(FlowLayoutConfig config,
                                 IntrusivePtr<const FlowLayoutNode> root) noexcept
    : config_(config), root_(std::move(root)) {}

FlowLayoutIndex FlowLayoutIndex::empty(FlowLayoutConfig config) {
    return FlowLayoutIndex(config, nullptr);
}

std::size_t FlowLayoutIndex::block_count() const noexcept {
    return root_ ? root_->block_count() : 0;
}

bool FlowLayoutIndex::is_empty() const noexcept {
    return !root_ || root_->block_count() == 0;
}

double FlowLayoutIndex::total_extent() const noexcept {
    return root_ ? root_->subtree_extent() : 0.0;
}

FlowLayoutIndex FlowLayoutIndex::insert(std::size_t position, LayoutEntry entry) const {
    assert(position <= block_count());

    if (!root_) {
        return FlowLayoutIndex(config_,
                               make_intrusive<FlowLayoutLeaf>(std::vector<LayoutEntry>{std::move(entry)}));
    }

    auto result = insert_into(root_.get(), position, std::move(entry), config_);

    if (!result.right) {
        return FlowLayoutIndex(config_, std::move(result.left));
    }

    std::vector<LayoutChildEntry> root_children;
    root_children.push_back(make_layout_child(result.left));
    root_children.push_back(make_layout_child(result.right));
    return FlowLayoutIndex(config_,
                           make_intrusive<FlowLayoutInternal>(std::move(root_children)));
}

FlowLayoutIndex FlowLayoutIndex::remove(std::size_t position) const {
    assert(root_ && position < block_count());
    auto new_root = remove_from(root_.get(), position);
    return FlowLayoutIndex(config_, std::move(new_root));
}

FlowLayoutIndex FlowLayoutIndex::update_extent(std::size_t position, double new_height) const {
    assert(root_ && position < block_count());
	auto replacement = at(position);
	replacement.measured_height = new_height;
	replacement.status = MeasurementStatus::measured;
	auto new_root = update_entry(root_.get(), position, std::move(replacement));
    return FlowLayoutIndex(config_, std::move(new_root));
}

FlowLayoutIndex FlowLayoutIndex::invalidate_extent(
	std::size_t position,
	std::uint64_t source_content_revision
) const {
	assert(root_ && position < block_count());
	auto replacement = at(position);
	replacement.source_content_revision = source_content_revision;
	replacement.status = MeasurementStatus::estimated;
	auto new_root = update_entry(root_.get(), position, std::move(replacement));
	return FlowLayoutIndex(config_, std::move(new_root));
}

double FlowLayoutIndex::prefix_extent(std::size_t position) const {
    if (position == 0 || !root_) {
        return 0.0;
    }
    assert(position <= block_count());

    double prefix = 0.0;
    const FlowLayoutNode* node = root_.get();
    std::size_t remaining = position;

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowLayoutInternal*>(node);
        auto children = internal->children();
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (remaining <= children[i].subtree_block_count) {
                node = children[i].child.get();
                break;
            }
            prefix += children[i].subtree_extent;
            remaining -= children[i].subtree_block_count;
        }
    }

    const auto* leaf = static_cast<const FlowLayoutLeaf*>(node);
    for (std::size_t i = 0; i < remaining && i < leaf->entries().size(); ++i) {
        prefix += leaf->entries()[i].measured_height;
    }
    return prefix;
}

std::size_t FlowLayoutIndex::lower_bound_extent(double y) const {
    if (!root_ || y <= 0.0) {
        return 0;
    }
    if (y >= root_->subtree_extent()) {
        return block_count();
    }

    double accumulated = 0.0;
    std::size_t base_position = 0;
    const FlowLayoutNode* node = root_.get();

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowLayoutInternal*>(node);
        auto children = internal->children();
        bool descended = false;
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (accumulated + children[i].subtree_extent > y) {
                node = children[i].child.get();
                descended = true;
                break;
            }
            accumulated += children[i].subtree_extent;
            base_position += children[i].subtree_block_count;
        }
        if (!descended) {
            return block_count();
        }
    }

    const auto* leaf = static_cast<const FlowLayoutLeaf*>(node);
    for (std::size_t i = 0; i < leaf->entries().size(); ++i) {
        accumulated += leaf->entries()[i].measured_height;
        if (accumulated > y) {
            return base_position + i;
        }
    }
    return base_position + leaf->entries().size();
}

const LayoutEntry& FlowLayoutIndex::at(std::size_t position) const {
    assert(root_ && position < block_count());
    const FlowLayoutNode* node = root_.get();
    std::size_t remaining = position;

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowLayoutInternal*>(node);
        auto children = internal->children();
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (remaining < children[i].subtree_block_count) {
                node = children[i].child.get();
                break;
            }
            remaining -= children[i].subtree_block_count;
        }
    }

    const auto* leaf = static_cast<const FlowLayoutLeaf*>(node);
    assert(remaining < leaf->entries().size());
    return leaf->entries()[remaining];
}

IntrusivePtr<const FlowLayoutNode> FlowLayoutIndex::root() const noexcept {
    return root_;
}

}  // namespace krepis
