#include "krepis/spatial_container.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

using krepis::BlockId;
using krepis::ObjectId;
using krepis::RectD;
using krepis::SpatialContainer;
using krepis::SpatialPlacement;
using krepis_test::expect;

namespace {

SpatialPlacement placement(std::uint64_t key, double x, double y, double width, double height) {
	return SpatialPlacement{
		key,
		BlockId{ObjectId{0, key}},
		RectD{x, y, width, height},
		width,
		height,
		0,
	};
}

void test_viewport_query_and_validation() {
	auto spatial = SpatialContainer::create({
		placement(1, 0, -100, 20, 20),
		placement(2, 10, 10, 30, 30),
		placement(3, 200, 20, 10, 10),
		placement(4, 20, 80, 20, 40),
	});
	expect(spatial.is_ok(), "合法 placements 可建立 SpatialContainer");
	if (!spatial.is_ok()) return;
	auto visible = spatial.value().query(RectD{0, 0, 100, 100});
	expect(visible.size() == 2, "viewport query 只回傳矩形相交 placements");
	expect(spatial.value().find(4) != nullptr, "placement key 可定位 stable placement");

	auto duplicate = SpatialContainer::create({
		placement(1, 0, 0, 10, 10),
		placement(1, 20, 20, 10, 10),
	});
	expect(!duplicate.is_ok(), "placement key 重複 fail closed");
	auto invalid = placement(8, 0, 0, 0, 10);
	expect(!SpatialContainer::create({invalid}).is_ok(), "零尺寸 frame fail closed");
}

void test_interval_query_matches_linear_reference() {
	std::mt19937_64 random(0x5350415449414CULL);
	std::uniform_real_distribution<double> coordinate(-10'000.0, 10'000.0);
	std::uniform_real_distribution<double> extent(1.0, 500.0);
	std::vector<SpatialPlacement> placements;
	placements.reserve(2'000);
	for (std::uint64_t key = 1; key <= 2'000; ++key) {
		placements.push_back(placement(
			key,
			coordinate(random),
			coordinate(random),
			extent(random),
			extent(random)
		));
	}
	auto spatial = SpatialContainer::create(placements);
	expect(spatial.is_ok(), "random interval fixture 可建立");
	if (!spatial.is_ok()) return;
	for (std::size_t sample = 0; sample < 200; ++sample) {
		const RectD viewport{
			coordinate(random),
			coordinate(random),
			extent(random) * 4,
			extent(random) * 4,
		};
		std::unordered_set<std::uint64_t> expected;
		for (const auto& candidate : placements) {
			if (candidate.frame.intersects(viewport)) expected.insert(candidate.placement_key);
		}
		auto actual = spatial.value().query(viewport);
		std::unordered_set<std::uint64_t> actual_keys;
		for (const auto& candidate : actual) actual_keys.insert(candidate.placement_key);
		expect(actual_keys == expected, "interval tree query 與線性 reference 完全一致");
	}
}

}  // namespace

int main() {
	test_viewport_query_and_validation();
	test_interval_query_matches_linear_reference();
	return krepis_test::report("krepis.spatial_container");
}
