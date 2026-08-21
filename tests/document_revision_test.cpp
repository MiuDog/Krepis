#include "krepis/document_revision.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "test_support.hpp"

#include <string>

using krepis::BlockId;
using krepis::ContainerId;
using krepis::DocumentRevision;
using krepis::ErrorCode;
using krepis::FlowSequence;
using krepis::FlowSequenceConfig;
using krepis::IdDirectory;
using krepis::IntrusivePtr;
using krepis::ObjectId;
using krepis::ObjectRecord;
using krepis::ObjectSlot;
using krepis::ObjectStoreSnapshot;
using krepis::RevisionValidation;
using krepis::SnapshotId;
using krepis::make_intrusive;
using krepis::shutdown_default_reclamation_queue;
using krepis_test::expect;

namespace {

// 測試用的具體記錄型別。**真正的 schema 屬於 DOC 決策，不在儲存層。**
class TextRecord final : public ObjectRecord {
public:
    TextRecord(std::uint64_t revision, std::string text) noexcept
        : ObjectRecord(revision), text_(std::move(text)) {}

    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    std::string text_;
};

BlockId make_block(std::uint64_t n) {
    return BlockId{ObjectId{0, n}};
}

ContainerId make_container(std::uint64_t n) {
    return ContainerId{ObjectId{0, n}};
}

// --- IdDirectory ---

void test_directory_allocates_sequential_slots() {
    auto dir = IdDirectory::empty();
    expect(dir->slot_count() == 0, "空 directory 沒有 slot");

    auto r1 = dir->allocate(make_block(1).raw());
    auto r2 = r1.directory->allocate(make_block(2).raw());

    expect(r1.slot.value == 0, "第一個 slot 為 0");
    expect(r2.slot.value == 1, "第二個 slot 為 1");
    expect(r2.directory->slot_count() == 2, "配置兩個後 slot_count == 2");
}

void test_directory_allocate_is_idempotent() {
    auto dir = IdDirectory::empty();
    auto r1 = dir->allocate(make_block(1).raw());
    auto r2 = r1.directory->allocate(make_block(1).raw());

    expect(r1.slot == r2.slot, "重複配置同一 ObjectId 回傳相同 slot");
    expect(r2.directory->slot_count() == 1, "重複配置不增加 slot 數");
}

void test_directory_reverse_lookup() {
    auto dir = IdDirectory::empty();
    auto r = dir->allocate(make_block(42).raw());

    expect(r.directory->id_for(r.slot) == make_block(42).raw(), "反向解析正確");
    expect(r.directory->id_for(ObjectSlot{999}).is_nil(), "超出範圍回傳 nil");
}

void test_directory_cow() {
    auto dir = IdDirectory::empty();
    auto r1 = dir->allocate(make_block(1).raw());
    auto r2 = r1.directory->allocate(make_block(2).raw());

    expect(r1.directory->slot_count() == 1, "舊 directory 不受後續配置影響");
    expect(!r1.directory->resolve(make_block(2).raw()).is_valid(),
           "舊 directory 看不到後來配置的 ObjectId");
}

// --- ObjectStoreSnapshot ---

void test_store_write_and_read() {
    auto store = ObjectStoreSnapshot::empty();
    auto record = make_intrusive<TextRecord>(1, std::string("hello"));

    auto store2 = store.with_record(ObjectSlot{5}, record);

    expect(!store.contains(ObjectSlot{5}), "原 store 不變");
    expect(store2.contains(ObjectSlot{5}), "新 store 有記錄");

    auto fetched = store2.get(ObjectSlot{5});
    expect(fetched != nullptr, "可取回記錄");
    expect(static_cast<const TextRecord*>(fetched.get())->text() == "hello", "記錄內容正確");
}

void test_store_tombstone_differs_from_absent() {
    auto store = ObjectStoreSnapshot::empty()
                     .with_record(ObjectSlot{3}, make_intrusive<TextRecord>(1, std::string("x")));

    auto deleted = store.with_tombstone(ObjectSlot{3});

    expect(deleted.get(ObjectSlot{3}) == nullptr, "tombstone 後讀不到記錄");
    expect(deleted.is_tombstoned(ObjectSlot{3}), "tombstone 標記存在");
    expect(!deleted.is_tombstoned(ObjectSlot{4}), "未配置的 slot 不是 tombstone");
    expect(store.contains(ObjectSlot{3}), "舊 snapshot 仍讀得到舊記錄（D10）");
}

void test_store_cow_shares_untouched_pages() {
    auto store = ObjectStoreSnapshot::empty();
    // 兩個 slot 落在不同 page。
    const ObjectSlot page0{1};
    const ObjectSlot page2{2 * 64 + 7};

    store = store.with_record(page0, make_intrusive<TextRecord>(1, std::string("a")));
    store = store.with_record(page2, make_intrusive<TextRecord>(1, std::string("b")));

    auto updated = store.with_record(page0, make_intrusive<TextRecord>(2, std::string("a2")));

    expect(static_cast<const TextRecord*>(store.get(page0).get())->text() == "a",
           "舊 snapshot 的 page 0 不變");
    expect(static_cast<const TextRecord*>(updated.get(page0).get())->text() == "a2",
           "新 snapshot 的 page 0 已更新");
    expect(static_cast<const TextRecord*>(updated.get(page2).get())->text() == "b",
           "未改動的 page 2 內容一致");
}

// 閘門 7／E1 的同型問題：record page table 也必須在 slot 超出深度時長高，
// 且更新只複製短路徑。fanout=64、page_capacity=64 → 深度 1 只容納 4,096 個 slot。
void test_record_page_table_grows_and_shares() {
    auto store = ObjectStoreSnapshot::empty();

    const ObjectSlot shallow{4095};    // 深度 1 邊界
    const ObjectSlot deep{10000};      // 需要深度 2
    const ObjectSlot deeper{300000};   // 需要深度 3

    store = store.with_record(shallow, make_intrusive<TextRecord>(1, std::string("s")));
    store = store.with_record(deep, make_intrusive<TextRecord>(1, std::string("d")));
    store = store.with_record(deeper, make_intrusive<TextRecord>(1, std::string("x")));

    expect(static_cast<const TextRecord*>(store.get(shallow).get())->text() == "s",
           "長高兩次後最舊的 slot 仍可讀");
    expect(static_cast<const TextRecord*>(store.get(deep).get())->text() == "d",
           "中間 slot 仍可讀");
    expect(static_cast<const TextRecord*>(store.get(deeper).get())->text() == "x",
           "最深 slot 正確");
    expect(store.get(ObjectSlot{deep.value + 1}) == nullptr, "未配置的鄰近 slot 為 null");
    expect(store.capacity() > deeper.value, "capacity 涵蓋最深的 slot");

    // 短路徑 COW：更新一個 slot 不得影響舊版本，也不得動到其他 slot。
    auto updated = store.with_record(deep, make_intrusive<TextRecord>(2, std::string("d2")));
    expect(static_cast<const TextRecord*>(store.get(deep).get())->text() == "d",
           "舊 snapshot 不受影響");
    expect(static_cast<const TextRecord*>(updated.get(deep).get())->text() == "d2",
           "新 snapshot 已更新");
    expect(static_cast<const TextRecord*>(updated.get(shallow).get())->text() == "s",
           "未改動的 slot 在新 snapshot 中一致");
    expect(static_cast<const TextRecord*>(updated.get(deeper).get())->text() == "x",
           "另一棵子樹完全共享");
}

// --- DocumentRevision ---

void test_initial_revision_is_empty() {
    auto rev = DocumentRevision::initial();

    expect(rev.snapshot_id() == SnapshotId{0, 0}, "初始 SnapshotId 為 {0,0}");
    expect(rev.container_count() == 0, "初始沒有 container");
    expect(rev.validate().ok(), "空 revision 通過驗證");
}

void test_new_object_advances_content_revision() {
    auto rev = DocumentRevision::initial();
    auto rev2 = rev.with_new_object(make_block(1), make_intrusive<TextRecord>(1, std::string("a")));

    expect(rev.snapshot_id().content_revision == 0, "原 revision 不變");
    expect(rev2.snapshot_id().content_revision == 1, "content_revision 遞增");
    expect(rev2.snapshot_id().storage_generation == 0, "storage_generation 不變");

    auto record = rev2.record_for(make_block(1));
    expect(record != nullptr, "可取回新建物件");
    expect(static_cast<const TextRecord*>(record.get())->text() == "a", "內容正確");
}

void test_updated_record_keeps_slot() {
    auto rev = DocumentRevision::initial()
                   .with_new_object(make_block(1), make_intrusive<TextRecord>(1, std::string("a")));
    const auto slot_before = rev.resolve(make_block(1));

    auto rev2 = rev.with_updated_record(make_block(1),
                                        make_intrusive<TextRecord>(2, std::string("b")));

    expect(rev2.resolve(make_block(1)) == slot_before, "修改內容不改變 slot");
    expect(static_cast<const TextRecord*>(rev2.record_for(make_block(1)).get())->text() == "b",
           "新 revision 內容已更新");
    expect(static_cast<const TextRecord*>(rev.record_for(make_block(1)).get())->text() == "a",
           "舊 revision 內容不變");
}

void test_delete_writes_tombstone_and_keeps_slot() {
    auto rev = DocumentRevision::initial()
                   .with_new_object(make_block(1), make_intrusive<TextRecord>(1, std::string("a")));
    const auto slot = rev.resolve(make_block(1));

    auto deleted = rev.with_deleted_object(make_block(1));

    expect(deleted.record_for(make_block(1)) == nullptr, "刪除後讀不到記錄");
    expect(deleted.resolve(make_block(1)) == slot, "slot 不因刪除而回收（D10）");
    expect(deleted.store().is_tombstoned(slot), "寫入 tombstone");
    expect(rev.record_for(make_block(1)) != nullptr, "舊 revision 仍可讀");
}

void test_storage_rebuild_only_bumps_generation() {
    auto rev = DocumentRevision::initial()
                   .with_new_object(make_block(1), make_intrusive<TextRecord>(1, std::string("a")));
    const auto before = rev.snapshot_id();

    auto rebuilt = rev.with_storage_rebuild();

    expect(rebuilt.snapshot_id().content_revision == before.content_revision,
           "compact 不改變 content_revision");
    expect(rebuilt.snapshot_id().storage_generation == before.storage_generation + 1,
           "compact 遞增 storage_generation");
    expect(!before.handles_valid_for(rebuilt.snapshot_id()),
           "compact 後舊 handle 失效（D18）");
}

void test_flow_root_populates_location_index() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 10; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    const auto container = make_container(100);
    auto rev = DocumentRevision::initial().with_flow_root(container, seq);

    expect(rev.container_count() == 1, "新增一個 container");
    expect(rev.flow_root(container) != nullptr, "可取回 FlowSequence");
    expect(rev.flow_root(container)->block_count() == 10, "FlowSequence 內容正確");
    expect(rev.flow_root(make_container(999)) == nullptr, "不存在的 container 回傳 null");

    // 每個 Block 都應該有 LocationIndex entry。
    bool all_located = true;
    for (std::size_t i = 0; i < 10; ++i) {
        const auto slot = rev.resolve(make_block(i + 1));
        if (!slot.is_valid()) {
            all_located = false;
            break;
        }
        const auto entry = rev.locations().lookup(slot);
        if (!entry.is_flow() || entry.owner != container) {
            all_located = false;
            break;
        }
    }
    expect(all_located, "所有 Block 都有正確的 LocationIndex entry");
}

// D12：FlowSequence 與 LocationIndex 一致時通過驗證。
void test_validate_passes_on_consistent_revision() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;

    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < 20; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto rev = DocumentRevision::initial().with_flow_root(make_container(1), seq);
    const auto result = rev.validate();

    expect(result.ok(), "一致的 revision 通過 D12 驗證");
}

// D12：兩個 Container 各自的 Block 都必須正確歸屬。
void test_validate_with_multiple_containers() {
    auto seq_a = FlowSequence::empty();
    for (std::size_t i = 0; i < 5; ++i) {
        seq_a = seq_a.insert(i, make_block(i + 1));
    }
    auto seq_b = FlowSequence::empty();
    for (std::size_t i = 0; i < 5; ++i) {
        seq_b = seq_b.insert(i, make_block(i + 100));
    }

    auto rev = DocumentRevision::initial()
                   .with_flow_root(make_container(1), seq_a)
                   .with_flow_root(make_container(2), seq_b);

    expect(rev.container_count() == 2, "兩個 container");
    expect(rev.validate().ok(), "兩個 container 各自一致時通過驗證");
}

// D12：同一個 Block 出現在兩個 Container 必須被攔截。
void test_validate_rejects_block_in_two_containers() {
    auto seq_a = FlowSequence::empty();
    for (std::size_t i = 0; i < 5; ++i) {
        seq_a = seq_a.insert(i, make_block(i + 1));
    }
    // seq_b 刻意包含 seq_a 已擁有的 block 3。
    auto seq_b = FlowSequence::empty()
                     .insert(0, make_block(3))
                     .insert(1, make_block(200));

    // 先設 A 再設 B：B 的 with_flow_root 會把 block 3 的 owner 改寫成 container 2，
    // 於是 container 1 的 FlowSequence 與 LocationIndex 不再一致。
    auto rev = DocumentRevision::initial()
                   .with_flow_root(make_container(1), seq_a)
                   .with_flow_root(make_container(2), seq_b);

    const auto result = rev.validate();
    expect(!result.ok(), "同一 Block 歸屬兩個 Container 必須驗證失敗");
    expect(result.failure == RevisionValidation::Failure::owner_mismatch,
           "失敗原因為 owner_mismatch");
    expect(result.offending_block == make_block(3), "指出出問題的 Block");
}

// D12：LocationIndex 缺 entry 必須被攔截。
void test_validate_rejects_missing_location_entry() {
    auto seq = FlowSequence::empty();
    for (std::size_t i = 0; i < 5; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto rev = DocumentRevision::initial().with_flow_root(make_container(1), seq);
    expect(rev.validate().ok(), "前置條件：原本是一致的");

    // 刪除其中一個 Block 的位置資訊，模擬交易漏更新。
    const auto slot = rev.resolve(make_block(3));
    auto broken = rev.with_deleted_object(make_block(3));

    const auto result = broken.validate();
    expect(!result.ok(), "缺少 LocationIndex entry 必須驗證失敗");
    expect(result.failure == RevisionValidation::Failure::missing_location_entry,
           "失敗原因為 missing_location_entry");
    expect(slot.is_valid(), "前置：slot 原本有效");
}

// D8：revision 必須同時涵蓋內容與順序，不得出現混合狀態。
void test_revision_bundles_content_and_order_atomically() {
    auto seq = FlowSequence::empty();
    for (std::size_t i = 0; i < 5; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }

    auto rev1 = DocumentRevision::initial()
                    .with_flow_root(make_container(1), seq)
                    .with_updated_record(make_block(1),
                                         make_intrusive<TextRecord>(1, std::string("v1")));

    // 同時改順序與內容。
    auto seq2 = seq.insert(0, make_block(999));
    auto rev2 = rev1.with_flow_root(make_container(1), seq2)
                    .with_updated_record(make_block(1),
                                         make_intrusive<TextRecord>(2, std::string("v2")));

    // 舊 revision 必須完整維持舊狀態：舊順序 ＋ 舊內容。
    expect(rev1.flow_root(make_container(1))->block_count() == 5, "舊 revision 保有舊順序");
    expect(static_cast<const TextRecord*>(rev1.record_for(make_block(1)).get())->text() == "v1",
           "舊 revision 保有舊內容");

    // 新 revision 必須完整呈現新狀態。
    expect(rev2.flow_root(make_container(1))->block_count() == 6, "新 revision 有新順序");
    expect(static_cast<const TextRecord*>(rev2.record_for(make_block(1)).get())->text() == "v2",
           "新 revision 有新內容");
    expect(rev2.validate().ok(), "新 revision 仍然一致");
}

void test_flow_insert_publishes_relabel_and_locations_atomically() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;
    config.merge_low_water = 1;
    config.initial_relabel_window = 8;

    auto seq = FlowSequence::empty(config);
    const auto container = make_container(77);
    bool exercised_relabel = false;

    for (std::size_t i = 0; i < 1000; ++i) {
        auto edit = seq.insert_with_updates(seq.block_count(), make_block(i + 1));
        if (edit.diagnostics().relabeled_leaf_count == 0) {
            seq = std::move(edit).take_sequence();
            continue;
        }

        exercised_relabel = true;
        auto base = DocumentRevision::initial().with_flow_root(container, seq);
        const auto base_id = base.snapshot_id();
        const auto base_count = base.flow_root(container)->block_count();

        auto applied = base.with_flow_insert(container, edit);
        expect(applied.is_ok(), "來源 root 相符時接受 typed Flow insert");
        if (!applied.is_ok()) {
            break;
        }
        auto next = std::move(applied).take();

        expect(next.validate().ok(), "sequence 與所有局部 locator 在同一 revision 發布");
        expect(next.flow_root(container)->block_count() == base_count + 1,
               "typed Flow insert 只加入一個 Block");
        expect(next.snapshot_id().content_revision == base_id.content_revision + 1,
               "局部 relabel 隨插入只前進一次 content revision");
        expect(next.snapshot_id().storage_generation == base_id.storage_generation,
               "bounded relabel 不誤標為全域 storage rebuild");
        expect(base.validate().ok() && base.flow_root(container)->block_count() == base_count,
               "舊 revision 的 sequence 與 locator 保持不變");

        auto stale = next.with_flow_insert(container, edit);
        expect(!stale.is_ok(), "同一 typed edit 不得套到較新的 root");
        if (!stale.is_ok()) {
            expect(stale.error().code() == ErrorCode::revision_conflict,
                   "stale Flow edit 回傳 revision_conflict");
        }
        break;
    }

    expect(exercised_relabel, "測試必須實際跨過 LeafKey 間距耗盡");
}

void test_first_flow_insert_creates_container_atomically() {
    const auto container = make_container(66);
    auto base = DocumentRevision::initial();
    auto edit = FlowSequence::empty().insert_with_updates(0, make_block(1));

    auto applied = base.with_flow_insert(container, edit);
    expect(applied.is_ok(), "空 revision 接受來源 root 為 null 的第一次 Flow insert");
    if (!applied.is_ok()) {
        return;
    }

    const auto next = std::move(applied).take();
    expect(next.container_count() == 1, "第一次 typed insert 同時建立 Container root");
    expect(next.flow_root(container)->at(0) == make_block(1), "第一次 typed insert 保存 Block");
    expect(next.validate().ok(), "第一次 typed insert 同時建立正確 locator");
    expect(base.container_count() == 0, "第一次 typed insert 不改寫舊 revision");
}

void test_global_relabel_advances_storage_generation() {
    FlowSequenceConfig config;
    config.leaf_capacity = 4;
    config.internal_fanout = 4;
    config.merge_low_water = 1;
    config.initial_relabel_window = 10000;

    auto seq = FlowSequence::empty(config);
    const auto container = make_container(88);
    bool exercised_global = false;

    for (std::size_t i = 0; i < 1000; ++i) {
        auto edit = seq.insert_with_updates(seq.block_count(), make_block(10000 + i));
        if (!edit.diagnostics().global_rebuild) {
            seq = std::move(edit).take_sequence();
            continue;
        }

        exercised_global = true;
        auto base = DocumentRevision::initial().with_flow_root(container, seq);
        auto applied = base.with_flow_insert(container, edit);
        expect(applied.is_ok(), "全域 relabel 可與內容插入原子發布");
        if (applied.is_ok()) {
            const auto next = std::move(applied).take();
            expect(next.snapshot_id().content_revision ==
                       base.snapshot_id().content_revision + 1,
                   "全域 relabel 同時前進 content revision");
            expect(next.snapshot_id().storage_generation ==
                       base.snapshot_id().storage_generation + 1,
                   "全域 relabel 前進 storage generation");
            expect(next.validate().ok(), "全域 relabel 後 sequence 與 locator 一致");
        }
        break;
    }

    expect(exercised_global, "大型 window 必須實際走到 global relabel 分支");
}

}  // namespace

int main() {
    test_directory_allocates_sequential_slots();
    test_directory_allocate_is_idempotent();
    test_directory_reverse_lookup();
    test_directory_cow();

    test_store_write_and_read();
    test_store_tombstone_differs_from_absent();
    test_store_cow_shares_untouched_pages();
    test_record_page_table_grows_and_shares();

    test_initial_revision_is_empty();
    test_new_object_advances_content_revision();
    test_updated_record_keeps_slot();
    test_delete_writes_tombstone_and_keeps_slot();
    test_storage_rebuild_only_bumps_generation();
    test_flow_root_populates_location_index();

    test_validate_passes_on_consistent_revision();
    test_validate_with_multiple_containers();
    test_validate_rejects_block_in_two_containers();
    test_validate_rejects_missing_location_entry();
    test_revision_bundles_content_and_order_atomically();
    test_flow_insert_publishes_relabel_and_locations_atomically();
    test_first_flow_insert_creates_container_atomically();
    test_global_relabel_advances_storage_generation();

    shutdown_default_reclamation_queue();
    return krepis_test::report("krepis.document_revision");
}
