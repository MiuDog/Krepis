#include "krepis/text_layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class BenchmarkFontProvider final : public krepis::FontProvider {
public:
	BenchmarkFontProvider() {
		std::ifstream stream(KREPIS_BENCH_LATIN_FONT, std::ios::binary);
		stream.seekg(0, std::ios::end);
		const auto size = stream.tellg();
		stream.seekg(0, std::ios::beg);
		bytes_.resize(static_cast<std::size_t>(size));
		stream.read(
			reinterpret_cast<char*>(bytes_.data()),
			static_cast<std::streamsize>(size)
		);
	}

	[[nodiscard]] std::uint64_t font_set_revision() const noexcept override { return 1; }
	[[nodiscard]] std::vector<krepis::FontId> candidates(
		std::uint32_t,
		std::string_view
	) const override {
		return {1};
	}
	[[nodiscard]] krepis::Result<krepis::FontDataView> open(
		krepis::FontId font_id
	) const override {
		if (font_id != 1 || bytes_.empty()) {
			return krepis::Error{krepis::ErrorCode::not_found, "benchmark font 不存在"};
		}
		return krepis::FontDataView{bytes_, 0};
	}

private:
	std::vector<std::byte> bytes_;
};

struct Measurement {
	std::size_t capacity;
	double hit_rate;
	double p99_ms;
	double max_ms;
};

Measurement measure(BenchmarkFontProvider& provider, std::size_t capacity) {
	krepis::CachedParagraphLayouter layouter(provider, capacity);
	std::vector<std::string> paragraphs;
	paragraphs.reserve(256);
	for (std::size_t index = 0; index < 256; ++index) {
		paragraphs.push_back(
			"Paragraph " + std::to_string(index) +
			" keeps a realistic editing line with words, numbers 2026, and punctuation."
		);
	}

	std::vector<double> durations;
	durations.reserve(4000);
	for (std::size_t operation = 0; operation < 4000; ++operation) {
		// 80% 操作落在最近 64 段；20% 模擬捲動掃過完整 256 段工作集。
		const auto paragraph_index = operation % 5 == 0
			? (operation / 5) % paragraphs.size()
			: operation % 64;
		const auto start = std::chrono::steady_clock::now();
		auto result = layouter.layout(
			paragraphs[paragraph_index],
			"en",
			krepis::BaseDirection::auto_ltr,
			paragraph_index + 1,
			0,
			0,
			16 * 64,
			480 * 64,
			20 * 64
		);
		const auto finish = std::chrono::steady_clock::now();
		if (!result.is_ok()) std::terminate();
		durations.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
	}

	std::sort(durations.begin(), durations.end());
	const auto stats = layouter.cache_stats();
	return Measurement{
		capacity,
		static_cast<double>(stats.hits) / static_cast<double>(stats.hits + stats.misses),
		durations[static_cast<std::size_t>(durations.size() * 0.99)],
		durations.back(),
	};
}

}  // namespace

int main() {
	BenchmarkFontProvider provider;
	std::cout << "capacity,hit_rate,p99_ms,max_ms\n";
	for (const auto capacity : {32u, 64u, 128u, 256u, 512u}) {
		const auto result = measure(provider, capacity);
		std::cout << result.capacity << ',' << result.hit_rate << ',' << result.p99_ms << ','
		          << result.max_ms << '\n';
	}
}
