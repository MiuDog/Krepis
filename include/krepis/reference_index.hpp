#pragma once

// DOC-0001 D3～D5：source ContainerId -> referencing Embed BlockIds 的 immutable 反向索引。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_id.hpp"

#include <span>
#include <unordered_map>
#include <vector>

namespace krepis {

class ReferenceIndex final : public RefCounted {
public:
	[[nodiscard]] static IntrusivePtr<const ReferenceIndex> empty();

	[[nodiscard]] std::span<const BlockId> referencing(ContainerId source) const noexcept;
	[[nodiscard]] IntrusivePtr<const ReferenceIndex> with_added(
		ContainerId source,
		BlockId embed
	) const;
	[[nodiscard]] IntrusivePtr<const ReferenceIndex> with_removed(
		ContainerId source,
		BlockId embed
	) const;

	explicit ReferenceIndex(std::unordered_map<ContainerId, std::vector<BlockId>> entries);

private:
	std::unordered_map<ContainerId, std::vector<BlockId>> entries_;
};

}  // namespace krepis
