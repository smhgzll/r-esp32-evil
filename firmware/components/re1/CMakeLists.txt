# The game: decomp game logic + this port's platform layer and software
# renderer, built as one static library whose .bss is placed in PSRAM
# (linker.lf) - the game's fixed buffers are ~5 MB.
get_filename_component(RE1_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include(${RE1_ROOT}/port/sources.cmake)

idf_component_register(SRCS ${RE1_DECOMP_SOURCES} ${RE1_PORT_SOURCES}
                       INCLUDE_DIRS ${RE1_ROOT}/port/include
                       PRIV_INCLUDE_DIRS ${RE1_DECOMP}/src ${RE1_ROOT}/port/marni
                       PRIV_REQUIRES esp_timer pthread re1_board
                       LDFRAGMENTS linker.lf)

if(CONFIG_RE1_TARGET_QEMU)
    set(_re1_target_name "esp32s3-qemu")
    # The emulated UART is fast enough to carry the mix to the browser.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RE1_REMOTE_AUDIO)
else()
    set(_re1_target_name "esp32s3")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RE1_TARGET_NAME="${_re1_target_name}"
    RE1_FB_W=160 RE1_FB_H=120
    RE1_PORT_TASKSWITCH_EXTERNAL
    RE1_PORT_OVERRIDE_RAND
    RE1_SMALL_MEM
    RE1_BG_ON_DEMAND
    RE1_PORT_VIDEO_RV1
    RE1_PORT_TEXTURE_BGR555
    RE1_PORT_TEXT_SCALE
    RE1_PORT_TEXTURE_INDEXED
    RE1_PORT_VRAM16
    RE1_PORT_EXTRAS
    RE1_PORT_BENCH
    RE1_PORT_PROFILE
    RE1_PORT_FAST
    RE1_PORT_TEXTURE_PAL8
    RE1_NO_HD_TEXTURES
    RE1_TMD_MAX_TRIS_COLLECT=3072
    RE1_AUDIO_STREAM_THRESHOLD=65536)
# -fsigned-char: the decomp assumes x86 (signed) plain char; Xtensa's is unsigned.
# -fno-char8_t: u8"" literals must stay char arrays (decomp is C++17, IDF C++23).
# -fcheck-new: operator new returns NULL on failure here (re1_board/newdelete.cpp),
#   so the compiler must keep the game's own NULL checks after new.
target_compile_options(${COMPONENT_LIB} PRIVATE ${RE1_COMMON_OPTIONS} -O2 -fsigned-char -fno-char8_t -fcheck-new)
