#include "krepis/display_list.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

double percentile(std::vector<double> samples, double fraction) {
	std::sort(samples.begin(), samples.end());
	const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
	return samples[index];
}

}  // namespace

int main() {
	constexpr std::size_t warmup_frames = 200;
	constexpr std::size_t measured_frames = 4'000;
	constexpr std::size_t visible_paragraphs = 200;
	constexpr std::size_t glyphs_per_run = 20;
	std::array<krepis::Glyph, glyphs_per_run> glyphs{};
	for (std::size_t i = 0; i < glyphs.size(); ++i) {
		glyphs[i] = krepis::Glyph{
			static_cast<std::uint32_t>(i + 1),
			i,
			640,
			0,
			0,
			0,
		};
	}
	krepis::DisplayListPublisher publisher;
	std::vector<double> samples;
	samples.reserve(measured_frames);
	std::size_t frame_bytes = 0;
	for (std::size_t frame = 0; frame < warmup_frames + measured_frames; ++frame) {
		const auto started = std::chrono::steady_clock::now();
		auto builder_result = publisher.begin_frame();
		if (!builder_result.is_ok()) return 2;
		auto& builder = *builder_result.value();
		if (!builder.add_rect(0, 0, 1'200, 800, 0xFFF8F8F8).is_ok() ||
		    !builder.push_clip(0, 0, 1'200, 800).is_ok()) {
			return 3;
		}
		for (std::size_t paragraph = 0; paragraph < visible_paragraphs; ++paragraph) {
			if (!builder.add_glyph_run(
				1'024,
				static_cast<std::int32_t>((paragraph * 24 + 18) * 64),
				1'024,
				0xFF202020,
				1,
				krepis::GlyphDirection::ltr,
				glyphs
			).is_ok()) {
				return 4;
			}
		}
		if (!builder.pop_clip().is_ok() || !publisher.publish().is_ok()) return 5;
		auto lease = publisher.acquire_front();
		if (!lease.is_ok()) return 6;
		frame_bytes = lease.value().size;
		auto decoded = krepis::validate_display_list(
			std::span<const std::byte>(lease.value().data, lease.value().size)
		);
		if (!decoded.is_ok() || !publisher.release(lease.value().lease_id).is_ok()) return 7;
		const auto elapsed = std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - started
		).count();
		if (frame >= warmup_frames) samples.push_back(elapsed);
	}
	const auto stats = publisher.stats();
	std::cout << "frames=" << measured_frames
	          << " commands_per_frame=" << visible_paragraphs + 3
	          << " bytes_per_frame=" << frame_bytes
	          << " retained_bytes=" << publisher.retained_capacity()
	          << " p50_us=" << percentile(samples, 0.50)
	          << " p99_us=" << percentile(samples, 0.99)
	          << " max_us=" << *std::max_element(samples.begin(), samples.end())
	          << " acquired=" << stats.acquired_leases
	          << " released=" << stats.released_leases
	          << '\n';
	if (percentile(samples, 0.99) > 3'000 ||
	    stats.acquired_leases != stats.released_leases ||
	    stats.outstanding_leases != 0) {
		return 1;
	}
	return 0;
}
