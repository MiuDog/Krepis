#pragma once

// TXT-0001 Phase 2A：把 Unicode 分析結果固定成不含平台 handle 的純值型別。

#include "krepis/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace krepis {

enum class BaseDirection : std::uint8_t {
	auto_ltr,
	auto_rtl,
	force_ltr,
	force_rtl,
};

enum class BreakKind : std::uint8_t {
	allowed,
	mandatory,
};

struct TextBreak {
	std::size_t byte_offset;
	BreakKind kind;
};

struct BidiRun {
	std::size_t byte_offset;
	std::size_t byte_length;
	std::uint8_t embedding_level;
};

struct ScriptRun {
	std::size_t byte_offset;
	std::size_t byte_length;
	std::uint32_t open_type_tag;
};

struct TextAnalysis {
	std::vector<std::size_t> grapheme_boundaries;
	std::vector<TextBreak> line_breaks;
	std::vector<BidiRun> bidi_runs;
	std::vector<ScriptRun> script_runs;
};

[[nodiscard]] Result<TextAnalysis> analyze_text(
	std::string_view utf8,
	std::string_view language,
	BaseDirection base_direction = BaseDirection::auto_ltr
);

// 對同一個已解析 paragraph 的實際行範圍套用 UAX #9 L1／L2。
// range 必須位於單一 bidi paragraph，且兩端都是 UTF-8 boundary。
[[nodiscard]] Result<std::vector<BidiRun>> analyze_bidi_line(
	std::string_view utf8,
	std::size_t byte_offset,
	std::size_t byte_length,
	BaseDirection base_direction = BaseDirection::auto_ltr
);

}  // namespace krepis
