#include "krepis/text_analysis.hpp"

#include "krepis/utf8.hpp"

extern "C" {
#include <SheenBidi.h>
#include <graphemebreak.h>
#include <linebreak.h>
}

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace krepis {
namespace {

struct AlgorithmRelease {
	void operator()(SBAlgorithmRef value) const noexcept { SBAlgorithmRelease(value); }
};

struct ParagraphRelease {
	void operator()(SBParagraphRef value) const noexcept { SBParagraphRelease(value); }
};

struct LineRelease {
	void operator()(SBLineRef value) const noexcept { SBLineRelease(value); }
};

struct ScriptLocatorRelease {
	void operator()(SBScriptLocatorRef value) const noexcept { SBScriptLocatorRelease(value); }
};

using AlgorithmOwner = std::unique_ptr<std::remove_pointer_t<SBAlgorithmRef>, AlgorithmRelease>;
using ParagraphOwner = std::unique_ptr<std::remove_pointer_t<SBParagraphRef>, ParagraphRelease>;
using LineOwner = std::unique_ptr<std::remove_pointer_t<SBLineRef>, LineRelease>;
using ScriptLocatorOwner =
	std::unique_ptr<std::remove_pointer_t<SBScriptLocatorRef>, ScriptLocatorRelease>;

template <typename Owner, typename Pointer>
[[nodiscard]] Owner own_or_terminate(Pointer pointer) {
	if (pointer == nullptr) std::terminate();
	return Owner(pointer);
}

[[nodiscard]] SBLevel to_sheen_level(BaseDirection direction) noexcept {
	switch (direction) {
		case BaseDirection::auto_ltr: return SBLevelDefaultLTR;
		case BaseDirection::auto_rtl: return SBLevelDefaultRTL;
		case BaseDirection::force_ltr: return 0;
		case BaseDirection::force_rtl: return 1;
	}
	std::terminate();
}

[[nodiscard]] SBCodepointSequence make_sequence(std::string_view text) noexcept {
	return SBCodepointSequence{
		SBStringEncodingUTF8,
		const_cast<char*>(text.data()),
		text.size(),
	};
}

[[nodiscard]] bool is_ascii_space(unsigned char value) noexcept {
	return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
	       value == '\f' || value == '\v';
}

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) noexcept {
	return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool is_explicit_url_or_path(std::string_view token) noexcept {
	if (starts_with(token, "http://") || starts_with(token, "https://")) return true;
	if (starts_with(token, "\\\\")) return true;
	if (!token.empty() && token.front() == '/') return true;
	const auto drive = static_cast<unsigned char>(token.empty() ? 0 : token[0]);
	const bool is_ascii_letter =
		(drive >= static_cast<unsigned char>('A') && drive <= static_cast<unsigned char>('Z')) ||
		(drive >= static_cast<unsigned char>('a') && drive <= static_cast<unsigned char>('z'));
	return token.size() >= 3 && is_ascii_letter &&
	       token[1] == ':' && (token[2] == '\\' || token[2] == '/');
}

struct ByteRange {
	std::size_t begin;
	std::size_t end;
};

[[nodiscard]] std::vector<ByteRange> protected_tokens(std::string_view text) {
	std::vector<ByteRange> result;
	std::size_t cursor = 0;
	while (cursor < text.size()) {
		while (cursor < text.size() &&
		       is_ascii_space(static_cast<unsigned char>(text[cursor]))) {
			++cursor;
		}
		const auto begin = cursor;
		while (cursor < text.size() &&
		       !is_ascii_space(static_cast<unsigned char>(text[cursor]))) {
			++cursor;
		}
		if (begin < cursor && is_explicit_url_or_path(text.substr(begin, cursor - begin))) {
			result.push_back(ByteRange{begin, cursor});
		}
	}
	return result;
}

[[nodiscard]] bool is_inside_protected_token(
	std::size_t byte_offset,
	const std::vector<ByteRange>& ranges
) noexcept {
	return std::any_of(ranges.begin(), ranges.end(), [byte_offset](const auto& range) {
		return byte_offset > range.begin && byte_offset < range.end;
	});
}

void analyze_boundaries(
	std::string_view text,
	const std::string& language,
	TextAnalysis& result
) {
	result.grapheme_boundaries.push_back(0);
	if (text.empty()) return;

	std::vector<char> grapheme_breaks(text.size());
	set_graphemebreaks_utf8(
		reinterpret_cast<const utf8_t*>(text.data()),
		text.size(),
		language.empty() ? nullptr : language.c_str(),
		grapheme_breaks.data()
	);
	for (std::size_t index = 0; index < grapheme_breaks.size(); ++index) {
		if (grapheme_breaks[index] == GRAPHEMEBREAK_BREAK) {
			result.grapheme_boundaries.push_back(index + 1);
		}
	}

	const auto protected_ranges = protected_tokens(text);
	std::vector<char> line_breaks(text.size());
	set_linebreaks_utf8(
		reinterpret_cast<const utf8_t*>(text.data()),
		text.size(),
		language.empty() ? nullptr : language.c_str(),
		line_breaks.data()
	);
	for (std::size_t index = 0; index < line_breaks.size(); ++index) {
		BreakKind kind;
		if (line_breaks[index] == LINEBREAK_ALLOWBREAK) {
			kind = BreakKind::allowed;
		} else if (line_breaks[index] == LINEBREAK_MUSTBREAK) {
			kind = BreakKind::mandatory;
		} else {
			continue;
		}

		const auto byte_offset = index + 1;
		if (!is_inside_protected_token(byte_offset, protected_ranges)) {
			result.line_breaks.push_back(TextBreak{byte_offset, kind});
		}
	}
}

void analyze_bidi(
	std::string_view text,
	BaseDirection base_direction,
	const SBCodepointSequence& sequence,
	TextAnalysis& result
) {
	if (text.empty()) return;

	auto algorithm = own_or_terminate<AlgorithmOwner>(SBAlgorithmCreate(&sequence));
	std::size_t paragraph_offset = 0;
	while (paragraph_offset < text.size()) {
		SBUInteger paragraph_length = 0;
		SBAlgorithmGetParagraphBoundary(
			algorithm.get(),
			paragraph_offset,
			text.size() - paragraph_offset,
			&paragraph_length,
			nullptr
		);
		if (paragraph_length == 0) std::terminate();

		auto paragraph = own_or_terminate<ParagraphOwner>(SBAlgorithmCreateParagraph(
			algorithm.get(),
			paragraph_offset,
			paragraph_length,
			to_sheen_level(base_direction)
		));
		auto line = own_or_terminate<LineOwner>(SBParagraphCreateLine(
			paragraph.get(),
			paragraph_offset,
			paragraph_length
		));

		const auto run_count = SBLineGetRunCount(line.get());
		const auto* runs = SBLineGetRunsPtr(line.get());
		for (SBUInteger index = 0; index < run_count; ++index) {
			result.bidi_runs.push_back(BidiRun{
				runs[index].offset,
				runs[index].length,
				runs[index].level,
			});
		}

		paragraph_offset += paragraph_length;
	}
}

void analyze_scripts(
	std::string_view text,
	const SBCodepointSequence& sequence,
	TextAnalysis& result
) {
	if (text.empty()) return;

	auto locator = own_or_terminate<ScriptLocatorOwner>(SBScriptLocatorCreate());
	SBScriptLocatorLoadCodepoints(locator.get(), &sequence);
	while (SBScriptLocatorMoveNext(locator.get())) {
		const auto* agent = SBScriptLocatorGetAgent(locator.get());
		if (agent == nullptr) std::terminate();
		result.script_runs.push_back(ScriptRun{
			agent->offset,
			agent->length,
			SBScriptGetOpenTypeTag(agent->script),
		});
	}
}

}  // namespace

Result<TextAnalysis> analyze_text(
	std::string_view utf8,
	std::string_view language,
	BaseDirection base_direction
) {
	if (auto validation = validate_utf8(utf8); !validation.is_ok()) {
		return validation.error();
	}

	TextAnalysis result;
	const std::string language_storage(language);
	analyze_boundaries(utf8, language_storage, result);
	const auto sequence = make_sequence(utf8);
	analyze_bidi(utf8, base_direction, sequence, result);
	analyze_scripts(utf8, sequence, result);
	return result;
}

}  // namespace krepis
