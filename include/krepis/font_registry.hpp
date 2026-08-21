#pragma once

// Flutter 只負責提供已隨 app 散佈的字型 bytes；fallback 決策仍由核心統一執行。

#include "krepis/text_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace krepis {

class FontRegistry final : public FontProvider {
public:
	[[nodiscard]] Result<void> register_font(
		FontId font_id,
		std::span<const std::byte> bytes,
		std::uint32_t face_index = 0
	);

	[[nodiscard]] std::uint64_t font_set_revision() const noexcept override {
		return revision_;
	}
	[[nodiscard]] std::vector<FontId> candidates(
		std::uint32_t script_tag,
		std::string_view language
	) const override;
	[[nodiscard]] Result<FontDataView> open(FontId font_id) const override;

private:
	struct Entry {
		std::vector<std::byte> bytes;
		std::uint32_t face_index = 0;
	};

	std::uint64_t revision_ = 1;
	std::vector<FontId> order_;
	std::unordered_map<FontId, Entry> entries_;
};

}  // namespace krepis
