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
)

if(PS5)
    # The menu (its fonts: gta4-ps5's, SIL Open Font License) and gta4-ps5's
    # own sound mixer for its song.
    set(GTA4_PS5_SRC "${REXSDK_DIR}/gta4-recomp/src/ps5")
    set(CD_PS5_FONTS "${REXSDK_DIR}/../../ps5/assets/fonts")
    list(APPEND CD_APP_SOURCES src/ps5/crackdown_menu.cpp src/ps5/cd_trophies.cpp src/ps5/cd_trophies_game.cpp
         src/ps5/cd_install.cpp
         "${GTA4_PS5_SRC}/game/sound.cpp" "${GTA4_PS5_SRC}/hub/hub_sfx.cpp")
    set_source_files_properties(src/ps5/crackdown_menu.cpp PROPERTIES
        COMPILE_DEFINITIONS "CDPS5_FONTS_DIR=\"${CD_PS5_FONTS}\"")
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
