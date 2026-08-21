#include "krepis/flow_editor.hpp"

#include "krepis/font_registry.hpp"
#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

using krepis::CompositionAttribute;
using krepis::CompositionUpdate;
using krepis::DisplayListPublisher;
using krepis::FlowEditor;
using krepis::FlowEditorSelection;
using krepis::FontRegistry;
using krepis::ParagraphRecord;
using krepis::SequentialIdGenerator;
using krepis_test::expect;

namespace {

std::vector<std::byte> read_font(const char* path = KREPIS_TEST_EDITOR_FONT) {
	std::ifstream stream(path, std::ios::binary);
	stream.seekg(0, std::ios::end);
	const auto size = stream.tellg();
	if (size <= 0) return {};
	stream.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
	return stream ? bytes : std::vector<std::byte>{};
}

std::string paragraph_text(const FlowEditor& editor, std::size_t position) {
	const auto* sequence = editor.revision().flow_root(editor.container());
	if (sequence == nullptr || position >= sequence->block_count()) return {};
	auto record = editor.revision().record_for(sequence->at(position));
	const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
	return paragraph == nullptr ? std::string{} : paragraph->utf8();
}

void test_edit_ime_undo_and_display_are_one_authority() {
	FontRegistry fonts;
	expect(fonts.register_font(1, read_font()).is_ok(), "FlowEditor fixture 字型註冊成功");
	expect(fonts.register_font(2, read_font(KREPIS_TEST_EDITOR_HAN_FONT)).is_ok(),
	       "FlowEditor fixture CJK fallback 字型註冊成功");
	SequentialIdGenerator ids(100);
	auto created = FlowEditor::create(fonts, ids, "AB");
	expect(created.is_ok(), "FlowEditor 可建立第一個 Flow Paragraph");
	if (!created.is_ok()) return;
	auto& editor = *created.value();
	expect(editor.set_caret(FlowEditorSelection{0, 1}).is_ok(), "caret 可設在 A／B 之間");
	expect(editor.insert_text("X", 100, 7).is_ok() && paragraph_text(editor, 0) == "AXB",
	       "文字 intent 經 Transaction 更新唯一 authority");
	expect(editor.insert_paragraph_break(200).is_ok(), "Enter 可在當前 caret 原子 split");
	expect(editor.layout_index().block_count() == 2 &&
	           paragraph_text(editor, 0) == "AX" && paragraph_text(editor, 1) == "B",
	       "split 同步更新 Flow 順序與 layout index");
	expect(editor.insert_text("Y", 300, 8).is_ok() && paragraph_text(editor, 1) == "YB",
	       "split 後 caret 位於新 Block 開頭");
	expect(editor.backspace(400).is_ok() && paragraph_text(editor, 1) == "B",
	       "Block 內 Backspace 以 grapheme command 刪除");
	expect(editor.backspace(500).is_ok() && editor.layout_index().block_count() == 1 &&
	           paragraph_text(editor, 0) == "AXB",
	       "Block 開頭 Backspace 原子合併前一 Paragraph");
	expect(editor.undo().is_ok() && editor.layout_index().block_count() == 2 &&
	           paragraph_text(editor, 0) == "AX" && paragraph_text(editor, 1) == "B",
	       "global undo 一次復原 merge 的 ID、文字與 layout entry");
	expect(editor.redo().is_ok() && editor.layout_index().block_count() == 1 &&
	           paragraph_text(editor, 0) == "AXB",
	       "global redo 一次重做 merge");

	expect(editor.set_caret(FlowEditorSelection{0, 3}).is_ok(), "composition caret 可設在文字尾");
	CompositionUpdate update{
		std::string("\xE7\x9F\xA5", 3),
		3,
		3,
		{{0, 3, CompositionAttribute::target_converted}},
	};
	auto begun = editor.begin_composition(update);
	expect(begun.is_ok(), "FlowEditor 可建立 session-local composition");
	expect(paragraph_text(editor, 0) == "AXB", "composition overlay 不進 authority record");
	DisplayListPublisher publisher;
	auto frame = editor.publish_display(publisher, 400, 240, 0);
	expect(frame.is_ok(), "composition overlay 可經正式 layout 產生 display frame");
	expect(editor.commit_composition(600).is_ok() &&
	           paragraph_text(editor, 0) == "AXB\xE7\x9F\xA5",
	       "composition 確定後才成為單一 authority transaction");
	expect(editor.undo().is_ok() && paragraph_text(editor, 0) == "AXB",
	       "composition commit 是一個不與打字合併的 undo 單位");
}

void test_viewport_layout_does_not_scale_with_total_blocks() {
	FontRegistry fonts;
	if (!fonts.register_font(1, read_font()).is_ok()) return;
	SequentialIdGenerator ids(1'000);
	auto created = FlowEditor::create(fonts, ids, "");
	if (!created.is_ok()) return;
	auto& editor = *created.value();
	for (std::size_t index = 0; index < 200; ++index) {
		expect(editor.insert_paragraph_break(index + 1).is_ok(),
		       "large Flow fixture 尾端 split 成功");
	}
	expect(editor.layout_index().block_count() == 201,
	       "large Flow fixture 有 201 個 authority blocks");
	DisplayListPublisher publisher;
	expect(editor.publish_display(publisher, 400, 48, 0).is_ok(),
	       "小 viewport 可渲染大 Flow");
	const auto stats = editor.layout_cache_stats();
	expect(stats.misses < 30,
	       "viewport 只 layout 可見區與 overscan，miss 不隨 201 blocks 成長");
}

}  // namespace

int main() {
	test_edit_ime_undo_and_display_are_one_authority();
	test_viewport_layout_does_not_scale_with_total_blocks();
	return krepis_test::report("krepis.flow_editor");
}
