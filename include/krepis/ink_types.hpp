#pragma once

// Ink 資料模型（INK-1＝B ＋ tilt、INK-2＝C，2026-08-18 人類裁決）。
//
// **不含繪製**——實際畫出來屬於平台外殼（05-ink README）。本檔只定義
// 「一筆手寫是什麼」與「怎麼從取樣點算出粗細」。
//
// 核心結論：**筆畫外框是衍生資料，不得儲存。**
// 筆刷可自訂（恆寬／壓力／速度驅動、可調平滑度），若把外框存起來，
// 改筆刷設定時舊筆跡不會跟著變。這也使 INK-1 選「量化保留」而非「曲線簡化」
// 成為**必要而非偏好**：簡化會破壞速度資訊，而速度是粗細的輸入。

#include "krepis/intrusive_ptr.hpp"
#include "krepis/object_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace krepis {

// 量化後的取樣點（INK-1＝B）。
//
// 責任：以最小體積保存數位板給的每一個取樣。
// 不負責：表達真實座標——`x` 是正規化比例，需搭配 Block 寬度還原（INK-2＝C）。
// 維持的不變條件：欄位皆為量化整數，解碼後才有物理意義。
// 生命週期：值型別。
//
// **為何保留 tilt 與 dt**：tilt 由使用者明確要求；dt 是速度的來源，
// 而速度驅動粗細，因此兩者都不能像原方案那樣丟棄。
struct InkSample {
    // 正規化到 Block 寬度的水平位置：0 → 左緣，65535 → 右緣（INK-2＝C）。
    // 正規化使「一般↔全寬切換時水平等比拉伸」變成免費——不需額外狀態。
    std::uint16_t x_normalized = 0;

    // 相對 Block 頂部的垂直位置，單位 0.01mm，範圍 ±327.67mm。
    // **刻意不正規化**：Block 高度會隨內容變動，正規化會使筆跡在文字增減時垂直漂移，
    // 而已定語意是「垂直方向不變」。
    std::int16_t y_fixed = 0;

    std::uint8_t pressure = 0;        // 0–255
    std::uint8_t tilt_altitude = 0;   // 0–90 度，精度 0.35 度
    std::uint8_t tilt_azimuth = 0;    // 0–360 度，精度 1.4 度（INK-7 待裁決是否足夠）

    // 距筆畫起點的毫秒差。單筆上限 65.5 秒。
    std::uint16_t dt_ms = 0;
};

// 儲存格式的位元組數。**與 sizeof(InkSample) 不同**——後者含對齊填充。
inline constexpr std::size_t ink_sample_encoded_size = 9;

void encode_ink_sample(const InkSample& sample,
                       std::span<std::byte, ink_sample_encoded_size> out) noexcept;
[[nodiscard]] InkSample decode_ink_sample(
    std::span<const std::byte, ink_sample_encoded_size> bytes) noexcept;

// 解碼後的取樣點，供計算使用。
struct InkSampleValue {
    double x_pixels = 0.0;        // 相對 Block 左緣
    double y_pixels = 0.0;        // 相對 Block 頂部
    double pressure = 0.0;        // 0–1
    double tilt_altitude_deg = 0.0;
    double tilt_azimuth_deg = 0.0;
    double time_seconds = 0.0;
};

// 每毫米對應的像素數。座標換算需要它，因為 y 以 0.01mm 儲存而輸出是像素。
inline constexpr double ink_fixed_units_per_mm = 100.0;

// 粗細映射曲線：以等距控制點做分段線性內插。
//
// 責任：把 0–1 的輸入（壓力或正規化速度）映射為粗細倍率。
// 不負責：決定輸入怎麼來——那是 BrushStyle 的 width_mode。
// 維持的不變條件：控制點等距分布於 [0, 1]。
struct ResponseCurve {
    static constexpr std::size_t control_point_count = 5;

    // 對應輸入 0、0.25、0.5、0.75、1.0 的倍率。
    std::array<double, control_point_count> multipliers{1.0, 1.0, 1.0, 1.0, 1.0};

    // 輸入超出 [0,1] 時鉗制於端點。
    [[nodiscard]] double evaluate(double input) const noexcept;

    [[nodiscard]] static ResponseCurve constant(double multiplier) noexcept;
    // 線性由 low 到 high。
    [[nodiscard]] static ResponseCurve linear(double low, double high) noexcept;
};

enum class WidthMode : std::uint8_t {
    constant_width,        // 恆寬筆
    pressure_driven,       // 壓力決定粗細
    velocity_driven,       // 速度決定粗細
    pressure_and_velocity, // 兩者相乘
};

// 筆刷設定。**獨立實體、可事後修改**——修改後所有引用它的筆跡都會重新算出新外框。
//
// 責任：定義「取樣點如何變成粗細」。
// 不負責：保存幾何——外框是衍生資料，不得儲存。
// 生命週期：值型別；由穩定的 BrushId 引用。
struct BrushStyle {
    WidthMode width_mode = WidthMode::pressure_driven;

    // 倍率為 1.0 時的粗細（像素）。
    double base_width = 2.0;

    ResponseCurve pressure_curve{};
    ResponseCurve velocity_curve{};

    // 對應 velocity_curve 輸入 1.0 的速度（像素／秒）。超過此速度則鉗制。
    double velocity_reference = 1000.0;

    // 平滑度 0–1。0 為不平滑。**此參數只影響外框產生，不改動取樣點。**
    double smoothing = 0.0;
};

// 一筆手寫。
//
// 責任：保存原始取樣點與其所屬 Block、所用筆刷。
// 不負責：保存外框（衍生資料）、保存繪製狀態（INK-3＝A：繪製中由外殼擁有）。
// 維持的不變條件：samples 非空；dt_ms 單調不減。
// 擁有哪些資源：samples 的儲存空間。
// 生命週期：不可變；以 IntrusivePtr 共享，使 undo 歷史與各 revision 不需複製取樣點。
// 執行緒安全程度：不可變，可跨執行緒共享。
//
// **跨實體關係一律以穩定 ID 表達**（brush_id、anchor_block），不持有 IntrusivePtr——
// 與 ObjectRecord 的 owning edge 限制同理，避免 reclamation 回收不掉的循環。
class InkStroke final : public RefCounted {
public:
    InkStroke(BrushId brush, BlockId anchor_block, double capture_block_width_pixels,
              std::vector<InkSample> samples) noexcept;

    [[nodiscard]] BrushId brush() const noexcept { return brush_; }
    [[nodiscard]] BlockId anchor_block() const noexcept { return anchor_block_; }
    [[nodiscard]] std::span<const InkSample> samples() const noexcept { return samples_; }
    [[nodiscard]] std::size_t sample_count() const noexcept { return samples_.size(); }

    // 擷取當下的 Block 寬度（像素）。
    //
    // **為何必須保存**：x 是正規化的（INK-2＝C），而速度需要真實距離。
    // 沒有擷取寬度就無法還原書寫當下的真實速度，
    // 同一筆在不同寬度的 Block 上會算出不同速度、進而不同粗細——
    // 那是使用者看得見卻無法解釋的行為。
    [[nodiscard]] double capture_block_width() const noexcept { return capture_block_width_; }

    // 以指定的 Block 寬度解碼取樣點。寬度不同即為「一般↔全寬」的水平等比拉伸。
    [[nodiscard]] InkSampleValue value_at(std::size_t index,
                                          double block_width_pixels) const noexcept;

    // 該取樣點的瞬時速度（像素／秒），以**擷取當下的寬度**計算。
    // 首點回傳 0。
    [[nodiscard]] double velocity_at(std::size_t index) const noexcept;

private:
    BrushId brush_;
    BlockId anchor_block_;
    double capture_block_width_;
    std::vector<InkSample> samples_;
};

// 依筆刷算出該取樣點的粗細（像素）。
[[nodiscard]] double stroke_width_at(const InkStroke& stroke, const BrushStyle& brush,
                                     std::size_t index) noexcept;

// 半開區間 [begin, end)。
struct SampleRange {
    std::size_t begin = 0;
    std::size_t end = 0;

    [[nodiscard]] bool is_empty() const noexcept { return begin >= end; }
};

// 移除指定範圍後剩下的子筆畫。
//
// **三種橡皮擦共用此單一運算**（INK-4）：
//   - 整段筆跡擦 → 移除 [0, sample_count)
//   - 分段擦     → 移除中間某一段
//   - 像素擦     → 以圓形筆頭做幾何相減後得到多個細碎範圍
//
// 像素擦**不得**以點陣化實作：點陣化後就無法再依筆刷重算外框，
// 與「筆刷可事後修改」直接衝突。它實為細粒度的分段擦。
//
// 前置條件：removed 的範圍遞增且不重疊。
// 回傳的子筆畫沿用原筆刷、錨點與擷取寬度；單點以下的碎片會被捨棄
// （單一取樣點無法構成可見筆畫）。
[[nodiscard]] std::vector<IntrusivePtr<const InkStroke>> erase_ranges(
    const InkStroke& stroke, std::span<const SampleRange> removed);

}  // namespace krepis
