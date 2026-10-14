# DualShock 4 / DualSense read straight from HID, for the in-game menu and the launcher.
add_library(dingosdk_playstation_input STATIC Extension/UI/Overlay/playstation_input.cpp)
target_link_libraries(dingosdk_playstation_input PUBLIC dingosdk_logging PRIVATE hid cfgmgr32)

add_library(dingosdk_overlay STATIC

    Extension/UI/Overlay/overlay.cpp
    Extension/UI/Overlay/overlay_render.cpp
    Extension/UI/Overlay/overlay_console.cpp
    Extension/UI/Overlay/overlay_input.cpp
    Extension/UI/Overlay/item_browser_overlay.cpp
    Extension/UI/Overlay/overlay_notices.cpp
    Extension/UI/Overlay/chat_overlay.cpp
    Extension/UI/Overlay/map_download_card.cpp
    Extension/UI/Overlay/message_card.cpp
    Extension/UI/Overlay/hub_page.cpp
    Extension/UI/Overlay/game_text_overlay.cpp
    Extension/UI/Overlay/drone_hud_overlay.cpp
    Extension/UI/Overlay/drone_page.cpp
    Extension/UI/Overlay/perf_overlay.cpp
    Extension/UI/Overlay/skate_hud_overlay.cpp
    Extension/UI/Overlay/nametag_overlay.cpp
    Extension/UI/Overlay/chat_emotes.cpp
    Extension/UI/Overlay/chat_rich.cpp
    Extension/UI/Overlay/console_suggestions.cpp
    Extension/UI/Overlay/skate_menu.cpp
    Extension/UI/Overlay/skate_menu_world.cpp
    Extension/UI/Overlay/skate_menu_skater.cpp
    Extension/UI/Overlay/skate_menu_settings.cpp
    Extension/UI/Overlay/park_editor.cpp
    Extension/UI/Overlay/park_editor_actions.cpp
    Extension/UI/Overlay/park_editor_panels.cpp
    Extension/UI/Overlay/park_editor_viewport.cpp
    Extension/UI/Overlay/park_previews.cpp
    Extension/UI/Overlay/gpu_diagnostics.cpp
    Extension/UI/Overlay/input_capture.cpp
    Extension/UI/Overlay/cursor.cpp
    Extension/UI/Overlay/progression_menu.cpp
    Extension/UI/Overlay/atmosphere_menu.cpp
    Extension/UI/Overlay/modding_menu.cpp
    Extension/UI/Overlay/multiplayer_menu.cpp
    Extension/UI/Overlay/multiplayer_lobbies.cpp
    Extension/UI/Overlay/multiplayer_session.cpp
    Extension/Trainer/trainer_page.cpp
    Extension/Music/music_page.cpp
    Extension/Trainer/trainer_view.cpp
    Extension/HallOfMeat/hall_of_meat_overlay.cpp
    Extension/Filmer/filmer_view.cpp
    Extension/Filmer/filmer_look.cpp
    Extension/Filmer/filmer_page.cpp
)
target_link_libraries(dingosdk_overlay PUBLIC dingosdk_logging dingosdk_profiler dingosdk_imgui dingosdk_hooks dingosdk_console_core dxguid PRIVATE dingosdk_playstation_input shell32 dingosdk_initfs dingosdk_custom_scripts dingosdk_game_archives dingosdk_mods dingosdk_json windowscodecs ole32)

# The window shown from the moment ReSkate loads until the game's own window appears.
add_library(dingosdk_startup_window STATIC Extension/UI/Startup/startup_window.cpp)
target_link_libraries(dingosdk_startup_window PRIVATE gdi32 user32 msimg32 shell32 ole32 windowscodecs)

# The Filmer's VX1000 look: its shader is built into the DLL as text and compiled when it is first
# drawn. The build checks it with the Windows SDK's fxc when there is one, so a shader that would not
# compile in the game fails here instead.
set(filmer_look_path "${PROJECT_SOURCE_DIR}/Extension/Filmer/filmer_look.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${filmer_look_path}")
file(READ "${filmer_look_path}" filmer_look_source)
configure_file(cmake/templates/filmer_look_hlsl.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/filmer_look_hlsl.h" @ONLY)
if(WIN32)
    file(GLOB filmer_fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    if(filmer_fxc_candidates)
        list(SORT filmer_fxc_candidates)
        list(REVERSE filmer_fxc_candidates)
        list(GET filmer_fxc_candidates 0 filmer_fxc_newest)
        set(DINGOSDK_FXC "${filmer_fxc_newest}" CACHE FILEPATH "fxc.exe that checks the Filmer's look shader")
    else()
        find_program(DINGOSDK_FXC fxc)
    endif()
    if(DINGOSDK_FXC)
        set(filmer_look_checked "${CMAKE_CURRENT_BINARY_DIR}/generated/filmer_look.checked")
        add_custom_command(OUTPUT "${filmer_look_checked}"
            COMMAND "${DINGOSDK_FXC}" /nologo /T vs_5_0 /E vs_main /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/filmer_look_vs.cso" "${filmer_look_path}"
            COMMAND "${DINGOSDK_FXC}" /nologo /T ps_5_0 /E ps_main /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/filmer_look_ps.cso" "${filmer_look_path}"
            COMMAND "${DINGOSDK_FXC}" /nologo /T ps_5_0 /E ps_adapt /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/filmer_look_iris.cso" "${filmer_look_path}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${filmer_look_checked}"
            DEPENDS "${filmer_look_path}"
            COMMENT "Checking the Filmer's look shader with fxc"
            VERBATIM)
        add_custom_target(dingosdk_filmer_look_check DEPENDS "${filmer_look_checked}")
        add_dependencies(dingosdk_overlay dingosdk_filmer_look_check)
        message(STATUS "Filmer look shader: checked with ${DINGOSDK_FXC}")
    else()
        message(STATUS "Filmer look shader: no fxc found; it is only compiled in the game.")
    endif()
endif()
