cmake_minimum_required(VERSION 3.24)

# The static CRT means the game needs no Visual C++ runtime on the target machine.
# It is the release one in every configuration, to match the PhysX libraries.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

project(WindsOfTeinora VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(WOT_LOCAL_DIR "${CMAKE_SOURCE_DIR}/.local")

add_library(TeinoraWarnings INTERFACE)
target_compile_options(TeinoraWarnings INTERFACE /W4 /WX /utf-8 /permissive- /Zi /FS)
target_compile_definitions(TeinoraWarnings INTERFACE _CRT_SECURE_NO_WARNINGS)
target_link_options(TeinoraWarnings INTERFACE /DEBUG /OPT:REF /OPT:ICF)

# ---------------------------------------------------------------- simulation
# No OS or graphics dependencies, so the tests run headless.
set(WOT_PHYSX_DIR "${WOT_LOCAL_DIR}/physx-sdk/static")
if(NOT EXISTS "${WOT_PHYSX_DIR}/lib/PhysX_static_64.lib")
    message(FATAL_ERROR "PhysX is not built. Run tools/build-physx.ps1.")
endif()

add_library(TeinoraPhysics STATIC Engine/Physics/Physics.cpp)
target_include_directories(TeinoraPhysics PUBLIC Engine)
target_include_directories(TeinoraPhysics SYSTEM PRIVATE "${WOT_PHYSX_DIR}/include")
target_compile_definitions(TeinoraPhysics PRIVATE PX_PHYSX_STATIC_LIB DISABLE_CUDA_PHYSX $<$<CONFIG:Debug>:NDEBUG>) # its libraries are release builds
target_link_libraries(TeinoraPhysics PRIVATE TeinoraWarnings)
foreach(WOT_LIB PhysX PhysXCommon PhysXFoundation PhysXExtensions PhysXCooking PhysXPvdSDK)
    target_link_libraries(TeinoraPhysics PUBLIC "${WOT_PHYSX_DIR}/lib/${WOT_LIB}_static_64.lib")
endforeach()

add_library(TeinoraSim STATIC Game/Liquid.cpp Game/MagicField.cpp Game/MagicShows.cpp Game/Plants.cpp Game/StarMap.cpp Game/Storms.cpp Game/SurfaceTextures.cpp Game/TeinoraSim.cpp Game/WorldMap.cpp)
target_include_directories(TeinoraSim PUBLIC Engine Game)
target_link_libraries(TeinoraSim PRIVATE TeinoraWarnings)

# ------------------------------------------------------------------- shaders
# One HLSL source per pass, compiled for both backends at build time and embedded.
set(WOT_DXC "${WOT_LOCAL_DIR}/dxc/bin/x64/dxc.exe")
set(WOT_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/Shaders")
set(WOT_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${WOT_SHADER_OUTPUT_DIR}")

set(WOT_SHADERS
    "TerrainVS Terrain.hlsl VSMain vs_6_0"
    "TerrainPS Terrain.hlsl PSMain ps_6_0"
    "TerrainRayPS Terrain.hlsl PSMain ps_6_5 RAY_TRACED"
    "FoliagePS Terrain.hlsl PSMain ps_6_0 FOLIAGE"
    "FoliageRayPS Terrain.hlsl PSMain ps_6_5 RAY_TRACED FOLIAGE"
    "FoliageShadowPS Terrain.hlsl ShadowMain ps_6_0 FOLIAGE"
    "ObjectVS Object.hlsl VSMain vs_6_0"
    "ObjectPS Object.hlsl PSMain ps_6_0"
    "ObjectRayPS Object.hlsl PSMain ps_6_5 RAY_TRACED"
    "FullscreenVS Fullscreen.hlsl VSMain vs_6_0"
    "SkyPS Sky.hlsl PSMain ps_6_0"
    "StarVS Stars.hlsl VSMain vs_6_0"
    "StarPS Stars.hlsl PSMain ps_6_0"
    "WaterVS Water.hlsl VSMain vs_6_0"
    "WaterPS Water.hlsl PSMain ps_6_0"
    "WaterRayPS Water.hlsl PSMain ps_6_5 RAY_TRACED"
    "BoltVS Bolts.hlsl VSMain vs_6_0"
    "BoltPS Bolts.hlsl PSMain ps_6_0"
    "FlameVS Flames.hlsl VSMain vs_6_0"
    "FlamePS Flames.hlsl PSMain ps_6_0"
    "PuffVS Puffs.hlsl VSMain vs_6_0"
    "PuffPS Puffs.hlsl PSMain ps_6_0"
    "UnderwaterPS Underwater.hlsl PSMain ps_6_0"
    "CopyPS Underwater.hlsl CopyMain ps_6_0"
    "MagicPS Magic.hlsl PSMain ps_6_0"
    "MagicBendPS Magic.hlsl BendMain ps_6_0"
    "TonemapPS Tonemap.hlsl PSMain ps_6_0"
    "BlurPS Blur.hlsl PSMain ps_6_0"
    "MotionPS Motion.hlsl PSMain ps_6_0"
    "FinalPS Fxaa.hlsl PSMain ps_6_0"
    "UiVS Ui.hlsl VSMain vs_6_0"
    "UiPS Ui.hlsl PSMain ps_6_0")

file(GLOB WOT_SHADER_HEADERS CONFIGURE_DEPENDS "${WOT_SHADER_SOURCE_DIR}/*.hlsli")
set(WOT_SHADER_OUTPUTS "")
set(WOT_SHADER_INCLUDES "")
set(WOT_SHADER_LIST "")

foreach(WOT_SHADER IN LISTS WOT_SHADERS)
    separate_arguments(WOT_PARTS UNIX_COMMAND "${WOT_SHADER}")
    list(GET WOT_PARTS 0 WOT_NAME)
    list(GET WOT_PARTS 1 WOT_FILE)
    list(GET WOT_PARTS 2 WOT_ENTRY)
    list(GET WOT_PARTS 3 WOT_PROFILE)

    set(WOT_DXIL "${WOT_SHADER_OUTPUT_DIR}/${WOT_NAME}.dxil.h")
    set(WOT_SPIRV "${WOT_SHADER_OUTPUT_DIR}/${WOT_NAME}.spirv.h")
    set(WOT_COMMON_FLAGS -nologo -Zpr -O3 -E ${WOT_ENTRY} -T ${WOT_PROFILE} -I "${WOT_SHADER_SOURCE_DIR}")
    list(LENGTH WOT_PARTS WOT_PART_COUNT)
    if(WOT_PART_COUNT GREATER 4)
        list(SUBLIST WOT_PARTS 4 -1 WOT_DEFINES)
        foreach(WOT_DEFINE IN LISTS WOT_DEFINES)
            list(APPEND WOT_COMMON_FLAGS -D ${WOT_DEFINE}=1)
        endforeach()
    endif()

    add_custom_command(OUTPUT "${WOT_DXIL}" "${WOT_SPIRV}"
        COMMAND "${WOT_DXC}" ${WOT_COMMON_FLAGS} -Wno-ignored-attributes
            -Vn g_${WOT_NAME}_Dxil -Fh "${WOT_DXIL}" "${WOT_SHADER_SOURCE_DIR}/${WOT_FILE}"
        COMMAND "${WOT_DXC}" ${WOT_COMMON_FLAGS} -spirv -fspv-target-env=vulkan1.3 -fspv-entrypoint-name=main
            -Vn g_${WOT_NAME}_Spirv -Fh "${WOT_SPIRV}" "${WOT_SHADER_SOURCE_DIR}/${WOT_FILE}"
        DEPENDS "${WOT_SHADER_SOURCE_DIR}/${WOT_FILE}" ${WOT_SHADER_HEADERS}
        COMMENT "Compiling shader ${WOT_NAME}"
        VERBATIM)

    list(APPEND WOT_SHADER_OUTPUTS "${WOT_DXIL}" "${WOT_SPIRV}")
    string(APPEND WOT_SHADER_INCLUDES "#include \"${WOT_NAME}.dxil.h\"\n#include \"${WOT_NAME}.spirv.h\"\n")
    string(APPEND WOT_SHADER_LIST "TEINORA_SHADER(${WOT_NAME})\n")
endforeach()

file(CONFIGURE OUTPUT "${WOT_SHADER_OUTPUT_DIR}/ShaderIncludes.inc" CONTENT "${WOT_SHADER_INCLUDES}" @ONLY)
file(CONFIGURE OUTPUT "${WOT_SHADER_OUTPUT_DIR}/ShaderList.inc" CONTENT "${WOT_SHADER_LIST}" @ONLY)
add_custom_target(TeinoraShaders DEPENDS ${WOT_SHADER_OUTPUTS})

# -------------------------------------------------------------------- engine
add_library(TeinoraEngine STATIC
    Engine/Core/Png.cpp
    Engine/Platform/Window.cpp
    Engine/Render/Renderer.cpp
    Engine/Render/Shaders.cpp
    Engine/Rhi/Rhi.cpp
    Engine/Rhi/RhiD3D12.cpp
    Engine/Rhi/RhiVulkan.cpp
    Engine/Rhi/Streamline.cpp)
target_include_directories(TeinoraEngine
    PUBLIC Engine
    PRIVATE "${WOT_SHADER_OUTPUT_DIR}" SYSTEM "${WOT_LOCAL_DIR}/vulkan-headers/include")
target_link_libraries(TeinoraEngine PRIVATE TeinoraWarnings d3d12 dxgi dxguid user32 gdi32 dbghelp)
add_dependencies(TeinoraEngine TeinoraShaders)

# ---------------------------------------------------------------------- game
add_executable(WindsOfTeinora WIN32
    Game/ChunkBuffers.cpp
    Game/GameWorld.cpp
    Game/Main.cpp
    Game/Menu.cpp
    Game/Settings.cpp)
target_link_libraries(WindsOfTeinora PRIVATE TeinoraEngine TeinoraPhysics TeinoraSim TeinoraWarnings)
target_link_options(WindsOfTeinora PRIVATE /ENTRY:mainCRTStartup)

# ---------------------------------------------------------------- nvidia, optional
# Present only after tools/bootstrap.ps1 -WithNvidia. At run time the game also works
# with these DLLs missing or on a GPU from another vendor.
option(WOT_WITH_STREAMLINE "Use the Streamline SDK when it is in .local" ON)
set(WOT_STREAMLINE_DIR "${WOT_LOCAL_DIR}/streamline")
if(WOT_WITH_STREAMLINE AND EXISTS "${WOT_STREAMLINE_DIR}/include/sl.h")
    message(STATUS "NVIDIA Streamline: enabled")
    target_compile_definitions(TeinoraEngine PRIVATE TEINORA_WITH_STREAMLINE=1)
    target_include_directories(TeinoraEngine SYSTEM PRIVATE "${WOT_STREAMLINE_DIR}/include")
    target_link_libraries(TeinoraEngine PRIVATE wintrust crypt32)

    foreach(WOT_DLL sl.interposer sl.common sl.dlss sl.reflex sl.pcl nvngx_dlss NvLowLatencyVk)
        add_custom_command(TARGET WindsOfTeinora POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${WOT_STREAMLINE_DIR}/bin/x64/${WOT_DLL}.dll" "$<TARGET_FILE_DIR:WindsOfTeinora>"
            VERBATIM)
    endforeach()
endif()

# --------------------------------------------------------------------- tests
enable_testing()

add_executable(TeinoraSimTests Tests/SimTests.cpp)
target_link_libraries(TeinoraSimTests PRIVATE TeinoraSim TeinoraWarnings)
add_test(NAME TeinoraSimTests COMMAND TeinoraSimTests)

add_executable(TeinoraPhysicsTests Tests/PhysicsTests.cpp)
target_link_libraries(TeinoraPhysicsTests PRIVATE TeinoraPhysics TeinoraSim TeinoraWarnings)
add_test(NAME TeinoraPhysicsTests COMMAND TeinoraPhysicsTests)
