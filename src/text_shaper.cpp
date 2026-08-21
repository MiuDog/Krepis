#include "krepis/text_shaper.hpp"

#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <climits>
#include <exception>
#include <string>
#include <unordered_map>
#include <utility>

namespace krepis {
namespace {

[[nodiscard]] std::uint32_t decode_scalar(
	std::string_view text,
	std::size_t& offset
) noexcept {
	const auto first = static_cast<unsigned char>(text[offset++]);
	if (first <= 0x7Fu) return first;
	if (first <= 0xDFu) {
		const auto second = static_cast<unsigned char>(text[offset++]);
		return ((first & 0x1Fu) << 6u) | (second & 0x3Fu);
	}
	if (first <= 0xEFu) {
		const auto second = static_cast<unsigned char>(text[offset++]);
		const auto third = static_cast<unsigned char>(text[offset++]);
		return ((first & 0x0Fu) << 12u) | ((second & 0x3Fu) << 6u) |
		       (third & 0x3Fu);
	}
	const auto second = static_cast<unsigned char>(text[offset++]);
	const auto third = static_cast<unsigned char>(text[offset++]);
	const auto fourth = static_cast<unsigned char>(text[offset++]);
	return ((first & 0x07u) << 18u) | ((second & 0x3Fu) << 12u) |
	       ((third & 0x3Fu) << 6u) | (fourth & 0x3Fu);
}

[[nodiscard]] bool does_not_require_nominal_glyph(std::uint32_t scalar) noexcept {
	return scalar == 0x200Cu || scalar == 0x200Du ||
	       (scalar >= 0xFE00u && scalar <= 0xFE0Fu) ||
	       (scalar >= 0xE0100u && scalar <= 0xE01EFu);
}

[[nodiscard]] const ScriptRun* find_script_run(
	const TextAnalysis& analysis,
	std::size_t byte_offset
) noexcept {
	const auto found = std::find_if(
		analysis.script_runs.begin(),
		analysis.script_runs.end(),
		[byte_offset](const auto& run) {
			return byte_offset >= run.byte_offset &&
			       byte_offset < run.byte_offset + run.byte_length;
		}
	);
	return found == analysis.script_runs.end() ? nullptr : &*found;
}

struct Segment {
	FontId font_id;
	std::size_t byte_offset;
	std::size_t byte_length;
	GlyphDirection direction;
	std::uint32_t script_tag;
};

}  // namespace

class TextShaper::Impl {
public:
	explicit Impl(const FontProvider& provider) : provider_(provider) {}

	~Impl() { clear_faces(); }

	[[nodiscard]] Result<ShapedParagraph> shape(
		std::string_view utf8,
		std::string_view language,
		BaseDirection base_direction,
		std::int32_t font_size_26_6
	) {
		if (font_size_26_6 <= 0) {
			return Error{ErrorCode::invalid_argument, "字型尺寸必須大於零"};
		}
		if (utf8.size() > static_cast<std::size_t>(INT_MAX)) {
			return Error{ErrorCode::invalid_argument, "單一段落超出 HarfBuzz 長度上限"};
		}
		refresh_revision();

		auto analysis_result = analyze_text(utf8, language, base_direction);
		if (!analysis_result.is_ok()) return analysis_result.error();

		ShapedParagraph result{std::move(analysis_result).take(), {}};
		if (utf8.empty()) return result;

		for (const auto& bidi : result.analysis.bidi_runs) {
			const auto direction = (bidi.embedding_level % 2u == 0u)
				? GlyphDirection::ltr
				: GlyphDirection::rtl;
			std::vector<Segment> segments;
			const auto bidi_limit = bidi.byte_offset + bidi.byte_length;

			for (std::size_t index = 0;
			     index + 1 < result.analysis.grapheme_boundaries.size();
			     ++index) {
				const auto begin = result.analysis.grapheme_boundaries[index];
				const auto end = result.analysis.grapheme_boundaries[index + 1];
				if (begin < bidi.byte_offset || end > bidi_limit) continue;

				const auto* script = find_script_run(result.analysis, begin);
				if (script == nullptr) std::terminate();
				auto font = select_font(utf8, begin, end, script->open_type_tag, language);
				if (!font.is_ok()) return font.error();

				if (!segments.empty() && segments.back().font_id == font.value() &&
				    segments.back().script_tag == script->open_type_tag &&
				    segments.back().byte_offset + segments.back().byte_length == begin) {
					segments.back().byte_length = end - segments.back().byte_offset;
				} else {
					segments.push_back(Segment{
						font.value(),
						begin,
						end - begin,
						direction,
						script->open_type_tag,
					});
				}
			}

			if (direction == GlyphDirection::rtl) {
				std::reverse(segments.begin(), segments.end());
			}
			for (const auto& segment : segments) {
				auto run = shape_segment(utf8, language, font_size_26_6, segment);
				if (!run.is_ok()) return run.error();
				result.glyph_runs.push_back(std::move(run).take());
			}
		}
		return result;
	}

private:
	struct Face {
		hb_blob_t* blob = nullptr;
		hb_face_t* face = nullptr;
		hb_font_t* font = nullptr;
		std::unordered_map<std::uint32_t, bool> coverage;

		~Face() {
			if (font != nullptr) hb_font_destroy(font);
			if (face != nullptr) hb_face_destroy(face);
			if (blob != nullptr) hb_blob_destroy(blob);
		}
	};

	void clear_faces() noexcept { faces_.clear(); }

	void refresh_revision() {
		const auto current = provider_.font_set_revision();
		if (has_revision_ && current == revision_) return;
		clear_faces();
		candidate_cache_.clear();
		revision_ = current;
		has_revision_ = true;
	}

	[[nodiscard]] Result<Face*> face(FontId font_id) {
		if (const auto found = faces_.find(font_id); found != faces_.end()) {
			return found->second.get();
		}

		auto opened = provider_.open(font_id);
		if (!opened.is_ok()) return opened.error();
		const auto data = opened.value();
		if (data.bytes.empty() || data.bytes.size() > UINT_MAX) {
			return Error{ErrorCode::invalid_argument, "字型 bytes 為空或過大"};
		}

		auto owner = std::make_unique<Face>();
		owner->blob = hb_blob_create(
			reinterpret_cast<const char*>(data.bytes.data()),
			static_cast<unsigned int>(data.bytes.size()),
			HB_MEMORY_MODE_READONLY,
			nullptr,
			nullptr
		);
		owner->face = hb_face_create(owner->blob, data.face_index);
		owner->font = hb_font_create(owner->face);
		if (owner->blob == hb_blob_get_empty() || owner->face == hb_face_get_empty() ||
		    owner->font == hb_font_get_empty()) {
			return Error{ErrorCode::invalid_argument, "無法建立 HarfBuzz 字型"};
		}
		hb_ot_font_set_funcs(owner->font);

		auto* result = owner.get();
		faces_.emplace(font_id, std::move(owner));
		return result;
	}

	[[nodiscard]] bool covers(
		Face& face_value,
		std::string_view text,
		std::size_t begin,
		std::size_t end
	) {
		std::size_t cursor = begin;
		while (cursor < end) {
			const auto scalar = decode_scalar(text, cursor);
			if (does_not_require_nominal_glyph(scalar)) continue;
			const auto cached = face_value.coverage.find(scalar);
			if (cached != face_value.coverage.end()) {
				if (!cached->second) return false;
				continue;
			}
			hb_codepoint_t glyph = 0;
			const bool present = hb_font_get_nominal_glyph(face_value.font, scalar, &glyph);
			face_value.coverage.emplace(scalar, present);
			if (!present) return false;
		}
		return true;
	}

	[[nodiscard]] const std::vector<FontId>& candidates(
		std::uint32_t script_tag,
		std::string_view language
	) {
		std::string key;
		key.append(reinterpret_cast<const char*>(&script_tag), sizeof(script_tag));
		key.push_back('\0');
		key.append(language);
		if (const auto found = candidate_cache_.find(key); found != candidate_cache_.end()) {
			return found->second;
		}
		auto inserted = candidate_cache_.emplace(
			std::move(key),
			provider_.candidates(script_tag, language)
		);
		return inserted.first->second;
	}

	[[nodiscard]] Result<FontId> select_font(
		std::string_view text,
		std::size_t begin,
		std::size_t end,
		std::uint32_t script_tag,
		std::string_view language
	) {
		for (const auto font_id : candidates(script_tag, language)) {
			auto opened = face(font_id);
			if (!opened.is_ok()) return opened.error();
			if (covers(*opened.value(), text, begin, end)) return font_id;
		}
		return Error{ErrorCode::missing_glyph, "所有候選字型都缺少 grapheme", begin};
	}

	[[nodiscard]] Result<GlyphRun> shape_segment(
		std::string_view text,
		std::string_view language,
		std::int32_t font_size_26_6,
		const Segment& segment
	) {
		auto opened = face(segment.font_id);
		if (!opened.is_ok()) return opened.error();
		hb_font_set_scale(opened.value()->font, font_size_26_6, font_size_26_6);

		hb_buffer_t* raw_buffer = hb_buffer_create();
		if (raw_buffer == hb_buffer_get_empty()) std::terminate();
		const auto destroy_buffer = [](hb_buffer_t* value) { hb_buffer_destroy(value); };
		std::unique_ptr<hb_buffer_t, decltype(destroy_buffer)> buffer(
			raw_buffer,
			destroy_buffer
		);
		hb_buffer_add_utf8(
			buffer.get(),
			text.data(),
			static_cast<int>(text.size()),
			static_cast<unsigned int>(segment.byte_offset),
			static_cast<int>(segment.byte_length)
		);
		hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
		hb_buffer_set_direction(
			buffer.get(),
			segment.direction == GlyphDirection::ltr ? HB_DIRECTION_LTR : HB_DIRECTION_RTL
		);
		hb_buffer_set_script(buffer.get(), hb_ot_tag_to_script(segment.script_tag));
		const auto hb_language = language.empty()
			? HB_LANGUAGE_INVALID
			: hb_language_from_string(language.data(), static_cast<int>(language.size()));
		hb_buffer_set_language(buffer.get(), hb_language);
		hb_shape(opened.value()->font, buffer.get(), nullptr, 0);

		unsigned int count = 0;
		const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
		const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), &count);
		GlyphRun result{
			segment.font_id,
			segment.byte_offset,
			segment.byte_length,
			segment.direction,
			segment.script_tag,
			{},
		};
		result.glyphs.reserve(count);
		for (unsigned int index = 0; index < count; ++index) {
			if (infos[index].codepoint == 0) {
				return Error{
					ErrorCode::missing_glyph,
					"HarfBuzz shaping 產生 .notdef",
					infos[index].cluster,
				};
			}
			result.glyphs.push_back(Glyph{
				infos[index].codepoint,
				infos[index].cluster,
				positions[index].x_advance,
				positions[index].y_advance,
				positions[index].x_offset,
				positions[index].y_offset,
			});
		}
		return result;
	}

	const FontProvider& provider_;
	std::uint64_t revision_ = 0;
	bool has_revision_ = false;
	std::unordered_map<FontId, std::unique_ptr<Face>> faces_;
	std::unordered_map<std::string, std::vector<FontId>> candidate_cache_;
};

TextShaper::TextShaper(const FontProvider& provider) : impl_(std::make_unique<Impl>(provider)) {}
TextShaper::~TextShaper() = default;
TextShaper::TextShaper(TextShaper&&) noexcept = default;
TextShaper& TextShaper::operator=(TextShaper&&) noexcept = default;

Result<ShapedParagraph> TextShaper::shape(
	std::string_view utf8,
	std::string_view language,
	BaseDirection base_direction,
	std::int32_t font_size_26_6
) {
	return impl_->shape(utf8, language, base_direction, font_size_26_6);
}

}  // namespace krepis
