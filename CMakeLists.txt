cmake_minimum_required(VERSION 3.21)
project(UniversalTextureDownscaler LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "This project only builds with MSVC (needs cl.exe's sibling dumpbin.exe).")
endif()

# dumpbin.exe always ships next to cl.exe in the same MSVC toolset install.
get_filename_component(MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(DUMPBIN_EXECUTABLE dumpbin PATHS "${MSVC_BIN_DIR}" NO_DEFAULT_PATH)
if(NOT DUMPBIN_EXECUTABLE)
    message(FATAL_ERROR "dumpbin.exe not found next to ${CMAKE_CXX_COMPILER}")
endif()

# combined D3D11+D3D12 proxy
# One binary for both APIs: Windows doesn't check that a DLL's file name
# matches what it exports. D3D11Hooks.cpp's hook only runs if the game calls
# D3D11CreateDevice(AndSwapChain); D3D12Hooks.cpp's only runs from
# D3D12CreateDevice, so the unused half stays inert. Deploy by renaming this
# build output to d3d11.dll or d3d12.dll (the installer does this).
#
# Each half's exports are pinned against its own real DLL's ordinals, so the
# two generated .def files are merged into one before linking.

set(D3D12_DEF "${CMAKE_CURRENT_BINARY_DIR}/UniversalTextureDownscaler_d3d12.generated.def")
execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_def.ps1"
            -DumpbinPath "${DUMPBIN_EXECUTABLE}"
            -OutFile "${D3D12_DEF}"
            -RealDllName "d3d12.dll"
            -LibraryName "d3d12"
            -Exports "D3D12CreateDevice,D3D12GetDebugInterface,D3D12GetInterface,D3D12SerializeRootSignature,D3D12SerializeVersionedRootSignature,D3D12CreateRootSignatureDeserializer,D3D12CreateVersionedRootSignatureDeserializer,D3D12EnableExperimentalFeatures"
    RESULT_VARIABLE GENERATE_D3D12_DEF_RESULT
    OUTPUT_VARIABLE GENERATE_D3D12_DEF_OUTPUT
    ERROR_VARIABLE GENERATE_D3D12_DEF_OUTPUT
)
if(NOT GENERATE_D3D12_DEF_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate the d3d12 .def:\n${GENERATE_D3D12_DEF_OUTPUT}")
endif()
message(STATUS "${GENERATE_D3D12_DEF_OUTPUT}")

set(D3D11_DEF "${CMAKE_CURRENT_BINARY_DIR}/UniversalTextureDownscaler_d3d11.generated.def")
execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_def.ps1"
            -DumpbinPath "${DUMPBIN_EXECUTABLE}"
            -OutFile "${D3D11_DEF}"
            -RealDllName "d3d11.dll"
            -LibraryName "d3d11"
            -Exports "D3D11CreateDevice,D3D11CreateDeviceAndSwapChain,D3D11On12CreateDevice"
    RESULT_VARIABLE GENERATE_D3D11_DEF_RESULT
    OUTPUT_VARIABLE GENERATE_D3D11_DEF_OUTPUT
    ERROR_VARIABLE GENERATE_D3D11_DEF_OUTPUT
)
if(NOT GENERATE_D3D11_DEF_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate the d3d11 .def:\n${GENERATE_D3D11_DEF_OUTPUT}")
endif()
message(STATUS "${GENERATE_D3D11_DEF_OUTPUT}")

# Merge: keep one "LIBRARY name" / "EXPORTS" header, concatenate both files'
# "    Name @ordinal" lines underneath it.
set(COMBINED_D3D_DEF "${CMAKE_CURRENT_BINARY_DIR}/UniversalTextureDownscaler_d3d.generated.def")
file(STRINGS "${D3D12_DEF}" D3D12_DEF_LINES)
file(STRINGS "${D3D11_DEF}" D3D11_DEF_LINES)
set(COMBINED_DEF_LINES "LIBRARY d3d12" "EXPORTS")
foreach(LINE IN LISTS D3D12_DEF_LINES D3D11_DEF_LINES)
    if(LINE MATCHES "^    ")
        list(APPEND COMBINED_DEF_LINES "${LINE}")
    endif()
endforeach()
list(JOIN COMBINED_DEF_LINES "\n" COMBINED_DEF_CONTENT)
file(WRITE "${COMBINED_D3D_DEF}" "${COMBINED_DEF_CONTENT}\n")

# src/version.rc stamps both binaries with a ProductName the installer matches
# on before it overwrites or deletes a d3d11.dll/d3d12.dll, since other mods
# deploy under those same names.
add_library(UniversalTextureDownscaler_D3D SHARED src/D3D11Hooks.cpp src/D3D12Hooks.cpp src/version.rc)
target_include_directories(UniversalTextureDownscaler_D3D PRIVATE src)
target_compile_features(UniversalTextureDownscaler_D3D PRIVATE cxx_std_20)
set_target_properties(UniversalTextureDownscaler_D3D PROPERTIES OUTPUT_NAME "d3d12" PREFIX "")
target_link_options(UniversalTextureDownscaler_D3D PRIVATE "/DEF:${COMBINED_D3D_DEF}")

# UniversalTextureDownscaler_Vulkan.dll (Vulkan implicit layer)
# Not a vulkan-1.dll proxy like the D3D targets, see VulkanHooks.cpp's
# header comment for why. No .def/ordinal pinning needed: the loader
# resolves a layer's exports by name only, so a plain SHARED library with
# __declspec(dllexport) is enough. Only needs the Vulkan SDK's headers,
# nothing to link against, and is skipped gracefully (not a hard error) if
# the SDK isn't installed.
#
# The DLL file name is ours to choose freely; the "VK_LAYER_" convention
# applies to the logical layer name inside the manifest, not the file on
# disk.

find_package(Vulkan)
if(Vulkan_FOUND)
    add_library(UniversalTextureDownscaler_Vulkan SHARED src/VulkanHooks.cpp src/version.rc)
    target_include_directories(UniversalTextureDownscaler_Vulkan PRIVATE src)
    target_include_directories(UniversalTextureDownscaler_Vulkan PRIVATE "${Vulkan_INCLUDE_DIRS}")
    target_compile_features(UniversalTextureDownscaler_Vulkan PRIVATE cxx_std_20)
    set_target_properties(UniversalTextureDownscaler_Vulkan PROPERTIES OUTPUT_NAME "UniversalTextureDownscaler_Vulkan" PREFIX "")
    target_link_options(UniversalTextureDownscaler_Vulkan PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/VulkanLayer.def")

    # configure_file runs at configure time and can't target a per-config
    # path; a multi-config generator (Visual Studio) only knows the actual
    # output directory at build time, hence a post-build copy instead.
    add_custom_command(TARGET UniversalTextureDownscaler_Vulkan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/UniversalTextureDownscaler_Vulkan.json"
                "$<TARGET_FILE_DIR:UniversalTextureDownscaler_Vulkan>/UniversalTextureDownscaler_Vulkan.json"
    )
else()
    message(WARNING "Vulkan SDK not found, skipping the Vulkan layer (D3D11/D3D12 still build normally). Install it from https://vulkan.lunarg.com/ to build this target.")
endif()
