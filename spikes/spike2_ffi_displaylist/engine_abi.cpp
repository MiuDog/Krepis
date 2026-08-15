#include "engine_abi.h"
#include "display_list.hpp"
#include "../spike1_directwrite/spike1_shaper.hpp"

#include <vector>
#include <string>
#include <memory>
#include <cmath>
#include <iostream>

namespace krepis::spike2 {

struct ParagraphItem {
    std::string text;
    bool is_dirty{true};
    spike1::ShapedParagraph shaped;
    float y_offset{0.0f};
    float height{24.0f}; // 預設行高
};

class EngineImpl {
public:
    EngineImpl() {
        m_shaper = spike1::DirectWriteShaper::create();
        m_arenas[0] = std::make_unique<Arena>(512 * 1024);
        m_arenas[1] = std::make_unique<Arena>(512 * 1024);
    }

    void set_viewport(float width, float height, float scroll_y) {
        m_viewport_width = width;
        m_viewport_height = height;
        m_scroll_y = scroll_y;
    }

    void insert_paragraph(uint32_t index, std::string_view text) {
        ParagraphItem item;
        item.text = text;
        item.is_dirty = true;
        if (index >= m_paragraphs.size()) {
            m_paragraphs.push_back(std::move(item));
        } else {
            m_paragraphs.insert(m_paragraphs.begin() + index, std::move(item));
        }
        m_layout_dirty = true;
    }

    void edit_paragraph(uint32_t index, std::string_view text) {
        if (index < m_paragraphs.size()) {
            if (m_paragraphs[index].text != text) {
                m_paragraphs[index].text = text;
                m_paragraphs[index].is_dirty = true;
                m_layout_dirty = true;
            }
        }
    }

    uint32_t get_paragraph_count() const {
        return static_cast<uint32_t>(m_paragraphs.size());
    }

    // 增量版面重排
    float layout() {
        if (!m_layout_dirty) {
            return m_total_height;
        }

        float current_y = 0.0f;
        for (auto& item : m_paragraphs) {
            if (item.is_dirty) {
                item.shaped = m_shaper->shape(item.text, "Segoe UI", 16.0f, "zh-TW");
                item.height = (std::max)(item.shaped.height, 24.0f);
                item.is_dirty = false;
            }
            item.y_offset = current_y;
            current_y += item.height + 4.0f; // 4px 段落間距
        }

        m_total_height = current_y;
        m_layout_dirty = false;
        return m_total_height;
    }

    // 產生 Display List
    std::pair<const uint8_t*, uint32_t> build_display_list(int arena_index) {
        Arena* arena = m_arenas[arena_index].get();
        arena->reset();

        DisplayListBuilder builder(arena);
        builder.set_viewport(0.0f, m_scroll_y, m_viewport_width, m_viewport_height);

        // 1. 繪製背景
        builder.add_rect(0.0f, 0.0f, m_viewport_width, m_viewport_height, 0xFFFFFFFF);

        // 2. 視窗可見區域範圍判定
        float view_top = m_scroll_y;
        float view_bottom = m_scroll_y + m_viewport_height;

        for (const auto& item : m_paragraphs) {
            float item_top = item.y_offset;
            float item_bottom = item.y_offset + item.height;

            // 視窗裁剪 (Culling)
            if (item_bottom < view_top || item_top > view_bottom) {
                continue;
            }

            // 渲染段落中的所有 Shaped Runs
            float run_x = 16.0f; // 左側 padding
            float baseline_y = item.y_offset - m_scroll_y + 18.0f; // 基準線

            for (const auto& run : item.shaped.runs) {
                if (run.glyphs.empty()) continue;

                std::vector<uint16_t> indices;
                std::vector<float> advances;
                indices.reserve(run.glyphs.size());
                advances.reserve(run.glyphs.size());

                for (const auto& g : run.glyphs) {
                    indices.push_back(g.glyph_index);
                    advances.push_back(g.advance_x);
                }

                builder.add_glyph_run(
                    run_x,
                    baseline_y,
                    run.font_size,
                    0xFF222222, // 深色字體
                    indices.data(),
                    advances.data(),
                    static_cast<uint32_t>(indices.size())
                );

                run_x += run.total_advance;
            }
        }

        builder.finalize();
        return {builder.raw_data(), static_cast<uint32_t>(builder.total_bytes())};
    }

    int acquire_arena() {
        // 雙緩衝切換
        int idx = m_active_arena_idx;
        m_active_arena_idx = (m_active_arena_idx + 1) % 2;
        return idx;
    }

private:
    std::unique_ptr<spike1::DirectWriteShaper> m_shaper;
    std::vector<ParagraphItem> m_paragraphs;
    float m_viewport_width{800.0f};
    float m_viewport_height{600.0f};
    float m_scroll_y{0.0f};
    float m_total_height{0.0f};
    bool m_layout_dirty{true};

    // 雙緩衝 Arena
    std::unique_ptr<Arena> m_arenas[2];
    int m_active_arena_idx{0};
};

} // namespace krepis::spike2

// C ABI Opaque Wrappers
struct KrepisEngineOpaque {
    krepis::spike2::EngineImpl impl;
};

struct KrepisDisplayListOpaque {
    int arena_index{0};
};

extern "C" {

KREPIS_EXPORT KrepisStatus krepis_engine_create(KrepisEngineHandle* out_engine) {
    if (!out_engine) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        *out_engine = new KrepisEngineOpaque();
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_destroy(KrepisEngineHandle engine) {
    if (!engine) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        delete engine;
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_set_viewport(
    KrepisEngineHandle engine,
    float width,
    float height,
    float scroll_y
) {
    if (!engine) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        engine->impl.set_viewport(width, height, scroll_y);
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_insert_paragraph(
    KrepisEngineHandle engine,
    uint32_t index,
    const char* utf8_text
) {
    if (!engine || !utf8_text) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        engine->impl.insert_paragraph(index, utf8_text);
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_edit_paragraph(
    KrepisEngineHandle engine,
    uint32_t index,
    const char* utf8_text
) {
    if (!engine || !utf8_text) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        engine->impl.edit_paragraph(index, utf8_text);
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_get_paragraph_count(
    KrepisEngineHandle engine,
    uint32_t* out_count
) {
    if (!engine || !out_count) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        *out_count = engine->impl.get_paragraph_count();
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_layout(
    KrepisEngineHandle engine,
    float* out_total_height
) {
    if (!engine) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        float h = engine->impl.layout();
        if (out_total_height) *out_total_height = h;
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_engine_acquire_display_list(
    KrepisEngineHandle engine,
    const uint8_t** out_buffer_ptr,
    uint32_t* out_buffer_size,
    KrepisDisplayListHandle* out_dl_handle
) {
    if (!engine || !out_buffer_ptr || !out_buffer_size || !out_dl_handle) {
        return KREPIS_ERROR_INVALID_ARGUMENT;
    }
    try {
        int arena_idx = engine->impl.acquire_arena();
        auto [ptr, size] = engine->impl.build_display_list(arena_idx);

        *out_buffer_ptr = ptr;
        *out_buffer_size = size;

        auto* handle = new KrepisDisplayListOpaque();
        handle->arena_index = arena_idx;
        *out_dl_handle = handle;

        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

KREPIS_EXPORT KrepisStatus krepis_display_list_release(
    KrepisEngineHandle engine,
    KrepisDisplayListHandle dl_handle
) {
    if (!engine || !dl_handle) return KREPIS_ERROR_INVALID_ARGUMENT;
    try {
        delete dl_handle;
        return KREPIS_OK;
    } catch (...) {
        return KREPIS_ERROR_PANIC;
    }
}

} // extern "C"
