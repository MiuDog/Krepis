#include "spike1_shaper.hpp"

#include <windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>

#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace krepis::spike1 {

// 輔助函式：UTF-8 轉 UTF-16 (std::wstring)
static std::wstring utf8_to_utf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring utf16(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), utf16.data(), count);
    return utf16;
}

// 輔助函式：UTF-16 轉 UTF-8 (std::string)
static std::string utf16_to_utf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string utf8(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), utf8.data(), count, nullptr, nullptr);
    return utf8;
}

// 自訂 DirectWrite TextRenderer，用來攔截並抽取所有純值型別的 GlyphRun 資料
class CustomGlyphRunExtractor : public IDWriteTextRenderer {
public:
    CustomGlyphRunExtractor(IDWriteFactory* factory, ShapedParagraph* out_paragraph)
        : m_refCount(1), m_factory(factory), m_paragraph(out_paragraph) {}

    // IUnknown 介面實作
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *ppvObject = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG res = InterlockedDecrement(&m_refCount);
        if (res == 0) {
            delete this;
        }
        return res;
    }

    // IDWritePixelSnapping 介面實作
    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* isDisabled) override {
        *isDisabled = FALSE;
        return S_OK;
    }

    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        transform->m11 = 1.0f; transform->m12 = 0.0f;
        transform->m21 = 0.0f; transform->m22 = 1.0f;
        transform->dx = 0.0f; transform->dy = 0.0f;
        return S_OK;
    }

    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
        *pixelsPerDip = 1.0f;
        return S_OK;
    }

    // IDWriteTextRenderer 核心抽取：DrawGlyphRun
    IFACEMETHODIMP DrawGlyphRun(
        void* clientDrawingContext,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE measuringMode,
        DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
        IUnknown* clientDrawingEffect
    ) override {
        (void)clientDrawingContext;
        (void)baselineOriginX;
        (void)baselineOriginY;
        (void)measuringMode;
        (void)clientDrawingEffect;

        ShapedRun run;
        run.font_size = glyphRun->fontEmSize;
        run.bidi_level = static_cast<uint8_t>(glyphRun->bidiLevel);
        run.is_rtl = (glyphRun->bidiLevel & 1) != 0;

        // 取得 Font Metrics
        if (glyphRun->fontFace) {
            DWRITE_FONT_METRICS metrics;
            glyphRun->fontFace->GetMetrics(&metrics);
            float scale = glyphRun->fontEmSize / static_cast<float>(metrics.designUnitsPerEm);
            run.ascent = static_cast<float>(metrics.ascent) * scale;
            run.descent = static_cast<float>(metrics.descent) * scale;
            run.line_gap = static_cast<float>(metrics.lineGap) * scale;
        }

        // 抽取字形索引與幾何資料
        if (glyphRun->glyphCount > 0) {
            run.glyphs.reserve(glyphRun->glyphCount);
            float current_x = 0.0f;
            for (UINT32 i = 0; i < glyphRun->glyphCount; ++i) {
                GlyphInfo g;
                g.glyph_index = glyphRun->glyphIndices[i];
                g.advance_x = glyphRun->glyphAdvances ? glyphRun->glyphAdvances[i] : 0.0f;
                g.advance_y = 0.0f;
                if (glyphRun->glyphOffsets) {
                    g.offset_x = glyphRun->glyphOffsets[i].advanceOffset;
                    g.offset_y = glyphRun->glyphOffsets[i].ascenderOffset;
                }
                current_x += g.advance_x;
                run.glyphs.push_back(g);
            }
            run.total_advance = current_x;
        }

        // 抽取 Cluster Map 與文字對應資訊
        if (glyphRunDescription) {
            run.text_start = glyphRunDescription->textPosition;
            run.text_length = glyphRunDescription->stringLength;
            if (glyphRunDescription->clusterMap && glyphRunDescription->stringLength > 0) {
                run.cluster_map.assign(
                    glyphRunDescription->clusterMap,
                    glyphRunDescription->clusterMap + glyphRunDescription->stringLength
                );
            }
        }

        m_paragraph->runs.push_back(std::move(run));
        return S_OK;
    }

    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

private:
    ULONG m_refCount;
    IDWriteFactory* m_factory;
    ShapedParagraph* m_paragraph;
};

class DirectWriteShaperImpl : public DirectWriteShaper {
public:
    DirectWriteShaperImpl() {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_factory.GetAddressOf())
        );
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create DirectWrite factory");
        }
    }

    ShapedParagraph shape(std::string_view utf8_text,
                          std::string_view font_family,
                          float font_size,
                          std::string_view locale) override {
        ShapedParagraph result;
        result.text_utf8 = utf8_text;
        if (utf8_text.empty()) {
            return result;
        }

        std::wstring text_utf16 = utf8_to_utf16(utf8_text);
        std::wstring font_utf16 = utf8_to_utf16(font_family);
        std::wstring locale_utf16 = utf8_to_utf16(locale);

        ComPtr<IDWriteTextFormat> textFormat;
        HRESULT hr = m_factory->CreateTextFormat(
            font_utf16.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            font_size,
            locale_utf16.c_str(),
            &textFormat
        );
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create DirectWrite TextFormat");
        }

        // 建立 TextLayout（由 DirectWrite 自帶字型 fallback 與 Bidi 分析）
        ComPtr<IDWriteTextLayout> textLayout;
        hr = m_factory->CreateTextLayout(
            text_utf16.c_str(),
            static_cast<UINT32>(text_utf16.size()),
            textFormat.Get(),
            100000.0f, // 無限寬度（單行/自然寬度分析）
            100000.0f,
            &textLayout
        );
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create DirectWrite TextLayout");
        }

        // 取得版面總尺寸
        DWRITE_TEXT_METRICS textMetrics;
        textLayout->GetMetrics(&textMetrics);
        result.total_width = textMetrics.widthIncludingTrailingWhitespace;
        result.height = textMetrics.height;

        // 使用 Custom Renderer 抽取所有的 Runs
        CustomGlyphRunExtractor* extractor = new CustomGlyphRunExtractor(m_factory.Get(), &result);
        hr = textLayout->Draw(nullptr, extractor, 0.0f, 0.0f);
        extractor->Release();

        if (FAILED(hr)) {
            throw std::runtime_error("Failed to extract glyph runs from DirectWrite TextLayout");
        }

        return result;
    }

private:
    ComPtr<IDWriteFactory> m_factory;
};

std::unique_ptr<DirectWriteShaper> DirectWriteShaper::create() {
    return std::make_unique<DirectWriteShaperImpl>();
}

} // namespace krepis::spike1
