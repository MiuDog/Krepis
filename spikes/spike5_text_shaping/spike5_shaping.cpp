// Spike 5：跨平台 text shaping stack 可行性
//
// 背景：使用者於 2026-08-17 確認平台需求為 **Windows + Linux + iPad 必要**，
// 其後延伸 macOS 與 Android。這使「各平台用原生 API」的方案出局——
// DirectWrite（Win）＋ CoreText（iPad／macOS）＋ 其他（Linux／Android）
// 會形成三套以上的 shaping 實作，而 FND-0001 明文禁止一條規則有多個實作
// （雙實作必然靜默分岔）。
//
// 因此本 spike 驗證單一跨平台 stack：**HarfBuzz 負責 shaping**。
// 本 spike **不涵蓋** bidi 與 grapheme 分段（SheenBidi／utf8proc），
// 那是選型的第二階段；先確認最大的一塊能不能用。
//
// 要回答的問題：
//   1. FetchContent 能不能取得並建置 HarfBuzz？（FND-0003 已定案用 FetchContent）
//   2. 二進位大小增加多少？
//   3. 混排文字（拉丁／CJK／阿拉伯）的 shaping 結果正確嗎？
//   4. API 複雜度——接一個 shaper 要多少程式碼？

#include <hb.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// 讀入整個字型檔。Spike 用途，不做健全的錯誤處理。
std::vector<char> read_file(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    std::vector<char> data(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size()) {
        return {};
    }
    return data;
}

struct ShapeSample {
    const char* name;
    const char* utf8;
    hb_script_t script;
    hb_direction_t direction;
};

void shape_and_report(hb_font_t* font, const ShapeSample& sample) {
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, sample.utf8, -1, 0, -1);
    hb_buffer_set_direction(buffer, sample.direction);
    hb_buffer_set_script(buffer, sample.script);
    hb_buffer_set_language(buffer, hb_language_get_default());

    hb_shape(font, buffer, nullptr, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &glyph_count);

    std::printf("\n  %s  (\"%s\")\n", sample.name, sample.utf8);
    std::printf("    glyph 數：%u\n", glyph_count);

    unsigned int notdef_count = 0;
    int total_advance = 0;
    for (unsigned int i = 0; i < glyph_count; ++i) {
        if (info[i].codepoint == 0) {
            ++notdef_count;
        }
        total_advance += pos[i].x_advance;
    }

    std::printf("    總 advance：%d\n", total_advance);
    std::printf("    .notdef（缺字）：%u\n", notdef_count);

    // 印出前幾個 glyph 供人工核對。
    std::printf("    前 8 個 glyph：");
    for (unsigned int i = 0; i < glyph_count && i < 8; ++i) {
        std::printf("[gid=%u adv=%d cluster=%u] ", info[i].codepoint, pos[i].x_advance,
                    info[i].cluster);
    }
    std::printf("\n");

    if (notdef_count == glyph_count && glyph_count > 0) {
        std::printf("    ** 全部缺字——此字型不支援該書寫系統（不是 HarfBuzz 的問題）**\n");
    }

    hb_buffer_destroy(buffer);
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("# Spike 5：跨平台 text shaping（HarfBuzz）\n\n");
    std::printf("HarfBuzz 版本：%s\n", hb_version_string());

    const char* font_path = (argc > 1) ? argv[1] :
#if defined(_WIN32)
                                       "C:/Windows/Fonts/segoeui.ttf";
#elif defined(__APPLE__)
                                       "/System/Library/Fonts/Helvetica.ttc";
#else
                                       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif

    std::printf("字型：%s\n", font_path);

    auto font_data = read_file(font_path);
    if (font_data.empty()) {
        std::printf("\n** 無法讀取字型檔。以 argv[1] 指定路徑後重跑。**\n");
        return 1;
    }
    std::printf("字型大小：%zu bytes\n", font_data.size());

    hb_blob_t* blob = hb_blob_create(font_data.data(), static_cast<unsigned int>(font_data.size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    hb_font_set_scale(font, 64 * 64, 64 * 64);  // 64pt，26.6 定點

    std::printf("\n## Shaping 結果\n");

    const ShapeSample samples[] = {
        {"拉丁（含 ligature 機會）", "Waffle office", HB_SCRIPT_LATIN, HB_DIRECTION_LTR},
        {"拉丁（kerning 機會）", "AVATAR To Yo", HB_SCRIPT_LATIN, HB_DIRECTION_LTR},
        {"CJK 繁體中文", "結構化筆記基座", HB_SCRIPT_HAN, HB_DIRECTION_LTR},
        {"阿拉伯文（RTL＋連字）", "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", HB_SCRIPT_ARABIC,
         HB_DIRECTION_RTL},
        {"組合字（e + 重音）", "cafe\xCC\x81", HB_SCRIPT_LATIN, HB_DIRECTION_LTR},
    };

    for (const auto& sample : samples) {
        shape_and_report(font, sample);
    }

    std::printf("\n## 判讀提示\n");
    std::printf("  - 「組合字」若 glyph 數 < 字元數，代表 HarfBuzz 正確合成了預組字形。\n");
    std::printf("  - 阿拉伯文若 glyph 數 < 字元數，代表連字（ligature）生效。\n");
    std::printf("  - 全部缺字通常是字型不含該書寫系統，需靠 font fallback 解決——\n");
    std::printf("    **font fallback 不是 HarfBuzz 的責任，仍需平台層提供字型清單。**\n");

    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    return 0;
}
