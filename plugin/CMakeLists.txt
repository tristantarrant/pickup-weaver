cmake_minimum_required(VERSION 3.21)
project(PickupWeaver C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Release hygiene: no absolute source paths in the binaries (DPF's assertion messages embed
# __FILE__), and stripped symbols in release builds.
get_filename_component(PW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT MSVC)
    add_compile_options("-ffile-prefix-map=${PW_ROOT}/=")
    if(NOT APPLE)
        string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " -s")
        string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE " -s")
    endif()
endif()

# DPF (ISC license): ../dpf is a submodule of https://github.com/DISTRHO/DPF
set(DPF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dpf" CACHE PATH "DPF checkout")
if(NOT EXISTS "${DPF_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DPF not found at ${DPF_DIR}: run 'git submodule update --init --recursive'")
endif()

# Local DPF fix (VST3 parameter flush / controller echo), applied here
# idempotently so that dpf/ stays a clean submodule checkout. The plugin
# enables it with DPF_VST3_PATCH_FLUSH_AND_CONTROLLER_ECHO in DistrhoPluginInfo.h.
set(PW_DPF_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/dpf-patches/0001-vst3-parameter-flush-and-controller-echo.patch")
set(PW_VST3_CPP "${DPF_DIR}/distrho/src/DistrhoPluginVST3.cpp")
if(EXISTS "${PW_DPF_PATCH}" AND EXISTS "${PW_VST3_CPP}")
    file(READ "${PW_VST3_CPP}" PW_VST3_SRC)
    if(NOT PW_VST3_SRC MATCHES "DPF_VST3_PATCH_FLUSH_AND_CONTROLLER_ECHO")
        execute_process(COMMAND git -C "${DPF_DIR}" apply --whitespace=nowarn "${PW_DPF_PATCH}"
                        RESULT_VARIABLE PW_PATCH_RC
                        OUTPUT_VARIABLE PW_PATCH_OUT
                        ERROR_VARIABLE PW_PATCH_ERR)
        if(NOT PW_PATCH_RC EQUAL 0)
            message(FATAL_ERROR "cannot apply ${PW_DPF_PATCH} to ${DPF_DIR}: ${PW_PATCH_ERR}")
        endif()
        message(STATUS "Pickup Weaver: applied the local DPF VST3 patch")
    endif()
endif()

add_subdirectory("${DPF_DIR}" dpf)

# DSP core (pickup_weaver_dsp: header-only + runtime-dispatched AVX translation unit on x86)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../dsp" pickup_weaver_dsp)

dpf_add_plugin(pickup_weaver
    TARGETS lv2 vst2 vst3 clap
    FILES_DSP src/PluginPickupWeaver.cpp
    FILES_UI src/PluginPickupWeaverUI.cpp src/UIPainter.cpp src/UIAssets.cpp)

target_include_directories(pickup_weaver PUBLIC src)
target_include_directories(pickup_weaver PUBLIC "${DPF_DIR}/dgl/src")   # nanovg headers for the UI painter
target_link_libraries(pickup_weaver PUBLIC pickup_weaver_dsp)

# Ship the modgui (MOD Devices / MOD Desktop / Zynthian) inside the LV2 bundle.
# The manifest wiring (rdfs:seeAlso <modgui.ttl>) comes from DISTRHO_PLUGIN_USES_MODGUI.
set(PW_LV2_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/bin/pickup_weaver.lv2")
add_custom_target(pickup_weaver_modgui ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PW_LV2_BUNDLE}/modgui"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/modgui.ttl"
        "${PW_LV2_BUNDLE}/modgui.ttl"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/icon.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/stylesheet.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/background.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/knobs.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/screenshot.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/modgui/thumbnail.png"
        "${PW_LV2_BUNDLE}/modgui"
    COMMENT "shipping modgui into the LV2 bundle")
add_dependencies(pickup_weaver_modgui pickup_weaver-lv2)

# Offscreen GUI renderer (developer tool: screenshots and modgui assets; needs Xvfb at runtime)
if(NOT MSVC)
    add_executable(pw_render_ui
        tools/render_ui.cpp
        src/UIPainter.cpp
        src/UIAssets.cpp)
    target_include_directories(pw_render_ui PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}/../dsp"
        "${DPF_DIR}/dgl/src")
    target_link_libraries(pw_render_ui PRIVATE X11 GL)
endif()
