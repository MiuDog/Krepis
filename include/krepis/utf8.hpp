#pragma once

// TXT-0001 的 canonical UTF-8 邊界。所有第三方文字處理前必須先通過這一層。

#include "krepis/error.hpp"

#include <cstddef>
#include <string_view>

namespace krepis {

[[nodiscard]] Result<void> validate_utf8(std::string_view text) noexcept;

// 前置條件：text 已通過 validate_utf8。byte_offset 可等於 text.size()；
// 落在 multi-byte sequence 內部時回傳 false。
[[nodiscard]] bool is_utf8_boundary(
	std::string_view text,
	std::size_t byte_offset
) noexcept;

}  // namespace krepis
