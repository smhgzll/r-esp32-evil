# sources.cmake - translation units shared by every r-esp32-evil target.
#
# Included by port/host/CMakeLists.txt and firmware/components/re1/CMakeLists.txt.
# Expects RE1_ROOT (repository root). Defines:
#   RE1_DECOMP_SOURCES  game logic + Marni layer from the decomp (unchanged
#                       apart from patches/*.patch) - the same list as the
#                       decomp's own Linux CMakeLists.txt, minus its SDL/GL
#                       platform layer
#   RE1_PORT_SOURCES    this port's platform layer + software renderer
#   RE1_INCLUDE_DIRS
set(RE1_DECOMP "${RE1_ROOT}/decomp")
if(NOT EXISTS "${RE1_DECOMP}/src/Globals.cpp")
    message(FATAL_ERROR "decomp/ is missing - run tools/fetch_decomp.sh (or: python3 tools/re1tool.py fetch)")
endif()

set(_d "${RE1_DECOMP}/src")
set(RE1_DECOMP_SOURCES
    ${_d}/Globals.cpp
    ${_d}/system/AssetPath.cpp
    ${_d}/system/AudioFile.cpp
    ${_d}/system/PngImage.cpp
    ${_d}/system/WebpImage.cpp
    ${_d}/system/HdTextures.cpp
    ${_d}/system/ConfigFile.cpp
    ${_d}/marni/MarniBits.cpp
    ${_d}/marni/PSXTexture.cpp
    ${_d}/marni/Marni3DObject.cpp
    ${_d}/marni/MarniSystem.cpp
    ${_d}/video/VideoPlayback.cpp
)
foreach(f
    Bonus CharacterSelectionScreen CmdFunctions CollisionDebug ComputerLab DeathScreen
    DebugMenu DebugSaveLoad DebugScreens DoorSystem EffectSprites EffectSystem EndingScreen
    Ps1EndingCredits Ps1FmvSubtitles EntityModelLoader FadeSprite FileLoader GameInit GameLoop
    GameStart GteMatrix InputSystem InteractiveScreen JpnTextTables LabSlides LogosScreen
    MainLoop MainMenu MenuData ObjectManager OptionsMenu PathTrail PlayerAnimations HdText
    PrintText Rendering Room RoomBackgroundWebp RoomCollision RoomEvents RoomInit
    SaveLoadScreen SoundApi SoundSystem SoundTables SpriteRenderer TaskScheduler
    TextureLoader GameOptions TitleMenuFont TitleScreen TmdAnimation TmdRenderer WeaponDamage)
    list(APPEND RE1_DECOMP_SOURCES ${_d}/game/${f}.cpp)
endforeach()
foreach(f WeaponDamageTables ItemTables ItemNames ItemModels ItemDescriptions Items
          EntityModels ArrangeStages ArrangeSoundRows)
    list(APPEND RE1_DECOMP_SOURCES ${_d}/game/dc/${f}.cpp)
endforeach()
foreach(f SaturnBattle SaturnModels SaturnRoomObjects SaturnTick SaturnTickAnim SaturnTyrant)
    list(APPEND RE1_DECOMP_SOURCES ${_d}/game/saturn/${f}.cpp)
endforeach()
foreach(f Adder BlackTiger Cerberus CharacterNpc Chimera ComputerArms Crow EntityCommon Hunter
          MonsterPlant Neptune Plant42 Plant42Roots SpiderWeb Tyrant Wasp WebSpinner Yawn Zombie)
    list(APPEND RE1_DECOMP_SOURCES ${_d}/game/entities/${f}.cpp)
endforeach()

set(RE1_PORT_SOURCES
    ${RE1_ROOT}/port/common/Audio.cpp
    ${RE1_ROOT}/port/common/BigAlloc.cpp
    ${RE1_ROOT}/port/common/Extras.cpp
    ${RE1_ROOT}/port/common/GameMain.cpp
    ${RE1_ROOT}/port/common/Input.cpp
    ${RE1_ROOT}/port/common/Platform.cpp
    ${RE1_ROOT}/port/common/PortMenu.cpp
    ${RE1_ROOT}/port/common/Prof.cpp
    ${RE1_ROOT}/port/common/FastMath.cpp
    ${RE1_ROOT}/port/common/Remote.cpp
    ${RE1_ROOT}/port/common/Video.cpp
    ${RE1_ROOT}/port/marni/MarniDX_SW.cpp
)

set(RE1_INCLUDE_DIRS
    ${_d}
    ${RE1_ROOT}/port/include
    ${RE1_ROOT}/port/marni
)

# Compile options both targets need: the decomp keeps the original's MSVC
# assumptions (raw-buffer struct casts, wrap-around signed math) - see the
# decomp's CMakeLists.txt for the failure that motivated them.
set(RE1_COMMON_OPTIONS -fno-strict-aliasing -fno-strict-overflow -fsigned-char -w)
