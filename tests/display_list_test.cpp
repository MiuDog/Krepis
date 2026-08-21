#include "krepis/display_list.hpp"

#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using krepis::DisplayListBuilder;
using krepis::DisplayListPublisher;
using krepis::ErrorCode;
using krepis::Glyph;
using krepis::GlyphDirection;
using krepis_test::expect;

namespace {

void patch_u16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
	bytes[offset] = static_cast<std::byte>(value & 0xFF);
	bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFF);
}

void patch_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
	for (std::size_t i = 0; i < 4; ++i) {
		bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
	}
}

std::vector<std::byte> copy_front(DisplayListPublisher& publisher) {
	auto lease = publisher.acquire_front();
	expect(lease.is_ok(), "display test 可 acquire front");
	if (!lease.is_ok()) return {};
	std::vector<std::byte> bytes(lease.value().data, lease.value().data + lease.value().size);
	expect(publisher.release(lease.value().lease_id).is_ok(), "display test release front");
	return bytes;
}

void test_round_trip_all_opcodes_and_every_truncation() {
	DisplayListPublisher publisher;
	auto begun = publisher.begin_frame();
	expect(begun.is_ok(), "第一幀取得 back slot");
	if (!begun.is_ok()) return;
	auto& builder = *begun.value();
	expect(builder.add_rect(1, 2, 30, 40, 0xFF112233).is_ok(), "DrawRect encode 成功");
	expect(builder.push_clip(0, 0, 100, 100).is_ok(), "PushClip encode 成功");
	expect(builder.push_transform({1, 0, 0, 1, 12, 24}).is_ok(),
	       "PushTransform encode 成功");
	const std::array<Glyph, 2> glyphs{
		Glyph{12, 0, 640, 0, 0, 0},
		Glyph{13, 1, 620, 0, 2, 0},
	};
	expect(builder.add_glyph_run(
		64,
		128,
		1024,
		0xFF000000,
		7,
		GlyphDirection::ltr,
		glyphs
	).is_ok(), "DrawGlyphRun encode 成功");
	expect(builder.pop_transform().is_ok() && builder.pop_clip().is_ok(),
	       "兩種 stack pop encode 成功");
	auto token = publisher.publish();
	expect(token.is_ok() && token.value() == 1, "第一個完整 frame 發布 token 1");
	auto bytes = copy_front(publisher);
	auto valid = krepis::validate_display_list(bytes);
	expect(valid.is_ok() && valid.value().command_count == 6,
	       "validator 接受全部正式 opcodes");
	for (std::size_t size = 0; size < bytes.size(); ++size) {
		expect(!krepis::validate_display_list(
			std::span<const std::byte>(bytes.data(), size)
		).is_ok(), "display list 任意截斷位置都 fail closed");
	}
}

void test_malformed_header_command_padding_and_stack_fail_closed() {
	DisplayListPublisher publisher;
	auto begun = publisher.begin_frame();
	if (!begun.is_ok()) return;
	auto& builder = *begun.value();
	expect(builder.add_rect(1, 2, 3, 4, 0xFFFFFFFF).is_ok(),
	       "malformed fixture DrawRect encode 成功");
	if (!publisher.publish().is_ok()) return;
	auto valid = copy_front(publisher);

	auto unknown_version = valid;
	patch_u16(unknown_version, 4, 2);
	auto version = krepis::validate_display_list(unknown_version);
	expect(!version.is_ok() && version.error().code() == ErrorCode::version_mismatch,
	       "未知 major version 回 version_mismatch");
	auto newer_minor = valid;
	patch_u16(newer_minor, 6, 1);
	expect(!krepis::validate_display_list(newer_minor, 1, 0).is_ok(),
	       "producer minor 高於 decoder 時回 version_mismatch");
	expect(krepis::validate_display_list(newer_minor, 1, 1).is_ok() &&
	           krepis::validate_display_list(valid, 1, 1).is_ok(),
	       "相同或較舊 producer minor 可由較新 decoder 讀取");
	auto flags = valid;
	patch_u32(flags, 20, 1);
	expect(!krepis::validate_display_list(flags).is_ok(), "未知 frame flags 整幀拒絕");
	auto command_flags = valid;
	patch_u16(command_flags, 34, 1);
	expect(!krepis::validate_display_list(command_flags).is_ok(),
	       "未知 command flags 整幀拒絕");
	auto frame_size = valid;
	patch_u32(frame_size, 12, static_cast<std::uint32_t>(valid.size() - 1));
	expect(!krepis::validate_display_list(frame_size).is_ok(),
	       "frame byte_size 必須與 span 完全一致");
	auto command_count = valid;
	patch_u32(command_count, 16, 2);
	expect(!krepis::validate_display_list(command_count).is_ok(),
	       "command_count 不可與實際 stream 分岔");
	auto opcode = valid;
	patch_u16(opcode, 32, 999);
	expect(!krepis::validate_display_list(opcode).is_ok(), "未知 opcode 整幀拒絕");
	auto size = valid;
	patch_u32(size, 36, 0xFFFFFFF8);
	expect(!krepis::validate_display_list(size).is_ok(), "溢出型 command size 整幀拒絕");
	auto padding = valid;
	padding[63] = std::byte{1};
	expect(!krepis::validate_display_list(padding).is_ok(), "非零 padding 整幀拒絕");

	auto unbalanced = publisher.begin_frame();
	if (!unbalanced.is_ok()) return;
	expect(unbalanced.value()->pop_clip().is_ok(),
	       "builder 允許先表達 command，再由整幀 gate 驗證");
	auto prior_stats = publisher.stats();
	expect(!publisher.publish().is_ok(), "stack underflow 不可 publish");
	expect(publisher.stats().published_frames == prior_stats.published_frames,
	       "publish 失敗時 front 與 frame counter 不變");
	auto preserved = publisher.acquire_front();
	expect(preserved.is_ok() && preserved.value().frame_token == 1,
	       "publish 失敗後仍可取得上一個完整 front");
	if (preserved.is_ok()) {
		expect(publisher.release(preserved.value().lease_id).is_ok(),
		       "釋放 preserved front lease");
	}
}

void test_explicit_leases_prevent_overwrite_and_balance_release() {
	DisplayListPublisher publisher;
	auto first_builder = publisher.begin_frame();
	if (!first_builder.is_ok()) return;
	expect(first_builder.value()->add_rect(0, 0, 10, 10, 1).is_ok(),
	       "第一幀 DrawRect encode 成功");
	if (!publisher.publish().is_ok()) return;
	auto first = publisher.acquire_front();
	expect(first.is_ok(), "第一個 frame lease 成功");
	if (!first.is_ok()) return;
	const auto first_magic = first.value().data[0];

	auto second_builder = publisher.begin_frame();
	if (!second_builder.is_ok()) return;
	expect(second_builder.value()->add_rect(0, 0, 20, 20, 2).is_ok(),
	       "第二幀 DrawRect encode 成功");
	expect(publisher.publish().is_ok(), "舊 front leased 時仍可使用第二個 slot");
	auto second = publisher.acquire_front();
	expect(second.is_ok(), "第二個 frame lease 成功");
	if (!second.is_ok()) return;
	expect(first.value().data[0] == first_magic,
	       "發布第二幀不覆寫尚未 release 的第一幀 pointer");
	expect(!publisher.begin_frame().is_ok(),
	       "兩個 slot 都被 lease 時第三幀無可寫 back slot");
	expect(publisher.release(first.value().lease_id).is_ok(), "release 第一個 lease");
	auto third_builder = publisher.begin_frame();
	if (!third_builder.is_ok()) return;
	expect(third_builder.value()->add_rect(0, 0, 30, 30, 3).is_ok(),
	       "第三幀 DrawRect encode 成功");
	expect(publisher.publish().is_ok(), "釋放舊 slot 後可發布第三幀");
	expect(!publisher.release(first.value().lease_id).is_ok(), "double release 被拒絕");
	expect(publisher.release(second.value().lease_id).is_ok(), "release 第二個 lease");
	const auto stats = publisher.stats();
	expect(stats.acquired_leases == stats.released_leases && stats.outstanding_leases == 0,
	       "配置數等於釋放數，沒有未結清 display lease");
}

}  // namespace

int main() {
	test_round_trip_all_opcodes_and_every_truncation();
	test_malformed_header_command_padding_and_stack_fail_closed();
	test_explicit_leases_prevent_overwrite_and_balance_release();
	return krepis_test::report("krepis.display_list");
}
