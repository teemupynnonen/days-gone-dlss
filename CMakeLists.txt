cmake_minimum_required(VERSION 3.25)
project(DaysGoneDLSS VERSION 1.7.2 LANGUAGES C CXX)

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

# NVIDIA Streamline SDK 2.14.1 for DLSS Frame Generation. Only its release archive carries the
# frame generation plugin, so the archive is downloaded once and the headers, the license and the
# x64 runtime files are extracted. The add-on loads Streamline at run time; nothing is linked.
set(STREAMLINE_VERSION 2.14.1)
set(STREAMLINE_DIR "${CMAKE_BINARY_DIR}/_deps/streamline-sdk-${STREAMLINE_VERSION}")
set(STREAMLINE_RUNTIME sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
if(NOT EXISTS "${STREAMLINE_DIR}/license.txt")
    set(archive "${CMAKE_BINARY_DIR}/_deps/streamline-sdk-v${STREAMLINE_VERSION}.zip")
    message(STATUS "Downloading the Streamline SDK ${STREAMLINE_VERSION}")
    file(DOWNLOAD "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${STREAMLINE_VERSION}/streamline-sdk-v${STREAMLINE_VERSION}.zip"
        "${archive}" EXPECTED_HASH SHA256=92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Could not download the Streamline SDK: ${status}")
    endif()
    list(TRANSFORM STREAMLINE_RUNTIME PREPEND "bin/x64/" OUTPUT_VARIABLE runtime)
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${STREAMLINE_DIR}" PATTERNS "include/*" license.txt ${runtime})
    file(REMOVE "${archive}")
endif()
list(TRANSFORM STREAMLINE_RUNTIME PREPEND "${STREAMLINE_DIR}/bin/x64/" OUTPUT_VARIABLE STREAMLINE_FILES)

# AMD FSR SDK 2.3.0 for FSR upscaling and frame generation: the API headers and AMD's signed
# Direct3D 12 runtime. The upscaler DLL holds FSR 4 and FSR 3.1 and picks by GPU. The add-on loads
# the runtime at run time; nothing is linked.
set(FIDELITYFX_COMMIT 60f4ea81909200d8542eca14dccb2628b763a9a3) # v2.3.0
set(FIDELITYFX_DIR "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk")
set(ffx "GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FIDELITYFX_COMMIT}/Kits/FidelityFX")
download_pinned("${FIDELITYFX_DIR}"
    "${ffx}/docs/license.md|license.md|6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963"
    "${ffx}/api/include/ffx_api.h|api/include/ffx_api.h|e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd"
    "${ffx}/api/include/ffx_api_types.h|api/include/ffx_api_types.h|cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49"
    "${ffx}/api/include/dx12/ffx_api_dx12.h|api/include/dx12/ffx_api_dx12.h|c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"
    "${ffx}/upscalers/include/ffx_upscale.h|upscalers/include/ffx_upscale.h|65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf"
    "${ffx}/framegeneration/include/ffx_framegeneration.h|framegeneration/include/ffx_framegeneration.h|e6c42f8a38aab2c7c4fc6931545a8e5463fdf87349902ec9687d9fe4949a0566"
    "${ffx}/framegeneration/include/ffx_framegeneration_api_types.h|framegeneration/include/ffx_framegeneration_api_types.h|396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4"
    "${ffx}/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h|framegeneration/include/dx12/ffx_api_framegeneration_dx12.h|d533810bbd6fd4f87dfa2c1657c1457c7b1597b66345827d375b081d67127f66"
    "${ffx}/signedbin/amd_fidelityfx_loader_dx12.dll|bin/amd_fidelityfx_loader_dx12.dll|e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa"
    "${ffx}/signedbin/amd_fidelityfx_upscaler_dx12.dll|bin/amd_fidelityfx_upscaler_dx12.dll|d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46"
    "${ffx}/signedbin/amd_fidelityfx_framegeneration_dx12.dll|bin/amd_fidelityfx_framegeneration_dx12.dll|02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18"
)
set(FIDELITYFX_FILES "${FIDELITYFX_DIR}/bin/amd_fidelityfx_loader_dx12.dll" "${FIDELITYFX_DIR}/bin/amd_fidelityfx_upscaler_dx12.dll"
    "${FIDELITYFX_DIR}/bin/amd_fidelityfx_framegeneration_dx12.dll")

add_library(ngx STATIC IMPORTED)
set_target_properties(ngx PROPERTIES
    IMPORTED_LOCATION "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
    IMPORTED_LOCATION_DEBUG "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${DLSS_SDK_DIR}/include"
)

# The add-on uses ReShade's ImGui through the overlay function table, so none is linked.
# Use the compiler's checksum routine after editing DXBC instructions. Standard MD5 has different padding.
set(DXBC_HASH_DIR "${CMAKE_BINARY_DIR}/_deps/dxbc-hash")
set(dxc "microsoft/DirectXShaderCompiler/6e0fab36dc9ac8c423e1b08fed3aeb0e739d9e54")
download_pinned("${DXBC_HASH_DIR}"
    "${dxc}/lib/DxilHash/DxilHash.cpp|DxilHash.cpp|d66b631ea28d4c90a8217fac004f9290eaac07f79115451e0c4e954c0b0c94af"
    "${dxc}/LICENSE.TXT|LICENSE.TXT|27a49e35d1da96eba18fba54bc882667ff0ff8c0254f16f2b6e165d605ba7df8"
)
add_library(dxbc_hash STATIC "${DXBC_HASH_DIR}/DxilHash.cpp")
target_compile_definitions(dxbc_hash PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# The ray-tracing experiment traces with Shader Model 6.5 ray queries, which d3dcompiler cannot
# compile. The Windows SDK's dxc signs the DXIL with the dxil.dll beside it.
find_program(DXC_EXECUTABLE dxc HINTS "$ENV{WindowsSdkVerBinPath}x64" REQUIRED)
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${GENERATED_DIR}/ray_tracing_trace.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E main -Vn TraceShader -Fh "${GENERATED_DIR}/ray_tracing_trace.h"
        "${CMAKE_SOURCE_DIR}/src/ray_tracing_trace.hlsl"
    DEPENDS src/ray_tracing_trace.hlsl src/ray_tracing_scene.hlsli
    VERBATIM
)
add_custom_command(OUTPUT "${GENERATED_DIR}/ray_tracing_ao.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E main -Vn AmbientTraceShader -Fh "${GENERATED_DIR}/ray_tracing_ao.h"
        "${CMAKE_SOURCE_DIR}/src/ray_tracing_ao.hlsl"
    DEPENDS src/ray_tracing_ao.hlsl src/ray_tracing_scene.hlsli
    VERBATIM
)
# Each bounce shader has a second entry point, `reuse`, which runs after `main` with the reuse of samples on.
add_custom_command(OUTPUT "${GENERATED_DIR}/ray_tracing_bounce.h" "${GENERATED_DIR}/ray_tracing_bounce_reuse.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E main -Vn BounceTraceShader -Fh "${GENERATED_DIR}/ray_tracing_bounce.h"
        "${CMAKE_SOURCE_DIR}/src/ray_tracing_bounce.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E reuse -Vn BounceReuseShader -Fh "${GENERATED_DIR}/ray_tracing_bounce_reuse.h"
        "${CMAKE_SOURCE_DIR}/src/ray_tracing_bounce.hlsl"
    DEPENDS src/ray_tracing_bounce.hlsl src/ray_tracing_scene.hlsli src/ray_tracing_reservoirs.hlsli
    VERBATIM
)
add_custom_command(OUTPUT "${GENERATED_DIR}/ray_tracing_lights.h" "${GENERATED_DIR}/ray_tracing_lights_reuse.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E main -Vn LightsTraceShader -Fh "${GENERATED_DIR}/ray_tracing_lights.h"
        "${CMAKE_SOURCE_DIR}/src/ray_tracing_lights.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T cs_6_5 -E reuse -Vn LightsReuseShader -Fh "${GENERATED_DIR}/ray_tracing_lights_reuse.h"
        "${CMAKE_SOURCE_DIR}/src/ray_tracing_lights.hlsl"
    DEPENDS src/ray_tracing_lights.hlsl src/ray_tracing_scene.hlsli src/ray_tracing_reservoirs.hlsli
    VERBATIM
)
add_custom_target(ray_tracing_shaders DEPENDS "${GENERATED_DIR}/ray_tracing_trace.h" "${GENERATED_DIR}/ray_tracing_ao.h"
    "${GENERATED_DIR}/ray_tracing_bounce.h" "${GENERATED_DIR}/ray_tracing_bounce_reuse.h" "${GENERATED_DIR}/ray_tracing_lights.h"
    "${GENERATED_DIR}/ray_tracing_lights_reuse.h")

add_library(days_gone_dlss SHARED src/upscaler.cpp src/upscaler_settings.cpp src/upscaler_menu.cpp src/upscaler_addon.cpp
    src/ambient_occlusion.cpp src/indirect_lighting.cpp src/shader_bytecode.cpp src/presenter.cpp src/frame_generation.cpp
    src/fidelityfx.cpp src/fsr_upscaler.cpp src/fsr_frame_generation.cpp
    src/ray_tracing.cpp src/ray_tracing_menu.cpp src/alpha_test.cpp src/blue_noise.cpp src/pass_timer.cpp)
add_dependencies(days_gone_dlss ray_tracing_shaders)
target_include_directories(days_gone_dlss PRIVATE src "${GENERATED_DIR}")
target_include_directories(days_gone_dlss SYSTEM PRIVATE "${RESHADE_DIR}/include" "${RESHADE_DIR}/imgui" "${STREAMLINE_DIR}/include"
    "${FIDELITYFX_DIR}")
target_link_libraries(days_gone_dlss PRIVATE minhook ngx dxbc_hash d3d11 d3d12 dxgi d3dcompiler)
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
# NGX loads the model DLL, and the add-on Streamline and the FSR runtime, from the add-on's directory.
add_custom_command(TARGET days_gone_dlss POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" ${STREAMLINE_FILES} ${FIDELITYFX_FILES} "$<TARGET_FILE_DIR:days_gone_dlss>"
)

install(TARGETS days_gone_dlss RUNTIME DESTINATION . COMPONENT addon)
install(FILES README.md CHANGELOG.md LICENSE DESTINATION DaysGoneDLSS COMPONENT addon)
install(FILES "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" ${STREAMLINE_FILES} ${FIDELITYFX_FILES} DESTINATION . COMPONENT addon)
install(FILES "${DLSS_SDK_DIR}/LICENSE.txt" DESTINATION DaysGoneDLSS RENAME NVIDIA-DLSS-LICENSE.txt COMPONENT addon)
install(FILES "${STREAMLINE_DIR}/license.txt" DESTINATION DaysGoneDLSS RENAME NVIDIA-Streamline-LICENSE.txt COMPONENT addon)
install(FILES "${FIDELITYFX_DIR}/license.md" DESTINATION DaysGoneDLSS RENAME AMD-FSR-SDK-LICENSE.txt COMPONENT addon)
install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION DaysGoneDLSS RENAME MinHook-LICENSE.txt COMPONENT addon)
install(FILES "${RESHADE_DIR}/LICENSE.md" DESTINATION DaysGoneDLSS RENAME ReShade-LICENSE.txt COMPONENT addon)
install(FILES "${RESHADE_DIR}/imgui/LICENSE.txt" DESTINATION DaysGoneDLSS RENAME ImGui-LICENSE.txt COMPONENT addon)
# The sharpening shader is ported from FidelityFX FSR 1 RCAS.
install(FILES licenses/AMD-FidelityFX-LICENSE.txt DESTINATION DaysGoneDLSS COMPONENT addon)
install(FILES "${DXBC_HASH_DIR}/LICENSE.TXT" DESTINATION DaysGoneDLSS RENAME DirectXShaderCompiler-LICENSE.txt COMPONENT addon)

include(CTest)
if(BUILD_TESTING)
    add_executable(upscaler_tests tests/upscaler_tests.cpp src/upscaler_settings.cpp)
    target_include_directories(upscaler_tests PRIVATE src)
    target_compile_features(upscaler_tests PRIVATE cxx_std_20)
    target_compile_definitions(upscaler_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(upscaler_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME upscaler_settings COMMAND upscaler_tests)
    add_executable(ambient_occlusion_tests tests/ambient_occlusion_tests.cpp src/ambient_occlusion.cpp src/shader_bytecode.cpp)
    target_include_directories(ambient_occlusion_tests PRIVATE src)
    target_compile_features(ambient_occlusion_tests PRIVATE cxx_std_20)
    target_compile_definitions(ambient_occlusion_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ambient_occlusion_tests PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_libraries(ambient_occlusion_tests PRIVATE dxbc_hash d3d11 d3dcompiler)
    add_test(NAME ambient_occlusion_rendering COMMAND ambient_occlusion_tests)
    add_executable(ray_tracing_tests tests/ray_tracing_tests.cpp src/ray_tracing.cpp src/alpha_test.cpp src/blue_noise.cpp
        src/shader_bytecode.cpp src/pass_timer.cpp)
    add_dependencies(ray_tracing_tests ray_tracing_shaders)
    target_include_directories(ray_tracing_tests PRIVATE src "${GENERATED_DIR}")
    target_compile_features(ray_tracing_tests PRIVATE cxx_std_20)
    target_compile_definitions(ray_tracing_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ray_tracing_tests PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_libraries(ray_tracing_tests PRIVATE dxbc_hash d3d11 d3d12 dxgi d3dcompiler)
    # Needs a DXR 1.1 GPU; reports 77 without one.
    add_test(NAME ray_tracing_capture COMMAND ray_tracing_tests)
    set_tests_properties(ray_tracing_capture PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(blue_noise_tests tests/blue_noise_tests.cpp src/blue_noise.cpp)
    target_include_directories(blue_noise_tests PRIVATE src)
    target_compile_features(blue_noise_tests PRIVATE cxx_std_20)
    target_compile_options(blue_noise_tests PRIVATE /W4 /WX /permissive- /utf-8)
    add_test(NAME blue_noise COMMAND blue_noise_tests)
endif()

# Package only runtime files and documentation, not dependency development files.
set(CPACK_GENERATOR ZIP)
set(CPACK_COMPONENTS_ALL addon)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_ADDON_FILE_NAME "DaysGoneDLSS-${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)
