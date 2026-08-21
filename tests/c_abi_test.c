#include "krepis/krepis_c.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

static void expect_status(int condition, const char* message) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static uint8_t* read_font(uint64_t* out_size) {
	*out_size = 0;
	FILE* stream = fopen(KREPIS_TEST_C_ABI_FONT, "rb");
	if (stream == NULL) return NULL;
	if (fseek(stream, 0, SEEK_END) != 0) {
		fclose(stream);
		return NULL;
	}
	const long size = ftell(stream);
	if (size <= 0 || fseek(stream, 0, SEEK_SET) != 0) {
		fclose(stream);
		return NULL;
	}
	uint8_t* bytes = (uint8_t*)malloc((size_t)size);
	if (bytes == NULL) {
		fclose(stream);
		return NULL;
	}
	const size_t read = fread(bytes, 1, (size_t)size, stream);
	fclose(stream);
	if (read != (size_t)size) {
		free(bytes);
		return NULL;
	}
	*out_size = (uint64_t)size;
	return bytes;
}

int main(void) {
	KrepisDisplayEngine engine = 0;
	expect_status(
		krepis_display_engine_create(2, 0, &engine) == KREPIS_STATUS_VERSION_MISMATCH,
		"unknown ABI major fails closed"
	);
	expect_status(
		krepis_display_engine_create(1, 0, &engine) == KREPIS_STATUS_OK && engine != 0,
		"C ABI engine create"
	);
	if (engine == 0) return 1;
	expect_status(
		krepis_display_add_rect(engine, 0, 0, 100, 100, UINT32_C(0xFFFFFFFF)) ==
			KREPIS_STATUS_OK,
		"C ABI DrawRect"
	);
	expect_status(
		krepis_display_push_clip(engine, 0, 0, 100, 100) == KREPIS_STATUS_OK,
		"C ABI PushClip"
	);
	KrepisGlyph glyph = {7, 640, 0, 0, 0, 0};
	expect_status(
		krepis_display_add_glyph_run(
			engine,
			64,
			128,
			1024,
			UINT32_C(0xFF000000),
			1,
			0,
			&glyph,
			1
		) == KREPIS_STATUS_OK,
		"C ABI DrawGlyphRun"
	);
	expect_status(krepis_display_pop_clip(engine) == KREPIS_STATUS_OK, "C ABI PopClip");
	uint64_t token = 0;
	expect_status(
		krepis_display_publish(engine, &token) == KREPIS_STATUS_OK && token == 1,
		"C ABI publish"
	);
	KrepisDisplaySpan span = {0};
	span.struct_size = (uint32_t)sizeof(span);
	expect_status(
		krepis_display_acquire(engine, &span) == KREPIS_STATUS_OK &&
			span.data != 0 && span.byte_size >= 32 && span.frame_token == token && span.lease != 0,
		"C ABI acquire span"
	);
	expect_status(
		krepis_display_validate(span.data, span.byte_size, 1, 0) == KREPIS_STATUS_OK,
		"C ABI validate span"
	);
	expect_status(
		krepis_display_engine_destroy(engine) == KREPIS_STATUS_INVALID_STATE,
		"destroy rejects outstanding lease"
	);
	expect_status(
		krepis_display_release(engine, span.lease) == KREPIS_STATUS_OK,
		"C ABI explicit release"
	);
	expect_status(
		krepis_display_release(engine, span.lease) == KREPIS_STATUS_INVALID_ARGUMENT,
		"numeric display lease rejects double release without dereference"
	);

	uint64_t font_size = 0;
	uint8_t* font_bytes = read_font(&font_size);
	expect_status(font_bytes != NULL && font_size > 0, "C ABI fixture font read");
	if (font_bytes != NULL) {
		expect_status(
			krepis_display_register_font(engine, 7, font_bytes, font_size, 0) ==
				KREPIS_STATUS_OK,
			"C ABI font registration"
		);
		free(font_bytes);
	}
	KrepisGlyphPathSpan path = {0};
	path.struct_size = (uint32_t)sizeof(path);
	expect_status(
		krepis_display_acquire_glyph_path(engine, 7, 37, 1024, &path) ==
			KREPIS_STATUS_OK && path.lease != 0 && path.commands != NULL &&
			path.command_count > 0,
		"C ABI glyph path acquire"
	);
	if (path.command_count > 0) {
		expect_status(
			path.commands[0].opcode >= KREPIS_GLYPH_PATH_MOVE_TO &&
				path.commands[0].opcode <= KREPIS_GLYPH_PATH_CLOSE,
			"C ABI glyph path opcode uses fixed C enum values"
		);
	}
	expect_status(
		krepis_display_engine_destroy(engine) == KREPIS_STATUS_INVALID_STATE,
		"destroy rejects outstanding glyph path lease"
	);
	expect_status(
		krepis_display_release_glyph_path(engine, path.lease) == KREPIS_STATUS_OK,
		"C ABI glyph path release"
	);
	expect_status(
		krepis_display_release_glyph_path(engine, path.lease) ==
			KREPIS_STATUS_INVALID_ARGUMENT,
		"numeric glyph path lease rejects double release"
	);
	KrepisDisplayStats stats = {0};
	stats.struct_size = (uint32_t)sizeof(stats);
	expect_status(
		krepis_display_get_stats(engine, &stats) == KREPIS_STATUS_OK &&
			stats.acquired_leases == 1 && stats.released_leases == 1 &&
			stats.outstanding_leases == 0,
		"C ABI allocation count equals release count"
	);
	expect_status(
		krepis_display_engine_destroy(engine) == KREPIS_STATUS_OK,
		"C ABI engine destroy"
	);
	if (failures == 0) {
		puts("krepis.c_abi: all checks passed");
		return 0;
	}
	fprintf(stderr, "krepis.c_abi: %d check(s) failed\n", failures);
	return 1;
}
