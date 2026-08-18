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

# Spike 5: 跨平台 text shaping（HarfBuzz via FetchContent，依 FND-0003）
#
# 刻意預設關閉：FetchContent 會 clone 並建置 HarfBuzz，使一般 configure 變慢。
# 啟用：cmake -B build -DKREPIS_BUILD_TEXT_SPIKE=ON
option(KREPIS_BUILD_TEXT_SPIKE "建置 text shaping spike（會下載 HarfBuzz）" OFF)

if(KREPIS_BUILD_TEXT_SPIKE)
    include(FetchContent)

    # libunibreak 與 SheenBidi 都是 **C** 函式庫，但本專案的 project() 只宣告 LANGUAGES CXX。
    # 缺少此行時 configure 會以 `Missing variable is: CMAKE_C_COMPILE_OBJECT` 失敗。
    # 只在需要時啟用，避免一般建置多做 C 編譯器偵測。
    enable_language(C)

    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG 10.1.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(harfbuzz)

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
    FetchContent_Declare(libunibreak
        GIT_REPOSITORY https://github.com/adah1972/libunibreak.git
        GIT_TAG libunibreak_6_1
        GIT_SHALLOW TRUE
    )
    # libunibreak **沒有 CMakeLists.txt**（autotools 專案），因此必須自行撰寫建置膠水。
    # 這是本 spike 記錄的整合成本之一。
    #
    # 使用 MakeAvailable 而非 Populate：後者的單引數形式自 CMake 3.30 起棄用（CMP0169），
    # 在 CI 的 3.31 上會發出警告並在未來版本移除。MakeAvailable 在來源沒有 CMakeLists.txt 時
    # 只做 populate、不呼叫 add_subdirectory，正好符合這裡的需求。
    FetchContent_MakeAvailable(libunibreak)

    add_library(unibreak STATIC
        ${libunibreak_SOURCE_DIR}/src/eastasianwidthdata.c
        ${libunibreak_SOURCE_DIR}/src/eastasianwidthdef.c
        ${libunibreak_SOURCE_DIR}/src/emojidata.c
        ${libunibreak_SOURCE_DIR}/src/emojidef.c
        ${libunibreak_SOURCE_DIR}/src/graphemebreak.c
        ${libunibreak_SOURCE_DIR}/src/graphemebreakdata.c
        ${libunibreak_SOURCE_DIR}/src/indicconjunctbreakdata.c
        ${libunibreak_SOURCE_DIR}/src/linebreak.c
        ${libunibreak_SOURCE_DIR}/src/linebreakdata.c
        ${libunibreak_SOURCE_DIR}/src/linebreakdef.c
        ${libunibreak_SOURCE_DIR}/src/unibreakbase.c
        ${libunibreak_SOURCE_DIR}/src/unibreakdef.c
        ${libunibreak_SOURCE_DIR}/src/wordbreak.c
        ${libunibreak_SOURCE_DIR}/src/wordbreakdata.c
    )
    target_include_directories(unibreak PUBLIC ${libunibreak_SOURCE_DIR}/src)

    add_executable(spike6_line_breaking
        spike6_line_breaking/spike6_linebreak.cpp
    )

    target_link_libraries(spike6_line_breaking PRIVATE unibreak)

    target_compile_features(spike6_line_breaking PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(spike6_line_breaking PRIVATE /utf-8 /W4 /permissive-)
    endif()
endif()

# Spike 7: 雙向文字（SheenBidi）——文字堆疊的最後一塊
if(KREPIS_BUILD_TEXT_SPIKE)
    FetchContent_Declare(sheenbidi
        GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
        GIT_TAG v2.8
        GIT_SHALLOW TRUE
    )
    # 與 libunibreak 同樣沒有 CMakeLists（Makefile ＋ meson），需自寫膠水。
    # 但 SheenBidi 提供 unity build：SheenBidi.c 在 SB_CONFIG_UNITY 下 include 其餘所有 .c，
    # 因此只需編譯單一檔案——**升級時不必人工同步原始碼清單**，
    # 這一點比 libunibreak 好。
    FetchContent_MakeAvailable(sheenbidi)

    add_library(sheenbidi STATIC ${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c)
    target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
    target_include_directories(sheenbidi PUBLIC ${sheenbidi_SOURCE_DIR}/Headers)
    target_include_directories(sheenbidi PRIVATE ${sheenbidi_SOURCE_DIR}/Source)

    add_executable(spike7_bidi
        spike7_bidi/spike7_bidi.cpp
    )

    target_link_libraries(spike7_bidi PRIVATE sheenbidi)

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
