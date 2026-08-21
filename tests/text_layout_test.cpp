#include "krepis/text_layout.hpp"
#include "krepis/utf8.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using krepis::BaseDirection;
using krepis::CachedParagraphLayouter;
using krepis::Error;
using krepis::ErrorCode;
using krepis::FontDataView;
using krepis::FontId;
using krepis::FontProvider;
using krepis::ParagraphLayouter;
using krepis::Result;
using krepis_test::expect;

namespace {

constexpr FontId latin_font = 1;

std::vector<std::byte> read_font(const char* path) {
	std::ifstream stream(path, std::ios::binary);
	stream.seekg(0, std::ios::end);
	const auto size = stream.tellg();
	if (size <= 0) return {};
	stream.seekg(0, std::ios::beg);
	std::vector<std::byte> result(static_cast<std::size_t>(size));
	stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
	return stream ? result : std::vector<std::byte>{};
}

class LayoutFontProvider final : public FontProvider {
public:
	LayoutFontProvider() : bytes_(read_font(KREPIS_TEST_LATIN_FONT)) {}

	[[nodiscard]] std::uint64_t font_set_revision() const noexcept override { return revision; }
	[[nodiscard]] std::vector<FontId> candidates(
		std::uint32_t,
		std::string_view
	) const override {
		return {latin_font};
	}
	[[nodiscard]] Result<FontDataView> open(FontId font_id) const override {
		if (font_id != latin_font || bytes_.empty()) {
			return Error{ErrorCode::not_found, "layout fixture font 不存在"};
		}
		return FontDataView{bytes_, 0};
	}

private:
	std::vector<std::byte> bytes_;

public:
	std::uint64_t revision = 1;
};

void test_greedy_lines_use_only_approved_boundaries() {
	LayoutFontProvider provider;
	ParagraphLayouter layouter(provider);
	const std::string text = "one two three four";
	auto result = layouter.layout(
		text,
		"en",
		BaseDirection::auto_ltr,
		7,
		16 * 64,
		50 * 64,
		20 * 64
	);
	expect(result.is_ok(), "Latin paragraph 可切成多行");
	if (!result.is_ok()) return;
	expect(result.value().lines.size() >= 2, "窄 viewport 產生多個 line fragments");
	std::size_t expected_offset = 0;
	for (const auto& line : result.value().lines) {
		expect(line.byte_offset == expected_offset, "line byte ranges 連續且無重疊");
		expect(krepis::is_utf8_boundary(text, line.byte_offset + line.byte_length),
		       "line 結尾是 UTF-8 boundary");
		expect(line.glyph_run_count > 0, "每個非空 line 都經過 reshaping");
		expect(!line.overflow, "一般單字不應標成 overflow");
		expect(line.width_26_6 <= 50 * 64, "一般 line 不超過 viewport width");
		expect(line.height_26_6 >= 20 * 64, "line height 不小於樣式要求與字型 natural height");
		expected_offset += line.byte_length;
	}
	expect(expected_offset == text.size(), "line fragments 完整覆蓋 paragraph");
	expect(result.value().source_revision == 7, "layout 保留來源 revision");
	expect(result.value().total_height_26_6 >=
	           static_cast<std::int32_t>(result.value().lines.size()) * 20 * 64,
	       "總高度由每行實際高度累加");
}

void test_unbreakable_url_overflows_without_arbitrary_split() {
	LayoutFontProvider provider;
	ParagraphLayouter layouter(provider);
	const std::string text = "https://example.com/very-long-path";
	auto result = layouter.layout(
		text,
		"en",
		BaseDirection::auto_ltr,
		8,
		16 * 64,
		20 * 64,
		20 * 64
	);
	expect(result.is_ok(), "長 URL 可 layout");
	if (!result.is_ok()) return;
	expect(result.value().lines.size() == 1, "URL 內部沒有任意切行");
	expect(result.value().lines.front().overflow, "超長 URL 明確標記 overflow");
	expect(result.value().lines.front().byte_length == text.size(), "overflow line 保留完整 URL");
}

void test_every_grapheme_boundary_has_a_caret_stop() {
	LayoutFontProvider provider;
	ParagraphLayouter layouter(provider);
	const std::string text = "office cafe\xCC\x81";
	auto analysis = krepis::analyze_text(text, "en");
	auto result = layouter.layout(
		text,
		"en",
		BaseDirection::auto_ltr,
		9,
		16 * 64,
		200 * 64,
		20 * 64
	);
	expect(analysis.is_ok() && result.is_ok(), "caret fixture 可分析並 layout");
	if (!analysis.is_ok() || !result.is_ok()) return;
	for (const auto boundary : analysis.value().grapheme_boundaries) {
		const auto found = std::find_if(
			result.value().caret_stops.begin(),
			result.value().caret_stops.end(),
			[boundary](const auto& stop) { return stop.byte_offset == boundary; }
		);
		expect(found != result.value().caret_stops.end(), "每個 grapheme boundary 都有 caret stop");
	}
}

void test_empty_and_explicit_newline_layout() {
	LayoutFontProvider provider;
	ParagraphLayouter layouter(provider);
	auto empty = layouter.layout(
		{}, "en", BaseDirection::auto_ltr, 10, 16 * 64, 100 * 64, 20 * 64
	);
	expect(empty.is_ok() && empty.value().lines.size() == 1,
	       "空 paragraph 保留一個可編輯 line");
	if (empty.is_ok()) {
		expect(empty.value().caret_stops.size() == 1 &&
		           empty.value().caret_stops.front().byte_offset == 0,
		       "空 paragraph 在 byte 0 有 caret stop");
	}

	const std::string text = "first\nsecond";
	auto newline = layouter.layout(
		text, "en", BaseDirection::auto_ltr, 11, 16 * 64, 200 * 64, 20 * 64
	);
	expect(newline.is_ok() && newline.value().lines.size() == 2,
	       "強制 newline 產生兩個 line fragments");
	if (newline.is_ok() && newline.value().lines.size() == 2) {
		expect(newline.value().lines[0].byte_offset == 0 &&
		           newline.value().lines[0].byte_length == 6,
		       "第一行 byte range 包含 newline，但 glyph 不包含 separator");
		expect(newline.value().lines[1].byte_offset == 6,
		       "第二行從 newline 後開始");
	}
}

void test_cache_key_and_lru_eviction_are_complete() {
	LayoutFontProvider provider;
	CachedParagraphLayouter layouter(provider, 2);
	const auto call = [&](std::string_view text, std::uint64_t composition, std::int32_t width) {
		return layouter.layout(
			text,
			"en",
			BaseDirection::auto_ltr,
			20,
			composition,
			0,
			16 * 64,
			width,
			20 * 64
		);
	};
	expect(call("alpha", 0, 100 * 64).is_ok(), "cache 第一次 miss 可計算");
	expect(call("alpha", 0, 100 * 64).is_ok(), "相同 dependency key 可命中");
	expect(layouter.cache_stats().hits == 1 && layouter.cache_stats().misses == 1,
	       "相同輸入只命中一次 cache");
	expect(call("alpha", 1, 100 * 64).is_ok(), "composition revision 改變會 miss");
	expect(call("alpha", 1, 120 * 64).is_ok(), "width 改變會 miss 並觸發 LRU eviction");
	expect(layouter.cache_stats().evictions == 1 && layouter.cache_stats().size == 2,
	       "capacity 2 嚴格淘汰最舊 entry");

	const auto misses_before_font_change = layouter.cache_stats().misses;
	++provider.revision;
	expect(call("alpha", 1, 120 * 64).is_ok(), "font-set revision 改變後可重新 layout");
	expect(layouter.cache_stats().misses == misses_before_font_change + 1,
	       "font-set revision 屬於 cache key");
}

}  // namespace

int main() {
	test_greedy_lines_use_only_approved_boundaries();
	test_unbreakable_url_overflows_without_arbitrary_split();
	test_every_grapheme_boundary_has_a_caret_stop();
	test_empty_and_explicit_newline_layout();
	test_cache_key_and_lru_eviction_are_complete();
	return krepis_test::report("krepis.text_layout");
}
