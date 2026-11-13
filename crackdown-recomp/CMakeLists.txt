# Crackdown: the recompiled game and its app, built inside gta4-ps5's tree
# (glue/rexglue-sdk-main/crackdown-recomp, see .github/workflows/ps5-build.yml) as
# one static PS5 program, the way gta4-recomp builds GTA IV.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake")
    message(FATAL_ERROR "crackdown-recomp generated sources are missing: run rexglue codegen crackdown_manifest.toml")
endif()
include(generated/sources.cmake)

set(CD_APP_SOURCES
    src/main.cpp
    src/crackdown_app.cpp
    src/hooks_ps5.cpp
    src/frame_pacing_ps5.cpp
    src/save_load_ps5.cpp
    src/audio_sync_ps5.cpp
    src/core_layout_ps5.cpp
    src/locale_ps5.cpp
    src/skip_intro_ps5.cpp
)

if(PS5)
    # The menu (its fonts: gta4-ps5's, SIL Open Font License) and gta4-ps5's
    # own sound mixer for its song.
    set(GTA4_PS5_SRC "${REXSDK_DIR}/gta4-recomp/src/ps5")
    set(CD_PS5_FONTS "${REXSDK_DIR}/../../ps5/assets/fonts")
    list(APPEND CD_APP_SOURCES src/ps5/crackdown_menu.cpp src/ps5/cd_trophies.cpp src/ps5/cd_trophies_game.cpp
         src/ps5/cd_install.cpp src/ps5/cd_glyphs.cpp src/ps5/cd_overlay.cpp src/ps5/cd_diag.cpp src/dualsense_ps5.cpp
         src/ps5/cd_quick_menu.cpp src/cheats_ps5.cpp src/ps5/cd_ptbr_install.cpp
         src/ps5/cd3_setup.cpp src/ps5/qrcodegen/qrcodegen.cpp
         # (the PC recomp's, copied in by ci/local_build.sh)
         src/font_atlas.cpp src/cheats.cpp src/cheat_hooks.cpp src/orb_map.cpp
         "${GTA4_PS5_SRC}/game/sound.cpp" "${GTA4_PS5_SRC}/hub/hub_sfx.cpp")
    set_source_files_properties(src/ps5/crackdown_menu.cpp PROPERTIES
        COMPILE_DEFINITIONS "CDPS5_FONTS_DIR=\"${CD_PS5_FONTS}\"")
    # Crackdown's text in Brazilian Portuguese (cd_ptbr_install.h): CD_PTBR_BIN, made from the user's game
    # files (Recomps/ptbr/crackdown pack.py; ci/local_build.sh passes it); without it, an empty blob (English).
    if(NOT CD_PTBR_BIN OR NOT EXISTS "${CD_PTBR_BIN}")
        set(CD_PTBR_BIN "${CMAKE_CURRENT_BINARY_DIR}/ptbr-none.bin")
        file(WRITE "${CD_PTBR_BIN}" "")
    endif()
    set_source_files_properties(src/ps5/cd_ptbr_install.cpp PROPERTIES
        COMPILE_DEFINITIONS "CDPS5_PTBR_BIN=\"${CD_PTBR_BIN}\""
        OBJECT_DEPENDS "${CD_PTBR_BIN}")
    add_executable(crackdown ${CD_APP_SOURCES} ${GENERATED_SOURCES})
    target_include_directories(crackdown PRIVATE "${GTA4_PS5_SRC}")
    # The recompiled game's own files, as gta4-recomp marks GTA IV's.
    set(CD_PS5_GUEST_SOURCES ${GENERATED_SOURCES})
    list(FILTER CD_PS5_GUEST_SOURCES INCLUDE REGEX "crackdown_recomp\\.[0-9]+\\.cpp$")
    set_property(SOURCE ${CD_PS5_GUEST_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS REX_PS5_GUEST_CODE=1)
    set_property(SOURCE generated/crackdown_register.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release,RelWithDebInfo>:-O0>")
    target_compile_definitions(crackdown PRIVATE REX_FUSED_VMADDFP=1)
    target_include_directories(crackdown PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/inja/third_party/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/stb
    )
    target_compile_options(crackdown PRIVATE "SHELL:-include ${REXSDK_DIR}/include/rex/ps5_compat.h")
    target_link_libraries(crackdown PRIVATE
        rex::runtime
        rexgpu-xenos
        xxHash::xxhash
        SDL3::SDL3
        SceUserService ScePad SceAudioOut SceSystemService SceVideoOut
    )
    rexglue_configure_target(crackdown)
else()
    add_executable(crackdown ${CD_APP_SOURCES} ${GENERATED_SOURCES})
    target_include_directories(crackdown PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
    )
    target_link_libraries(crackdown PRIVATE rex::runtime)
    rexglue_configure_target(crackdown GPU_PLUGINS xenos)
endif()

# Crackdown 2 (cd2.bin, the program the menu starts for it: cd2/src/crackdown2_app.h), from its own
# codegen (cd2/crackdown2_manifest.toml) with the menu's shared pieces. Built when its generated sources
# are there (ci/local_build.sh makes them from your Crackdown 2 default.xex and Title Update 5's xexp).
if(PS5 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cd2/generated/sources.cmake")
    set(GENERATED_SOURCES_CD1 ${GENERATED_SOURCES})
    set(GENERATED_SOURCES "")
    include(cd2/generated/sources.cmake)
    set(CD2_SOURCES
        cd2/src/main.cpp
        cd2/src/crackdown2_app.cpp cd2/src/cd2_features.cpp cd2/src/cd2_cheats.cpp cd2/src/cd2_prompts.cpp cd2/src/cd2_prompts_core.cpp src/ps5/cd_quick_menu.cpp
        src/ps5/crackdown_menu.cpp src/ps5/cd_trophies.cpp src/ps5/cd_trophies_game.cpp
        src/ps5/cd_install.cpp src/ps5/cd_overlay.cpp src/ps5/cd_diag.cpp src/ps5/cd3_setup.cpp src/ps5/qrcodegen/qrcodegen.cpp
        "${GTA4_PS5_SRC}/game/sound.cpp" "${GTA4_PS5_SRC}/hub/hub_sfx.cpp")
    add_executable(crackdown2 ${CD2_SOURCES} ${GENERATED_SOURCES})
    target_include_directories(crackdown2 PRIVATE "${GTA4_PS5_SRC}")
    set(CD2_PS5_GUEST_SOURCES ${GENERATED_SOURCES})
    list(FILTER CD2_PS5_GUEST_SOURCES INCLUDE REGEX "crackdown2_recomp\.[0-9]+\.cpp$")
    set_property(SOURCE ${CD2_PS5_GUEST_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS REX_PS5_GUEST_CODE=1)
    set_property(SOURCE cd2/generated/crackdown2_register.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release,RelWithDebInfo>:-O0>")
    target_compile_definitions(crackdown2 PRIVATE REX_FUSED_VMADDFP=1 CD_GAME=2)
    target_include_directories(crackdown2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/cd2/src
        ${CMAKE_CURRENT_SOURCE_DIR}/cd2/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/inja/third_party/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/stb
    )
    target_compile_options(crackdown2 PRIVATE "SHELL:-include ${REXSDK_DIR}/include/rex/ps5_compat.h")
    target_link_libraries(crackdown2 PRIVATE
        rex::runtime
        rexgpu-xenos
        xxHash::xxhash
        SDL3::SDL3
        SceUserService ScePad SceAudioOut SceSystemService SceVideoOut
    )
    rexglue_configure_target(crackdown2)
    set(GENERATED_SOURCES ${GENERATED_SOURCES_CD1})
endif()
