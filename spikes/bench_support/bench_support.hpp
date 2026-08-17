#pragma once

// Benchmark harness 共用設施。
//
// 責任：計時、百分位統計、Markdown 表格輸出。
// 不負責：定義 workload —— 由各 benchmark 自行提供。
//
// **刻意只報百分位，不以平均值作為判準。** 平均值會被大量快路徑稀釋；
// 吃掉 frame budget 的是尾端延遲（LAY-0001 的一幀預算是對 p99 的要求，不是對平均）。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace krepis_bench {

using Clock = std::chrono::steady_clock;

// 單一 workload 的計時樣本集合。
class Samples {
public:
    void reserve(std::size_t n) { values_.reserve(n); }

    void add(Clock::duration d) {
        values_.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    }

    [[nodiscard]] std::size_t count() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    // 百分位。p 為 0.0–1.0。呼叫前會就地排序。
    [[nodiscard]] double percentile_us(double p) {
        if (values_.empty()) return 0.0;
        if (!sorted_) {
            std::sort(values_.begin(), values_.end());
            sorted_ = true;
        }
        auto idx = static_cast<std::size_t>(p * static_cast<double>(values_.size() - 1));
        return static_cast<double>(values_[idx]) / 1000.0;
    }

    [[nodiscard]] double mean_us() const {
        if (values_.empty()) return 0.0;
        long double total = 0.0L;
        for (auto v : values_) total += static_cast<long double>(v);
        return static_cast<double>(total / static_cast<long double>(values_.size())) / 1000.0;
    }

private:
    std::vector<long long> values_;
    bool sorted_ = false;
};

// 計時一次操作。
template <typename Fn>
Clock::duration time_once(Fn&& fn) {
    const auto start = Clock::now();
    fn();
    return Clock::now() - start;
}

// 一列結果。
struct Row {
    std::string label;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
    double mean_us = 0.0;
};

inline Row make_row(std::string label, Samples& s) {
    return Row{
        std::move(label),
        s.percentile_us(0.50),
        s.percentile_us(0.95),
        s.percentile_us(0.99),
        s.percentile_us(1.00),
        s.mean_us(),
    };
}

// 輸出可直接貼進 ADR 的 Markdown 表格。
inline void print_table(const char* title, const std::vector<Row>& rows) {
    std::printf("\n### %s\n\n", title);
    std::printf("| 設定 | p50 (us) | p95 (us) | p99 (us) | max (us) | mean (us) |\n");
    std::printf("|---|---:|---:|---:|---:|---:|\n");
    for (const auto& r : rows) {
        std::printf("| %s | %.3f | %.3f | %.3f | %.3f | %.3f |\n", r.label.c_str(), r.p50_us,
                    r.p95_us, r.p99_us, r.max_us, r.mean_us);
    }
}

// 確定性 PRNG。benchmark 必須可重現，不使用 random_device。
class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ULL) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        // xorshift64*
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    // 回傳 [0, bound) 的值。
    std::size_t below(std::size_t bound) noexcept {
        if (bound == 0) return 0;
        return static_cast<std::size_t>(next() % bound);
    }

private:
    std::uint64_t state_;
};

}  // namespace krepis_bench
