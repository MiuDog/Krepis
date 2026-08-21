#include "krepis/flow_sequence.hpp"

#include <cassert>
#include <iterator>
#include <utility>

namespace krepis {

// --- FlowLeafNode ---

FlowLeafNode::FlowLeafNode(LeafKey key, std::vector<BlockId> blocks) noexcept
    : key_(key), blocks_(std::move(blocks)) {
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

LeafKey get_min_leaf_key(const FlowSequenceNode* node) {
    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowInternalNode*>(node);
        node = internal->children()[0].child.get();
    }
    return static_cast<const FlowLeafNode*>(node)->key();
}

ChildEntry make_child_entry(IntrusivePtr<const FlowSequenceNode> child) {
    auto count = child->block_count();
    auto key = get_min_leaf_key(child.get());
    return {std::move(child), count, key};
}

struct InsertResult {
    IntrusivePtr<const FlowSequenceNode> left;
    IntrusivePtr<const FlowSequenceNode> right;
};

InsertResult insert_into_leaf(const FlowLeafNode* leaf, std::size_t position,
                              BlockId block_id, const FlowSequenceConfig& config,
                              const LeafKey& right_bound) {
    auto blocks = std::vector<BlockId>(leaf->blocks().begin(), leaf->blocks().end());
    assert(position <= blocks.size());
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(position), block_id);

    if (blocks.size() <= config.leaf_capacity) {
        return {make_intrusive<FlowLeafNode>(leaf->key(), std::move(blocks)), nullptr};
    }

    auto mid = static_cast<std::ptrdiff_t>(blocks.size() / 2);
    auto left_blocks = std::vector<BlockId>(blocks.begin(), blocks.begin() + mid);
    auto right_blocks = std::vector<BlockId>(blocks.begin() + mid, blocks.end());

    auto new_key = leaf_key_midpoint(leaf->key(), right_bound);
    LeafKey right_key = new_key.value_or(right_bound);

    return {
        make_intrusive<FlowLeafNode>(leaf->key(), std::move(left_blocks)),
        make_intrusive<FlowLeafNode>(right_key, std::move(right_blocks)),
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
            result.push_back(make_child_entry(replacement.left));
            if (replacement.right) {
                result.push_back(make_child_entry(replacement.right));
            }
        } else {
            result.push_back({old_children[i].child, old_children[i].subtree_block_count,
                              old_children[i].min_leaf_key});
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
                         BlockId block_id, const FlowSequenceConfig& config,
                         const LeafKey& right_bound) {
    if (node->is_leaf()) {
        return insert_into_leaf(static_cast<const FlowLeafNode*>(node),
                                position, block_id, config, right_bound);
    }

    const auto* internal = static_cast<const FlowInternalNode*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

    LeafKey child_right_bound = (child_idx + 1 < children.size())
        ? children[child_idx + 1].min_leaf_key
        : right_bound;

    auto child_result = insert_into(children[child_idx].child.get(),
                                    remaining, block_id, config, child_right_bound);

    auto new_children = copy_children_with_replacement(children, child_idx, child_result);

    if (new_children.size() <= config.internal_fanout) {
        return {make_intrusive<FlowInternalNode>(std::move(new_children)), nullptr};
    }

    return split_children(std::move(new_children));
}

bool is_underflowed_leaf(const FlowSequenceNode* node, const FlowSequenceConfig& config) {
    return node->is_leaf() && node->block_count() < config.merge_low_water;
}

void rebalance_children(std::vector<ChildEntry>& children, const FlowSequenceConfig& config) {
    if (config.merge_low_water == 0) {
        return;
    }
    bool merged = true;
    while (merged) {
        merged = false;
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (!is_underflowed_leaf(children[i].child.get(), config)) {
                continue;
            }
            if (children.size() < 2) {
                break;
            }
            std::size_t sibling_idx = (i + 1 < children.size()) ? i + 1 : i - 1;
            if (!children[sibling_idx].child->is_leaf()) {
                continue;
            }

            std::size_t first = (i < sibling_idx) ? i : sibling_idx;
            std::size_t second = (i < sibling_idx) ? sibling_idx : i;

            // **Owner pin**（閘門 7／D2）：先複製 owning pointer 到區域變數，
            // 再從它取 raw pointer。稍後覆寫 children[first]／children[second] 時，
            // 舊節點仍由這兩個 pin 保活，因此重排順序不可能造成 use-after-free。
            //
            // 原本的寫法（直接從 children[] 取 raw pointer）**目前也正確**，
            // 但那是依賴敘述順序的脆弱保證——改動很容易破壞它而不留痕跡。
            const IntrusivePtr<const FlowSequenceNode> left_pin = children[first].child;
            const IntrusivePtr<const FlowSequenceNode> right_pin = children[second].child;
            const auto* left_src = static_cast<const FlowLeafNode*>(left_pin.get());
            const auto* right_src = static_cast<const FlowLeafNode*>(right_pin.get());

            std::vector<BlockId> combined;
            auto total = left_src->block_count() + right_src->block_count();
            combined.reserve(total);
            combined.insert(combined.end(), left_src->blocks().begin(), left_src->blocks().end());
            combined.insert(combined.end(), right_src->blocks().begin(), right_src->blocks().end());

            if (total <= config.leaf_capacity) {
                // Merge: keep leftmost key.
                auto merged_count = combined.size();
                auto merged_key = left_src->key();
                children[first] = {make_intrusive<FlowLeafNode>(merged_key, std::move(combined)),
                                   merged_count, merged_key};
                children.erase(children.begin() + static_cast<std::ptrdiff_t>(second));
                merged = true;
                break;
            }
            // Redistribute evenly. Left keeps its key, right keeps its key.
            auto mid = static_cast<std::ptrdiff_t>(total / 2);
            auto left_blocks = std::vector<BlockId>(combined.begin(), combined.begin() + mid);
            auto right_blocks = std::vector<BlockId>(combined.begin() + mid, combined.end());
            auto left_count = left_blocks.size();
            auto right_count = right_blocks.size();
            auto left_key = left_src->key();
            auto right_key = right_src->key();
            children[first] = {make_intrusive<FlowLeafNode>(left_key, std::move(left_blocks)),
                               left_count, left_key};
            children[second] = {make_intrusive<FlowLeafNode>(right_key, std::move(right_blocks)),
                                right_count, right_key};
        }
    }
}

IntrusivePtr<const FlowSequenceNode> remove_from(const FlowSequenceNode* node,
                                                  std::size_t position,
                                                  const FlowSequenceConfig& config) {
    if (node->is_leaf()) {
        const auto* leaf = static_cast<const FlowLeafNode*>(node);
        auto blocks = std::vector<BlockId>(leaf->blocks().begin(), leaf->blocks().end());
        assert(position < blocks.size());
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(position));

        if (blocks.empty()) {
            return nullptr;
        }
        return make_intrusive<FlowLeafNode>(leaf->key(), std::move(blocks));
    }

    const auto* internal = static_cast<const FlowInternalNode*>(node);
    auto children = internal->children();

    std::size_t remaining = position;
    std::size_t child_idx = find_child_for_position(children, remaining);

    auto new_child = remove_from(children[child_idx].child.get(), remaining, config);

    std::vector<ChildEntry> new_children;
    new_children.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i == child_idx) {
            if (new_child) {
                new_children.push_back(make_child_entry(std::move(new_child)));
            }
        } else {
            new_children.push_back({children[i].child, children[i].subtree_block_count,
                                    children[i].min_leaf_key});
        }
    }

    if (new_children.empty()) {
        return nullptr;
    }
    if (new_children.size() == 1) {
        return std::move(new_children[0].child);
    }

    // D16: rebalance underflowed leaf children.
    rebalance_children(new_children, config);

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

LeafKey FlowSequence::leaf_key_at(std::size_t position) const {
    assert(root_ && position < block_count());
    const FlowSequenceNode* node = root_.get();
    std::size_t remaining = position;

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowInternalNode*>(node);
        for (const auto& entry : internal->children()) {
            if (remaining < entry.subtree_block_count) {
                node = entry.child.get();
                break;
            }
            remaining -= entry.subtree_block_count;
        }
    }
    return static_cast<const FlowLeafNode*>(node)->key();
}

std::size_t FlowSequence::find_by_key(const LeafKey& key) const {
    if (!root_) {
        return 0;
    }
    const FlowSequenceNode* node = root_.get();
    std::size_t prefix = 0;

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowInternalNode*>(node);
        auto children = internal->children();
        bool found = false;
        for (std::size_t i = children.size(); i > 0; --i) {
            if (children[i - 1].min_leaf_key <= key) {
                node = children[i - 1].child.get();
                found = true;
                break;
            }
            // Don't add to prefix yet - we're searching backwards
        }
        if (!found) {
            return block_count();
        }
        // Recompute prefix: add block counts of all children before the selected one
        auto children2 = internal->children();
        for (std::size_t i = 0; i < children2.size(); ++i) {
            if (children2[i].child.get() == node) {
                break;
            }
            prefix += children2[i].subtree_block_count;
        }
    }

    const auto* leaf = static_cast<const FlowLeafNode*>(node);
    if (leaf->key() == key) {
        return prefix;
    }
    return block_count();
}

std::optional<std::size_t> FlowSequence::find_block_in_leaf(
	const LeafKey& key,
	BlockId block
) const {
	if (!root_) return std::nullopt;

	const FlowSequenceNode* node = root_.get();
	std::size_t prefix = 0;
	while (!node->is_leaf()) {
		const auto* internal = static_cast<const FlowInternalNode*>(node);
		auto children = internal->children();
		std::size_t selected = children.size();
		for (std::size_t i = children.size(); i > 0; --i) {
			if (children[i - 1].min_leaf_key <= key) {
				selected = i - 1;
				break;
			}
		}
		if (selected == children.size()) return std::nullopt;

		for (std::size_t i = 0; i < selected; ++i) {
			prefix += children[i].subtree_block_count;
		}
		node = children[selected].child.get();
	}

	const auto* leaf = static_cast<const FlowLeafNode*>(node);
	if (leaf->key() != key) return std::nullopt;

	auto blocks = leaf->blocks();
	for (std::size_t i = 0; i < blocks.size(); ++i) {
		if (blocks[i] == block) return prefix + i;
	}
	return std::nullopt;
}

FlowSequence FlowSequence::insert(std::size_t position, BlockId block_id) const {
    assert(position <= block_count());

    if (!root_) {
        auto initial_key = leaf_key_midpoint(leaf_key_min, leaf_key_max).value();
        return FlowSequence(config_,
                            make_intrusive<FlowLeafNode>(initial_key, std::vector<BlockId>{block_id}));
    }

    auto result = insert_into(root_.get(), position, block_id, config_, leaf_key_max);

    if (!result.right) {
        return FlowSequence(config_, std::move(result.left));
    }

    std::vector<ChildEntry> root_children;
    root_children.push_back(make_child_entry(result.left));
    root_children.push_back(make_child_entry(result.right));
    return FlowSequence(config_,
                        make_intrusive<FlowInternalNode>(std::move(root_children)));
}

FlowSequence FlowSequence::remove(std::size_t position) const {
    assert(root_ && position < block_count());
    auto new_root = remove_from(root_.get(), position, config_);
    return FlowSequence(config_, std::move(new_root));
}

const FlowSequenceConfig& FlowSequence::config() const noexcept {
    return config_;
}

IntrusivePtr<const FlowSequenceNode> FlowSequence::root() const noexcept {
    return root_;
}

// --- TreeCursor ---

TreeCursor::TreeCursor(const FlowSequence& seq, std::size_t position)
    : root_(seq.root()), global_position_(position) {
    if (!root_) {
        return;
    }
    assert(position < root_->block_count());

    const FlowSequenceNode* node = root_.get();
    std::size_t remaining = position;

    while (!node->is_leaf()) {
        const auto* internal = static_cast<const FlowInternalNode*>(node);
        auto children = internal->children();
        std::size_t idx = 0;
        for (; idx + 1 < children.size(); ++idx) {
            if (remaining < children[idx].subtree_block_count) {
                break;
            }
            remaining -= children[idx].subtree_block_count;
        }
        ancestors_.push_back({internal, idx});
        node = children[idx].child.get();
    }

    leaf_ = static_cast<const FlowLeafNode*>(node);
    local_offset_ = remaining;
}

TreeCursor::TreeCursor(TreeCursor&& other) noexcept
    : root_(std::move(other.root_)),
      ancestors_(std::move(other.ancestors_)),
      leaf_(other.leaf_),
      local_offset_(other.local_offset_),
      global_position_(other.global_position_) {
    // leaf_ 是借用 pointer；來源已交出 owning root，必須一併失效。
    other.leaf_ = nullptr;
    other.local_offset_ = 0;
    other.global_position_ = 0;
}

TreeCursor& TreeCursor::operator=(TreeCursor&& other) noexcept {
    if (this != &other) {
        root_ = std::move(other.root_);
        ancestors_ = std::move(other.ancestors_);
        leaf_ = other.leaf_;
        local_offset_ = other.local_offset_;
        global_position_ = other.global_position_;

        other.leaf_ = nullptr;
        other.local_offset_ = 0;
        other.global_position_ = 0;
    }
    return *this;
}

bool TreeCursor::is_valid() const noexcept {
    return leaf_ != nullptr;
}

BlockId TreeCursor::current() const {
    assert(leaf_ && "存取無效 cursor");
    return leaf_->blocks()[local_offset_];
}

std::size_t TreeCursor::position() const noexcept {
    return global_position_;
}

bool TreeCursor::advance() {
    if (!leaf_) {
        return false;
    }

    if (local_offset_ + 1 < leaf_->blocks().size()) {
        ++local_offset_;
        ++global_position_;
        return true;
    }

    while (!ancestors_.empty()) {
        auto& frame = ancestors_.back();
        if (frame.child_index + 1 < frame.node->children().size()) {
            ++frame.child_index;
            const FlowSequenceNode* node =
                frame.node->children()[frame.child_index].child.get();
            while (!node->is_leaf()) {
                const auto* internal = static_cast<const FlowInternalNode*>(node);
                ancestors_.push_back({internal, 0});
                node = internal->children()[0].child.get();
            }
            leaf_ = static_cast<const FlowLeafNode*>(node);
            local_offset_ = 0;
            ++global_position_;
            return true;
        }
        ancestors_.pop_back();
    }

    leaf_ = nullptr;
    return false;
}

bool TreeCursor::retreat() {
    if (!leaf_ || global_position_ == 0) {
        return false;
    }

    if (local_offset_ > 0) {
        --local_offset_;
        --global_position_;
        return true;
    }

    while (!ancestors_.empty()) {
        auto& frame = ancestors_.back();
        if (frame.child_index > 0) {
            --frame.child_index;
            const FlowSequenceNode* node =
                frame.node->children()[frame.child_index].child.get();
            while (!node->is_leaf()) {
                const auto* internal = static_cast<const FlowInternalNode*>(node);
                auto last_idx = internal->children().size() - 1;
                ancestors_.push_back({internal, last_idx});
                node = internal->children()[last_idx].child.get();
            }
            leaf_ = static_cast<const FlowLeafNode*>(node);
            local_offset_ = leaf_->blocks().size() - 1;
            --global_position_;
            return true;
        }
        ancestors_.pop_back();
    }

    assert(false && "retreat: global_position > 0 但找不到前一個 leaf");
    return false;
}

}  // namespace krepis
