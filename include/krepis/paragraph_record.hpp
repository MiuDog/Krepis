#pragma once

// P1 的第一個正式 Block record：保存經驗證的 UTF-8 Paragraph 內容。
// 發布後不可變；每次文字修改都建立新的 ParagraphRecord。

#include "krepis/error.hpp"
#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_store.hpp"

#include <cstdint>
#include <string>

namespace krepis {

class ParagraphRecord final : public ObjectRecord {
public:
	[[nodiscard]] static Result<IntrusivePtr<const ParagraphRecord>> create(
		std::uint64_t content_revision,
		std::string utf8
	);

	[[nodiscard]] const std::string& utf8() const noexcept { return utf8_; }

private:
	template <typename T, typename... Args>
	friend IntrusivePtr<const T> make_intrusive(Args&&... args);

	ParagraphRecord(std::uint64_t content_revision, std::string utf8) noexcept;

	std::string utf8_;
};

}  // namespace krepis
