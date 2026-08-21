#pragma once

// BND-0001：little-endian TLV display list、checked validator 與顯式 lease 雙緩衝。

#include "krepis/error.hpp"
#include "krepis/text_shaper.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace krepis {

enum class DisplayOpcode : std::uint16_t {
	draw_rect = 1,
	draw_glyph_run = 2,
	push_clip = 3,
	pop_clip = 4,
	push_transform = 5,
	pop_transform = 6,
};

struct DisplayListSummary {
	std::uint64_t frame_token = 0;
	std::uint32_t command_count = 0;
	std::size_t byte_size = 0;
};

class DisplayListBuilder {
public:
	DisplayListBuilder();

	void reset();
	[[nodiscard]] Result<void> add_rect(
		float x,
		float y,
		float width,
		float height,
		std::uint32_t color_rgba
	);
	[[nodiscard]] Result<void> add_glyph_run(
		std::int32_t baseline_x_26_6,
		std::int32_t baseline_y_26_6,
		std::int32_t font_size_26_6,
		std::uint32_t color_rgba,
		FontId font_id,
		GlyphDirection direction,
		std::span<const Glyph> glyphs
	);
	[[nodiscard]] Result<void> push_clip(float x, float y, float width, float height);
	[[nodiscard]] Result<void> pop_clip();
	[[nodiscard]] Result<void> push_transform(const std::array<float, 6>& affine);
	[[nodiscard]] Result<void> pop_transform();

	[[nodiscard]] std::uint32_t command_count() const noexcept { return command_count_; }
	[[nodiscard]] std::size_t retained_capacity() const noexcept { return bytes_.capacity(); }

private:
	friend class DisplayListPublisher;

	[[nodiscard]] Result<std::size_t> append_command(
		DisplayOpcode opcode,
		std::size_t payload_size
	);
	void finalize(std::uint64_t frame_token);
	void compact_retained_capacity();

	std::vector<std::byte> bytes_;
	std::uint32_t command_count_ = 0;
};

[[nodiscard]] Result<DisplayListSummary> validate_display_list(
	std::span<const std::byte> bytes,
	std::uint16_t supported_major = 1,
	std::uint16_t supported_minor = 0
);

struct DisplayListLease {
	const std::byte* data = nullptr;
	std::size_t size = 0;
	std::uint64_t frame_token = 0;
	std::uint64_t lease_id = 0;
};

struct DisplayListPublisherStats {
	std::uint64_t published_frames = 0;
	std::uint64_t acquired_leases = 0;
	std::uint64_t released_leases = 0;
	std::size_t outstanding_leases = 0;
	std::uint64_t compacted_slots = 0;
};

class DisplayListPublisher {
public:
	~DisplayListPublisher() noexcept;

	[[nodiscard]] Result<DisplayListBuilder*> begin_frame();
	[[nodiscard]] Result<std::uint64_t> publish();
	[[nodiscard]] Result<DisplayListLease> acquire_front();
	[[nodiscard]] Result<void> release(std::uint64_t lease_id);

	[[nodiscard]] const DisplayListPublisherStats& stats() const noexcept { return stats_; }
	[[nodiscard]] bool has_front() const noexcept { return front_ != no_slot; }
	[[nodiscard]] std::size_t retained_capacity() const noexcept;

private:
	struct Slot {
		DisplayListBuilder builder;
		std::uint64_t frame_token = 0;
		std::size_t lease_count = 0;
		std::size_t underused_frames = 0;
	};

	static constexpr std::size_t no_slot = static_cast<std::size_t>(-1);

	std::array<Slot, 2> slots_;
	std::size_t front_ = no_slot;
	std::size_t building_ = no_slot;
	std::uint64_t next_frame_token_ = 1;
	std::uint64_t next_lease_id_ = 1;
	std::unordered_map<std::uint64_t, std::size_t> leases_;
	DisplayListPublisherStats stats_;
};

}  // namespace krepis
