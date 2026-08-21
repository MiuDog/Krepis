# 第一版不引入測試框架依賴：以退出碼作為 pass／fail，交由 CTest 判定。
# 引入框架（Catch2／GoogleTest）需在依賴管理方式定案後另行決定。

function(krepis_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE krepis::krepis)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

krepis_add_test(krepis_version_test version_test.cpp)
add_test(NAME krepis.version COMMAND krepis_version_test)

krepis_add_test(krepis_error_test error_test.cpp)
add_test(NAME krepis.error COMMAND krepis_error_test)

krepis_add_test(krepis_arena_test arena_test.cpp)
add_test(NAME krepis.arena COMMAND krepis_arena_test)

krepis_add_test(krepis_object_id_test object_id_test.cpp)
add_test(NAME krepis.object_id COMMAND krepis_object_id_test)

krepis_add_test(krepis_id_generator_test id_generator_test.cpp)
add_test(NAME krepis.id_generator COMMAND krepis_id_generator_test)

krepis_add_test(krepis_leaf_key_test leaf_key_test.cpp)
add_test(NAME krepis.leaf_key COMMAND krepis_leaf_key_test)

krepis_add_test(krepis_intrusive_ptr_test intrusive_ptr_test.cpp)
add_test(NAME krepis.intrusive_ptr COMMAND krepis_intrusive_ptr_test)

krepis_add_test(krepis_ink_types_test ink_types_test.cpp)
add_test(NAME krepis.ink_types COMMAND krepis_ink_types_test)

krepis_add_test(krepis_document_revision_test document_revision_test.cpp)
add_test(NAME krepis.document_revision COMMAND krepis_document_revision_test)

krepis_add_test(krepis_dogfood_file_test dogfood_file_test.cpp)
add_test(NAME krepis.dogfood_file COMMAND krepis_dogfood_file_test)

krepis_add_test(krepis_location_index_test location_index_test.cpp)
add_test(NAME krepis.location_index COMMAND krepis_location_index_test)

krepis_add_test(krepis_flow_layout_index_test flow_layout_index_test.cpp)
add_test(NAME krepis.flow_layout_index COMMAND krepis_flow_layout_index_test)

krepis_add_test(krepis_flow_sequence_test flow_sequence_test.cpp)
add_test(NAME krepis.flow_sequence COMMAND krepis_flow_sequence_test)

krepis_add_test(krepis_paragraph_record_test paragraph_record_test.cpp)
add_test(NAME krepis.paragraph_record COMMAND krepis_paragraph_record_test)

krepis_add_test(krepis_utf8_test utf8_test.cpp)
add_test(NAME krepis.utf8 COMMAND krepis_utf8_test)

krepis_add_test(krepis_text_analysis_test text_analysis_test.cpp)
add_test(NAME krepis.text_analysis COMMAND krepis_text_analysis_test)

krepis_add_test(krepis_text_shaper_test text_shaper_test.cpp)
target_compile_definitions(krepis_text_shaper_test PRIVATE
    KREPIS_TEST_LATIN_FONT="${harfbuzz_SOURCE_DIR}/test/subset/data/fonts/Roboto-Regular.ttf"
    KREPIS_TEST_HAN_FONT="${harfbuzz_SOURCE_DIR}/test/subset/data/fonts/SourceHanSans-Regular_subset.otf"
    KREPIS_TEST_ARABIC_FONT="${harfbuzz_SOURCE_DIR}/test/subset/data/fonts/Amiri-Regular.ttf"
)
add_test(NAME krepis.text_shaper COMMAND krepis_text_shaper_test)

krepis_add_test(krepis_text_layout_test text_layout_test.cpp)
target_compile_definitions(krepis_text_layout_test PRIVATE
    KREPIS_TEST_LATIN_FONT="${harfbuzz_SOURCE_DIR}/test/subset/data/fonts/Roboto-Regular.ttf"
)
add_test(NAME krepis.text_layout COMMAND krepis_text_layout_test)

krepis_add_test(krepis_transaction_test transaction_test.cpp)
add_test(NAME krepis.transaction COMMAND krepis_transaction_test)

krepis_add_test(krepis_editing_test editing_test.cpp)
add_test(NAME krepis.editing COMMAND krepis_editing_test)

krepis_add_test(krepis_editing_session_test editing_session_test.cpp)
add_test(NAME krepis.editing_session COMMAND krepis_editing_session_test)

krepis_add_test(krepis_undo_test undo_test.cpp)
add_test(NAME krepis.undo COMMAND krepis_undo_test)

krepis_add_test(krepis_layout_invalidation_test layout_invalidation_test.cpp)
add_test(NAME krepis.layout_invalidation COMMAND krepis_layout_invalidation_test)

krepis_add_test(krepis_spatial_container_test spatial_container_test.cpp)
add_test(NAME krepis.spatial_container COMMAND krepis_spatial_container_test)
