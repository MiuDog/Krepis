#pragma once

// Flutter Canvas 沒有公開 glyph-ID 繪製 API；核心以 HarfBuzz 輸出可快取的字形輪廓。

#include "krepis/error.hpp"
#include "krepis/text_shaper.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace krepis {

enum class GlyphPathOpcode : std::uint32_t {
	move_to = 1,
	line_to = 2,
	quadratic_to = 3,
	cubic_to = 4,
	close_path = 5,
};

struct GlyphPathCommand {
	GlyphPathOpcode opcode;
	std::array<float, 6> values{};
};

using SharedGlyphPath = std::shared_ptr<const std::vector<GlyphPathCommand>>;

struct GlyphOutlineCacheStats {
	std::size_t hits = 0;
	std::size_t misses = 0;
	std::size_t entries = 0;
};

class GlyphOutlineCache {
public:
	explicit GlyphOutlineCache(const FontProvider& provider);
	~GlyphOutlineCache();

	GlyphOutlineCache(const GlyphOutlineCache&) = delete;
	GlyphOutlineCache& operator=(const GlyphOutlineCache&) = delete;

	[[nodiscard]] Result<SharedGlyphPath> outline(
		FontId font_id,
		std::uint32_t glyph_id,
		std::int32_t font_size_26_6
	);
	[[nodiscard]] GlyphOutlineCacheStats stats() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace krepis
