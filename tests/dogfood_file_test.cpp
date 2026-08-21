#include "krepis/dogfood_file.hpp"

#include "krepis/embed_record.hpp"
#include "krepis/paragraph_record.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::DocumentRevision;
using krepis::ErrorCode;
using krepis::EmbedRecord;
using krepis::FlowSequence;
using krepis::FlowRangeTarget;
using krepis::ObjectId;
using krepis::ParagraphRecord;
using krepis::RectD;
using krepis::SpatialContainer;
using krepis::SpatialPlacement;
using krepis::SpatialViewportTarget;
using krepis_test::expect;

namespace {

BlockId block(std::uint64_t value) {
	return BlockId{ObjectId{1, value}};
}

ContainerId container(std::uint64_t value) {
	return ContainerId{ObjectId{2, value}};
}

DocumentRevision fixture(const char* first_text = "第一段") {
	auto revision = DocumentRevision::initial();
	auto first = ParagraphRecord::create(1, first_text);
	auto second = ParagraphRecord::create(2, "cafe\xCC\x81");
	expect(first.is_ok() && second.is_ok(), "dogfood fixture Paragraph 建立成功");
	revision = revision.with_new_object(block(10), std::move(first).take());
	revision = revision.with_new_object(block(20), std::move(second).take());
	auto sequence = FlowSequence::empty().insert(0, block(20)).insert(1, block(10));
	return revision.with_flow_root(container(7), std::move(sequence));
}

std::string text_of(const DocumentRevision& revision, BlockId id) {
	auto record = revision.record_for(id);
	const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
	return paragraph == nullptr ? std::string{} : paragraph->utf8();
}

void test_round_trip_preserves_ids_text_and_flow_order() {
	auto encoded = krepis::encode_dogfood_file(fixture());
	expect(encoded.is_ok(), "dogfood encode 成功");
	if (!encoded.is_ok()) return;
	auto decoded = krepis::decode_dogfood_file(encoded.value());
	expect(decoded.is_ok(), "dogfood decode 成功");
	if (!decoded.is_ok()) return;
	expect(text_of(decoded.value(), block(10)) == "第一段" &&
	           text_of(decoded.value(), block(20)) == "cafe\xCC\x81",
	       "round-trip 保存 UTF-8 與 stable BlockId");
	const auto* sequence = decoded.value().flow_root(container(7));
	expect(sequence != nullptr && sequence->block_count() == 2 &&
	           sequence->at(0) == block(20) && sequence->at(1) == block(10),
	       "round-trip 保存 ContainerId 與 Flow 順序");
}

void test_truncation_unknown_version_and_trailing_bytes_fail_closed() {
	auto encoded = krepis::encode_dogfood_file(fixture());
	if (!encoded.is_ok()) return;
	for (std::size_t size = 0; size < encoded.value().size(); ++size) {
		auto decoded = krepis::decode_dogfood_file(
			std::span<const std::byte>(encoded.value().data(), size)
		);
		expect(!decoded.is_ok(), "任意截斷位置都 fail closed");
	}
	auto unknown = encoded.value();
	unknown[8] = std::byte{2};
	auto version = krepis::decode_dogfood_file(unknown);
	expect(!version.is_ok() && version.error().code() == ErrorCode::version_mismatch,
	       "未知 major version 回 version_mismatch");
	auto trailing = encoded.value();
	trailing.push_back(std::byte{0});
	expect(!krepis::decode_dogfood_file(trailing).is_ok(), "尾端資料不被默默忽略");
	auto duplicate_flow_block = encoded.value();
	// v1.1 尾端是兩個 Flow BlockId（各 16 bytes）與 spatial_count（8 bytes）。
	const auto first_flow_id = duplicate_flow_block.end() - 40;
	const auto second_flow_id = duplicate_flow_block.end() - 24;
	std::copy_n(second_flow_id, 16, first_flow_id);
	expect(!krepis::decode_dogfood_file(duplicate_flow_block).is_ok(),
	       "同一 BlockId 不可在 Flow ownership 中出現兩次");
}

void test_atomic_save_replaces_existing_file() {
	const auto path = std::filesystem::temp_directory_path() /
	                  "krepis-dogfood-file-atomic-test.krp";
	std::error_code ignored;
	std::filesystem::remove(path, ignored);
	std::filesystem::remove(path.string() + ".krepis.tmp", ignored);
	expect(krepis::save_dogfood_file(path, fixture("old")).is_ok(), "首次原子存檔成功");
	expect(krepis::save_dogfood_file(path, fixture("new")).is_ok(), "既有檔案可原子替換");
	auto loaded = krepis::load_dogfood_file(path);
	expect(loaded.is_ok() && text_of(loaded.value(), block(10)) == "new",
	       "替換後只讀到完整新 revision");
	expect(!std::filesystem::exists(path.string() + ".krepis.tmp"),
	       "成功替換後不留下暫存檔");
	std::filesystem::remove(path, ignored);
}

void test_round_trip_preserves_embeds_and_spatial_placements() {
	auto revision = fixture();
	auto flow_embed = EmbedRecord::create(
		revision.snapshot_id().content_revision + 1,
		FlowRangeTarget{container(7), block(20), block(10)}
	);
	auto spatial_embed = EmbedRecord::create(
		revision.snapshot_id().content_revision + 2,
		SpatialViewportTarget{container(8), RectD{5, 6, 70, 80}}
	);
	expect(flow_embed.is_ok() && spatial_embed.is_ok(), "dogfood Embed fixture 建立成功");
	if (!flow_embed.is_ok() || !spatial_embed.is_ok()) return;
	revision = revision.with_new_object(block(30), std::move(flow_embed).take());
	revision = revision.with_new_object(block(40), std::move(spatial_embed).take());
	auto spatial = SpatialContainer::create({
		SpatialPlacement{91, block(30), RectD{10, 20, 300, 200}, 600, 400, 12},
		SpatialPlacement{92, block(40), RectD{400, 50, 100, 120}, 100, 120, 0},
	});
	expect(spatial.is_ok(), "dogfood Spatial fixture 建立成功");
	if (!spatial.is_ok()) return;
	revision = revision.with_spatial_root(container(8), std::move(spatial).take());

	auto encoded = krepis::encode_dogfood_file(revision);
	auto decoded = encoded.is_ok()
		? krepis::decode_dogfood_file(encoded.value())
		: krepis::Result<DocumentRevision>{encoded.error()};
	expect(decoded.is_ok(), "Embed 與 Spatial document round-trip 成功");
	if (!decoded.is_ok()) return;
	const auto* decoded_spatial = decoded.value().spatial_root(container(8));
	expect(decoded_spatial != nullptr && decoded_spatial->placement_count() == 2,
	       "round-trip 保存 SpatialContainer 與 placement 數量");
	const auto* first = decoded_spatial == nullptr ? nullptr : decoded_spatial->find(91);
	expect(first != nullptr && first->frame.width == 300 &&
	           first->source_width == 600 && first->vertical_scroll == 12,
	       "round-trip 保存 frame、來源比例與垂直 scroll");
	auto flow_record = decoded.value().record_for(block(30));
	const auto* decoded_flow_embed = dynamic_cast<const EmbedRecord*>(flow_record.get());
	expect(decoded_flow_embed != nullptr &&
	           std::get<FlowRangeTarget>(decoded_flow_embed->target()).anchor_a == block(20),
	       "round-trip 保存 FlowRangeEmbed stable anchors");
	auto viewport_record = decoded.value().record_for(block(40));
	const auto* decoded_viewport_embed = dynamic_cast<const EmbedRecord*>(viewport_record.get());
	expect(decoded_viewport_embed != nullptr &&
	           std::get<SpatialViewportTarget>(decoded_viewport_embed->target()).viewport.height == 80,
	       "round-trip 保存 SpatialViewportEmbed viewport");
	expect(decoded.value().references().referencing(container(7)).size() == 1 &&
	           decoded.value().references().referencing(container(8)).size() == 1,
	       "載入時由 Embed records 重建 reverse-reference index");
}

}  // namespace

int main() {
	test_round_trip_preserves_ids_text_and_flow_order();
	test_truncation_unknown_version_and_trailing_bytes_fail_closed();
	test_atomic_save_replaces_existing_file();
	test_round_trip_preserves_embeds_and_spatial_placements();
	return krepis_test::report("krepis.dogfood_file");
}
