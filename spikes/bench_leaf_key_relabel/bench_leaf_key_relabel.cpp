#include "krepis/flow_sequence.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using krepis::BlockId;
using krepis::FlowSequence;
using krepis::FlowSequenceConfig;
using krepis::ObjectId;
using krepis::default_reclamation_queue;
using krepis::shutdown_default_reclamation_queue;

constexpr std::size_t insert_count = 50000;
constexpr std::size_t repetitions = 3;

enum class Pattern { head, tail, middle };

struct Metrics {
    double milliseconds = 0.0;
    std::size_t relabel_events = 0;
    std::size_t locator_updates = 0;
    std::size_t max_window = 0;
    std::size_t global_rebuilds = 0;
    bool valid = false;
};

[[nodiscard]] BlockId make_block(std::size_t value) {
    return BlockId{ObjectId{0, static_cast<std::uint64_t>(value + 1)}};
}

[[nodiscard]] std::size_t insertion_position(
    Pattern pattern, const FlowSequence& sequence) {
    switch (pattern) {
        case Pattern::head:
            return 0;
        case Pattern::tail:
            return sequence.block_count();
        case Pattern::middle:
            return sequence.block_count() / 2;
    }
    return 0;
}

[[nodiscard]] Metrics run_workload(std::size_t window, Pattern pattern) {
    // 隔離上一個 workload 的背景回收，避免不同候選的垃圾量污染下一段計時。
    default_reclamation_queue().wait_until_idle();

    FlowSequenceConfig config;
    config.initial_relabel_window = window;
    auto sequence = FlowSequence::empty(config);
    Metrics metrics;

    const auto started = Clock::now();
    for (std::size_t i = 0; i < insert_count; ++i) {
        auto edit = sequence.insert_with_updates(
            insertion_position(pattern, sequence), make_block(i));
        if (edit.diagnostics().relabeled_leaf_count > 0) {
            ++metrics.relabel_events;
            metrics.locator_updates += edit.locator_updates().size();
            metrics.max_window = std::max(
                metrics.max_window, edit.diagnostics().relabel_window);
        }
        if (edit.diagnostics().global_rebuild) {
            ++metrics.global_rebuilds;
        }
        sequence = std::move(edit).take_sequence();
    }
    const auto stopped = Clock::now();

    metrics.milliseconds =
        std::chrono::duration<double, std::milli>(stopped - started).count();
    metrics.valid = sequence.block_count() == insert_count;
    if (pattern == Pattern::tail) {
        metrics.valid = metrics.valid &&
            sequence.at(0) == make_block(0) &&
            sequence.at(insert_count - 1) == make_block(insert_count - 1);
    } else if (pattern == Pattern::head) {
        metrics.valid = metrics.valid &&
            sequence.at(0) == make_block(insert_count - 1) &&
            sequence.at(insert_count - 1) == make_block(0);
    }
    return metrics;
}

[[nodiscard]] Metrics combine(const Metrics& a, const Metrics& b, const Metrics& c) {
    return Metrics{
        a.milliseconds + b.milliseconds + c.milliseconds,
        a.relabel_events + b.relabel_events + c.relabel_events,
        a.locator_updates + b.locator_updates + c.locator_updates,
        std::max({a.max_window, b.max_window, c.max_window}),
        a.global_rebuilds + b.global_rebuilds + c.global_rebuilds,
        a.valid && b.valid && c.valid,
    };
}

}  // namespace

int main() {
    constexpr std::array<std::size_t, 6> windows{2, 4, 8, 16, 32, 64};

    std::printf("# LeafKey relabel window benchmark\n\n");
    std::printf("每個候選執行 %zu 輪；每輪各做 %zu 次頭插、尾插與中間插入。\n\n",
                repetitions, insert_count);
    std::printf("| initial window | median total ms | relabel events | locator updates | max window | global rebuilds | valid |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|:---:|\n");

    for (const auto window : windows) {
        std::vector<Metrics> samples;
        samples.reserve(repetitions);
        for (std::size_t repeat = 0; repeat < repetitions; ++repeat) {
            default_reclamation_queue().wait_until_idle();
            samples.push_back(combine(
                run_workload(window, Pattern::head),
                run_workload(window, Pattern::tail),
                run_workload(window, Pattern::middle)));
        }

        std::sort(samples.begin(), samples.end(), [](const Metrics& a, const Metrics& b) {
            return a.milliseconds < b.milliseconds;
        });
        const auto& median = samples[samples.size() / 2];
        std::printf("| %zu | %.3f | %zu | %zu | %zu | %zu | %s |\n",
                    window, median.milliseconds, median.relabel_events,
                    median.locator_updates, median.max_window,
                    median.global_rebuilds, median.valid ? "yes" : "no");
    }

    shutdown_default_reclamation_queue();
    return 0;
}
