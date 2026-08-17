// Spike 7：雙向文字（UAX #9）—— 文字堆疊的最後一塊
//
// Spike 5（HarfBuzz shaping）與 Spike 6（libunibreak 斷行／分群）已通過。
// 剩下 bidi。若這塊也不需要 ICU，整個文字堆疊就能維持在約 1 MB。
//
// 要回答的問題：
//   1. SheenBidi 能不能取代 ICU 的 bidi？
//   2. 二進位成本多少？
//   3. 混排與**數字**的重排結果正確嗎？（數字是 bidi 最容易做錯的地方）
//
// 為何 bidi 必須在 C++：重排錯誤是**靜默**的——文字仍然顯示，只是順序錯了，
// 而寫程式的人若不讀阿拉伯文根本不會發現。依 FND-0001 的判準，這屬於必須可審查的部分。

// SheenBidi 的 header **沒有 extern "C" 護欄**，從 C++ 引入會使符號被 C++ mangling，
// 連結時全部找不到。這是整合成本之一，必須自行包裝。
extern "C" {
#include <SheenBidi.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct BidiSample {
    const char* name;
    const char* utf8;
    SBLevel base_direction;  // SBLevelDefaultLTR / SBLevelDefaultRTL
    const char* expectation;
};

void report_bidi(const BidiSample& sample) {
    const std::size_t length = std::strlen(sample.utf8);

    SBCodepointSequence sequence{SBStringEncodingUTF8,
                                 const_cast<void*>(static_cast<const void*>(sample.utf8)),
                                 length};

    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    SBParagraphRef paragraph =
        SBAlgorithmCreateParagraph(algorithm, 0, INT32_MAX, sample.base_direction);
    const SBUInteger paragraph_length = SBParagraphGetLength(paragraph);
    SBLineRef line = SBParagraphCreateLine(paragraph, 0, paragraph_length);

    const SBUInteger run_count = SBLineGetRunCount(line);
    const SBRun* runs = SBLineGetRunsPtr(line);
    const SBLevel base_level = SBParagraphGetBaseLevel(paragraph);

    std::printf("\n  %s\n", sample.name);
    std::printf("    輸入：\"%s\"（%zu bytes）\n", sample.utf8, length);
    std::printf("    段落基準 level：%u（%s）\n", static_cast<unsigned>(base_level),
                (base_level % 2 == 0) ? "LTR" : "RTL");
    std::printf("    視覺順序的 run 數：%zu\n", static_cast<std::size_t>(run_count));

    for (SBUInteger i = 0; i < run_count; ++i) {
        const bool rtl = (runs[i].level % 2) != 0;
        // 取出該 run 的位元組內容，供人工核對。
        std::string fragment(sample.utf8 + runs[i].offset, runs[i].length);
        std::printf("      run %zu：offset=%zu length=%zu level=%u（%s）\"%s\"\n",
                    static_cast<std::size_t>(i), static_cast<std::size_t>(runs[i].offset),
                    static_cast<std::size_t>(runs[i].length),
                    static_cast<unsigned>(runs[i].level), rtl ? "RTL" : "LTR", fragment.c_str());
    }

    std::printf("    預期：%s\n", sample.expectation);

    SBLineRelease(line);
    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
}

}  // namespace

int main() {
    std::printf("# Spike 7：雙向文字（SheenBidi）\n");

    const BidiSample samples[] = {
        {
            "純拉丁（LTR 基準）",
            "Hello world",
            SBLevelDefaultLTR,
            "1 個 LTR run，level 0",
        },
        {
            "純阿拉伯（自動偵測基準）",
            "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",  // مرحبا
            SBLevelDefaultLTR,
            "基準應自動偵測為 RTL（level 1），1 個 RTL run",
        },
        {
            "英文句中夾阿拉伯",
            "The word \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 means hello",
            SBLevelDefaultLTR,
            "3 個 run：LTR / RTL / LTR",
        },
        {
            "阿拉伯句中夾英文",
            "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 Krepis \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",
            SBLevelDefaultLTR,
            "基準 RTL；英文為 level 2 的 LTR run 嵌在其中",
        },
        {
            "阿拉伯文中的歐洲數字（最易錯）",
            "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 2026 \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",
            SBLevelDefaultLTR,
            "數字必須是 level 2（RTL 中的 LTR），否則 2026 會被反轉成 6202",
        },
        {
            "中性字元夾在方向相反之間",
            "abc \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7, def",
            SBLevelDefaultLTR,
            "逗號與空白依 UAX #9 的中性字元規則歸屬",
        },
        {
            // SBLevelDefaultRTL 是「**若無強方向字元**則用 RTL」，不是「強制 RTL」。
            // "Hello world" 有強 LTR 字元，因此自動偵測為 LTR —— 這是正確的 UAX #9 行為。
            "SBLevelDefaultRTL ＋ 有強 LTR 字元",
            "Hello world",
            SBLevelDefaultRTL,
            "自動偵測應勝出為 LTR（level 0）——Default 系列是 fallback 而非強制",
        },
        {
            // 無強方向字元（只有數字與標點），此時 fallback 才生效。
            "SBLevelDefaultRTL ＋ 無強方向字元",
            "123 456.",
            SBLevelDefaultRTL,
            "無強字元，fallback 生效，基準應為 RTL（level 1）",
        },
        {
            // 真正的強制：直接傳明確 level，不用 Default 常數。
            "明確 level 1（真正強制 RTL）",
            "Hello world",
            1,
            "基準 level 1；拉丁文為 level 2 的 LTR run 嵌在其中",
        },
    };

    std::printf("\n## 重排結果\n");
    for (const auto& sample : samples) {
        report_bidi(sample);
    }

    std::printf("\n## 判讀提示\n");
    std::printf("  - run 的 level 偶數為 LTR、奇數為 RTL。\n");
    std::printf("  - **數字那一項最關鍵**：歐洲數字在阿拉伯文中必須是 level 2。\n");
    std::printf("    若被算成 level 1（RTL），2026 會顯示成 6202——而寫程式的人不會發現。\n");
    std::printf("  - run 陣列已是**視覺順序**；渲染時依序擺放即可，不需自行反轉。\n");

    return 0;
}
