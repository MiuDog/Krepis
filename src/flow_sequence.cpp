#include "krepis/flow_sequence.hpp"

#include <cassert>
#include <iterator>
#include <utility>

namespace krepis {

// --- FlowLeafNode ---

FlowLeafNode::FlowLeafNode(std::vector<BlockId> blocks) noexcept
    : blocks_(std::move(blocks)) {
    assert(!blocks_.empty() && "leaf 不應為空");
}

bool FlowLeafNode::is_leaf() const noexcept { return true; }
std::size_t FlowLeafNode::block_count() const noexcept { return blocks_.size(); }
std::span<const BlockId> FlowLeafNode::blocks() const noexcept { return blocks_; }

// --- FlowInternalNode ---

FlowInternalNode::FlowInternalNode(std::vector<ChildEntry> children) noexcept
    : children_(std::move(children)), total_block_count_(0) {
    assert(children_.size() >= 2 && "internal node 至少兩個 child");
    for (const auto& c : children_) {
        assert(c.child && "child 不得為 null");
        total_block_count_ += c.subtree_block_count;
    }
}

bool FlowInternalNode::is_leaf() const noexcept { return false; }
std::size_t FlowInternalNode::block_count() const noexcept { return total_block_count_; }
std::span<const ChildEntry> FlowInternalNode::children() const noexcept { return children_; }

// --- Internal helpers ---

namespace {

struct InsertResult {
    IntrusivePtr<const FlowSequenceNode> left;
    IntrusivePtr<const FlowSequenceNode> right;
};

InsertResult insert_into_leaf(const FlowLeafNode* leaf, std::size_t position,
                              BlockId block_id, const FlowSequenceConfig& config) {
    auto blocks = std::vector<BlockId>(leaf->blocks().begin(), leaf->blocks().end());
    assert(position <= blocks.size());
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(position), block_id);

    if (blocks.size() <= config.leaf_capacity) {
        return {make_intrusive<FlowLeafNode>(std::move(blocks)), nullptr};
    }

    auto mid = static_cast<std::ptrdiff_t>(blocks.size() / 2);
    auto left_blocks = std::vector<BlockId>(blocks.begin(), blocks.begin() + mid);
    auto right_blocks = std::vector<BlockId>(blocks.begin() + mid, blocks.end());
    return {
        make_intrusive<FlowLeafNode>(std::move(left_blocks)),
        make_intrusive<FlowLeafNode>(std::move(right_blocks)),
    };
}

std::size_t find_child_for_position(std::span<const ChildEntry> children,
                                    std::size_t& remaining) {
    for (std::size_t i = 0; i + 1 < children.size(); ++i) {
        if (remaining < children[i].subtree_block_count) {
            return i;
        }
        remaining -= children[i].subtree_block_count;
    }
    return children.size() - 1;
}

std::vector<ChildEntry> copy_children_with_replacement(
    std::span<const ChildEntry> old_children, std::size_t replaced_idx,
    const InsertResult& replacement) {
    std::vector<ChildEntry> result;
    result.reserve(old_children.size() + (replacement.right ? 1 : 0));
    for (std::size_t i = 0; i < old_children.size(); ++i) {
        if (i == replaced_idx) {
            result.push_back({replacement.left, replacement.left->block_count()});
            if (replacement.right) {
                result.push_back({replacement.right, replacement.right->block_count()});
            }
        } else {
            result.push_back({old_children[i].child, old_children[i].subtree_block_count});
        }
    }
    return result;
}

InsertResult split_children(std::vector<ChildEntry> children) {
    auto mid = static_cast<std::ptrdiff_t>(children.size() / 2);
    auto left = std::vector<ChildEntry>(
        std::make_move_iterator(children.begin()),
        std::make_move_iterator(children.begin() + mid));
    auto right = std::vector<ChildEntry>(
        std::make_move_iterator(children.begin() + mid),
        std::make_move_iterator(children.end()));
    return {
        make_intrusive<FlowInternalNode>(std::move(left)),
        make_intrusive<FlowInternalNode>(std::move(right)),
    };
}

InsertResult insert_into(const FlowSequenceNode* node, std::size_t position,
                         BlockId block_id, const FlowSequenceConfig& config) {
    if (node->is_leaf()) {
        return insert_into_leaf(static_cast<const FlowLeafNode*>(node),
                                position, block_id, config);
    }

    const auto* internal = static_cast<const FlowInternalNode*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

    auto child_result = insert_into(children[child_idx].child.get(),
                                    remaining, block_id, config);

    auto new_children = copy_children_with_replacement(children, child_idx, child_result);

    if (new_children.size() <= config.internal_fanout) {
        return {make_intrusive<FlowInternalNode>(std::move(new_children)), nullptr};
    }

    return split_children(std::move(new_children));
}

IntrusivePtr<const FlowSequenceNode> remove_from(const FlowSequenceNode* node,
                                                  std::size_t position) {
    if (node->is_leaf()) {
        const auto* leaf = static_cast<const FlowLeafNode*>(node);
        auto blocks = std::vector<BlockId>(leaf->blocks().begin(), leaf->blocks().end());
        assert(position < blocks.size());
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(position));

        if (blocks.empty()) {
            return nullptr;
        }
        return make_intrusive<FlowLeafNode>(std::move(blocks));
    }

    const auto* internal = static_cast<const FlowInternalNode*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

    auto new_child = remove_from(children[child_idx].child.get(), remaining);

    std::vector<ChildEntry> new_children;
    new_children.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i == child_idx) {
            if (new_child) {
                auto count = new_child->block_count();
                new_children.push_back({std::move(new_child), count});
            }
        } else {
            new_children.push_back({children[i].child, children[i].subtree_block_count});
        }
    }

    if (new_children.empty()) {
        return nullptr;
    }
    if (new_children.size() == 1) {
        return std::move(new_children[0].child);
    }
    return make_intrusive<FlowInternalNode>(std::move(new_children));
}

}  // anonymous namespace

// --- FlowSequence ---

FlowSequence::FlowSequence(FlowSequenceConfig config,
                           IntrusivePtr<const FlowSequenceNode> root) noexcept
    : config_(config), root_(std::move(root)) {}

FlowSequence FlowSequence::empty(FlowSequenceConfig config) {
    return FlowSequence(config, nullptr);
}

std::size_t FlowSequence::block_count() const noexcept {
    return root_ ? root_->block_count() : 0;
}

bool FlowSequence::is_empty() const noexcept {
    return !root_ || root_->block_count() == 0;
}

BlockId FlowSequence::at(std::size_t position) const {
    assert(root_ && position < block_count());
    const FlowSequenceNode* node = root_.get();
    std::size_t remaining = position;

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowInternalNode*>(node);
        bool descended = false;
        for (const auto& entry : internal->children()) {
            if (remaining < entry.subtree_block_count) {
                node = entry.child.get();
                descended = true;
                break;
            }
            remaining -= entry.subtree_block_count;
        }
        assert(descended);
        (void)descended;
    }

    const auto* leaf = static_cast<const FlowLeafNode*>(node);
    assert(remaining < leaf->blocks().size());
    return leaf->blocks()[remaining];
}

FlowSequence FlowSequence::insert(std::size_t position, BlockId block_id) const {
    assert(position <= block_count());

    if (!root_) {
        return FlowSequence(config_,
                            make_intrusive<FlowLeafNode>(std::vector<BlockId>{block_id}));
    }

    auto result = insert_into(root_.get(), position, block_id, config_);

    if (!result.right) {
        return FlowSequence(config_, std::move(result.left));
    }

    std::vector<ChildEntry> root_children;
    root_children.push_back({result.left, result.left->block_count()});
    root_children.push_back({result.right, result.right->block_count()});
    return FlowSequence(config_,
                        make_intrusive<FlowInternalNode>(std::move(root_children)));
}

FlowSequence FlowSequence::remove(std::size_t position) const {
    assert(root_ && position < block_count());
    auto new_root = remove_from(root_.get(), position);
    return FlowSequence(config_, std::move(new_root));
}

const FlowSequenceConfig& FlowSequence::config() const noexcept {
    return config_;
}

IntrusivePtr<const FlowSequenceNode> FlowSequence::root() const noexcept {
    return root_;
}

}  // namespace krepis
