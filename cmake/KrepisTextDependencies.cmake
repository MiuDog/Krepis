include(FetchContent)

# TXT-0001：正式文字管線只接受固定 commit，避免 upstream tag 移動造成不可重現建置。
# 消費端可用 FETCHCONTENT_SOURCE_DIR_<NAME> 指向已審核的離線來源樹。
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)

FetchContent_Declare(harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG c11b534f6e95663368bf3b93d7457db92bda7227
)

# CMake 3.24 尚未支援 FetchContent_Declare(EXCLUDE_FROM_ALL)。這裡以舊版相容的
# populate + add_subdirectory 明確隔離 upstream install／ALL；3.30 後則局部採 CMP0169 OLD。
cmake_policy(PUSH)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(harfbuzz)
if(NOT harfbuzz_POPULATED)
    FetchContent_Populate(harfbuzz)
    add_subdirectory(
        "${harfbuzz_SOURCE_DIR}"
        "${harfbuzz_BINARY_DIR}"
        EXCLUDE_FROM_ALL
    )
endif()
cmake_policy(POP)

FetchContent_Declare(libunibreak
    GIT_REPOSITORY https://github.com/adah1972/libunibreak.git
    GIT_TAG 304585d8e2d63187507368d612c3d5fff1486368
)

cmake_policy(PUSH)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(libunibreak)
if(NOT libunibreak_POPULATED)
    FetchContent_Populate(libunibreak)
endif()
cmake_policy(POP)

add_library(krepis_text_unibreak STATIC
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
target_include_directories(krepis_text_unibreak PUBLIC ${libunibreak_SOURCE_DIR}/src)

FetchContent_Declare(sheenbidi
    GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
    GIT_TAG 488ba0fcc323efdf596c4821a2351ef98ec1bd0e
)

cmake_policy(PUSH)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(sheenbidi)
if(NOT sheenbidi_POPULATED)
    FetchContent_Populate(sheenbidi)
endif()
cmake_policy(POP)

# SheenBidi 2.8 提供 unity build；升級時不必人工同步其內部 C 原始碼清單。
add_library(krepis_text_sheenbidi STATIC ${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c)
target_compile_definitions(krepis_text_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(krepis_text_sheenbidi PUBLIC ${sheenbidi_SOURCE_DIR}/Headers)
target_include_directories(krepis_text_sheenbidi PRIVATE ${sheenbidi_SOURCE_DIR}/Source)
