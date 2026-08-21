#include "krepis/paragraph_record.hpp"
#include "krepis/utf8.hpp"

#include <utility>

namespace krepis {

ParagraphRecord::ParagraphRecord(std::uint64_t content_revision, std::string utf8) noexcept
	: ObjectRecord(content_revision), utf8_(std::move(utf8)) {}

Result<IntrusivePtr<const ParagraphRecord>> ParagraphRecord::create(
	std::uint64_t content_revision,
	std::string utf8
) {
	if (auto validation = validate_utf8(utf8); !validation.is_ok()) {
		return validation.error();
	}

	return make_intrusive<ParagraphRecord>(content_revision, std::move(utf8));
}

}  // namespace krepis
