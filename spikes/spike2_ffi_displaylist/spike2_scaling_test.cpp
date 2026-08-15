// 目的：驗證 P0 報告「打字增量重排耗時為 O(1)」的主張。
//
// 報告只測到 N = 10,000。若 layout() 對全部段落跑迴圈重算 y_offset，其 O(N) 項在該規模下
// 會被單段 DirectWrite shaping 的常數（約 130μs）蓋掉。本測試把 N 拉到百萬級使其顯形，
// 並分離「首次 layout（含 shaping）」與「純 y_offset 重算」兩項成本。
//
// 判準：若耗時真為 O(1)，N 增加 100 倍時耗時應維持不變。

#include "engine_abi.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace {

using Clock = std::chrono::steady_clock;

double micros_since(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// 對指定段落數的文件，量測「編輯一個段落後重新 layout」的耗時。
// 編輯位置固定在中間，與 P0 報告一致。
void measure(uint32_t n, int runs) {
    KrepisEngineHandle engine = nullptr;
    if (krepis_engine_create(&engine) != KREPIS_OK) {
        std::printf("engine_create 失敗 (N=%u)\n", n);
        return;
    }
    krepis_engine_set_viewport(engine, 800.0f, 600.0f, 0.0f);

    const std::string body = "這是一段用於量測的中文內容，長度固定以排除 shaping 成本的變異。";
    for (uint32_t i = 0; i < n; ++i) {
        krepis_engine_insert_paragraph(engine, i, body.c_str());
    }

    // 先做一次完整 layout，讓所有段落離開 dirty 狀態。
    float total_height = 0.0f;
    krepis_engine_layout(engine, &total_height);

    const uint32_t edit_index = n / 2;
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(runs));

    for (int r = 0; r < runs; ++r) {
        // 每次改成不同內容，確保 edit_paragraph 真的把該段標為 dirty。
        std::string edited = body + std::to_string(r);

        auto start = Clock::now();
        krepis_engine_edit_paragraph(engine, edit_index, edited.c_str());
        krepis_engine_layout(engine, &total_height);
        samples.push_back(micros_since(start));
    }

    std::sort(samples.begin(), samples.end());
    const double p50 = samples[samples.size() / 2];
    const double p99 = samples[static_cast<size_t>(samples.size() * 0.99)];
    double sum = 0.0;
    for (double s : samples) sum += s;
    const double avg = sum / static_cast<double>(samples.size());

    std::printf("N = %9u | avg %9.1f us | p50 %9.1f us | p99 %9.1f us\n",
                n, avg, p50, p99);

    krepis_engine_destroy(engine);
}

}  // namespace

int main() {
    std::printf("=== 增量性驗證：編輯中間段落後重新 layout ===\n");
    std::printf("判準：若為 O(1)，N 增加 100 倍時耗時應維持不變。\n\n");

    measure(1000, 200);
    measure(10000, 200);
    measure(100000, 100);
    measure(1000000, 30);

    return 0;
}
