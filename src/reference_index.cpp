#include "krepis/reference_index.hpp"

#include <algorithm>
#include <utility>

namespace krepis {

IntrusivePtr<const ReferenceIndex> ReferenceIndex::empty() {
	return make_intrusive<ReferenceIndex>(
		std::unordered_map<ContainerId, std::vector<BlockId>>{}
	);
}

ReferenceIndex::ReferenceIndex(
	std::unordered_map<ContainerId, std::vector<BlockId>> entries
) : entries_(std::move(entries)) {}

std::span<const BlockId> ReferenceIndex::referencing(ContainerId source) const noexcept {
	const auto found = entries_.find(source);
	if (found == entries_.end()) return {};
	return found->second;
}

IntrusivePtr<const ReferenceIndex> ReferenceIndex::with_added(
	ContainerId source,
	BlockId embed
) const {
	auto entries = entries_;
	auto& references = entries[source];
	if (std::find(references.begin(), references.end(), embed) == references.end()) {
		references.push_back(embed);
	}
	return make_intrusive<ReferenceIndex>(std::move(entries));
}

IntrusivePtr<const ReferenceIndex> ReferenceIndex::with_removed(
	ContainerId source,
	BlockId embed
) const {
	auto entries = entries_;
	const auto found = entries.find(source);
	if (found == entries.end()) return make_intrusive<ReferenceIndex>(std::move(entries));
	auto& references = found->second;
	references.erase(std::remove(references.begin(), references.end(), embed), references.end());
	if (references.empty()) entries.erase(found);
	return make_intrusive<ReferenceIndex>(std::move(entries));
}

}  // namespace krepis
