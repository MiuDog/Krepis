#include "krepis/display_list_layout.hpp"
#include "krepis/font_registry.hpp"
#include "krepis/text_layout.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <fstream>
#include <vector>

using krepis::BaseDirection;
using krepis::DisplayListPublisher;
using krepis::FontRegistry;
using krepis::ParagraphLayouter;
using krepis_test::expect;

namespace {

std::vector<std::byte> read_font() {
	std::ifstream stream(KREPIS_TEST_DISPLAY_FONT, std::ios::binary);
	stream.seekg(0, std::ios::end);
	const auto size = stream.tellg();
	if (size <= 0) return {};
	stream.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
	return stream ? bytes : std::vector<std::byte>{};
}

void test_layout_encodes_one_command_per_visual_run() {
	FontRegistry fonts;
	expect(fonts.register_font(1, read_font()).is_ok(), "display layout fixture 字型註冊成功");
	ParagraphLayouter layouter(fonts);
	auto layout = layouter.layout(
		"first line\nsecond line",
		"en",
		BaseDirection::auto_ltr,
		3,
		16 * 64,
		120 * 64,
		20 * 64
	);
	expect(layout.is_ok(), "paragraph 可產生多行 layout");
	if (!layout.is_ok()) return;
	DisplayListPublisher publisher;
	auto builder = publisher.begin_frame();
	if (!builder.is_ok()) return;
	expect(krepis::append_paragraph_layout(
		*builder.value(),
		layout.value(),
		8 * 64,
		12 * 64,
		16 * 64,
		0xFF202020
	).is_ok(), "ParagraphLayout 可直接序列化為 glyph runs");
	auto token = publisher.publish();
	expect(token.is_ok(), "layout display frame 通過完整 validator");
	if (!token.is_ok()) return;
	auto lease = publisher.acquire_front();
	if (!lease.is_ok()) return;
	auto summary = krepis::validate_display_list(
		std::span<const std::byte>(lease.value().data, lease.value().size)
	);
	expect(summary.is_ok() &&
	           summary.value().command_count == layout.value().glyph_runs.size(),
	       "display command 數與核心 visual glyph run 數完全相同");
	expect(publisher.release(lease.value().lease_id).is_ok(), "layout display lease 釋放");
}

void test_late_run_failure_rolls_back_prior_commands() {
	krepis::ParagraphLayout invalid{1, 64, 64, {}, {}, {}};
	invalid.glyph_runs.push_back({
		1,
		0,
		1,
		krepis::GlyphDirection::ltr,
		0,
		64,
		-16,
		0,
		{{7, 0, 64, 0, 0, 0}},
	});
	invalid.glyph_runs.push_back({
		1,
		1,
		0,
		krepis::GlyphDirection::ltr,
		0,
		64,
		-16,
		0,
		{},
	});
	invalid.lines.push_back({0, 1, 64, 64, 64, 0, 2, false});
	DisplayListPublisher publisher;
	auto builder = publisher.begin_frame();
	if (!builder.is_ok()) return;
	auto result = krepis::append_paragraph_layout(
		*builder.value(),
		invalid,
		0,
		0,
		16 * 64,
		0xFFFFFFFF
	);
	expect(!result.is_ok(), "後續 glyph run 不合法時整個 paragraph append fail closed");
	auto published = publisher.publish();
	expect(published.is_ok(), "paragraph append 失敗後 builder 仍可發布完整空 frame");
	if (!published.is_ok()) return;
	auto lease = publisher.acquire_front();
	if (!lease.is_ok()) return;
	auto summary = krepis::validate_display_list(
		std::span<const std::byte>(lease.value().data, lease.value().size)
	);
	expect(summary.is_ok() && summary.value().command_count == 0,
	       "paragraph append 失敗不殘留部分 glyph command");
	expect(publisher.release(lease.value().lease_id).is_ok(), "空 frame lease 釋放");
}

}  // namespace

int main() {
	test_layout_encodes_one_command_per_visual_run();
	test_late_run_failure_rolls_back_prior_commands();
	return krepis_test::report("krepis.display_list_layout");
}
