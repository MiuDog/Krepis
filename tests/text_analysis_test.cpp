#include "krepis/text_analysis.hpp"
#include "krepis/utf8.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using krepis::BaseDirection;
using krepis::BreakKind;
using krepis::TextAnalysis;
using krepis::analyze_text;
using krepis::is_utf8_boundary;
using krepis_test::expect;

namespace {

constexpr std::uint32_t tag(char a, char b, char c, char d) {
	return (static_cast<std::uint32_t>(a) << 24u) |
	       (static_cast<std::uint32_t>(b) << 16u) |
	       (static_cast<std::uint32_t>(c) << 8u) |
	       static_cast<std::uint32_t>(d);
}

bool has_break_inside(const TextAnalysis& analysis, std::size_t begin, std::size_t end) {
	return std::any_of(
		analysis.line_breaks.begin(),
		analysis.line_breaks.end(),
		[begin, end](const auto& point) {
			return point.byte_offset > begin && point.byte_offset < end;
		}
	);
}

void expect_all_ranges_are_utf8_boundaries(
	std::string_view text,
	const TextAnalysis& analysis
) {
	for (const auto offset : analysis.grapheme_boundaries) {
		expect(is_utf8_boundary(text, offset), "grapheme boundary 位於 UTF-8 邊界");
	}
	for (const auto& point : analysis.line_breaks) {
		expect(is_utf8_boundary(text, point.byte_offset), "line break 位於 UTF-8 邊界");
	}
	for (const auto& run : analysis.bidi_runs) {
		expect(is_utf8_boundary(text, run.byte_offset), "bidi run 開頭位於 UTF-8 邊界");
		expect(is_utf8_boundary(text, run.byte_offset + run.byte_length),
		       "bidi run 結尾位於 UTF-8 邊界");
	}
	for (const auto& run : analysis.script_runs) {
		expect(is_utf8_boundary(text, run.byte_offset), "script run 開頭位於 UTF-8 邊界");
		expect(is_utf8_boundary(text, run.byte_offset + run.byte_length),
		       "script run 結尾位於 UTF-8 邊界");
	}
}

void test_invalid_and_empty_input() {
	const std::string invalid("\xED\xA0\x80", 3);
	expect(!analyze_text(invalid, "zh").is_ok(), "第三方分析前拒絕不合法 UTF-8");

	auto empty = analyze_text({}, "zh");
	expect(empty.is_ok(), "空文字可分析");
	if (!empty.is_ok()) return;
	expect(empty.value().grapheme_boundaries == std::vector<std::size_t>{0},
	       "空文字仍有唯一 caret boundary");
	expect(empty.value().bidi_runs.empty() && empty.value().script_runs.empty(),
	       "空文字不產生虛構 run");
}

void test_grapheme_and_cjk_breaks() {
	const std::string combining = "cafe\xCC\x81";
	auto combining_result = analyze_text(combining, "en");
	expect(combining_result.is_ok(), "combining sequence 可分析");
	if (combining_result.is_ok()) {
		const auto& boundaries = combining_result.value().grapheme_boundaries;
		expect(boundaries.size() == 5 && boundaries.back() == combining.size(),
		       "e 加 combining acute 是單一 grapheme");
		expect_all_ranges_are_utf8_boundaries(combining, combining_result.value());
	}

	const std::string family =
		"\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
		"\xF0\x9F\x91\xA6";
	auto family_result = analyze_text(family, "en");
	expect(family_result.is_ok(), "emoji ZWJ 可分析");
	if (family_result.is_ok()) {
		expect(family_result.value().grapheme_boundaries ==
		           std::vector<std::size_t>{0, family.size()},
		       "emoji 家庭 ZWJ 序列不可被 caret 拆開");
	}

	const std::string cjk = "測試。測試";
	auto cjk_result = analyze_text(cjk, "zh");
	expect(cjk_result.is_ok(), "CJK 標點可分析");
	if (cjk_result.is_ok()) {
		const auto punctuation = cjk.find("。");
		expect(!has_break_inside(cjk_result.value(), punctuation, punctuation + 3),
		       "句號本身不可被拆開");
		expect(std::none_of(
			cjk_result.value().line_breaks.begin(),
			cjk_result.value().line_breaks.end(),
			[punctuation](const auto& point) { return point.byte_offset == punctuation; }
		), "CJK 句號前沒有行首候選點");
	}
}

void test_explicit_url_and_path_tokens_are_atomic() {
	const std::string samples[] = {
		"before https://example.com/a-b after",
		"before C:\\notes\\child-file.md after",
		"before \\\\server\\share\\child after",
		"before /home/user/child-file after",
	};

	for (const auto& text : samples) {
		const auto begin = text.find(' ') + 1;
		const auto end = text.find(' ', begin);
		auto result = analyze_text(text, "en");
		expect(result.is_ok(), "明確 URL／path token 可分析");
		if (!result.is_ok()) continue;
		expect(!has_break_inside(result.value(), begin, end),
		       "明確 URL／path token 內部候選斷點被移除");
	}
}

void test_bidi_numbers_and_script_runs() {
	const std::string text =
		"Latin \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 2026 \xE6\xB3\xA8";
	auto result = analyze_text(text, "ar", BaseDirection::auto_ltr);
	expect(result.is_ok(), "Latin／Arabic／數字／Han 混排可分析");
	if (!result.is_ok()) return;

	const auto number_offset = text.find("2026");
	const auto number_run = std::find_if(
		result.value().bidi_runs.begin(),
		result.value().bidi_runs.end(),
		[number_offset](const auto& run) {
			return number_offset >= run.byte_offset &&
			       number_offset < run.byte_offset + run.byte_length;
		}
	);
	expect(number_run != result.value().bidi_runs.end(), "找到歐洲數字所屬 bidi run");
	if (number_run != result.value().bidi_runs.end()) {
		expect(number_run->embedding_level % 2 == 0,
		       "Arabic 內容中的 2026 維持 LTR embedding level");
	}

	const auto has_latin = std::any_of(
		result.value().script_runs.begin(),
		result.value().script_runs.end(),
		[](const auto& run) { return run.open_type_tag == tag('l', 'a', 't', 'n'); }
	);
	const auto has_arabic = std::any_of(
		result.value().script_runs.begin(),
		result.value().script_runs.end(),
		[](const auto& run) { return run.open_type_tag == tag('a', 'r', 'a', 'b'); }
	);
	const auto has_han = std::any_of(
		result.value().script_runs.begin(),
		result.value().script_runs.end(),
		[](const auto& run) { return run.open_type_tag == tag('h', 'a', 'n', 'i'); }
	);
	expect(has_latin && has_arabic && has_han, "script runs 保留三種 OpenType script tag");
	expect_all_ranges_are_utf8_boundaries(text, result.value());
}

}  // namespace

int main() {
	test_invalid_and_empty_input();
	test_grapheme_and_cjk_breaks();
	test_explicit_url_and_path_tokens_are_atomic();
	test_bidi_numbers_and_script_runs();
	return krepis_test::report("krepis.text_analysis");
}
