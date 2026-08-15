#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define KREPIS_EXPORT __declspec(dllexport)
#else
#define KREPIS_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 狀態碼
typedef enum KrepisStatus {
    KREPIS_OK = 0,
    KREPIS_ERROR_INVALID_ARGUMENT = 1,
    KREPIS_ERROR_OUT_OF_RANGE = 2,
    KREPIS_ERROR_PANIC = 99,
} KrepisStatus;

// 不透明引擎句柄
typedef struct KrepisEngineOpaque* KrepisEngineHandle;

// 不透明 DisplayList 緩衝區句柄
typedef struct KrepisDisplayListOpaque* KrepisDisplayListHandle;

// 建立引擎實例
KREPIS_EXPORT KrepisStatus krepis_engine_create(KrepisEngineHandle* out_engine);

// 銷毀引擎實例
KREPIS_EXPORT KrepisStatus krepis_engine_destroy(KrepisEngineHandle engine);

// 設定 Viewport 尺寸與捲動位移
KREPIS_EXPORT KrepisStatus krepis_engine_set_viewport(
    KrepisEngineHandle engine,
    float width,
    float height,
    float scroll_y
);

// 插入段落 (UTF-8)
KREPIS_EXPORT KrepisStatus krepis_engine_insert_paragraph(
    KrepisEngineHandle engine,
    uint32_t index,
    const char* utf8_text
);

// 編輯既有段落 (UTF-8)
KREPIS_EXPORT KrepisStatus krepis_engine_edit_paragraph(
    KrepisEngineHandle engine,
    uint32_t index,
    const char* utf8_text
);

// 取得總段落數
KREPIS_EXPORT KrepisStatus krepis_engine_get_paragraph_count(
    KrepisEngineHandle engine,
    uint32_t* out_count
);

// 觸發版面增量計算
KREPIS_EXPORT KrepisStatus krepis_engine_layout(
    KrepisEngineHandle engine,
    float* out_total_height
);

// 取得當前視窗的 Display List 二進位連續記憶體指標與長度
// 遵守 FND-0002 D3：外殼在明確呼叫 krepis_display_list_release 前，指標保證有效
KREPIS_EXPORT KrepisStatus krepis_engine_acquire_display_list(
    KrepisEngineHandle engine,
    const uint8_t** out_buffer_ptr,
    uint32_t* out_buffer_size,
    KrepisDisplayListHandle* out_dl_handle
);

// 釋放 Display List 句柄
KREPIS_EXPORT KrepisStatus krepis_display_list_release(
    KrepisEngineHandle engine,
    KrepisDisplayListHandle dl_handle
);

#ifdef __cplusplus
}
#endif
