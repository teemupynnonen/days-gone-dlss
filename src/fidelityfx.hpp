#pragma once

#include <api/include/ffx_api.h>

#include <filesystem>
#include <string>

// The AMD FSR API, from AMD's signed amd_fidelityfx_loader_dx12.dll beside the add-on. The loader
// finds the upscaler and frame generation DLLs beside it, and the driver may offer newer ones.
// fsr_upscaler.hpp and fsr_frame_generation.hpp use it.
namespace days_gone_dlss::fidelityfx
{
struct Api
{
    PfnFfxCreateContext createContext{};
    PfnFfxDestroyContext destroyContext{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};

// Loads the API once and keeps it loaded. Throws when the DLL is missing or incomplete.
const Api& load(const std::filesystem::path& directory);
// Throws `what` and the code's meaning unless `code` is FFX_API_RETURN_OK.
void check(ffxReturnCode_t code, const char* what);
// The version a context runs, such as "4.1.1"; empty when the API does not say.
std::string version(const Api& api, ffxContext& context);
}
