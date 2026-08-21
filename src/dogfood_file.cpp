#include "krepis/dogfood_file.hpp"

#include "krepis/embed_record.hpp"
#include "krepis/object_slot.hpp"
#include "krepis/paragraph_record.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace krepis {
namespace {

constexpr std::array<std::byte, 8> magic{
	std::byte{'K'}, std::byte{'R'}, std::byte{'P'}, std::byte{'D'},
	std::byte{'O'}, std::byte{'G'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint16_t format_major = 1;
constexpr std::uint16_t format_minor = 1;
constexpr std::uint32_t disposable_flag = 1;
constexpr std::uint32_t paragraph_kind = 1;
constexpr std::uint32_t flow_range_embed_kind = 2;
constexpr std::uint32_t spatial_viewport_embed_kind = 3;
constexpr std::uint64_t maximum_objects = 10'000'000;
constexpr std::uint64_t maximum_containers = 1'000'000;
constexpr std::uint64_t maximum_text_bytes = 64 * 1024 * 1024;
constexpr std::uint64_t maximum_file_bytes = 1024ULL * 1024ULL * 1024ULL;

template <typename Integer>
void append_little(std::vector<std::byte>& out, Integer value) {
	static_assert(std::is_unsigned_v<Integer>);
	for (std::size_t i = 0; i < sizeof(Integer); ++i) {
		out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFF));
	}
}

void append_id(std::vector<std::byte>& out, const ObjectId& id) {
	std::array<std::byte, ObjectId::encoded_size> encoded{};
	encode_object_id(id, encoded);
	out.insert(out.end(), encoded.begin(), encoded.end());
}

void append_double(std::vector<std::byte>& out, double value) {
	static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == sizeof(std::uint64_t));
	append_little(out, std::bit_cast<std::uint64_t>(value));
}

class Reader {
public:
	explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

	template <typename Integer>
	[[nodiscard]] Result<Integer> little() {
		static_assert(std::is_unsigned_v<Integer>);
		if (remaining() < sizeof(Integer)) return truncated();
		Integer value = 0;
		for (std::size_t i = 0; i < sizeof(Integer); ++i) {
			value |= static_cast<Integer>(std::to_integer<unsigned int>(bytes_[offset_ + i]))
			         << (i * 8);
		}
		offset_ += sizeof(Integer);
		return value;
	}

	[[nodiscard]] Result<ObjectId> id() {
		if (remaining() < ObjectId::encoded_size) return truncated();
		std::array<std::byte, ObjectId::encoded_size> encoded{};
		std::memcpy(encoded.data(), bytes_.data() + offset_, encoded.size());
		offset_ += encoded.size();
		return decode_object_id(encoded);
	}

	[[nodiscard]] Result<double> floating() {
		auto bits = little<std::uint64_t>();
		if (!bits.is_ok()) return bits.error();
		return std::bit_cast<double>(bits.value());
	}

	[[nodiscard]] Result<std::string> text(std::uint64_t size) {
		if (size > maximum_text_bytes || size > remaining()) {
			return Error{ErrorCode::corrupt_data, "dogfood text 長度越界"};
		}
		std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_),
		                  static_cast<std::size_t>(size));
		offset_ += static_cast<std::size_t>(size);
		return value;
	}

	[[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

private:
	[[nodiscard]] static Error truncated() noexcept {
		return Error{ErrorCode::corrupt_data, "dogfood file 截斷"};
	}

	std::span<const std::byte> bytes_;
	std::size_t offset_ = 0;
};

Result<void> atomic_replace(
	const std::filesystem::path& target,
	std::span<const std::byte> bytes
) {
	if (target.empty()) return Error{ErrorCode::invalid_argument, "存檔路徑不得為空"};
	auto temporary = target;
	temporary += ".krepis.tmp";
#if defined(_WIN32)
	HANDLE file = CreateFileW(
		temporary.c_str(),
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);
	if (file == INVALID_HANDLE_VALUE) return Error{ErrorCode::io_failure, "無法建立暫存檔"};
	std::size_t offset = 0;
	bool ok = true;
	while (offset < bytes.size()) {
		const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
			bytes.size() - offset,
			std::numeric_limits<DWORD>::max()
		));
		DWORD written = 0;
		if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
			ok = false;
			break;
		}
		offset += written;
	}
	if (ok) ok = FlushFileBuffers(file) != 0;
	if (!CloseHandle(file)) ok = false;
	if (!ok) return Error{ErrorCode::io_failure, "寫入或 flush 暫存檔失敗"};
	if (!MoveFileExW(
		temporary.c_str(),
		target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
	)) {
		return Error{ErrorCode::io_failure, "原子替換存檔失敗"};
	}
#else
	const auto temporary_text = temporary.string();
	const int file = ::open(temporary_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (file < 0) return Error{ErrorCode::io_failure, "無法建立暫存檔"};
	std::size_t offset = 0;
	bool ok = true;
	while (offset < bytes.size()) {
		const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
		if (written <= 0) {
			if (errno == EINTR) continue;
			ok = false;
			break;
		}
		offset += static_cast<std::size_t>(written);
	}
	if (ok) ok = ::fsync(file) == 0;
	if (::close(file) != 0) ok = false;
	if (!ok) return Error{ErrorCode::io_failure, "寫入或 flush 暫存檔失敗"};
	const auto parent = target.parent_path().empty()
		? std::filesystem::path{"."}
		: target.parent_path();
	const int directory = ::open(parent.string().c_str(), O_RDONLY | O_DIRECTORY);
	if (directory < 0) return Error{ErrorCode::io_failure, "無法開啟存檔目錄"};
	if (::rename(temporary_text.c_str(), target.string().c_str()) != 0) {
		::close(directory);
		return Error{ErrorCode::io_failure, "原子替換存檔失敗"};
	}
	const bool directory_ok = ::fsync(directory) == 0;
	::close(directory);
	if (!directory_ok) return Error{ErrorCode::io_failure, "flush 存檔目錄失敗"};
#endif
	return {};
}

}  // namespace

Result<std::vector<std::byte>> encode_dogfood_file(const DocumentRevision& revision) {
	if (revision.directory().slot_count() > maximum_objects ||
	    revision.container_count() > maximum_containers ||
	    revision.spatial_container_count() > maximum_containers) {
		return Error{ErrorCode::out_of_range, "dogfood document 超過格式數量上限"};
	}
	std::vector<std::pair<ObjectId, IntrusivePtr<const ObjectRecord>>> records;
	records.reserve(revision.directory().slot_count());
	for (std::size_t index = 0; index < revision.directory().slot_count(); ++index) {
		const auto slot = ObjectSlot{static_cast<std::uint32_t>(index)};
		auto record = revision.store().get(slot);
		if (record == nullptr) continue;
		const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
		const auto* embed = dynamic_cast<const EmbedRecord*>(record.get());
		if (paragraph == nullptr && embed == nullptr) {
			return Error{ErrorCode::unsupported, "dogfood format 不支援此 ObjectRecord"};
		}
		if (paragraph != nullptr && paragraph->utf8().size() > maximum_text_bytes) {
			return Error{ErrorCode::out_of_range, "dogfood Paragraph 超過長度上限"};
		}
		const auto id = revision.directory().id_for(slot);
		if (id.is_nil()) {
			return Error{ErrorCode::invalid_state, "visible record 沒有 stable ObjectId"};
		}
		records.emplace_back(id, std::move(record));
	}

	std::vector<std::byte> out;
	out.reserve(32);
	out.insert(out.end(), magic.begin(), magic.end());
	append_little(out, format_major);
	append_little(out, format_minor);
	append_little(out, disposable_flag);
	append_little(out, static_cast<std::uint64_t>(records.size()));
	for (const auto& [id, record] : records) {
		const auto* paragraph = dynamic_cast<const ParagraphRecord*>(record.get());
		const auto* embed = dynamic_cast<const EmbedRecord*>(record.get());
		append_little(out, paragraph != nullptr
			? paragraph_kind
			: std::holds_alternative<FlowRangeTarget>(embed->target())
				? flow_range_embed_kind
				: spatial_viewport_embed_kind);
		append_id(out, id);
		if (paragraph != nullptr) {
			append_little(out, static_cast<std::uint64_t>(paragraph->utf8().size()));
			const auto* begin = reinterpret_cast<const std::byte*>(paragraph->utf8().data());
			out.insert(out.end(), begin, begin + paragraph->utf8().size());
		} else if (const auto* flow = std::get_if<FlowRangeTarget>(&embed->target())) {
			append_id(out, flow->source_flow.raw());
			append_id(out, flow->anchor_a.raw());
			append_id(out, flow->anchor_b.raw());
		} else {
			const auto& spatial = std::get<SpatialViewportTarget>(embed->target());
			append_id(out, spatial.source_spatial.raw());
			append_double(out, spatial.viewport.x);
			append_double(out, spatial.viewport.y);
			append_double(out, spatial.viewport.width);
			append_double(out, spatial.viewport.height);
		}
	}
	append_little(out, static_cast<std::uint64_t>(revision.container_count()));
	for (std::size_t i = 0; i < revision.container_count(); ++i) {
		const auto container = revision.container_id_at(i);
		const auto* sequence = revision.flow_root(container);
		append_id(out, container.raw());
		append_little(out, static_cast<std::uint64_t>(sequence->block_count()));
		for (std::size_t rank = 0; rank < sequence->block_count(); ++rank) {
			append_id(out, sequence->at(rank).raw());
		}
	}
	append_little(out, static_cast<std::uint64_t>(revision.spatial_container_count()));
	for (std::size_t i = 0; i < revision.spatial_container_count(); ++i) {
		const auto container = revision.spatial_container_id_at(i);
		const auto* spatial = revision.spatial_root(container);
		append_id(out, container.raw());
		append_little(out, static_cast<std::uint64_t>(spatial->placement_count()));
		for (std::size_t index = 0; index < spatial->placement_count(); ++index) {
			const auto& placement = spatial->placement_at(index);
			append_little(out, placement.placement_key);
			append_id(out, placement.child.raw());
			append_double(out, placement.frame.x);
			append_double(out, placement.frame.y);
			append_double(out, placement.frame.width);
			append_double(out, placement.frame.height);
			append_double(out, placement.source_width);
			append_double(out, placement.source_height);
			append_double(out, placement.vertical_scroll);
		}
	}
	if (out.size() > maximum_file_bytes) {
		return Error{ErrorCode::out_of_range, "dogfood encoded file 超過大小上限"};
	}
	return out;
}

Result<DocumentRevision> decode_dogfood_file(std::span<const std::byte> bytes) {
	if (bytes.size() > maximum_file_bytes || bytes.size() < magic.size()) {
		return Error{ErrorCode::corrupt_data, "dogfood file 大小不合法"};
	}
	if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
		return Error{ErrorCode::corrupt_data, "dogfood magic 不符"};
	}
	Reader reader(bytes.subspan(magic.size()));
	auto major = reader.little<std::uint16_t>();
	auto minor = reader.little<std::uint16_t>();
	auto flags = reader.little<std::uint32_t>();
	if (!major.is_ok() || !minor.is_ok() || !flags.is_ok()) {
		return Error{ErrorCode::corrupt_data, "dogfood header 截斷"};
	}
	if (major.value() != format_major || minor.value() != format_minor) {
		return Error{ErrorCode::version_mismatch, "dogfood format 版本不支援"};
	}
	if (flags.value() != disposable_flag) {
		return Error{ErrorCode::corrupt_data, "dogfood disposable flag 不符"};
	}
	auto object_count = reader.little<std::uint64_t>();
	if (!object_count.is_ok() || object_count.value() > maximum_objects) {
		return Error{ErrorCode::corrupt_data, "dogfood object count 不合法"};
	}
	auto revision = DocumentRevision::initial();
	for (std::uint64_t i = 0; i < object_count.value(); ++i) {
		auto kind = reader.little<std::uint32_t>();
		auto id = reader.id();
		if (!kind.is_ok() || !id.is_ok()) {
			return Error{ErrorCode::corrupt_data, "dogfood object header 截斷"};
		}
		if (id.value().is_nil()) {
			return Error{ErrorCode::corrupt_data, "dogfood object kind 或 ID 不合法"};
		}
		const BlockId block{id.value()};
		if (revision.record_for(block) != nullptr) {
			return Error{ErrorCode::corrupt_data, "dogfood BlockId 重複"};
		}
		IntrusivePtr<const ObjectRecord> record;
		if (kind.value() == paragraph_kind) {
			auto size = reader.little<std::uint64_t>();
			if (!size.is_ok()) return Error{ErrorCode::corrupt_data, "Paragraph 長度截斷"};
			auto text = reader.text(size.value());
			if (!text.is_ok()) return text.error();
			auto paragraph = ParagraphRecord::create(
				revision.snapshot_id().content_revision + 1,
				std::move(text).take()
			);
			if (!paragraph.is_ok()) return paragraph.error();
			record = std::move(paragraph).take();
		} else if (kind.value() == flow_range_embed_kind) {
			auto source = reader.id();
			auto anchor_a = reader.id();
			auto anchor_b = reader.id();
			if (!source.is_ok() || !anchor_a.is_ok() || !anchor_b.is_ok()) {
				return Error{ErrorCode::corrupt_data, "FlowRangeEmbed 截斷"};
			}
			auto embed = EmbedRecord::create(
				revision.snapshot_id().content_revision + 1,
				FlowRangeTarget{
					ContainerId{source.value()},
					BlockId{anchor_a.value()},
					BlockId{anchor_b.value()},
				}
			);
			if (!embed.is_ok()) return Error{ErrorCode::corrupt_data, "FlowRangeEmbed 不合法"};
			record = std::move(embed).take();
		} else if (kind.value() == spatial_viewport_embed_kind) {
			auto source = reader.id();
			auto x = reader.floating();
			auto y = reader.floating();
			auto width = reader.floating();
			auto height = reader.floating();
			if (!source.is_ok() || !x.is_ok() || !y.is_ok() ||
			    !width.is_ok() || !height.is_ok()) {
				return Error{ErrorCode::corrupt_data, "SpatialViewportEmbed 截斷"};
			}
			auto embed = EmbedRecord::create(
				revision.snapshot_id().content_revision + 1,
				SpatialViewportTarget{
					ContainerId{source.value()},
					RectD{x.value(), y.value(), width.value(), height.value()},
				}
			);
			if (!embed.is_ok()) {
				return Error{ErrorCode::corrupt_data, "SpatialViewportEmbed 不合法"};
			}
			record = std::move(embed).take();
		} else {
			return Error{ErrorCode::corrupt_data, "dogfood object kind 未知"};
		}
		revision = revision.with_new_object(block, std::move(record));
	}
	auto container_count = reader.little<std::uint64_t>();
	if (!container_count.is_ok() || container_count.value() > maximum_containers) {
		return Error{ErrorCode::corrupt_data, "dogfood container count 不合法"};
	}
	std::unordered_set<ObjectId> seen_containers;
	std::unordered_set<ObjectId> owned_blocks;
	for (std::uint64_t i = 0; i < container_count.value(); ++i) {
		auto id = reader.id();
		auto block_count = reader.little<std::uint64_t>();
		if (!id.is_ok() || !block_count.is_ok() || id.value().is_nil() ||
		    block_count.value() > maximum_objects) {
			return Error{ErrorCode::corrupt_data, "dogfood Container header 不合法"};
		}
		const ContainerId container{id.value()};
		if (!seen_containers.insert(id.value()).second) {
			return Error{ErrorCode::corrupt_data, "dogfood ContainerId 重複"};
		}
		auto sequence = FlowSequence::empty();
		for (std::uint64_t rank = 0; rank < block_count.value(); ++rank) {
			auto block_id = reader.id();
			if (!block_id.is_ok() || block_id.value().is_nil()) {
				return Error{ErrorCode::corrupt_data, "dogfood Flow BlockId 不合法"};
			}
			const BlockId block{block_id.value()};
			if (revision.record_for(block) == nullptr) {
				return Error{ErrorCode::corrupt_data, "dogfood Flow 引用不存在的 Block"};
			}
			if (!owned_blocks.insert(block_id.value()).second) {
				return Error{ErrorCode::corrupt_data, "dogfood Block 出現在多個 Flow 位置"};
			}
			sequence = sequence.insert(sequence.block_count(), block);
		}
		revision = revision.with_flow_root(container, std::move(sequence));
	}
	auto spatial_count = reader.little<std::uint64_t>();
	if (!spatial_count.is_ok() || spatial_count.value() > maximum_containers) {
		return Error{ErrorCode::corrupt_data, "dogfood spatial container count 不合法"};
	}
	for (std::uint64_t i = 0; i < spatial_count.value(); ++i) {
		auto id = reader.id();
		auto placement_count = reader.little<std::uint64_t>();
		if (!id.is_ok() || !placement_count.is_ok() || id.value().is_nil() ||
		    placement_count.value() > maximum_objects ||
		    !seen_containers.insert(id.value()).second) {
			return Error{ErrorCode::corrupt_data, "dogfood SpatialContainer header 不合法"};
		}
		std::vector<SpatialPlacement> placements;
		placements.reserve(static_cast<std::size_t>(placement_count.value()));
		for (std::uint64_t placement_index = 0;
		     placement_index < placement_count.value();
		     ++placement_index) {
			auto key = reader.little<std::uint64_t>();
			auto child_id = reader.id();
			auto x = reader.floating();
			auto y = reader.floating();
			auto width = reader.floating();
			auto height = reader.floating();
			auto source_width = reader.floating();
			auto source_height = reader.floating();
			auto scroll = reader.floating();
			if (!key.is_ok() || !child_id.is_ok() || !x.is_ok() || !y.is_ok() ||
			    !width.is_ok() || !height.is_ok() || !source_width.is_ok() ||
			    !source_height.is_ok() || !scroll.is_ok() || child_id.value().is_nil() ||
			    revision.record_for(BlockId{child_id.value()}) == nullptr ||
			    !owned_blocks.insert(child_id.value()).second) {
				return Error{ErrorCode::corrupt_data, "dogfood SpatialPlacement 不合法"};
			}
			placements.push_back(SpatialPlacement{
				key.value(),
				BlockId{child_id.value()},
				RectD{x.value(), y.value(), width.value(), height.value()},
				source_width.value(),
				source_height.value(),
				scroll.value(),
			});
		}
		auto spatial = SpatialContainer::create(std::move(placements));
		if (!spatial.is_ok()) {
			return Error{ErrorCode::corrupt_data, "dogfood SpatialContainer invariant 失敗"};
		}
		revision = revision.with_spatial_root(
			ContainerId{id.value()},
			std::move(spatial).take()
		);
	}
	if (reader.remaining() != 0 || !revision.validate().ok()) {
		return Error{ErrorCode::corrupt_data, "dogfood file 有尾端資料或索引不一致"};
	}
	return revision;
}

Result<void> save_dogfood_file(
	const std::filesystem::path& path,
	const DocumentRevision& revision
) {
	auto encoded = encode_dogfood_file(revision);
	if (!encoded.is_ok()) return encoded.error();
	return atomic_replace(path, encoded.value());
}

Result<DocumentRevision> load_dogfood_file(const std::filesystem::path& path) {
	std::error_code size_error;
	const auto size = std::filesystem::file_size(path, size_error);
	if (size_error) return Error{ErrorCode::io_failure, "無法取得 dogfood file 大小"};
	if (size > maximum_file_bytes) {
		return Error{ErrorCode::corrupt_data, "dogfood file 超過大小上限"};
	}
	std::ifstream input(path, std::ios::binary);
	if (!input) return Error{ErrorCode::io_failure, "無法開啟 dogfood file"};
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty()) {
		input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	if (!input || input.peek() != std::ifstream::traits_type::eof()) {
		return Error{ErrorCode::io_failure, "讀取 dogfood file 失敗"};
	}
	return decode_dogfood_file(bytes);
}

}  // namespace krepis
