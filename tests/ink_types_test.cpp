#include "krepis/ink_types.hpp"
#include "krepis/intrusive_ptr.hpp"

#include "test_support.hpp"

#include <cmath>
#include <vector>

using krepis::BlockId;
using krepis::BrushId;
using krepis::BrushStyle;
using krepis::InkSample;
using krepis::InkStroke;
using krepis::ObjectId;
using krepis::ResponseCurve;
using krepis::SampleRange;
using krepis::WidthMode;
using krepis::decode_ink_sample;
using krepis::encode_ink_sample;
using krepis::erase_ranges;
using krepis::ink_sample_encoded_size;
using krepis::make_intrusive;
using krepis::shutdown_default_reclamation_queue;
using krepis::stroke_width_at;
using krepis_test::expect;

namespace {

BlockId make_block(std::uint64_t n) {
    return BlockId{ObjectId{0, n}};
}

BrushId make_brush(std::uint64_t n) {
    return BrushId{ObjectId{0, n}};
}

bool approx(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) < tolerance;
}

InkSample make_sample(std::uint16_t x, std::int16_t y, std::uint8_t pressure,
                      std::uint16_t dt_ms) {
    InkSample sample;
    sample.x_normalized = x;
    sample.y_fixed = y;
    sample.pressure = pressure;
    sample.dt_ms = dt_ms;
    return sample;
}

// --- 編解碼 ---

void test_sample_encoding_roundtrip() {
    InkSample original;
    original.x_normalized = 40000;
    original.y_fixed = -1234;
    original.pressure = 200;
    original.tilt_altitude = 45;
    original.tilt_azimuth = 180;
    original.dt_ms = 5000;

    std::array<std::byte, ink_sample_encoded_size> bytes{};
    encode_ink_sample(original, bytes);
    const InkSample decoded = decode_ink_sample(bytes);

    expect(decoded.x_normalized == original.x_normalized, "x 往返一致");
    expect(decoded.y_fixed == original.y_fixed, "y 往返一致（含負值）");
    expect(decoded.pressure == original.pressure, "pressure 往返一致");
    expect(decoded.tilt_altitude == original.tilt_altitude, "tilt altitude 往返一致");
    expect(decoded.tilt_azimuth == original.tilt_azimuth, "tilt azimuth 往返一致");
    expect(decoded.dt_ms == original.dt_ms, "dt 往返一致");
}

void test_encoded_size_is_nine_bytes() {
    // 儲存格式為 9 bytes；sizeof 因對齊填充而較大，兩者刻意不同。
    expect(ink_sample_encoded_size == 9, "編碼後為 9 bytes");
    expect(sizeof(InkSample) >= ink_sample_encoded_size, "記憶體表示含對齊填充");
}

// --- INK-2＝C：正規化座標與全寬拉伸 ---

void test_normalized_x_scales_with_block_width() {
    std::vector<InkSample> samples{
        make_sample(0, 0, 128, 0),
        make_sample(65535, 0, 128, 10),
    };
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    const auto left_narrow = stroke->value_at(0, 400.0);
    const auto right_narrow = stroke->value_at(1, 400.0);
    expect(approx(left_narrow.x_pixels, 0.0), "正規化 0 對應左緣");
    expect(approx(right_narrow.x_pixels, 400.0, 1e-6), "正規化 65535 對應右緣");

    // 切換到全寬（寬度加倍）——水平應等比拉伸。
    const auto right_wide = stroke->value_at(1, 800.0);
    expect(approx(right_wide.x_pixels, 800.0, 1e-6), "寬度加倍後水平等比拉伸");
}

void test_vertical_is_independent_of_width() {
    std::vector<InkSample> samples{make_sample(30000, 500, 128, 0),
                                   make_sample(30001, 500, 128, 10)};
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    const double narrow_y = stroke->value_at(0, 400.0).y_pixels;
    const double wide_y = stroke->value_at(0, 1600.0).y_pixels;
    expect(approx(narrow_y, wide_y), "垂直座標不隨寬度改變（已定語意）");
    expect(approx(narrow_y, 5.0), "500 個 0.01mm 單位 = 5mm");
}

// --- 速度 ---

void test_velocity_uses_capture_width_not_current_width() {
    // 兩點水平相距半個 Block 寬，間隔 100ms。
    std::vector<InkSample> samples{
        make_sample(0, 0, 128, 0),
        make_sample(32768, 0, 128, 100),
    };
    const double capture_width = 400.0;
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), capture_width,
                                            std::move(samples));

    // 距離 = 400 * (32768/65535) ≈ 200 px，時間 0.1s → 約 2000 px/s。
    const double velocity = stroke->velocity_at(1);
    expect(velocity > 1990.0 && velocity < 2010.0, "速度以擷取寬度計算");
    expect(approx(stroke->velocity_at(0), 0.0), "首點速度為 0");

    // **關鍵**：velocity_at 不接受寬度參數，因此不可能因顯示寬度而改變。
    // 若它用當前寬度，同一筆在全寬模式下速度會加倍、粗細跟著變，
    // 那是使用者看得見卻無法解釋的行為。
    expect(approx(stroke->capture_block_width(), capture_width), "擷取寬度被保存");
}

void test_zero_time_delta_yields_zero_velocity() {
    std::vector<InkSample> samples{make_sample(0, 0, 128, 50), make_sample(60000, 0, 128, 50)};
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));
    expect(approx(stroke->velocity_at(1), 0.0), "時間差為零時速度定義為 0，不除以零");
}

// --- ResponseCurve ---

void test_response_curve_endpoints_and_interpolation() {
    const ResponseCurve curve = ResponseCurve::linear(0.5, 2.5);

    expect(approx(curve.evaluate(0.0), 0.5), "輸入 0 對應低端");
    expect(approx(curve.evaluate(1.0), 2.5), "輸入 1 對應高端");
    expect(approx(curve.evaluate(0.5), 1.5), "中點線性內插");

    expect(approx(curve.evaluate(-1.0), 0.5), "低於範圍鉗制於低端");
    expect(approx(curve.evaluate(99.0), 2.5), "高於範圍鉗制於高端");

    const ResponseCurve flat = ResponseCurve::constant(1.75);
    expect(approx(flat.evaluate(0.3), 1.75), "常數曲線處處相同");
}

// --- 筆刷驅動的粗細（使用者要求的三種筆種）---

void test_constant_width_brush_ignores_pressure() {
    std::vector<InkSample> samples{make_sample(0, 0, 0, 0), make_sample(10000, 0, 255, 10)};
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    BrushStyle brush;
    brush.width_mode = WidthMode::constant_width;
    brush.base_width = 3.0;
    brush.pressure_curve = ResponseCurve::linear(0.1, 5.0);  // 刻意設極端值

    expect(approx(stroke_width_at(*stroke, brush, 0), 3.0), "恆寬筆忽略壓力（最小壓力）");
    expect(approx(stroke_width_at(*stroke, brush, 1), 3.0), "恆寬筆忽略壓力（最大壓力）");
}

void test_pressure_driven_width() {
    std::vector<InkSample> samples{make_sample(0, 0, 0, 0), make_sample(10000, 0, 255, 10)};
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    BrushStyle brush;
    brush.width_mode = WidthMode::pressure_driven;
    brush.base_width = 2.0;
    brush.pressure_curve = ResponseCurve::linear(0.5, 2.0);

    expect(approx(stroke_width_at(*stroke, brush, 0), 1.0), "最小壓力 → 2.0 * 0.5");
    expect(approx(stroke_width_at(*stroke, brush, 1), 4.0, 1e-6), "最大壓力 → 2.0 * 2.0");
}

void test_velocity_driven_width() {
    // 第二點快速移動，第三點幾乎不動。
    std::vector<InkSample> samples{
        make_sample(0, 0, 128, 0),
        make_sample(32768, 0, 128, 100),  // 約 2000 px/s
        make_sample(32800, 0, 128, 200),  // 幾乎靜止
    };
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    BrushStyle brush;
    brush.width_mode = WidthMode::velocity_driven;
    brush.base_width = 4.0;
    brush.velocity_reference = 2000.0;
    // 越快越細。
    brush.velocity_curve = ResponseCurve::linear(1.0, 0.25);

    const double fast = stroke_width_at(*stroke, brush, 1);
    const double slow = stroke_width_at(*stroke, brush, 2);
    expect(fast < slow, "速度驅動：快筆較細");
    expect(approx(fast, 1.0, 0.05), "接近參考速度 → 倍率接近 0.25");
    expect(approx(slow, 4.0, 0.2), "近乎靜止 → 倍率接近 1.0");
}

void test_pressure_and_velocity_multiply() {
    std::vector<InkSample> samples{make_sample(0, 0, 255, 0), make_sample(32768, 0, 255, 100)};
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    BrushStyle brush;
    brush.width_mode = WidthMode::pressure_and_velocity;
    brush.base_width = 2.0;
    brush.pressure_curve = ResponseCurve::constant(3.0);
    brush.velocity_curve = ResponseCurve::constant(0.5);

    expect(approx(stroke_width_at(*stroke, brush, 1), 3.0), "兩者相乘：2.0 * 3.0 * 0.5");
}

// --- 橡皮擦：三種共用同一運算（INK-4）---

void test_erase_whole_stroke() {
    std::vector<InkSample> samples;
    for (std::uint16_t i = 0; i < 10; ++i) {
        samples.push_back(make_sample(static_cast<std::uint16_t>(i * 1000), 0, 128, i));
    }
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    const SampleRange all{0, 10};
    const auto result = erase_ranges(*stroke, std::span<const SampleRange>(&all, 1));
    expect(result.empty(), "整段擦後沒有剩餘子筆畫");
}

void test_erase_middle_splits_into_two() {
    std::vector<InkSample> samples;
    for (std::uint16_t i = 0; i < 10; ++i) {
        samples.push_back(make_sample(static_cast<std::uint16_t>(i * 1000), 0, 128, i));
    }
    auto stroke = make_intrusive<InkStroke>(make_brush(7), make_block(3), 400.0,
                                            std::move(samples));

    const SampleRange middle{4, 6};
    const auto result = erase_ranges(*stroke, std::span<const SampleRange>(&middle, 1));

    expect(result.size() == 2, "分段擦中段 → 兩個子筆畫");
    expect(result[0]->sample_count() == 4, "第一段保留 [0,4)");
    expect(result[1]->sample_count() == 4, "第二段保留 [6,10)");
    expect(result[0]->brush() == make_brush(7), "子筆畫沿用原筆刷");
    expect(result[0]->anchor_block() == make_block(3), "子筆畫沿用原錨點");
    expect(approx(result[0]->capture_block_width(), 400.0), "子筆畫沿用擷取寬度");
}

void test_erase_many_small_ranges_like_pixel_eraser() {
    // 像素擦實為細粒度分段擦：多個細碎範圍。
    std::vector<InkSample> samples;
    for (std::uint16_t i = 0; i < 20; ++i) {
        samples.push_back(make_sample(static_cast<std::uint16_t>(i * 500), 0, 128, i));
    }
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    const SampleRange ranges[] = {{3, 5}, {9, 11}, {15, 16}};
    const auto result = erase_ranges(*stroke, ranges);

    // 保留 [0,3)=3、[5,9)=4、[11,15)=4、[16,20)=4 → 四段
    expect(result.size() == 4, "多個細碎範圍 → 四個子筆畫");
    expect(result[0]->sample_count() == 3, "第一段 3 點");
    expect(result[1]->sample_count() == 4, "第二段 4 點");
    expect(result[3]->sample_count() == 4, "最後一段 4 點");
}

void test_erase_discards_single_point_fragments() {
    std::vector<InkSample> samples;
    for (std::uint16_t i = 0; i < 5; ++i) {
        samples.push_back(make_sample(static_cast<std::uint16_t>(i * 1000), 0, 128, i));
    }
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    // 移除 [1,4) 後剩下 [0,1) 與 [4,5)，兩者都只有一點。
    const SampleRange range{1, 4};
    const auto result = erase_ranges(*stroke, std::span<const SampleRange>(&range, 1));
    expect(result.empty(), "單點碎片被捨棄——單一取樣點無法構成可見筆畫");
}

void test_erase_nothing_returns_equivalent_stroke() {
    std::vector<InkSample> samples;
    for (std::uint16_t i = 0; i < 6; ++i) {
        samples.push_back(make_sample(static_cast<std::uint16_t>(i * 1000), 0, 128, i));
    }
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    const auto result = erase_ranges(*stroke, {});
    expect(result.size() == 1, "未移除任何範圍 → 單一子筆畫");
    expect(result[0]->sample_count() == 6, "全部取樣點保留");
}

// --- 筆刷可事後修改，外框為衍生資料 ---

void test_changing_brush_changes_width_without_touching_samples() {
    std::vector<InkSample> samples{make_sample(0, 0, 255, 0), make_sample(10000, 0, 255, 10)};
    auto stroke = make_intrusive<InkStroke>(make_brush(1), make_block(1), 400.0,
                                            std::move(samples));

    BrushStyle thin;
    thin.width_mode = WidthMode::pressure_driven;
    thin.base_width = 1.0;
    thin.pressure_curve = ResponseCurve::constant(1.0);

    BrushStyle thick = thin;
    thick.base_width = 8.0;

    expect(approx(stroke_width_at(*stroke, thin, 1), 1.0), "細筆刷");
    expect(approx(stroke_width_at(*stroke, thick, 1), 8.0), "同一筆跡換筆刷後變粗");
    expect(stroke->sample_count() == 2, "取樣點未被修改——外框是衍生資料");
}

}  // namespace

int main() {
    test_sample_encoding_roundtrip();
    test_encoded_size_is_nine_bytes();

    test_normalized_x_scales_with_block_width();
    test_vertical_is_independent_of_width();

    test_velocity_uses_capture_width_not_current_width();
    test_zero_time_delta_yields_zero_velocity();

    test_response_curve_endpoints_and_interpolation();

    test_constant_width_brush_ignores_pressure();
    test_pressure_driven_width();
    test_velocity_driven_width();
    test_pressure_and_velocity_multiply();

    test_erase_whole_stroke();
    test_erase_middle_splits_into_two();
    test_erase_many_small_ranges_like_pixel_eraser();
    test_erase_discards_single_point_fragments();
    test_erase_nothing_returns_equivalent_stroke();

    test_changing_brush_changes_width_without_touching_samples();

    shutdown_default_reclamation_queue();
    return krepis_test::report("krepis.ink_types");
}
