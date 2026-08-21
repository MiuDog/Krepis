#include "krepis/font_registry.hpp"

namespace krepis {

Result<void> FontRegistry::register_font(
	FontId font_id,
	std::span<const std::byte> bytes,
	std::uint32_t face_index
) {
	if (font_id == 0 || bytes.empty()) {
		return Error{ErrorCode::invalid_argument, "字型 ID 與 bytes 不得為空"};
	}
	if (entries_.contains(font_id)) {
		return Error{ErrorCode::invalid_argument, "字型 ID 不得重複註冊"};
	}
	Entry entry{std::vector<std::byte>(bytes.begin(), bytes.end()), face_index};
	entries_.emplace(font_id, std::move(entry));
	order_.push_back(font_id);
	++revision_;
	return {};
}

std::vector<FontId> FontRegistry::candidates(
	std::uint32_t,
	std::string_view
) const {
	return order_;
}

Result<FontDataView> FontRegistry::open(FontId font_id) const {
	const auto found = entries_.find(font_id);
	if (found == entries_.end()) {
		return Error{ErrorCode::not_found, "字型 ID 尚未註冊"};
	}
	return FontDataView{found->second.bytes, found->second.face_index};
}

}  // namespace krepis
