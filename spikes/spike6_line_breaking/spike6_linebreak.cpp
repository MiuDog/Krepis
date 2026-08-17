// Spike 6：不引入 ICU 的斷行與分群
//
// 這是 Spike 5 之後**唯一可能推翻其結論的項目**。Spike 5 得出「HarfBuzz 772 KB 可接受」，
// 但那只涵蓋 shaping。若斷行（UAX #14）與 grapheme 分群（UAX #29）必須靠 ICU，
// 體積會從 ~1 MB 跳到數十 MB，「772 KB 可接受」就失去意義。
//
// 候選：libunibreak —— 專做 UAX #14 斷行與 UAX #29 word／grapheme 分群的小型 C 函式庫。
//
// 要回答的問題：
//   1. 能不能取代 ICU 的 BreakIterator？
//   2. 二進位成本多少？
//   3. CJK、Latin、Thai 的斷行結果正確嗎？
//   4. grapheme 分群能不能支撐游標移動？（錯了使用者立刻看得見）

#include <linebreak.h>
#include <linebreakdef.h>
#include <graphemebreak.h>
#include <wordbreak.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct BreakSample {
    const char* name;
    const char* utf8;
    const char* lang;  // nullptr = 預設
};

const char* break_kind(char code) {
    switch (code) {
        case LINEBREAK_MUSTBREAK: return "MUST";
        case LINEBREAK_ALLOWBREAK: return "ALLOW";
        case LINEBREAK_NOBREAK: return "NONE";
        case LINEBREAK_INDETERMINATE: return "INDET";
        case LINEBREAK_INSIDEACHAR: return "INSIDE";
        default: return "?";
    }
}

void report_line_breaks(const BreakSample& sample) {
    const std::size_t length = std::strlen(sample.utf8);
    std::vector<char> breaks(length);

    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(sample.utf8), length, sample.lang,
                        breaks.data());

    std::size_t allow_count = 0;
    std::size_t must_count = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (breaks[i] == LINEBREAK_ALLOWBREAK) ++allow_count;
        if (breaks[i] == LINEBREAK_MUSTBREAK) ++must_count;
    }

    std::printf("\n  %s\n", sample.name);
    std::printf("    輸入：\"%s\"（%zu bytes）\n", sample.utf8, length);
    std::printf("    可斷點：%zu　強制斷點：%zu\n", allow_count, must_count);

    // 印出斷點位置，供人工核對。
    std::printf("    斷點位置：");
    for (std::size_t i = 0; i < length; ++i) {
        if (breaks[i] == LINEBREAK_ALLOWBREAK || breaks[i] == LINEBREAK_MUSTBREAK) {
            std::printf("[%zu:%s] ", i, break_kind(breaks[i]));
        }
    }
    std::printf("\n");
}

void report_grapheme_breaks(const char* name, const char* utf8) {
    const std::size_t length = std::strlen(utf8);
    std::vector<char> breaks(length);

    set_graphemebreaks_utf8(reinterpret_cast<const utf8_t*>(utf8), length, "en", breaks.data());

    std::size_t cluster_count = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (breaks[i] == GRAPHEMEBREAK_BREAK) ++cluster_count;
    }

    std::printf("\n  %s\n", utf8);
    std::printf("    %s：%zu bytes → %zu 個 grapheme cluster 邊界\n", name, length, cluster_count);
}

}  // namespace

int main() {
    std::printf("# Spike 6：不引入 ICU 的斷行與分群（libunibreak）\n\n");

    init_linebreak();
    init_graphemebreak();
    init_wordbreak();
    std::printf("libunibreak 初始化完成\n");

    std::printf("\n## UAX #14 斷行\n");

    const BreakSample samples[] = {
        {"英文（空白斷行）", "The quick brown fox jumps", "en"},
        {"英文（連字號）", "well-known state-of-the-art", "en"},
        {"CJK 繁中（逐字可斷，但標點禁則）", "結構化筆記基座，是一個實驗。", "zh"},
        {"CJK 禁則（行首不得為句號）", "測試。測試", "zh"},
        {"混排中英", "使用 HarfBuzz 做 shaping", "zh"},
        {"不可斷的 URL", "https://example.com/path", "en"},
    };

    for (const auto& sample : samples) {
        report_line_breaks(sample);
    }

    std::printf("\n## UAX #29 grapheme 分群（游標移動用）\n");

    report_grapheme_breaks("拉丁預組", "cafe\xCC\x81");
    report_grapheme_breaks("Emoji ZWJ 序列",
                           "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                           "\xF0\x9F\x91\xA6");
    report_grapheme_breaks("天城文組合", "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7");

    std::printf("\n## 判讀提示\n");
    std::printf("  - CJK 應該幾乎每個字之間都可斷，但標點前後受禁則限制。\n");
    std::printf("  - 「測試。測試」的句號**後面**可斷，句號**前面**不可斷（行首禁則）。\n");
    std::printf("  - Emoji ZWJ 家族序列應為 **1 個** cluster——若是 3 個，游標會走進 emoji 內部。\n");
    std::printf("  - URL 內部不應有可斷點（除非明確允許）。\n");

    return 0;
}
