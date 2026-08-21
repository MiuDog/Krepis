# Spikes 建置配置

# Spike 1／2 依賴 DirectWrite 與 <windows.h>，**只能在 Windows 建置**。
#
# 未加此保護前，Linux 建置會在 spike1_shaper.cpp 的 `#include <windows.h>` 失敗，
# 連帶使整個 build 中止——包含與 Windows 無關的核心測試。
# 這個缺陷是 2026-08-18 在 WSL 跑閘門 5 的 sanitizer 時才暴露：
# CI 的 Linux job 早就會撞到，只是 CI 從未實際執行過（受 GitHub 帳務阻塞）。
#
# **教訓：沒跑過的 CI 設定等於沒有 CI 設定。**
if(WIN32)

# Spike 1: DirectWrite Shaper
add_executable(spike1_directwrite
    spike1_directwrite/spike1_shaper.cpp
    spike1_directwrite/spike1_main.cpp
)

target_include_directories(spike1_directwrite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/spike1_directwrite
)

target_link_libraries(spike1_directwrite PRIVATE
    dwrite
)

target_compile_features(spike1_directwrite PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(spike1_directwrite PRIVATE /utf-8 /W4 /permissive-)
endif()

# Spike 2: C ABI DLL
add_library(krepis_c_abi SHARED
    spike1_directwrite/spike1_shaper.cpp
    spike2_ffi_displaylist/engine_abi.cpp
)

target_include_directories(krepis_c_abi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/spike1_directwrite
    ${CMAKE_CURRENT_SOURCE_DIR}/spike2_ffi_displaylist
)

target_link_libraries(krepis_c_abi PRIVATE
    dwrite
)

target_compile_features(krepis_c_abi PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(krepis_c_abi PRIVATE /utf-8 /W4 /permissive-)
endif()

# Spike 2 Native Test
add_executable(spike2_native_test
    spike2_ffi_displaylist/spike2_native_test.cpp
)

target_include_directories(spike2_native_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/spike2_ffi_displaylist
)

target_link_libraries(spike2_native_test PRIVATE
    krepis_c_abi
)

target_compile_features(spike2_native_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(spike2_native_test PRIVATE /utf-8 /W4 /permissive-)
endif()

# Spike 2 Scaling Test：驗證增量性主張（原報告只測到 N=10,000，此處拉到百萬級）
add_executable(spike2_scaling_test
    spike2_ffi_displaylist/spike2_scaling_test.cpp
)

target_include_directories(spike2_scaling_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/spike2_ffi_displaylist
)

target_link_libraries(spike2_scaling_test PRIVATE
    krepis_c_abi
)

target_compile_features(spike2_scaling_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(spike2_scaling_test PRIVATE /utf-8 /W4 /permissive-)
endif()

endif()  # WIN32 —— Spike 1／2 的 Windows 專屬區段到此結束

# 以下皆為跨平台，不受上方 WIN32 保護限制。

# Spike 4: IntrusivePtr vs std::shared_ptr benchmark（LAY-0002 D17 gate 6）
add_executable(spike4_intrusive_vs_shared
    spike4_intrusive_vs_shared/spike4_benchmark.cpp
)

target_link_libraries(spike4_intrusive_vs_shared PRIVATE krepis::krepis)

target_compile_features(spike4_intrusive_vs_shared PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(spike4_intrusive_vs_shared PRIVATE /utf-8 /W4 /permissive-)
endif()

# Benchmark: FlowSequence 分塊參數（LAY-0002「尚未決定」第一項）
add_executable(bench_flow_sequence
    bench_flow_sequence/bench_flow_sequence.cpp
)

target_include_directories(bench_flow_sequence PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_support
)

target_link_libraries(bench_flow_sequence PRIVATE krepis::krepis)

target_compile_features(bench_flow_sequence PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(bench_flow_sequence PRIVATE /utf-8 /W4 /permissive-)
endif()

# Benchmark: LeafKey 局部 relabel 初始 window（LAY-0002 D22）
add_executable(bench_leaf_key_relabel
    bench_leaf_key_relabel/bench_leaf_key_relabel.cpp
)

target_link_libraries(bench_leaf_key_relabel PRIVATE krepis::krepis)
target_compile_features(bench_leaf_key_relabel PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(bench_leaf_key_relabel PRIVATE /utf-8 /W4 /permissive-)
endif()

# Benchmark: paragraph layout LRU 容量與同步路徑 p99（TXT-0001 Phase 2C）
add_executable(bench_text_layout_cache
    bench_text_layout_cache/bench_text_layout_cache.cpp
)
target_link_libraries(bench_text_layout_cache PRIVATE krepis::krepis)
target_compile_definitions(bench_text_layout_cache PRIVATE
    KREPIS_BENCH_LATIN_FONT="${harfbuzz_SOURCE_DIR}/test/subset/data/fonts/Roboto-Regular.ttf"
)
target_compile_features(bench_text_layout_cache PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(bench_text_layout_cache PRIVATE /utf-8 /W4 /permissive-)
endif()

# Benchmark: display-list encode + publish validation + consumer validation（BND-0001 BND-5）
add_executable(bench_display_list
    bench_display_list/bench_display_list.cpp
)
target_link_libraries(bench_display_list PRIVATE krepis::krepis)
target_compile_features(bench_display_list PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(bench_display_list PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(bench_display_list PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Spike 5: 跨平台 text shaping（重用正式 HarfBuzz target，依 FND-0003）
#
# 依賴已由正式文字管線載入；此開關只控制列印型 spike executable。
# 啟用：cmake -B build -DKREPIS_BUILD_TEXT_SPIKE=ON
option(KREPIS_BUILD_TEXT_SPIKE "建置跨平台文字驗證 spike executables" OFF)

if(KREPIS_BUILD_TEXT_SPIKE)
    add_executable(spike5_text_shaping
        spike5_text_shaping/spike5_shaping.cpp
    )

    target_link_libraries(spike5_text_shaping PRIVATE harfbuzz)

    target_compile_features(spike5_text_shaping PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(spike5_text_shaping PRIVATE /utf-8 /W4 /permissive-)
    endif()
endif()

# Spike 6: 斷行與分群（libunibreak）——與 spike 5 共用同一個 opt-in 開關
#
# 這是唯一可能推翻 spike 5「772 KB 可接受」結論的項目：
# 若斷行必須靠 ICU，整個文字堆疊的體積結論就要重寫。
if(KREPIS_BUILD_TEXT_SPIKE)
    add_executable(spike6_line_breaking
        spike6_line_breaking/spike6_linebreak.cpp
    )

    target_link_libraries(spike6_line_breaking PRIVATE krepis_text_unibreak)

    target_compile_features(spike6_line_breaking PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(spike6_line_breaking PRIVATE /utf-8 /W4 /permissive-)
    endif()
endif()

# Spike 7: 雙向文字（SheenBidi）——文字堆疊的最後一塊
if(KREPIS_BUILD_TEXT_SPIKE)
    add_executable(spike7_bidi
        spike7_bidi/spike7_bidi.cpp
    )

    target_link_libraries(spike7_bidi PRIVATE krepis_text_sheenbidi)

    target_compile_features(spike7_bidi PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(spike7_bidi PRIVATE /utf-8 /W4 /permissive-)
    endif()
endif()

# Spike 3: Submission Boundary Evaluation
add_executable(spike3_submission_boundary
    spike3_submission_boundary/spike3_main.cpp
)

target_compile_features(spike3_submission_boundary PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(spike3_submission_boundary PRIVATE /utf-8 /W4 /permissive-)
endif()
