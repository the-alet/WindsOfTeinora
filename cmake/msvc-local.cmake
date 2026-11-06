# MSVC and the Windows SDK unpacked into the project by tools/fetch-msvc.ps1.
# No Visual Studio installation or developer prompt is involved.
get_filename_component(WOT_MSVC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.local/msvc" ABSOLUTE)

file(GLOB WOT_VC_VERSIONS LIST_DIRECTORIES true "${WOT_MSVC_ROOT}/VC/Tools/MSVC/*")
file(GLOB WOT_SDK_VERSIONS LIST_DIRECTORIES true "${WOT_MSVC_ROOT}/winsdk/Include/*")
if(NOT WOT_VC_VERSIONS OR NOT WOT_SDK_VERSIONS)
    message(FATAL_ERROR "MSVC is not installed. Run tools/fetch-msvc.ps1 -AcceptLicense.")
endif()

list(SORT WOT_VC_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(SORT WOT_SDK_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(GET WOT_VC_VERSIONS 0 WOT_VC)
list(GET WOT_SDK_VERSIONS 0 WOT_SDK_INCLUDE)
get_filename_component(WOT_SDK_VERSION "${WOT_SDK_INCLUDE}" NAME)
set(WOT_SDK "${WOT_MSVC_ROOT}/winsdk")

set(CMAKE_C_COMPILER "${WOT_VC}/bin/Hostx64/x64/cl.exe")
set(CMAKE_CXX_COMPILER "${WOT_VC}/bin/Hostx64/x64/cl.exe")
set(CMAKE_LINKER "${WOT_VC}/bin/Hostx64/x64/link.exe")
set(CMAKE_AR "${WOT_VC}/bin/Hostx64/x64/lib.exe")
set(CMAKE_RC_COMPILER "${WOT_SDK}/bin/${WOT_SDK_VERSION}/x64/rc.exe")
set(CMAKE_MT "${WOT_SDK}/bin/${WOT_SDK_VERSION}/x64/mt.exe")

set(WOT_INCLUDES
    "${WOT_VC}/include"
    "${WOT_SDK}/Include/${WOT_SDK_VERSION}/ucrt"
    "${WOT_SDK}/Include/${WOT_SDK_VERSION}/shared"
    "${WOT_SDK}/Include/${WOT_SDK_VERSION}/um"
    "${WOT_SDK}/Include/${WOT_SDK_VERSION}/winrt"
    "${WOT_SDK}/Include/${WOT_SDK_VERSION}/cppwinrt")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${WOT_INCLUDES})
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${WOT_INCLUDES})
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES ${WOT_INCLUDES})

set(WOT_LIB_DIRS
    "${WOT_VC}/lib/x64"
    "${WOT_SDK}/Lib/${WOT_SDK_VERSION}/ucrt/x64"
    "${WOT_SDK}/Lib/${WOT_SDK_VERSION}/um/x64")
list(TRANSFORM WOT_LIB_DIRS PREPEND "/LIBPATH:" OUTPUT_VARIABLE WOT_LINK_DIRS)
list(JOIN WOT_LINK_DIRS " " WOT_LINK_FLAGS)

# CMake's compiler identification runs cl.exe directly, without the flags below.
string(REPLACE ";" "\\;" WOT_INCLUDE_ENV "${WOT_INCLUDES}")
string(REPLACE ";" "\\;" WOT_LIB_ENV "${WOT_LIB_DIRS}")
set(ENV{INCLUDE} "${WOT_INCLUDE_ENV}")
set(ENV{LIB} "${WOT_LIB_ENV}")

set(CMAKE_EXE_LINKER_FLAGS_INIT "${WOT_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${WOT_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${WOT_LINK_FLAGS}")
