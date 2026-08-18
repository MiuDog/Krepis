#include "krepis/ink_types.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace krepis {
namespace {

// 儲存採 big-endian，與 ObjectId 的正規編碼保持同一慣例。
void write_u16(std::byte* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::byte>((value >> 8) & 0xFF);
    out[1] = static_cast<std::byte>(value & 0xFF);
}

std::uint16_t read_u16(const std::byte* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

constexpr double normalized_scale = 65535.0;
constexpr double pressure_scale = 255.0;
constexpr double altitude_scale = 90.0 / 255.0;
constexpr double azimuth_scale = 360.0 / 255.0;

}  // namespace

// --- InkSample 編解碼 ---

void encode_ink_sample(const InkSample& sample,
                       std::span<std::byte, ink_sample_encoded_size> out) noexcept {
    write_u16(out.data() + 0, sample.x_normalized);
    write_u16(out.data() + 2, static_cast<std::uint16_t>(sample.y_fixed));
    out[4] = static_cast<std::byte>(sample.pressure);
    out[5] = static_cast<std::byte>(sample.tilt_altitude);
    out[6] = static_cast<std::byte>(sample.tilt_azimuth);
    write_u16(out.data() + 7, sample.dt_ms);
}

InkSample decode_ink_sample(std::span<const std::byte, ink_sample_encoded_size> bytes) noexcept {
    InkSample sample;
    sample.x_normalized = read_u16(bytes.data() + 0);
    sample.y_fixed = static_cast<std::int16_t>(read_u16(bytes.data() + 2));
    sample.pressure = static_cast<std::uint8_t>(bytes[4]);
    sample.tilt_altitude = static_cast<std::uint8_t>(bytes[5]);
    sample.tilt_azimuth = static_cast<std::uint8_t>(bytes[6]);
    sample.dt_ms = read_u16(bytes.data() + 7);
    return sample;
}

// --- ResponseCurve ---

double ResponseCurve::evaluate(double input) const noexcept {
    if (!(input > 0.0)) {  // 同時處理 NaN
        return multipliers.front();
    }
    if (input >= 1.0) {
        return multipliers.back();
    }

    constexpr double segment_count = static_cast<double>(control_point_count - 1);
    const double scaled = input * segment_count;
    const auto index = static_cast<std::size_t>(scaled);
    const double fraction = scaled - static_cast<double>(index);

    // index 必定 < control_point_count - 1，因為 input < 1.0。
    return multipliers[index] * (1.0 - fraction) + multipliers[index + 1] * fraction;
}

ResponseCurve ResponseCurve::constant(double multiplier) noexcept {
    ResponseCurve curve;
    curve.multipliers.fill(multiplier);
    return curve;
}

ResponseCurve ResponseCurve::linear(double low, double high) noexcept {
    ResponseCurve curve;
    constexpr double segment_count = static_cast<double>(control_point_count - 1);
    for (std::size_t i = 0; i < control_point_count; ++i) {
        const double t = static_cast<double>(i) / segment_count;
        curve.multipliers[i] = low * (1.0 - t) + high * t;
    }
    return curve;
}

// --- InkStroke ---

InkStroke::InkStroke(BrushId brush, BlockId anchor_block, double capture_block_width_pixels,
                     std::vector<InkSample> samples) noexcept
    : brush_(brush),
      anchor_block_(anchor_block),
      capture_block_width_(capture_block_width_pixels),
      samples_(std::move(samples)) {
    assert(!samples_.empty() && "筆畫不得為空");
    assert(capture_block_width_ > 0.0 && "擷取寬度必須為正——速度計算需要它");
}

InkSampleValue InkStroke::value_at(std::size_t index, double block_width_pixels) const noexcept {
    assert(index < samples_.size());
    const InkSample& sample = samples_[index];

    InkSampleValue value;
    // 水平：正規化比例乘上當前寬度。**這就是全寬切換的等比拉伸**，不需額外狀態。
    value.x_pixels =
        (static_cast<double>(sample.x_normalized) / normalized_scale) * block_width_pixels;
    // 垂直：0.01mm 定點，與寬度無關。
    value.y_pixels = static_cast<double>(sample.y_fixed) / ink_fixed_units_per_mm;
    value.pressure = static_cast<double>(sample.pressure) / pressure_scale;
    value.tilt_altitude_deg = static_cast<double>(sample.tilt_altitude) * altitude_scale;
    value.tilt_azimuth_deg = static_cast<double>(sample.tilt_azimuth) * azimuth_scale;
    value.time_seconds = static_cast<double>(sample.dt_ms) / 1000.0;
    return value;
}

double InkStroke::velocity_at(std::size_t index) const noexcept {
    assert(index < samples_.size());
    if (index == 0) {
        return 0.0;
    }

    // **必須以擷取寬度計算**：x 是正規化的，用當前寬度會使同一筆在不同寬度下
    // 算出不同速度、進而不同粗細。
    const InkSampleValue current = value_at(index, capture_block_width_);
    const InkSampleValue previous = value_at(index - 1, capture_block_width_);

    const double dt = current.time_seconds - previous.time_seconds;
    if (!(dt > 0.0)) {
        // 取樣時間相同（或時鐘倒退）時無法定義速度。
        return 0.0;
    }

    const double dx = current.x_pixels - previous.x_pixels;
    const double dy = current.y_pixels - previous.y_pixels;
    return std::sqrt(dx * dx + dy * dy) / dt;
}

double stroke_width_at(const InkStroke& stroke, const BrushStyle& brush,
                       std::size_t index) noexcept {
    assert(index < stroke.sample_count());

    double multiplier = 1.0;

    const bool uses_pressure = brush.width_mode == WidthMode::pressure_driven ||
                               brush.width_mode == WidthMode::pressure_and_velocity;
    const bool uses_velocity = brush.width_mode == WidthMode::velocity_driven ||
                               brush.width_mode == WidthMode::pressure_and_velocity;

    if (uses_pressure) {
        const double pressure = stroke.value_at(index, stroke.capture_block_width()).pressure;
        multiplier *= brush.pressure_curve.evaluate(pressure);
    }

    if (uses_velocity) {
        const double reference = brush.velocity_reference;
        const double normalized =
            (reference > 0.0) ? (stroke.velocity_at(index) / reference) : 0.0;
        multiplier *= brush.velocity_curve.evaluate(normalized);
    }

    return brush.base_width * multiplier;
}

// --- 橡皮擦的共用運算 ---

std::vector<IntrusivePtr<const InkStroke>> erase_ranges(const InkStroke& stroke,
                                                        std::span<const SampleRange> removed) {
    const std::size_t count = stroke.sample_count();
    auto samples = stroke.samples();

    std::vector<IntrusivePtr<const InkStroke>> result;

    std::size_t cursor = 0;
    auto emit_run = [&](std::size_t begin, std::size_t end) {
        // 單一取樣點無法構成可見筆畫，捨棄。
        if (end <= begin + 1) {
            return;
        }
        std::vector<InkSample> run(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                                   samples.begin() + static_cast<std::ptrdiff_t>(end));
        result.push_back(make_intrusive<InkStroke>(stroke.brush(), stroke.anchor_block(),
                                                   stroke.capture_block_width(), std::move(run)));
    };

    for (const SampleRange& range : removed) {
        if (range.is_empty()) {
            continue;
        }
        assert(range.begin >= cursor && "removed 範圍必須遞增且不重疊");
        emit_run(cursor, std::min(range.begin, count));
        cursor = std::max(cursor, std::min(range.end, count));
    }
    emit_run(cursor, count);

    return result;
}

}  // namespace krepis
