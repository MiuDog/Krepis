#pragma once

#include <string_view>

namespace krepis {

// 語意化版本。0.x 期間公開介面隨時可能變更。
inline constexpr int version_major = 0;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 1;

// 回傳 "major.minor.patch" 形式的版本字串。
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace krepis
