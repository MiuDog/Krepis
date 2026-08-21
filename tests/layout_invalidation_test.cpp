#include "krepis/layout_invalidation.hpp"

#include "krepis/flow_layout_index.hpp"
#include "krepis/flow_sequence.hpp"
#include "krepis/paragraph_record.hpp"
#include "krepis/transaction.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <utility>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::DocumentRevision;
using krepis::ErrorCode;
using krepis::FlowLayoutIndex;
using krepis::FlowSequence;
using krepis::LayoutEntry;
using krepis::MeasurementStatus;
using krepis::ObjectId;
using krepis::ParagraphRecord;
using krepis::Transaction;
using krepis::apply_layout_invalidations;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

BlockId make_block(std::uint64_t value) {
	return BlockId{ObjectId{0, value}};
}

ContainerId make_container(std::uint64_t value) {
	return ContainerId{ObjectId{1, value}};
}

DocumentRevision make_document(ContainerId container, std::size_t count) {
	auto revision = DocumentRevision::initial();
	auto sequence = FlowSequence::empty();
	for (std::size_t i = 0; i < count; ++i) {
		auto record = ParagraphRecord::create(revision.snapshot_id().content_revision + 1, "text");
		revision = revision.with_new_object(make_block(i + 1), std::move(record).take());
		sequence = sequence.insert(sequence.block_count(), make_block(i + 1));
	}
	return revision.with_flow_root(container, std::move(sequence));
}

FlowLayoutIndex make_layout(std::size_t count, double height = 10.0) {
	auto layout = FlowLayoutIndex::empty();
	for (std::size_t i = 0; i < count; ++i) {
		layout = layout.insert(layout.block_count(), {make_block(i + 1), height});
	}
	return layout;
}

void test_transaction_invalidates_only_target_and_remeasure_restores_status() {
	const auto container = make_container(1);
	auto base = make_document(container, 3);
	auto layout = make_layout(3);

	Transaction transaction(base.snapshot_id().content_revision);
	transaction.replace_paragraph_text(make_block(2), "two lines");
	auto commit_result = transaction.commit(base);
	expect(commit_result.is_ok(), "整合測試 Transaction 成功");
	if (!commit_result.is_ok()) return;

	auto committed = std::move(commit_result).take();
	auto invalidated_result = apply_layout_invalidations(
		committed.revision,
		container,
		layout,
		committed.invalidations
	);
	expect(invalidated_result.is_ok(), "失效集合成功套用到對應 FlowLayoutIndex");
	if (!invalidated_result.is_ok()) return;

	auto invalidated = std::move(invalidated_result).take();
	expect(layout.at(1).status == MeasurementStatus::measured, "舊 index 保持 measured");
	expect(invalidated.at(0).status == MeasurementStatus::measured, "前一項不失效");
	expect(invalidated.at(1).status == MeasurementStatus::estimated, "目標項變為 estimated");
	expect(invalidated.at(1).measured_height == 10.0, "舊高度保留為 estimate");
	expect(invalidated.at(2).status == MeasurementStatus::measured, "後一項不失效");
	expect(invalidated.at(1).source_content_revision ==
	           committed.revision.snapshot_id().content_revision,
	       "estimated entry 綁定新 revision");

	auto measured = invalidated.update_extent(1, 25.0);
	expect(measured.at(1).status == MeasurementStatus::measured, "重測後恢復 measured");
	expect(measured.prefix_extent(2) == 35.0, "prefix extent 反映新高度");
	expect(measured.lower_bound_extent(20.0) == 1, "viewport 查詢反映新高度");
}

void test_mismatched_layout_and_stale_invalidation_fail_closed() {
	const auto container = make_container(1);
	auto base = make_document(container, 2);
	auto layout = FlowLayoutIndex::empty()
		.insert(0, {make_block(1), 10.0})
		.insert(1, {make_block(99), 10.0});

	Transaction transaction(base.snapshot_id().content_revision);
	transaction.replace_paragraph_text(make_block(2), "changed");
	auto committed = std::move(transaction.commit(base)).take();

	auto mismatch = apply_layout_invalidations(
		committed.revision,
		container,
		layout,
		committed.invalidations
	);
	expect(!mismatch.is_ok(), "LayoutIndex BlockId 漂移時拒絕");
	if (!mismatch.is_ok()) {
		expect(mismatch.error().code() == ErrorCode::invalid_state, "漂移回 invalid_state");
	}
	expect(layout.at(1).status == MeasurementStatus::measured, "拒絕不修改輸入 index");

	auto stale_invalidations = committed.invalidations;
	stale_invalidations[0].source_content_revision -= 1;
	auto stale = apply_layout_invalidations(
		committed.revision,
		container,
		make_layout(2),
		stale_invalidations
	);
	expect(!stale.is_ok(), "來源 revision 不符時拒絕");
}

void test_container_layout_filters_other_container_invalidations() {
	const auto container_a = make_container(1);
	const auto container_b = make_container(2);
	auto revision = DocumentRevision::initial();
	auto first = ParagraphRecord::create(1, "A");
	revision = revision.with_new_object(make_block(1), std::move(first).take());
	auto second = ParagraphRecord::create(2, "B");
	revision = revision.with_new_object(make_block(2), std::move(second).take());
	revision = revision.with_flow_root(container_a, FlowSequence::empty().insert(0, make_block(1)));
	revision = revision.with_flow_root(container_b, FlowSequence::empty().insert(0, make_block(2)));

	Transaction transaction(revision.snapshot_id().content_revision);
	transaction.replace_paragraph_text(make_block(1), "A1");
	transaction.replace_paragraph_text(make_block(2), "B1");
	auto committed = std::move(transaction.commit(revision)).take();

	auto result = apply_layout_invalidations(
		committed.revision,
		container_a,
		FlowLayoutIndex::empty().insert(0, {make_block(1), 10.0}),
		committed.invalidations
	);
	expect(result.is_ok(), "指定 Container 忽略其他 Container 的合法失效");
	if (result.is_ok()) {
		expect(result.value().at(0).status == MeasurementStatus::estimated,
		       "指定 Container 的目標仍正確失效");
	}
}

void test_large_index_updates_one_logical_entry() {
	constexpr std::size_t count = 50000;
	constexpr std::size_t target = 37421;
	auto layout = make_layout(count);
	auto invalidated = layout.invalidate_extent(target, 88);

	expect(layout.at(target).status == MeasurementStatus::measured, "大型舊 index 不變");
	expect(invalidated.at(target - 1).status == MeasurementStatus::measured, "大型前一項不變");
	expect(invalidated.at(target).status == MeasurementStatus::estimated, "大型目標項失效");
	expect(invalidated.at(target + 1).status == MeasurementStatus::measured, "大型後一項不變");
	expect(invalidated.total_extent() == layout.total_extent(), "保留 estimate 時總高度不跳動");
}

}  // namespace

int main() {
	test_transaction_invalidates_only_target_and_remeasure_restores_status();
	test_mismatched_layout_and_stale_invalidation_fail_closed();
	test_container_layout_filters_other_container_invalidations();
	test_large_index_updates_one_logical_entry();

	shutdown_default_reclamation_queue();
	return krepis_test::report("krepis.layout_invalidation");
}
