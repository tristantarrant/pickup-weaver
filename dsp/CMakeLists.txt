cmake_minimum_required(VERSION 3.21)
project(PickupWeaverDSP CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- library target to link into a plugin (header-only + optional AVX dispatch file) ----
add_library(pickup_weaver_dsp STATIC PickupWeaverDSP_AVX.cpp)
target_include_directories(pickup_weaver_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pickup_weaver_dsp PUBLIC cxx_std_17)
# linked into shared plugin modules: PIC, nothing exported
set_target_properties(pickup_weaver_dsp PROPERTIES POSITION_INDEPENDENT_CODE TRUE
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN TRUE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    # Only the dispatch file is compiled with AVX; everything else keeps the target's baseline (SSE2).
    if(MSVC)
        set_source_files_properties(PickupWeaverDSP_AVX.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")
    else()
        set_source_files_properties(PickupWeaverDSP_AVX.cpp PROPERTIES COMPILE_OPTIONS "-mavx")
    endif()
    target_compile_definitions(pickup_weaver_dsp PUBLIC PW_ENABLE_AVX_DISPATCH=1)
else()
    # ARM64 (NEON) and others: no dispatch file needed.
    set_source_files_properties(PickupWeaverDSP_AVX.cpp PROPERTIES HEADER_FILE_ONLY ON)
    target_sources(pickup_weaver_dsp PRIVATE tests/empty.cpp)
endif()

# ---- tools and tests (only when this is the top-level project) ----
if(PROJECT_IS_TOP_LEVEL)
    add_executable(pw_cli pw_cli.cpp)
    target_link_libraries(pw_cli PRIVATE pickup_weaver_dsp)
    add_executable(pw_cli_ref tests/pw_cli_ref.cpp)
    add_executable(test_vmath tests/test_vmath.cpp)
    add_executable(bench tests/bench.cpp)
    target_link_libraries(bench PRIVATE pickup_weaver_dsp)
endif()
