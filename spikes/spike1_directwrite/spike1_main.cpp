#include "spike1_shaper.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cassert>

using namespace krepis::spike1;

void test_shaping_correctness(DirectWriteShaper* shaper) {
    std::cout << "========================================\n";
    std::cout << "[Spike 1] 測試 1：多語系與複雜文字 Shaping 驗證\n";
    std::cout << "========================================\n";

    // 1. 繁體中文與英文混排
    std::string text_cjk = "Krepis 基座庫：結構化筆記 120Hz 增量版面引擎！";
    auto result_cjk = shaper->shape(text_cjk, "Segoe UI", 16.0f, "zh-TW");
    std::cout << "輸入文字: " << text_cjk << "\n";
    std::cout << "總寬度: " << result_cjk.total_width << ", 高度: " << result_cjk.height << "\n";
    std::cout << "產生 Runs 數量: " << result_cjk.runs.size() << "\n";
    for (size_t i = 0; i < result_cjk.runs.size(); ++i) {
        const auto& r = result_cjk.runs[i];
        std::cout << "  Run[" << i << "]: glyphs=" << r.glyphs.size()
                  << ", is_rtl=" << (r.is_rtl ? "true" : "false")
                  << ", total_advance=" << r.total_advance
                  << ", ascent=" << r.ascent << ", descent=" << r.descent << "\n";
    }
    assert(!result_cjk.runs.empty());
    assert(result_cjk.total_width > 0.0f);

    // 2. RTL 雙向文字測試（阿拉伯文 / 英文混排）
    std::string text_bidi = "Hello مرحبا World";
    auto result_bidi = shaper->shape(text_bidi, "Segoe UI", 16.0f, "ar-SA");
    std::cout << "\n輸入文字 (Bidi): " << text_bidi << "\n";
    std::cout << "產生 Runs 數量: " << result_bidi.runs.size() << "\n";
    bool has_rtl = false;
    for (size_t i = 0; i < result_bidi.runs.size(); ++i) {
        const auto& r = result_bidi.runs[i];
        std::cout << "  Run[" << i << "]: glyphs=" << r.glyphs.size()
                  << ", is_rtl=" << (r.is_rtl ? "true" : "false")
                  << ", bidi_level=" << (int)r.bidi_level
                  << ", advance=" << r.total_advance << "\n";
        if (r.is_rtl) has_rtl = true;
    }
    assert(has_rtl && "Bidi 文字必須正確識別出 RTL Run");

    // 3. Emoji 與多碼元測試
    std::string text_emoji = "筆記 🚀 Note ✨ 繁體 🇹🇼";
    auto result_emoji = shaper->shape(text_emoji, "Segoe UI", 16.0f, "zh-TW");
    std::cout << "\n輸入文字 (Emoji): " << text_emoji << "\n";
    std::cout << "產生 Runs 數量: " << result_emoji.runs.size() << "\n";
    assert(!result_emoji.runs.empty());

    std::cout << "--> [通過] DirectWrite 純值型別抽取與多語系/Bidi/Fallback 驗證成功！\n\n";
}

void benchmark_shaping_performance(DirectWriteShaper* shaper) {
    std::cout << "========================================\n";
    std::cout << "[Spike 1] 測試 2：Shaping 耗時與吞吐量基準測試\n";
    std::cout << "========================================\n";

    std::vector<std::pair<std::string, std::string>> test_cases = {
        {"單行短句 (10 字)", "結構化筆記領域中立基座。"},
        {"單個段落 (60 字)", "Krepis 是結構化筆記的領域中立基座，以 C++20 撰寫。提供多平台、本地優先的資料與版面核心，不含任何特定產品的語意。"},
        {"中長段落 (300 字)", "Krepis 是結構化筆記的領域中立基座，以 C++20 撰寫。提供多平台、本地優先的資料與版面核心，不含任何特定產品的語意。權威狀態包含 identity、session、permission、persistence、protocol 與 event digest。文件模型包含 node tree、stable ID、schema 與 codec。版面引擎支援流式 layout ＋ 空間 layout，以及 selection 模型、undo 與 typed transaction。"}
    };

    const int WARMUP_RUNS = 100;
    const int BENCHMARK_RUNS = 1000;

    for (const auto& [label, text] : test_cases) {
        // Warmup
        for (int i = 0; i < WARMUP_RUNS; ++i) {
            auto res = shaper->shape(text);
            (void)res;
        }

        std::vector<double> latencies_us;
        latencies_us.reserve(BENCHMARK_RUNS);

        for (int i = 0; i < BENCHMARK_RUNS; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            auto res = shaper->shape(text);
            auto end = std::chrono::high_resolution_clock::now();
            double us = std::chrono::duration<double, std::micro>(end - start).count();
            latencies_us.push_back(us);
        }

        std::sort(latencies_us.begin(), latencies_us.end());
        double p50 = latencies_us[BENCHMARK_RUNS * 50 / 100];
        double p90 = latencies_us[BENCHMARK_RUNS * 90 / 100];
        double p99 = latencies_us[BENCHMARK_RUNS * 99 / 100];
        double max_val = latencies_us.back();
        double avg = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0) / BENCHMARK_RUNS;

        std::cout << "情境: " << label << " (字元數: " << text.size() << " bytes)\n";
        std::cout << "  平均: " << avg << " us | p50: " << p50 << " us | p90: " << p90 << " us | p99: " << p99 << " us | max: " << max_val << " us\n";
    }
}

int main() {
    try {
        auto shaper = DirectWriteShaper::create();
        test_shaping_correctness(shaper.get());
        benchmark_shaping_performance(shaper.get());
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "例外發生: " << ex.what() << "\n";
        return 1;
    }
}
