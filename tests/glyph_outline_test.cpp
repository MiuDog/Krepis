#include "krepis/font_registry.hpp"
#include "krepis/glyph_outline.hpp"
#include "krepis/text_shaper.hpp"

#include "test_support.hpp"

#include <cmath>
#include <cstddef>
#include <fstream>
#include <vector>

using krepis::BaseDirection;
using krepis::FontRegistry;
using krepis::GlyphOutlineCache;
using krepis::GlyphPathOpcode;
using krepis::TextShaper;
using krepis_test::expect;

namespace {

std::vector<std::byte> read_font() {
	std::ifstream stream(KREPIS_TEST_OUTLINE_FONT, std::ios::binary);
	stream.seekg(0, std::ios::end);
	const auto size = stream.tellg();
	if (size <= 0) return {};
	stream.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
	return stream ? bytes : std::vector<std::byte>{};
}

void test_shape_and_outline_share_registered_font() {
	FontRegistry registry;
	const auto bytes = read_font();
	expect(registry.register_font(7, bytes).is_ok(), "字型 bytes 可註冊到核心 registry");
	expect(!registry.register_font(7, bytes).is_ok(), "重複 FontId fail closed");
	TextShaper shaper(registry);
	auto shaped = shaper.shape("A", "en", BaseDirection::auto_ltr, 16 * 64);
	expect(shaped.is_ok() && !shaped.value().glyph_runs.empty() &&
	           !shaped.value().glyph_runs.front().glyphs.empty(),
	       "同一 registry 可用於 shaping");
	if (!shaped.is_ok() || shaped.value().glyph_runs.empty() ||
	    shaped.value().glyph_runs.front().glyphs.empty()) {
		return;
	}
	const auto glyph_id = shaped.value().glyph_runs.front().glyphs.front().glyph_id;
	GlyphOutlineCache outlines(registry);
	auto first = outlines.outline(7, glyph_id, 16 * 64);
	expect(first.is_ok() && !first.value()->empty(), "核心可取得實際 glyph outline");
	if (!first.is_ok()) return;
	bool has_move = false;
	bool has_close = false;
	for (const auto& command : *first.value()) {
		has_move = has_move || command.opcode == GlyphPathOpcode::move_to;
		has_close = has_close || command.opcode == GlyphPathOpcode::close_path;
		for (const auto value : command.values) {
			expect(std::isfinite(value), "glyph outline 座標全部為有限值");
		}
	}
	expect(has_move && has_close, "實際字形含 move 與 close path command");
	auto cached = outlines.outline(7, glyph_id, 16 * 64);
	expect(cached.is_ok() && cached.value().get() == first.value().get(),
	       "相同 font、glyph、size 重用同一 immutable outline");
	expect(outlines.stats().hits == 1 && outlines.stats().misses == 1,
	       "outline cache 命中與 miss 可觀測");
	expect(registry.register_font(8, bytes).is_ok(), "新字型註冊會推進 font-set revision");
	auto after_revision = outlines.outline(7, glyph_id, 16 * 64);
	expect(after_revision.is_ok() && after_revision.value().get() != first.value().get() &&
	           outlines.stats().misses == 2 && outlines.stats().entries == 1,
	       "font-set revision 變更後清除舊 outline 並重建");
}

}  // namespace

int main() {
	test_shape_and_outline_share_registered_font();
	return krepis_test::report("krepis.glyph_outline");
}
