#pragma once

// LAY-0002 D21：把 Transaction 產生的 stable-ID 失效集合套到指定 FlowLayoutIndex。

#include "krepis/document_revision.hpp"
#include "krepis/error.hpp"
#include "krepis/flow_layout_index.hpp"
#include "krepis/transaction.hpp"

#include <span>

namespace krepis {

// `invalidations` 是全文件集合；本函式只套用 owner 等於 `container` 的 Flow Block，
// 其他 Container／Spatial Block 由其各自 cache 處理。
[[nodiscard]] Result<FlowLayoutIndex> apply_layout_invalidations(
	const DocumentRevision& revision,
	ContainerId container,
	const FlowLayoutIndex& layout,
	std::span<const LayoutInvalidation> invalidations
);

}  // namespace krepis
