#include "krepis/paragraph_record.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "test_support.hpp"

#include <string>

using krepis::ErrorCode;
using krepis::ParagraphRecord;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

void expect_invalid_utf8(std::string text, const char* message) {
	auto result = ParagraphRecord::create(1, std::move(text));
	expect(!result.is_ok(), message);
	if (!result.is_ok()) {
		expect(result.error().code() == ErrorCode::invalid_argument,
		       "不合法 UTF-8 回傳 invalid_argument");
	}
}

void test_accepts_valid_utf8_and_preserves_revision() {
	const std::string text = "ASCII \xE6\xB3\xA8\xE9\x9F\xB3 \xF0\x9F\x98\x80";
	auto result = ParagraphRecord::create(42, text);

	expect(result.is_ok(), "合法 UTF-8 建立成功");
	if (!result.is_ok()) return;

	auto record = std::move(result).take();
	expect(record->content_revision() == 42, "保存來源 content revision");
	expect(record->utf8() == text, "逐 byte 保存 UTF-8 內容");
}

void test_rejects_invalid_utf8_forms() {
	expect_invalid_utf8(std::string("\xE4\xB8", 2), "拒絕截斷的三 byte sequence");
	expect_invalid_utf8(std::string("\xC0\xAF", 2), "拒絕 overlong sequence");
	expect_invalid_utf8(std::string("\xED\xA0\x80", 3), "拒絕 UTF-16 surrogate");
	expect_invalid_utf8(std::string("\xF4\x90\x80\x80", 4), "拒絕大於 U+10FFFF");
	expect_invalid_utf8(std::string("\x80", 1), "拒絕單獨 continuation byte");
}

void test_accepts_empty_paragraph() {
	auto result = ParagraphRecord::create(7, {});
	expect(result.is_ok(), "空 Paragraph 是合法內容");
	if (result.is_ok()) {
		expect(result.value()->utf8().empty(), "空 Paragraph 保持空字串");
	}
}

}  // namespace

int main() {
	test_accepts_valid_utf8_and_preserves_revision();
	test_rejects_invalid_utf8_forms();
	test_accepts_empty_paragraph();

	shutdown_default_reclamation_queue();
	return krepis_test::report("krepis.paragraph_record");
}
