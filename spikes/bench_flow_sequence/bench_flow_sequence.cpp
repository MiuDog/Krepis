// LAY-0002「尚未決定」第一項：Chunked B+ tree 的 leaf 容量、fanout 與 low-water mark。
//
// **本 benchmark 只產生數字，不做決定。** 判準必須在讀數字之前寫下（見報告檔），
// 否則會不自覺地挑「數字好看」的參數，而不是「符合 frame budget」的參數。
//
// 四種 workload 刻意模擬真實編輯，不用「隨機插入 N 次」——
// 真實編輯是**局部**的，隨機存取會高估樹的深度成本並低估 cache 效益。

#include "krepis/flow_sequence.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "bench_support.hpp"

#include <cstdio>
#include <string>
#include <vector>

using krepis::BlockId;
using krepis::FlowSequence;
using krepis::FlowSequenceConfig;
using krepis::ObjectId;
using krepis::default_reclamation_queue;
using krepis::shutdown_default_reclamation_queue;
using krepis_bench::Rng;
using krepis_bench::Row;
using krepis_bench::Samples;
using krepis_bench::make_row;
using krepis_bench::print_table;
using krepis_bench::time_once;

namespace {

constexpr std::size_t document_blocks = 10000;   // 參數掃描用的文件長度
constexpr std::size_t warmup_ops = 200;          // 捨棄的暖機次數
constexpr std::size_t measured_ops = 2000;       // 每個 workload 的計時次數

BlockId make_block(std::uint64_t n) {
    return BlockId{ObjectId{0, n}};
}

FlowSequence build_document(const FlowSequenceConfig& config, std::size_t count) {
    auto seq = FlowSequence::empty(config);
    for (std::size_t i = 0; i < count; ++i) {
        seq = seq.insert(i, make_block(i + 1));
    }
    return seq;
}

// Workload 1：游標附近連續打字。
// 每次在游標處插入一個 Block，游標前進。反覆命中同一個 leaf，
// 這是最高頻的編輯模式，也是 frame budget 最敏感的路徑。
Samples workload_local_typing(const FlowSequence& base) {
    Samples samples;
    samples.reserve(measured_ops);

    auto seq = base;
    std::size_t cursor = base.block_count() / 2;
    std::uint64_t next_id = 1000000;

    for (std::size_t i = 0; i < warmup_ops + measured_ops; ++i) {
        const auto id = make_block(next_id++);
        const std::size_t at = cursor;
        FlowSequence result = seq;
        const auto elapsed = time_once([&] { result = seq.insert(at, id); });
        seq = std::move(result);
        ++cursor;

        if (i >= warmup_ops) {
            samples.add(elapsed);
        }
    }
    return samples;
}

// Workload 2：貼上一大段。
// 在同一位置連續插入，觸發連鎖 split。測 split 路徑的成本。
Samples workload_paste(const FlowSequence& base) {
    Samples samples;
    samples.reserve(measured_ops);

    auto seq = base;
    const std::size_t paste_at = base.block_count() / 3;
    std::uint64_t next_id = 2000000;

    for (std::size_t i = 0; i < warmup_ops + measured_ops; ++i) {
        const auto id = make_block(next_id++);
        // 一律插在同一個位置，模擬連續貼上的內容彼此相鄰。
        const std::size_t at = paste_at + i;
        FlowSequence result = seq;
        const auto elapsed = time_once([&] { result = seq.insert(at, id); });
        seq = std::move(result);

        if (i >= warmup_ops) {
            samples.add(elapsed);
        }
    }
    return samples;
}

// Workload 3：刪除一整段。
// 連續 remove，觸發 merge／redistribution。D16 的遲滯區間該在這裡證明自己。
Samples workload_delete_range(const FlowSequence& base) {
    Samples samples;
    samples.reserve(measured_ops);

    auto seq = base;
    const std::size_t start = base.block_count() / 4;

    for (std::size_t i = 0; i < warmup_ops + measured_ops; ++i) {
        if (seq.block_count() <= 1) break;
        const std::size_t at = (start < seq.block_count()) ? start : 0;
        FlowSequence result = seq;
        const auto elapsed = time_once([&] { result = seq.remove(at); });
        seq = std::move(result);

        if (i >= warmup_ops) {
            samples.add(elapsed);
        }
    }
    return samples;
}

// Workload 4：捲動時的隨機存取。
// Viewport 定位後逐 Block 取值。測純讀路徑（不產生新 revision）。
Samples workload_viewport_scan(const FlowSequence& base) {
    Samples samples;
    samples.reserve(measured_ops);

    Rng rng(0xC0FFEE);
    const std::size_t count = base.block_count();

    for (std::size_t i = 0; i < warmup_ops + measured_ops; ++i) {
        const std::size_t at = rng.below(count);
        BlockId sink{};
        const auto elapsed = time_once([&] { sink = base.at(at); });
        // 阻止最佳化器移除讀取。
        if (sink.raw().low == 0xFFFFFFFFFFFFFFFFULL) {
            std::printf("unreachable\n");
        }

        if (i >= warmup_ops) {
            samples.add(elapsed);
        }
    }
    return samples;
}

struct WorkloadResults {
    Samples typing;
    Samples paste;
    Samples deletion;
    Samples scan;
};

WorkloadResults run_all_workloads(const FlowSequenceConfig& config) {
    // 每個設定從乾淨的回收佇列開始，避免前一輪的背景銷毀污染量測。
    default_reclamation_queue().wait_until_idle();

    const auto base = build_document(config, document_blocks);

    WorkloadResults results;
    results.typing = workload_local_typing(base);
    results.paste = workload_paste(base);
    results.deletion = workload_delete_range(base);
    results.scan = workload_viewport_scan(base);
    return results;
}

// 記憶體放大倍率：一次編輯實際產生多少個廢棄節點。
//
// 這是延遲以外的第二個判準，而且**不受計時噪音影響**。COW 每次編輯複製 root-to-leaf
// 路徑，因此廢棄節點數 ≈ 樹深度。leaf_capacity 越小樹越深（節點數越多），
// 但每個 leaf 越小（單一節點越便宜）——兩者相反，所以最佳值在中間，必須量。
//
// 利用 ReclamationQueue::total_reclaimed()：被取代的舊路徑正是被回收的節點。
double measure_nodes_per_edit(const FlowSequenceConfig& config) {
    auto& queue = default_reclamation_queue();

    const auto base = build_document(config, document_blocks);
    queue.wait_until_idle();
    const std::size_t baseline = queue.total_reclaimed();

    constexpr std::size_t edits = 2000;
    auto seq = base;
    std::size_t cursor = base.block_count() / 2;
    std::uint64_t next_id = 3000000;

    for (std::size_t i = 0; i < edits; ++i) {
        seq = seq.insert(cursor, make_block(next_id++));
        ++cursor;
    }

    // 只 drain 編輯過程產生的垃圾；base 與 seq 仍持有的節點不會被計入。
    queue.wait_until_idle();
    const std::size_t delta = queue.total_reclaimed() - baseline;

    return static_cast<double>(delta) / static_cast<double>(edits);
}

void sweep_allocation_amplification() {
    const std::size_t capacities[] = {8, 16, 32, 64, 128, 256};

    std::printf("\n### 記憶體放大 — 每次編輯產生的廢棄節點數\n\n");
    std::printf("| 設定 | 節點/編輯 | leaf 承載 BlockId | 概算 leaf 位元組/編輯 |\n");
    std::printf("|---|---:|---:|---:|\n");

    for (std::size_t cap : capacities) {
        FlowSequenceConfig config;
        config.leaf_capacity = cap;
        config.internal_fanout = 32;
        config.merge_low_water = cap / 4;

        const double nodes = measure_nodes_per_edit(config);
        // 每次編輯必定複製恰好一個 leaf；其餘為 internal node。
        const double leaf_bytes = static_cast<double>(cap) * static_cast<double>(sizeof(BlockId));

        std::printf("| leaf=%zu fanout=32 | %.2f | %zu | %.0f |\n", cap, nodes, cap, leaf_bytes);
    }
}

std::string label_for(const FlowSequenceConfig& c) {
    return "leaf=" + std::to_string(c.leaf_capacity) +
           " fanout=" + std::to_string(c.internal_fanout) +
           " low=" + std::to_string(c.merge_low_water);
}

// 掃描 leaf_capacity，其餘固定。
void sweep_leaf_capacity() {
    const std::size_t capacities[] = {8, 16, 32, 64, 128, 256};

    std::vector<Row> typing, paste, deletion, scan;
    for (std::size_t cap : capacities) {
        FlowSequenceConfig config;
        config.leaf_capacity = cap;
        config.internal_fanout = 32;
        config.merge_low_water = cap / 4;

        auto r = run_all_workloads(config);
        const auto label = label_for(config);
        typing.push_back(make_row(label, r.typing));
        paste.push_back(make_row(label, r.paste));
        deletion.push_back(make_row(label, r.deletion));
        scan.push_back(make_row(label, r.scan));
    }

    print_table("Leaf capacity 掃描 — Workload 1：游標附近打字", typing);
    print_table("Leaf capacity 掃描 — Workload 2：貼上大段", paste);
    print_table("Leaf capacity 掃描 — Workload 3：刪除一段", deletion);
    print_table("Leaf capacity 掃描 — Workload 4：viewport 隨機存取", scan);
}

// 掃描 internal_fanout，leaf_capacity 固定於 64。
void sweep_fanout() {
    const std::size_t fanouts[] = {8, 16, 32, 64, 128};

    std::vector<Row> typing, scan;
    for (std::size_t fanout : fanouts) {
        FlowSequenceConfig config;
        config.leaf_capacity = 64;
        config.internal_fanout = fanout;
        config.merge_low_water = 16;

        auto r = run_all_workloads(config);
        const auto label = label_for(config);
        typing.push_back(make_row(label, r.typing));
        scan.push_back(make_row(label, r.scan));
    }

    print_table("Fanout 掃描 — Workload 1：游標附近打字", typing);
    print_table("Fanout 掃描 — Workload 4：viewport 隨機存取", scan);
}

// 掃描 merge_low_water，其餘固定。只有刪除路徑會受影響。
void sweep_low_water() {
    const std::size_t low_waters[] = {0, 8, 16, 24, 32};

    std::vector<Row> deletion;
    for (std::size_t low : low_waters) {
        FlowSequenceConfig config;
        config.leaf_capacity = 64;
        config.internal_fanout = 32;
        config.merge_low_water = low;

        auto r = run_all_workloads(config);
        deletion.push_back(make_row(label_for(config), r.deletion));
    }

    print_table("Low-water 掃描 — Workload 3：刪除一段（0 表示關閉重平衡）", deletion);
}

// 以預設設定檢查隨文件長度的成長。應為對數，不應為線性。
void scaling_check() {
    const std::size_t sizes[] = {1000, 5000, 20000, 50000};

    std::vector<Row> typing, scan;
    for (std::size_t size : sizes) {
        FlowSequenceConfig config;  // 預設值
        default_reclamation_queue().wait_until_idle();
        const auto base = build_document(config, size);

        auto t = workload_local_typing(base);
        auto s = workload_viewport_scan(base);

        const auto label = std::to_string(size) + " blocks";
        typing.push_back(make_row(label, t));
        scan.push_back(make_row(label, s));
    }

    print_table("長度擴展 — Workload 1：游標附近打字（預設設定）", typing);
    print_table("長度擴展 — Workload 4：viewport 隨機存取（預設設定）", scan);
}

}  // namespace

int main() {
    std::printf("# FlowSequence 分塊參數 benchmark\n");
    std::printf("\n文件長度 %zu blocks；每個 workload 暖機 %zu 次、計時 %zu 次。\n",
                document_blocks, warmup_ops, measured_ops);
    std::printf("**判準見 `tasks/lay-0002-chunking-parameters-report.md`，先讀判準再讀數字。**\n");

    sweep_leaf_capacity();
    sweep_fanout();
    sweep_low_water();
    scaling_check();
    sweep_allocation_amplification();

    std::printf("\n");
    shutdown_default_reclamation_queue();
    return 0;
}
