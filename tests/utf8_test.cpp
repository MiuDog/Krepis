#include "krepis/utf8.hpp"

#include "test_support.hpp"

#include <string>

using krepis::ErrorCode;
using krepis::is_utf8_boundary;
using krepis::validate_utf8;
using krepis_test::expect;

namespace {

void test_validation_accepts_unicode_scalars() {
	const std::string text = "A\xE6\xB3\xA8\xF0\x9F\x98\x80";
	expect(validate_utf8(text).is_ok(), "ASCII、CJK 與四 byte scalar 通過 UTF-8 驗證");
	expect(validate_utf8({}).is_ok(), "空字串是合法 UTF-8");
}

void test_validation_rejects_invalid_forms() {
	const std::string truncated("\xE4\xB8", 2);
	const std::string overlong("\xC0\xAF", 2);
	const std::string surrogate("\xED\xA0\x80", 3);
	const std::string too_large("\xF4\x90\x80\x80", 4);

	for (const auto& text : {truncated, overlong, surrogate, too_large}) {
		auto result = validate_utf8(text);
		expect(!result.is_ok(), "拒絕不合法 UTF-8 形式");
		if (!result.is_ok()) {
			expect(result.error().code() == ErrorCode::invalid_argument,
			       "不合法 UTF-8 回傳 invalid_argument");
		}
	}
}

void test_boundary_rejects_continuation_offsets() {
	const std::string text = "A\xE6\xB3\xA8\xF0\x9F\x98\x80";
	expect(is_utf8_boundary(text, 0), "開頭是 UTF-8 boundary");
	expect(is_utf8_boundary(text, 1), "CJK code point 前是 boundary");
	expect(!is_utf8_boundary(text, 2) && !is_utf8_boundary(text, 3),
	       "CJK continuation byte 不是 boundary");
	expect(is_utf8_boundary(text, 4), "emoji 前是 boundary");
	expect(!is_utf8_boundary(text, 5) && !is_utf8_boundary(text, 6) &&
	           !is_utf8_boundary(text, 7),
	       "emoji continuation byte 不是 boundary");
	expect(is_utf8_boundary(text, text.size()), "past-the-end 是合法 boundary");
	expect(!is_utf8_boundary(text, text.size() + 1), "超出字串不是 boundary");
}

}  // namespace

int main() {
	test_validation_accepts_unicode_scalars();
	test_validation_rejects_invalid_forms();
	test_boundary_rejects_continuation_offsets();
	return krepis_test::report("krepis.utf8");
}
