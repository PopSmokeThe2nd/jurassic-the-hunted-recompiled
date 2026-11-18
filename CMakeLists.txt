# jurassichunted - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(jurassichunted LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(JURASSICHUNTED_SOURCES
    src/main.cpp
    src/cheats.cpp
    src/fixes.cpp
    src/game_files.cpp
    src/dev_lua.cpp
    src/crash_log.cpp
    src/button_prompts.cpp
    src/framerate.cpp
    src/graphics_settings.cpp
    src/menu_mouse.cpp
    src/pc_input.cpp
    src/pc_options.cpp
    src/settings_file.cpp
    src/test_input.cpp
    src/weapon_art.cpp
    src/weapon_wheel.cpp
)

if(WIN32)
    enable_language(RC)  # icons, embedded launcher art, version info
    list(APPEND JURASSICHUNTED_SOURCES src/jurassichunted.rc)
    add_executable(jurassichunted WIN32 ${JURASSICHUNTED_SOURCES})
else()
    add_executable(jurassichunted ${JURASSICHUNTED_SOURCES})
endif()

rexglue_setup_target(jurassichunted GPU_PLUGINS xenos)
target_include_directories(jurassichunted PRIVATE third_party/stb)  # stb_image (PNG decoding)

if(WIN32)
    target_link_libraries(jurassichunted PRIVATE user32)  # GetAsyncKeyState (src/pc_input.cpp)

    # AMD FidelityFX runtime (FSR). rexruntime delay-loads it, but the installed SDK package doesn't
    # export the target, so the SDK's own staging step misses it; copy it from the SDK's bin dir.
    if(rexglue_DIR)
        get_filename_component(_rexglue_bin "${rexglue_DIR}/../../../bin" ABSOLUTE)
        file(GLOB _ffx_dlls "${_rexglue_bin}/amd_fidelityfx_*.dll")
        if(_ffx_dlls)
            add_custom_command(TARGET jurassichunted POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffx_dlls}
                        $<TARGET_FILE_DIR:jurassichunted>
                VERBATIM)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# Launcher: JurassicTheHunted.exe (graphics/cheat settings, ISO install, PLAY). Standalone Win32 +
# Direct3D 11 + Dear ImGui app; doesn't link the ReXGlue runtime. Built next to the game exe.
# ---------------------------------------------------------------------------------------------
if(WIN32)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_executable(launcher WIN32
        launcher/main.cpp
        launcher/iso_install.cpp
        launcher/prepare.cpp
        launcher/launcher.rc
        src/settings_file.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    )
    set_target_properties(launcher PROPERTIES
        OUTPUT_NAME JurassicTheHunted
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:jurassichunted>)
    target_include_directories(launcher PRIVATE ${IMGUI_DIR} src)
    target_compile_definitions(launcher PRIVATE UNICODE _UNICODE)
    target_link_libraries(launcher PRIVATE
        d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 bcrypt user32 dwmapi)
    # Art is embedded via the .rc files; rebuild the resources when the asset files change. Neither
    # set is in the repository (the launcher art is the game's own), so each is optional.
    # JTH_NO_GAME_ART=ON leaves both out even when present: release packages for others
    # (tools/package.ps1 -Release) must not contain the game's art or the button images.
    option(JTH_NO_GAME_ART "Build without the launcher art and button images (release packages)" OFF)
    set(_art "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets")
    set(_rc_defs "")
    if(JTH_NO_GAME_ART)
        message(STATUS "JTH_NO_GAME_ART: building without the launcher art and button images")
    elseif(EXISTS "${_art}/icon.ico" AND EXISTS "${_art}/background.jpg" AND EXISTS "${_art}/logo.png")
        list(APPEND _rc_defs JTH_LAUNCHER_ART)
    else()
        message(STATUS "launcher/assets (icon, background, logo) not found: building without them")
    endif()
    file(GLOB _glyph_pngs "${CMAKE_CURRENT_SOURCE_DIR}/assets/glyphs/*.png")
    if(NOT JTH_NO_GAME_ART AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/glyphs/ps_cross.png")
        list(APPEND _rc_defs JTH_GLYPHS)
    elseif(NOT JTH_NO_GAME_ART)
        message(STATUS "assets/glyphs not found: button images are drawn in code")
    endif()
    set_source_files_properties(launcher/launcher.rc src/jurassichunted.rc PROPERTIES
        COMPILE_DEFINITIONS "${_rc_defs}")
    set_source_files_properties(launcher/launcher.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/icon.ico;${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/background.jpg;${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/logo.png;${_glyph_pngs}")
    set_source_files_properties(src/jurassichunted.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/icon.ico;${_glyph_pngs}")
endif()
