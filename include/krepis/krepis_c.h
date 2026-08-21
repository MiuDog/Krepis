#pragma once

/* FND-0002／BND-0001：Krepis 穩定 C ABI。公開邊界只使用 C 型別與固定寬度欄位。 */

#include <stdint.h>

#if defined(_WIN32)
#if defined(KREPIS_BUILD_DLL)
#define KREPIS_C_API __declspec(dllexport)
#else
#define KREPIS_C_API __declspec(dllimport)
#endif
#else
#define KREPIS_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t KrepisStatus;

enum {
	KREPIS_STATUS_OK = 0,
	KREPIS_STATUS_INVALID_ARGUMENT = 1,
	KREPIS_STATUS_OUT_OF_RANGE = 2,
	KREPIS_STATUS_NOT_FOUND = 3,
	KREPIS_STATUS_INVALID_STATE = 4,
	KREPIS_STATUS_UNSUPPORTED = 5,
	KREPIS_STATUS_VERSION_MISMATCH = 6,
	KREPIS_STATUS_REVISION_CONFLICT = 7,
	KREPIS_STATUS_MISSING_GLYPH = 8,
	KREPIS_STATUS_IO_FAILURE = 9,
	KREPIS_STATUS_CORRUPT_DATA = 10,
	KREPIS_STATUS_PANIC = 255,
};

typedef struct KrepisDisplayEngineOpaque* KrepisDisplayEngine;
typedef uint64_t KrepisDisplayLease;
typedef uint64_t KrepisGlyphPathLease;

typedef struct KrepisGlyph {
	uint32_t glyph_id;
	int32_t x_advance_26_6;
	int32_t y_advance_26_6;
	int32_t x_offset_26_6;
	int32_t y_offset_26_6;
	uint32_t cluster_byte_offset;
} KrepisGlyph;

typedef struct KrepisDisplaySpan {
	uint32_t struct_size;
	uint16_t abi_major;
	uint16_t abi_minor;
	const uint8_t* data;
	uint64_t byte_size;
	uint64_t frame_token;
	KrepisDisplayLease lease;
} KrepisDisplaySpan;

typedef struct KrepisDisplayStats {
	uint32_t struct_size;
	uint32_t reserved;
	uint64_t published_frames;
	uint64_t acquired_leases;
	uint64_t released_leases;
	uint64_t outstanding_leases;
} KrepisDisplayStats;

enum {
	KREPIS_GLYPH_PATH_MOVE_TO = 1,
	KREPIS_GLYPH_PATH_LINE_TO = 2,
	KREPIS_GLYPH_PATH_QUADRATIC_TO = 3,
	KREPIS_GLYPH_PATH_CUBIC_TO = 4,
	KREPIS_GLYPH_PATH_CLOSE = 5,
};

typedef struct KrepisGlyphPathCommand {
	uint32_t opcode;
	float values[6];
} KrepisGlyphPathCommand;

typedef struct KrepisGlyphPathSpan {
	uint32_t struct_size;
	uint16_t abi_major;
	uint16_t abi_minor;
	const KrepisGlyphPathCommand* commands;
	uint64_t command_count;
	KrepisGlyphPathLease lease;
} KrepisGlyphPathSpan;

/*
 * 執行緒契約：下列所有 engine／lease 入口只能由建立 engine 的 UI 執行緒呼叫。
 * 記憶體契約：acquire 回傳的 data 在對應 release 成功前有效且唯讀；不得由外殼釋放。
 */
KREPIS_C_API KrepisStatus krepis_display_engine_create(
	uint16_t abi_major,
	uint16_t abi_minor,
	KrepisDisplayEngine* out_engine
);

KREPIS_C_API KrepisStatus krepis_display_engine_destroy(KrepisDisplayEngine engine);
KREPIS_C_API KrepisStatus krepis_display_register_font(
	KrepisDisplayEngine engine,
	uint64_t font_id,
	const uint8_t* bytes,
	uint64_t byte_size,
	uint32_t face_index
);
KREPIS_C_API KrepisStatus krepis_display_acquire_glyph_path(
	KrepisDisplayEngine engine,
	uint64_t font_id,
	uint32_t glyph_id,
	int32_t font_size_26_6,
	KrepisGlyphPathSpan* out_span
);
KREPIS_C_API KrepisStatus krepis_display_release_glyph_path(
	KrepisDisplayEngine engine,
	KrepisGlyphPathLease lease
);
KREPIS_C_API KrepisStatus krepis_display_builder_reset(KrepisDisplayEngine engine);

KREPIS_C_API KrepisStatus krepis_display_add_rect(
	KrepisDisplayEngine engine,
	float x,
	float y,
	float width,
	float height,
	uint32_t color_rgba
);

KREPIS_C_API KrepisStatus krepis_display_add_glyph_run(
	KrepisDisplayEngine engine,
	int32_t baseline_x_26_6,
	int32_t baseline_y_26_6,
	int32_t font_size_26_6,
	uint32_t color_rgba,
	uint64_t font_id,
	uint32_t direction,
	const KrepisGlyph* glyphs,
	uint32_t glyph_count
);

KREPIS_C_API KrepisStatus krepis_display_push_clip(
	KrepisDisplayEngine engine,
	float x,
	float y,
	float width,
	float height
);

KREPIS_C_API KrepisStatus krepis_display_pop_clip(KrepisDisplayEngine engine);

KREPIS_C_API KrepisStatus krepis_display_push_transform(
	KrepisDisplayEngine engine,
	const float affine_2x3[6]
);

KREPIS_C_API KrepisStatus krepis_display_pop_transform(KrepisDisplayEngine engine);
KREPIS_C_API KrepisStatus krepis_display_publish(
	KrepisDisplayEngine engine,
	uint64_t* out_frame_token
);
KREPIS_C_API KrepisStatus krepis_display_acquire(
	KrepisDisplayEngine engine,
	KrepisDisplaySpan* out_span
);
KREPIS_C_API KrepisStatus krepis_display_release(
	KrepisDisplayEngine engine,
	KrepisDisplayLease lease
);
KREPIS_C_API KrepisStatus krepis_display_get_stats(
	KrepisDisplayEngine engine,
	KrepisDisplayStats* out_stats
);
KREPIS_C_API KrepisStatus krepis_display_validate(
	const uint8_t* data,
	uint64_t byte_size,
	uint16_t supported_major,
	uint16_t supported_minor
);

#ifdef __cplusplus
}
#endif
