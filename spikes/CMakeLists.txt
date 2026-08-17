# Spikes 建置配置

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

# Spike 3: Submission Boundary Evaluation
add_executable(spike3_submission_boundary
    spike3_submission_boundary/spike3_main.cpp
)

target_compile_features(spike3_submission_boundary PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(spike3_submission_boundary PRIVATE /utf-8 /W4 /permissive-)
endif()
