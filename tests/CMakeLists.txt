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

krepis_add_test(krepis_location_index_test location_index_test.cpp)
add_test(NAME krepis.location_index COMMAND krepis_location_index_test)

krepis_add_test(krepis_flow_layout_index_test flow_layout_index_test.cpp)
add_test(NAME krepis.flow_layout_index COMMAND krepis_flow_layout_index_test)

krepis_add_test(krepis_flow_sequence_test flow_sequence_test.cpp)
add_test(NAME krepis.flow_sequence COMMAND krepis_flow_sequence_test)
