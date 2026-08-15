#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace krepis::spike1 {

// 純值型別：單一 Glyph 幾何與索引資訊（跨平台通用表示）
struct GlyphInfo {
    uint16_t glyph_index{0};
    float advance_x{0.0f};
    float advance_y{0.0f};
    float offset_x{0.0f};
    float offset_y{0.0f};
};

// 純值型別：Shaped Run（單一字型、單一方向的字形序列）
struct ShapedRun {
    std::string font_family;
    float font_size{16.0f};
    uint8_t bidi_level{0}; // 0 = LTR, 1 = RTL
    bool is_rtl{false};
    
    // 文字對應範圍（UTF-16 碼元位移與長度）
    uint32_t text_start{0};
    uint32_t text_length{0};

    // 每個字符對應到的第一個 Glyph index
    std::vector<uint16_t> cluster_map;
    
    // Glyph 幾何序列
    std::vector<GlyphInfo> glyphs;

    // 該 Run 的總寬度與尺寸
    float total_advance{0.0f};
    float ascent{0.0f};
    float descent{0.0f};
    float line_gap{0.0f};
};

// 純值型別：單行或單段文字經 Shaping 後的完整結果
struct ShapedParagraph {
    std::string text_utf8;
    std::vector<ShapedRun> runs;
    float total_width{0.0f};
    float height{0.0f};
};

// DirectWrite Shaper 介面與工廠
class DirectWriteShaper {
public:
    virtual ~DirectWriteShaper() = default;
    
    // 對 UTF-8 字串進行 Shaping，產出純值型別結構
    virtual ShapedParagraph shape(std::string_view utf8_text, 
                                  std::string_view font_family = "Segoe UI", 
                                  float font_size = 16.0f,
                                  std::string_view locale = "zh-TW") = 0;

    static std::unique_ptr<DirectWriteShaper> create();
};

} // namespace krepis::spike1
