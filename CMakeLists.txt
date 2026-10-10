cmake_minimum_required(VERSION 3.25)
project(DaysGoneDLSS VERSION 1.2.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Days Gone DLSS requires Windows and MSVC.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Days Gone DLSS must be built for 64-bit Windows.")
endif()

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
FetchContent_MakeAvailable(minhook)

# Downloads `source|path|sha256` entries from raw.githubusercontent.com into `directory`.
function(download_pinned directory)
    foreach(entry IN LISTS ARGN)
        string(REPLACE "|" ";" entry "${entry}")
        list(GET entry 0 source)
        list(GET entry 1 path)
        list(GET entry 2 hash)
        set(target "${directory}/${path}")
        if(EXISTS "${target}")
            file(SHA256 "${target}" existing)
            if(existing STREQUAL hash)
                continue()
            endif()
        endif()
        message(STATUS "Downloading ${source}")
        file(DOWNLOAD "https://raw.githubusercontent.com/${source}" "${target}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Could not download ${source}: ${status}")
        endif()
    endforeach()
endfunction()

# NVIDIA DLSS SDK v310.9.1. The repository carries every platform's binaries (over 1 GB),
# so only the D3D11 headers, the static NGX loader and the x64 model DLL are downloaded.
set(DLSS_SDK_COMMIT 374959484e79a640feaba44c93ac8cfb0a03f5b5)
set(DLSS_SDK_DIR "${CMAKE_BINARY_DIR}/_deps/dlss-sdk")
set(sdk "NVIDIA/DLSS/${DLSS_SDK_COMMIT}")
download_pinned("${DLSS_SDK_DIR}"
    "${sdk}/LICENSE.txt|LICENSE.txt|d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
    "${sdk}/include/nvsdk_ngx.h|include/nvsdk_ngx.h|dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
    "${sdk}/include/nvsdk_ngx_defs.h|include/nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
    "${sdk}/include/nvsdk_ngx_params.h|include/nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
    "${sdk}/include/nvsdk_ngx_helpers.h|include/nvsdk_ngx_helpers.h|5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"
    "${sdk}/include/nvsdk_ngx_helpers_d3d.h|include/nvsdk_ngx_helpers_d3d.h|ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"
    "${sdk}/include/nvsdk_ngx_helpers_cuda.h|include/nvsdk_ngx_helpers_cuda.h|f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"
    "${sdk}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib|lib/Windows_x86_64/x64/nvsdk_ngx_s.lib|4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3"
    "${sdk}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib|lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib|f62f94f7a444ddffa118e71bc412298d803784bf042464be178a989915198cab"
    "${sdk}/lib/Windows_x86_64/rel/nvngx_dlss.dll|lib/Windows_x86_64/rel/nvngx_dlss.dll|3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"
)

# ReShade 6.8.0 add-on API headers and the Dear ImGui 1.92.5 header its overlay requires.
set(RESHADE_COMMIT 18deaa52de0c425a78b329e9cb3c497281cd00ec) # v6.8.0
set(IMGUI_COMMIT 3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c) # ReShade's deps/imgui
set(RESHADE_DIR "${CMAKE_BINARY_DIR}/_deps/reshade-api")
set(reshade "crosire/reshade/${RESHADE_COMMIT}")
set(imgui "ocornut/imgui/${IMGUI_COMMIT}")
download_pinned("${RESHADE_DIR}"
    "${reshade}/LICENSE.md|LICENSE.md|237ded5b8344f820113efab1e65e91e1f159d9202c5b4856606a0590d3ffdab0"
    "${reshade}/include/reshade.hpp|include/reshade.hpp|cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c"
    "${reshade}/include/reshade_api.hpp|include/reshade_api.hpp|8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb"
    "${reshade}/include/reshade_api_device.hpp|include/reshade_api_device.hpp|a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0"
    "${reshade}/include/reshade_api_format.hpp|include/reshade_api_format.hpp|68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7"
    "${reshade}/include/reshade_api_pipeline.hpp|include/reshade_api_pipeline.hpp|5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f"
    "${reshade}/include/reshade_api_resource.hpp|include/reshade_api_resource.hpp|32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3"
    "${reshade}/include/reshade_events.hpp|include/reshade_events.hpp|2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89"
    "${reshade}/include/reshade_overlay.hpp|include/reshade_overlay.hpp|ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79"
    "${imgui}/imgui.h|imgui/imgui.h|07562842049eaca4e47c3fcdcc9d8eafd442695b9856d6f6639e92e1bb6c737e"
    "${imgui}/imconfig.h|imgui/imconfig.h|6e5687893594ebfaf8569280cfea83d025904a8a4e23bec073086f6c5bc8f7fb"
    "${imgui}/LICENSE.txt|imgui/LICENSE.txt|c80c5789748d955c4a650562baa0d750494e2c7128c2ca66aaebe2e3c4e198bf"
)

add_library(ngx STATIC IMPORTED)
set_target_properties(ngx PROPERTIES
    IMPORTED_LOCATION "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
    IMPORTED_LOCATION_DEBUG "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${DLSS_SDK_DIR}/include"
)

# The add-on uses ReShade's ImGui through the overlay function table, so none is linked.
add_library(days_gone_dlss SHARED src/upscaler.cpp src/upscaler_settings.cpp src/upscaler_menu.cpp src/upscaler_addon.cpp)
target_include_directories(days_gone_dlss PRIVATE src)
target_include_directories(days_gone_dlss SYSTEM PRIVATE "${RESHADE_DIR}/include" "${RESHADE_DIR}/imgui")
target_link_libraries(days_gone_dlss PRIVATE minhook ngx d3d11 dxgi d3dcompiler)
target_compile_features(days_gone_dlss PRIVATE cxx_std_20)
target_compile_definitions(days_gone_dlss PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ImTextureID=ImU64)
target_compile_options(days_gone_dlss PRIVATE /W4 /WX /permissive- /utf-8)
# Record only the PDB's file name, not the build machine's path.
target_link_options(days_gone_dlss PRIVATE /PDBALTPATH:%_PDB%)
set_target_properties(days_gone_dlss PROPERTIES
    OUTPUT_NAME DaysGoneDLSS
    SUFFIX .addon64
    CXX_EXTENSIONS OFF
)
# NGX loads the model DLL from the add-on's directory.
add_custom_command(TARGET days_gone_dlss POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:days_gone_dlss>/nvngx_dlss.dll"
)

install(TARGETS days_gone_dlss RUNTIME DESTINATION . COMPONENT addon)
install(FILES README.md CHANGELOG.md LICENSE DESTINATION DaysGoneDLSS COMPONENT addon)
install(FILES "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" DESTINATION . COMPONENT addon)
install(FILES "${DLSS_SDK_DIR}/LICENSE.txt" DESTINATION DaysGoneDLSS RENAME NVIDIA-DLSS-LICENSE.txt COMPONENT addon)
install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION DaysGoneDLSS RENAME MinHook-LICENSE.txt COMPONENT addon)
install(FILES "${RESHADE_DIR}/LICENSE.md" DESTINATION DaysGoneDLSS RENAME ReShade-LICENSE.txt COMPONENT addon)
install(FILES "${RESHADE_DIR}/imgui/LICENSE.txt" DESTINATION DaysGoneDLSS RENAME ImGui-LICENSE.txt COMPONENT addon)
# The sharpening shader is ported from FidelityFX FSR 1 RCAS.
install(FILES licenses/AMD-FidelityFX-LICENSE.txt DESTINATION DaysGoneDLSS COMPONENT addon)

include(CTest)
if(BUILD_TESTING)
    add_executable(upscaler_tests tests/upscaler_tests.cpp src/upscaler_settings.cpp)
    target_include_directories(upscaler_tests PRIVATE src)
    target_compile_features(upscaler_tests PRIVATE cxx_std_20)
    target_compile_definitions(upscaler_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(upscaler_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME upscaler_settings COMMAND upscaler_tests)
endif()

# Package only runtime files and documentation, not dependency development files.
set(CPACK_GENERATOR ZIP)
set(CPACK_COMPONENTS_ALL addon)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_ADDON_FILE_NAME "DaysGoneDLSS-${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
