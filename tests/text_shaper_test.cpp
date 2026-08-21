#include "krepis/text_shaper.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using krepis::BaseDirection;
using krepis::Error;
using krepis::ErrorCode;
using krepis::FontDataView;
using krepis::FontId;
using krepis::FontProvider;
using krepis::GlyphDirection;
using krepis::Result;
using krepis::TextShaper;
using krepis_test::expect;

namespace {

constexpr FontId latin_font = 1;
constexpr FontId han_font = 2;
constexpr FontId arabic_font = 3;

constexpr std::uint32_t tag(char a, char b, char c, char d) {
	return (static_cast<std::uint32_t>(a) << 24u) |
	       (static_cast<std::uint32_t>(b) << 16u) |
	       (static_cast<std::uint32_t>(c) << 8u) |
	       static_cast<std::uint32_t>(d);
}

std::vector<std::byte> read_font(const char* path) {
	std::ifstream stream(path, std::ios::binary);
	stream.seekg(0, std::ios::end);
	const auto size = stream.tellg();
	if (size <= 0) return {};
	stream.seekg(0, std::ios::beg);
	std::vector<std::byte> result(static_cast<std::size_t>(size));
	stream.read(
		reinterpret_cast<char*>(result.data()),
		static_cast<std::streamsize>(size)
	);
	if (!stream) return {};
	return result;
}

class FixtureFontProvider final : public FontProvider {
public:
	FixtureFontProvider() {
		fonts_.emplace(latin_font, read_font(KREPIS_TEST_LATIN_FONT));
		fonts_.emplace(han_font, read_font(KREPIS_TEST_HAN_FONT));
		fonts_.emplace(arabic_font, read_font(KREPIS_TEST_ARABIC_FONT));
	}

	[[nodiscard]] std::uint64_t font_set_revision() const noexcept override { return revision; }

	[[nodiscard]] std::vector<FontId> candidates(
		std::uint32_t script_tag,
		std::string_view
	) const override {
		++candidate_calls;
		if (script_tag == tag('a', 'r', 'a', 'b')) return {arabic_font};
		if (script_tag == tag('h', 'a', 'n', 'i')) return {latin_font, han_font};
		return {latin_font};
	}

	[[nodiscard]] Result<FontDataView> open(FontId font_id) const override {
		++open_calls;
		const auto found = fonts_.find(font_id);
		if (found == fonts_.end() || found->second.empty()) {
			return Error{ErrorCode::not_found, "測試字型不存在"};
		}
		return FontDataView{found->second, 0};
	}

	std::uint64_t revision = 1;
	mutable std::size_t candidate_calls = 0;
	mutable std::size_t open_calls = 0;

private:
	std::unordered_map<FontId, std::vector<std::byte>> fonts_;
};

void test_latin_and_han_fallback() {
	FixtureFontProvider provider;
	TextShaper shaper(provider);
	// U+5005 是固定 SourceHanSans subset 明確包含、Roboto 明確不包含的 Han fixture。
	auto result = shaper.shape("A\xE5\x80\x85", "zh", BaseDirection::auto_ltr, 16 * 64);
	expect(result.is_ok(), "Latin 加 Han 可完成 fallback 與 shaping");
	if (!result.is_ok()) return;
	expect(result.value().glyph_runs.size() == 2, "script 與 fallback 邊界產生兩個 glyph runs");
	if (result.value().glyph_runs.size() == 2) {
		expect(result.value().glyph_runs[0].font_id == latin_font, "Latin 使用首選字型");
		expect(result.value().glyph_runs[1].font_id == han_font, "Han 跳過缺字首選並使用 fallback");
	}
	for (const auto& run : result.value().glyph_runs) {
		for (const auto& glyph : run.glyphs) {
			expect(glyph.glyph_id != 0, "正式輸出不可含 .notdef");
		}
	}
}

void test_combining_and_rtl_cluster_mapping() {
	FixtureFontProvider provider;
	TextShaper shaper(provider);
	auto combining = shaper.shape("cafe\xCC\x81", "en", BaseDirection::auto_ltr, 16 * 64);
	expect(combining.is_ok(), "combining grapheme 可由單一字型 shaping");
	if (combining.is_ok()) {
		for (const auto& glyph : combining.value().glyph_runs.front().glyphs) {
			expect(glyph.cluster_byte_offset < 6, "HarfBuzz cluster 回映原 UTF-8 byte offset");
		}
	}

	const std::string arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
	auto rtl = shaper.shape(arabic, "ar", BaseDirection::auto_ltr, 16 * 64);
	expect(rtl.is_ok(), "Arabic 可 shaping");
	if (rtl.is_ok()) {
		expect(rtl.value().glyph_runs.front().direction == GlyphDirection::rtl,
		       "Arabic glyph run 明確標示 RTL");
	}
}

void test_missing_glyph_fails_with_byte_offset() {
	FixtureFontProvider provider;
	TextShaper shaper(provider);
	auto result = shaper.shape("A\xF0\x9F\xA6\x84", "en", BaseDirection::auto_ltr, 16 * 64);
	expect(!result.is_ok(), "所有候選缺字時 fail closed");
	if (!result.is_ok()) {
		expect(result.error().code() == ErrorCode::missing_glyph,
		       "缺字有機器可判斷的 error code");
		expect(result.error().has_context() && result.error().context() == 1,
		       "缺字錯誤指出第一個缺字 grapheme byte offset");
	}
}

void test_coverage_cache_follows_font_set_revision() {
	FixtureFontProvider provider;
	TextShaper shaper(provider);
	expect(shaper.shape("abc", "en", BaseDirection::auto_ltr, 16 * 64).is_ok(),
	       "第一次 shaping 建立 coverage cache");
	const auto first_candidate_calls = provider.candidate_calls;
	const auto first_open_calls = provider.open_calls;
	expect(shaper.shape("abc", "en", BaseDirection::auto_ltr, 16 * 64).is_ok(),
	       "相同 font-set revision 可重用 cache");
	expect(provider.candidate_calls == first_candidate_calls &&
	           provider.open_calls == first_open_calls,
	       "相同 revision 不重列候選也不重開字型");

	++provider.revision;
	expect(shaper.shape("abc", "en", BaseDirection::auto_ltr, 16 * 64).is_ok(),
	       "font-set revision 改變後仍可 shaping");
	expect(provider.candidate_calls > first_candidate_calls &&
	           provider.open_calls > first_open_calls,
	       "revision 改變會清除候選與 face coverage cache");
}

}  // namespace

int main() {
	test_latin_and_han_fallback();
	test_combining_and_rtl_cluster_mapping();
	test_missing_glyph_fails_with_byte_offset();
	test_coverage_cache_follows_font_set_revision();
	return krepis_test::report("krepis.text_shaper");
}
