# fifastreet4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fifastreet4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Launcher: native graphics system (no GPU plugin) and PC options.
set(FIFASTREET4_SOURCES
    src/main.cpp
    src/pc_settings.cpp
    src/pc_theme.cpp
    src/pc_font.cpp
)

if(WIN32)
    add_executable(fifastreet4 WIN32 ${FIFASTREET4_SOURCES})
else()
    add_executable(fifastreet4 ${FIFASTREET4_SOURCES})
endif()
target_include_directories(fifastreet4 PRIVATE src)

# No GPU_PLUGINS: the Xenos emulation plugin is intentionally not staged.
rexglue_setup_target(fifastreet4)

# Hooks on functions of the game module must be compiled into that module.
if(TARGET fifastreet4_fifadllzf_xex)
    target_sources(fifastreet4_fifadllzf_xex PRIVATE
        src/kernel_extra.cpp
        src/menu_hooks.cpp
        src/native/d3d_hooks.cpp
        src/native/renderer.cpp
        src/native/shader_translator.cpp
        src/native/texture_loader.cpp
        src/native/native_audio.cpp
        src/native/game_log.cpp
        src/pc_settings.cpp
        src/pc_theme.cpp
        src/pc_font.cpp
    )
    target_include_directories(fifastreet4_fifadllzf_xex PRIVATE src src/native)
    # XMA decoding with the SDK's prebuilt libavcodec (Xenia FFmpeg fork); headers from
    # the matching commit in third_party/ffmpeg.
    target_include_directories(fifastreet4_fifadllzf_xex PRIVATE ../third_party/ffmpeg)
    find_library(STREET4_AVCODEC NAMES libavcodecrd libavcodec PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
    find_library(STREET4_AVUTIL NAMES libavutilrd libavutil PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
    target_link_libraries(fifastreet4_fifadllzf_xex PRIVATE ${STREET4_AVCODEC} ${STREET4_AVUTIL})
    if(WIN32)
        target_link_libraries(fifastreet4_fifadllzf_xex PRIVATE d3d11 dxgi xaudio2 ole32 bcrypt)
    endif()
endif()


# Offline shader translator check (tool; never runs the game).
if(WIN32)
    add_executable(street4_shader_test tools/shader_test.cpp src/native/shader_translator.cpp)
    target_include_directories(street4_shader_test PRIVATE src ${CMAKE_PREFIX_PATH}/include)
    target_compile_definitions(street4_shader_test PRIVATE NOMINMAX)
endif()

# Launcher dispatch table relocation (see src/street4_layout.h): appended to the
# recompiled launcher's precompiled header so it is processed after the generated one.
if(TARGET fifastreet4_recomp)
    target_precompile_headers(fifastreet4_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/street4_layout.h")
endif()
if(TARGET fifastreet4_fifadllzf_xex)
    target_precompile_headers(fifastreet4_fifadllzf_xex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/street4_dll_dispatch.h")
endif()

# Vulkan option of the settings menu: DXVK's d3d11.dll/dxgi.dll in vulkan\ next to the
# executable (pc_settings.cpp loads them by full path when "API grafica" is Vulkan).
set(DXVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dxvk-3.1.1" CACHE PATH
    "DXVK release for Windows (x64/d3d11.dll, x64/dxgi.dll, LICENSE)")
if(WIN32 AND EXISTS "${DXVK_DIR}/x64/d3d11.dll")
    add_custom_command(TARGET fifastreet4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:fifastreet4>/vulkan"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/x64/d3d11.dll" "${DXVK_DIR}/x64/dxgi.dll"
                "$<TARGET_FILE_DIR:fifastreet4>/vulkan/")
    if(EXISTS "${DXVK_DIR}/LICENSE")
        add_custom_command(TARGET fifastreet4 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/LICENSE"
                    "$<TARGET_FILE_DIR:fifastreet4>/vulkan/LICENSE.dxvk.txt")
    endif()
endif()
