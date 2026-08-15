#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
#include <memory>
#include <algorithm>

namespace krepis::spike2 {

// 二進位 Display List 指令型別
enum class DisplayCommandType : uint32_t {
    DrawRect = 1,
    DrawGlyphRun = 2,
    ClipRect = 3,
};

#pragma pack(push, 4)

// 矩形繪製指令（如背景、選取框、游標）
struct DrawRectCommand {
    DisplayCommandType type{DisplayCommandType::DrawRect};
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    uint32_t color_rgba{0xFF000000};
};

// GlyphRun 繪製指令標頭（緊接著 glyph_indices 與 advances 陣列）
struct DrawGlyphRunHeader {
    DisplayCommandType type{DisplayCommandType::DrawGlyphRun};
    float baseline_x{0.0f};
    float baseline_y{0.0f};
    float font_size{16.0f};
    uint32_t color_rgba{0xFF000000};
    uint32_t glyph_count{0};
    // 緊跟 uint16_t glyph_indices[glyph_count]
    // 緊跟 float advances[glyph_count]
    // 依 4-byte 對齊
};

// 二進位 Display List 整體 Header
struct DisplayListHeader {
    uint32_t magic{0x4B524550}; // 'KREP'
    uint32_t version{1};
    uint32_t total_bytes{0};
    uint32_t command_count{0};
    float viewport_x{0.0f};
    float viewport_y{0.0f};
    float viewport_width{0.0f};
    float viewport_height{0.0f};
};

#pragma pack(pop)

// Arena (Bump Allocator) 實作：整塊配置、單向 bump、整塊重設/丟棄
class Arena {
public:
    explicit Arena(size_t initial_capacity = 256 * 1024) 
        : m_capacity(initial_capacity), m_offset(0) {
        m_buffer = static_cast<uint8_t*>(std::malloc(m_capacity));
    }

    ~Arena() {
        if (m_buffer) {
            std::free(m_buffer);
        }
    }

    // 禁用拷貝
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // 支援移動
    Arena(Arena&& other) noexcept 
        : m_buffer(other.m_buffer), m_capacity(other.m_capacity), m_offset(other.m_offset) {
        other.m_buffer = nullptr;
        other.m_capacity = 0;
        other.m_offset = 0;
    }

    void* allocate(size_t size, size_t alignment = 4) {
        size_t current = reinterpret_cast<uintptr_t>(m_buffer + m_offset);
        size_t aligned = (current + (alignment - 1)) & ~(alignment - 1);
        size_t new_offset = (aligned - reinterpret_cast<uintptr_t>(m_buffer)) + size;

        if (new_offset > m_capacity) {
            // 擴展 capacity
            size_t new_capacity = (std::max)(m_capacity * 2, new_offset + 64 * 1024);
            uint8_t* new_buf = static_cast<uint8_t*>(std::realloc(m_buffer, new_capacity));
            if (!new_buf) {
                std::terminate(); // FND-0002 D5: OOM terminate
            }
            m_buffer = new_buf;
            m_capacity = new_capacity;
            current = reinterpret_cast<uintptr_t>(m_buffer + m_offset);
            aligned = (current + (alignment - 1)) & ~(alignment - 1);
            new_offset = (aligned - reinterpret_cast<uintptr_t>(m_buffer)) + size;
        }

        m_offset = new_offset;
        return reinterpret_cast<void*>(aligned);
    }

    void reset() {
        m_offset = 0;
    }

    uint8_t* data() const { return m_buffer; }
    size_t size() const { return m_offset; }
    size_t capacity() const { return m_capacity; }

private:
    uint8_t* m_buffer{nullptr};
    size_t m_capacity{0};
    size_t m_offset{0};
};

// Display List 建構器
class DisplayListBuilder {
public:
    explicit DisplayListBuilder(Arena* arena) : m_arena(arena) {
        m_header = static_cast<DisplayListHeader*>(m_arena->allocate(sizeof(DisplayListHeader), 4));
        *m_header = DisplayListHeader();
    }

    void set_viewport(float x, float y, float w, float h) {
        m_header->viewport_x = x;
        m_header->viewport_y = y;
        m_header->viewport_width = w;
        m_header->viewport_height = h;
    }

    void add_rect(float x, float y, float w, float h, uint32_t color_rgba) {
        auto* cmd = static_cast<DrawRectCommand*>(m_arena->allocate(sizeof(DrawRectCommand), 4));
        cmd->type = DisplayCommandType::DrawRect;
        cmd->x = x;
        cmd->y = y;
        cmd->width = w;
        cmd->height = h;
        cmd->color_rgba = color_rgba;
        m_header->command_count++;
    }

    void add_glyph_run(float baseline_x, float baseline_y, float font_size, uint32_t color_rgba,
                       const uint16_t* glyph_indices, const float* advances, uint32_t count) {
        size_t header_size = sizeof(DrawGlyphRunHeader);
        size_t indices_size = count * sizeof(uint16_t);
        // 對齊 float 陣列
        size_t indices_aligned_size = (indices_size + 3) & ~3;
        size_t advances_size = count * sizeof(float);
        size_t total_cmd_size = header_size + indices_aligned_size + advances_size;

        auto* ptr = static_cast<uint8_t*>(m_arena->allocate(total_cmd_size, 4));
        auto* header = reinterpret_cast<DrawGlyphRunHeader*>(ptr);
        header->type = DisplayCommandType::DrawGlyphRun;
        header->baseline_x = baseline_x;
        header->baseline_y = baseline_y;
        header->font_size = font_size;
        header->color_rgba = color_rgba;
        header->glyph_count = count;

        if (count > 0) {
            uint16_t* out_indices = reinterpret_cast<uint16_t*>(ptr + header_size);
            std::memcpy(out_indices, glyph_indices, indices_size);

            float* out_advances = reinterpret_cast<float*>(ptr + header_size + indices_aligned_size);
            std::memcpy(out_advances, advances, advances_size);
        }

        m_header->command_count++;
    }

    void finalize() {
        m_header->total_bytes = static_cast<uint32_t>(m_arena->size());
    }

    const uint8_t* raw_data() const {
        return m_arena->data();
    }

    size_t total_bytes() const {
        return m_arena->size();
    }

private:
    Arena* m_arena;
    DisplayListHeader* m_header;
};

} // namespace krepis::spike2
