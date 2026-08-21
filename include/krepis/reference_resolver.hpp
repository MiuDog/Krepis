#pragma once

// DOC-0001 D5：從起點建立無環 rendering view；不把結果存回 ObjectStore。

#include "krepis/document_revision.hpp"
#include "krepis/embed_record.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace krepis {

enum class ReferenceResolutionStatus : std::uint8_t {
	resolved,
	empty,
	cycle_cut,
	missing_target,
	detached,
};

struct ResolvedBlockView;

struct ResolvedContainerView {
	ContainerId source;
	ReferenceResolutionStatus status = ReferenceResolutionStatus::resolved;
	std::vector<ResolvedBlockView> blocks;
};

struct ResolvedBlockView {
	BlockId block;
	ReferenceResolutionStatus status = ReferenceResolutionStatus::resolved;
	std::optional<RectD> spatial_frame;
	std::optional<ResolvedContainerView> embed;
};

class ReferenceResolver {
public:
	explicit ReferenceResolver(const DocumentRevision& revision) noexcept;

	[[nodiscard]] ResolvedContainerView resolve_flow(ContainerId source) const;
	[[nodiscard]] ResolvedContainerView resolve_spatial(
		ContainerId source,
		RectD viewport
	) const;

private:
	[[nodiscard]] ResolvedContainerView resolve_flow_range(
		const FlowRangeTarget& target,
		std::vector<ContainerId>& active_path
	) const;
	[[nodiscard]] ResolvedContainerView resolve_spatial_viewport(
		const SpatialViewportTarget& target,
		std::vector<ContainerId>& active_path
	) const;
	[[nodiscard]] ResolvedBlockView resolve_block(
		BlockId block,
		std::optional<RectD> frame,
		std::vector<ContainerId>& active_path
	) const;

	const DocumentRevision& revision_;
};

}  // namespace krepis
