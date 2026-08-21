#pragma once

// Q1=A：P1 dogfood 專用、P4 必須丟棄的暫時檔案格式。

#include "krepis/document_revision.hpp"
#include "krepis/error.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace krepis {

[[nodiscard]] Result<std::vector<std::byte>> encode_dogfood_file(
	const DocumentRevision& revision
);

[[nodiscard]] Result<DocumentRevision> decode_dogfood_file(
	std::span<const std::byte> bytes
);

// 先完整編碼，再於目標同目錄寫入、flush 並原子替換；失敗時既有目標保持不變。
[[nodiscard]] Result<void> save_dogfood_file(
	const std::filesystem::path& path,
	const DocumentRevision& revision
);

[[nodiscard]] Result<DocumentRevision> load_dogfood_file(
	const std::filesystem::path& path
);

}  // namespace krepis
