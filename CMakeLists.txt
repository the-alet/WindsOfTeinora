cmake_minimum_required(VERSION 3.28)
project(WondersOfTaelia LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # no C++20 modules here: skip the dependency scan
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(LOCAL ${CMAKE_SOURCE_DIR}/.local)
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)

add_compile_options(/utf-8 /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

# ---------------------------------------------------------------- shared numbers (one source)

set(SHARED_OUT ${GEN_DIR}/taelia/shared_constants.h ${GEN_DIR}/shaders/shared_constants.hlsli ${GEN_DIR}/unreal/shared_constants.py)
add_custom_command(OUTPUT ${SHARED_OUT}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/shared/constants.txt -DOUT=${GEN_DIR} -P ${CMAKE_SOURCE_DIR}/.utilities/gen_constants.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/shared/constants.txt ${CMAKE_SOURCE_DIR}/.utilities/gen_constants.cmake
  COMMENT "Generating shared constants")
add_custom_target(shared_constants DEPENDS ${SHARED_OUT})

# ---------------------------------------------------------------- simulation library (no engine or graphics code)
file(GLOB_RECURSE SIM_SRC CONFIGURE_DEPENDS sim/src/*.cpp sim/include/*.h)
add_library(taelia_sim STATIC ${SIM_SRC})
target_include_directories(taelia_sim PUBLIC sim/include ${GEN_DIR} shared)
target_compile_options(taelia_sim PRIVATE /W4 /WX)
add_dependencies(taelia_sim shared_constants)
target_link_libraries(taelia_sim PRIVATE physx)  # defined below; sim headers don't expose PhysX types

enable_testing()
file(GLOB TEST_SRC CONFIGURE_DEPENDS tests/*.cpp tests/*.h)
add_executable(taelia_tests ${TEST_SRC})
target_link_libraries(taelia_tests PRIVATE taelia_sim physx)
target_compile_options(taelia_tests PRIVATE /W4 /WX)
add_test(NAME sim COMMAND taelia_tests)

# ---------------------------------------------------------------- third party (fetched into .local by .utilities/bootstrap.py)
add_library(imgui STATIC
  ${LOCAL}/imgui/imgui.cpp ${LOCAL}/imgui/imgui_draw.cpp ${LOCAL}/imgui/imgui_tables.cpp
  ${LOCAL}/imgui/imgui_widgets.cpp ${LOCAL}/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${LOCAL}/imgui ${LOCAL}/imgui/backends)

add_library(volk STATIC ${LOCAL}/volk/volk.c)
target_include_directories(volk PUBLIC ${LOCAL}/volk ${LOCAL}/vulkan-headers/include)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)

add_library(d3d12ma STATIC ${LOCAL}/d3d12ma/src/D3D12MemAlloc.cpp)
target_include_directories(d3d12ma PUBLIC ${LOCAL}/d3d12ma/include ${LOCAL}/agility/include)

add_library(thirdparty_headers INTERFACE)
target_include_directories(thirdparty_headers INTERFACE ${LOCAL}/vma/include ${LOCAL}/stb ${LOCAL}/agility/include)

# NVIDIA PhysX 5, CPU only, static: PhysX 'debug' (/MTd) for our Debug, PhysX 'release' (/MT) otherwise.
set(PHYSX_DIR ${LOCAL}/physx)
set(PHYSX_LIB_DIR ${PHYSX_DIR}/lib/$<IF:$<CONFIG:Debug>,debug,release>)
add_library(physx INTERFACE)
target_include_directories(physx SYSTEM INTERFACE ${PHYSX_DIR}/include)
target_compile_definitions(physx INTERFACE PX_PHYSX_STATIC_LIB DISABLE_CUDA_PHYSX $<$<CONFIG:Debug>:PX_DEBUG=1 PX_CHECKED=1>)
foreach(lib PhysXExtensions PhysX PhysXCooking PhysXCharacterKinematic PhysXPvdSDK PhysXCommon PhysXFoundation)
  target_link_libraries(physx INTERFACE ${PHYSX_LIB_DIR}/${lib}_static_64.lib)
endforeach()

# ---------------------------------------------------------------- shaders: HLSL -> DXIL and SPIR-V
set(DXC ${LOCAL}/dxc/bin/x64/dxc.exe)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/bin/shaders)
set(SHADER_LINES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/shaders.txt)  # a new shader reconfigures
if(EXISTS ${CMAKE_SOURCE_DIR}/shaders/shaders.txt)
  file(STRINGS shaders/shaders.txt SHADER_LINES)
endif()
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS shaders/*.hlsli shared/*.h)
set(SHADER_BINS)
foreach(line IN LISTS SHADER_LINES)
  string(STRIP "${line}" line)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  separate_arguments(parts UNIX_COMMAND "${line}")
  list(GET parts 0 sfile)
  list(GET parts 1 entry)
  list(GET parts 2 profile)
  get_filename_component(sname ${sfile} NAME_WE)
  set(src ${CMAKE_SOURCE_DIR}/shaders/${sfile})
  set(common -E ${entry} -T ${profile} -HV 2021 -enable-16bit-types -I ${CMAKE_SOURCE_DIR}/shaders -I ${CMAKE_SOURCE_DIR}/shared -I ${GEN_DIR}/shaders -WX)
  set(dxil ${SHADER_OUT}/${sname}.${entry}.dxil)
  set(spv ${SHADER_OUT}/${sname}.${entry}.spv)
  add_custom_command(OUTPUT ${dxil}
    COMMAND ${DXC} ${common} -O3 -Wno-ignored-attributes -Qstrip_debug -Qstrip_reflect -Fo ${dxil} ${src}
    DEPENDS ${src} ${SHADER_INCLUDES} ${SHARED_OUT} VERBATIM)
  add_custom_command(OUTPUT ${spv}
    COMMAND ${DXC} ${common} -O3 -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout -Fo ${spv} ${src}
    DEPENDS ${src} ${SHADER_INCLUDES} ${SHARED_OUT} VERBATIM)
  list(APPEND SHADER_BINS ${dxil} ${spv})
endforeach()
add_custom_target(shaders ALL DEPENDS ${SHADER_BINS})

# ---------------------------------------------------------------- the renderer front end
file(READ ${LOCAL}/agility/version.txt AGILITY_VERSION)
string(STRIP "${AGILITY_VERSION}" AGILITY_VERSION)
string(REGEX REPLACE "^1[.]([0-9]+)[.].*$" "\\1" AGILITY_SDK_VERSION "${AGILITY_VERSION}")

# RHI: common part, then one library per backend so each builds on its own.
add_library(taelia_rhi STATIC engine/rhi/rhi.h engine/rhi/backends.h engine/rhi/rhi_common.cpp engine/rhi/rhi_factory.cpp
  engine/rhi/streamline.h engine/rhi/streamline.cpp)
target_include_directories(taelia_rhi PUBLIC ${LOCAL}/streamline/include)  # the backends use it too
target_include_directories(taelia_rhi PUBLIC engine shared ${GEN_DIR})
target_compile_options(taelia_rhi PRIVATE /W4)

file(GLOB RHI_D3D12_SRC CONFIGURE_DEPENDS engine/rhi/d3d12/*.cpp engine/rhi/d3d12/*.h)
if(RHI_D3D12_SRC)
  add_library(taelia_rhi_d3d12 STATIC ${RHI_D3D12_SRC})
  target_link_libraries(taelia_rhi_d3d12 PUBLIC taelia_rhi d3d12ma thirdparty_headers d3d12 dxgi dxguid)
  target_compile_options(taelia_rhi_d3d12 PRIVATE /W4)
endif()

file(GLOB RHI_VULKAN_SRC CONFIGURE_DEPENDS engine/rhi/vulkan/*.cpp engine/rhi/vulkan/*.h)
if(RHI_VULKAN_SRC)
  add_library(taelia_rhi_vulkan STATIC ${RHI_VULKAN_SRC})
  target_link_libraries(taelia_rhi_vulkan PUBLIC taelia_rhi volk thirdparty_headers)
  target_compile_options(taelia_rhi_vulkan PRIVATE /W4)
endif()

# Runtime files next to the exes: the Agility SDK (D3D12 + debug layer) and, if built, the Vulkan validation layer.
set(BIN ${CMAKE_BINARY_DIR}/bin)
set(RUNTIME_STAMP ${CMAKE_BINARY_DIR}/runtime.stamp)
set(RUNTIME_CMDS
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN}/D3D12
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LOCAL}/agility/bin/x64/D3D12Core.dll ${LOCAL}/agility/bin/x64/d3d12SDKLayers.dll ${BIN}/D3D12/)
# NVIDIA Streamline (DLSS, Reflex): the signed production DLLs next to the exe.
set(SL_BIN ${LOCAL}/streamline/bin/x64)
list(APPEND RUNTIME_CMDS COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SL_BIN}/sl.interposer.dll ${SL_BIN}/sl.common.dll
  ${SL_BIN}/sl.dlss.dll ${SL_BIN}/sl.reflex.dll ${SL_BIN}/sl.pcl.dll ${SL_BIN}/nvngx_dlss.dll ${SL_BIN}/NvLowLatencyVk.dll ${BIN}/)
if(EXISTS ${LOCAL}/vulkan-layers/bin/VkLayer_khronos_validation.dll)
  list(APPEND RUNTIME_CMDS
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN}/vulkan-layers
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LOCAL}/vulkan-layers/bin/VkLayer_khronos_validation.dll ${LOCAL}/vulkan-layers/bin/VkLayer_khronos_validation.json ${BIN}/vulkan-layers/)
endif()
add_custom_command(OUTPUT ${RUNTIME_STAMP} ${RUNTIME_CMDS} COMMAND ${CMAKE_COMMAND} -E touch ${RUNTIME_STAMP})
add_custom_target(runtime_files DEPENDS ${RUNTIME_STAMP})

# Headless smoke tests, one per backend (each links only its own backend).
if(RHI_D3D12_SRC)
  add_executable(rhi_smoke_d3d12 tests/rhi/rhi_smoke.cpp)
  target_link_libraries(rhi_smoke_d3d12 PRIVATE taelia_rhi_d3d12)
  target_compile_definitions(rhi_smoke_d3d12 PRIVATE SMOKE_D3D12 TAELIA_AGILITY_SDK_VERSION=${AGILITY_SDK_VERSION})
  add_dependencies(rhi_smoke_d3d12 shaders runtime_files)
  add_test(NAME rhi_smoke_d3d12 COMMAND rhi_smoke_d3d12)
endif()
if(RHI_VULKAN_SRC)
  add_executable(rhi_smoke_vulkan tests/rhi/rhi_smoke.cpp)
  target_link_libraries(rhi_smoke_vulkan PRIVATE taelia_rhi_vulkan)
  target_compile_definitions(rhi_smoke_vulkan PRIVATE SMOKE_VULKAN)
  add_dependencies(rhi_smoke_vulkan shaders runtime_files)
  add_test(NAME rhi_smoke_vulkan COMMAND rhi_smoke_vulkan)
endif()

file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS engine/platform/*.cpp engine/platform/*.h engine/render/*.cpp engine/render/*.h engine/app/*.cpp engine/app/*.h)
if(NOT GAME_SRC)
  return()
endif()
# The game's own code as objects, so it compiles (and can be checked) before both backends exist.
add_library(taelia_game_objs OBJECT ${GAME_SRC})
target_include_directories(taelia_game_objs PUBLIC engine shared ${GEN_DIR})
target_link_libraries(taelia_game_objs PUBLIC taelia_sim taelia_rhi imgui thirdparty_headers)
target_compile_options(taelia_game_objs PRIVATE /W4)
target_compile_definitions(taelia_game_objs PRIVATE TAELIA_AGILITY_SDK_VERSION=${AGILITY_SDK_VERSION})
add_dependencies(taelia_game_objs shared_constants)
if(NOT RHI_D3D12_SRC OR NOT RHI_VULKAN_SRC)
  return()
endif()
add_executable(WondersOfTaelia WIN32 $<TARGET_OBJECTS:taelia_game_objs>)
target_link_libraries(WondersOfTaelia PRIVATE taelia_game_objs taelia_sim taelia_rhi_d3d12 taelia_rhi_vulkan imgui
  user32 gdi32 shell32 ole32 winmm)
add_dependencies(WondersOfTaelia shaders runtime_files)
# The game itself, headless, on both APIs with the debug layers: a frame must come out with no debug messages.
foreach(api d3d12 vulkan)
  add_test(NAME game_frame_${api} COMMAND WondersOfTaelia --api=${api} --gpu-debug --frames=3 --size=640x360 --menu=graphics
           --shot=${CMAKE_BINARY_DIR}/test_frames/${api}.png)
endforeach()
# The windowed path (swapchain present, resize) on a window that is never shown.
foreach(api d3d12 vulkan)
  add_test(NAME game_swapchain_${api} COMMAND WondersOfTaelia --api=${api} --gpu-debug --frames=40 --size=800x450 --hidden-window)
endforeach()
