cmake_minimum_required(VERSION 3.24)

project(krepis
    VERSION 0.0.1
    DESCRIPTION "結構化筆記基座：權威狀態、文件模型、版面、選取、undo 與 ink 資料模型"
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

# 本專案為基座庫，可被其他專案 add_subdirectory 引入；只有作為頂層專案時才建置測試。
if(PROJECT_IS_TOP_LEVEL)
    set(KREPIS_BUILD_TESTS_DEFAULT ON)
else()
    set(KREPIS_BUILD_TESTS_DEFAULT OFF)
endif()
option(KREPIS_BUILD_TESTS "建置 Krepis 測試" ${KREPIS_BUILD_TESTS_DEFAULT})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 原始碼一律為無 BOM 的 UTF-8，且註解使用繁體中文。
# MSVC 預設以系統 ANSI codepage（繁中 Windows 為 CP950／Big5）解讀無 BOM 檔案；
# Big5 有字元的第二個位元組為 0x5C（反斜線），會使中文註解尾端被當成續行符，
# 靜默吞掉下一行程式碼。/utf-8 同時設定來源與執行字元集，必須全域套用。
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(krepis)
add_library(krepis::krepis ALIAS krepis)

target_sources(krepis PRIVATE
    src/arena.cpp
    src/flow_layout_index.cpp
    src/location_index.cpp
    src/flow_sequence.cpp
    src/id_generator.cpp
    src/leaf_key.cpp
    src/object_id.cpp
    src/reclamation.cpp
    src/version.cpp
)

target_include_directories(krepis PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(krepis PUBLIC cxx_std_20)
target_link_libraries(krepis PUBLIC Threads::Threads)

if(MSVC)
    # /W4 為警告基線；/permissive- 關閉 MSVC 的寬鬆一致性模式，避免寫出不可攜的程式碼。
    target_compile_options(krepis PRIVATE /W4 /permissive-)
else()
    target_compile_options(krepis PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(KREPIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(spikes)
endif()
