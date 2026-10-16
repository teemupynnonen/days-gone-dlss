// Loading the AMD FSR API; fidelityfx.hpp describes it.
#include "fidelityfx.hpp"

#include <Windows.h>
#include <mutex>
#include <stdexcept>

namespace days_gone_dlss::fidelityfx
{
namespace
{
std::mutex loadMutex;
Api api;

template<class Function> void resolve(HMODULE module, Function& function, const char* name)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!function) throw std::runtime_error(std::string("amd_fidelityfx_loader_dx12.dll lacks ") + name + ".");
}

const char* meaning(ffxReturnCode_t code)
{
    switch (code)
    {
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown structure";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "Direct3D 12 error";
    case FFX_API_RETURN_NO_PROVIDER: return "no FSR DLL provides it";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "the FSR DLL is too old";
    default: return "error";
    }
}
}

const Api& load(const std::filesystem::path& directory)
{
    std::lock_guard lock(loadMutex);
    if (api.dispatch) return api;

    const auto path = directory / L"amd_fidelityfx_loader_dx12.dll";
    if (!std::filesystem::exists(path)) throw std::runtime_error("amd_fidelityfx_loader_dx12.dll is missing beside the add-on.");
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) throw std::runtime_error("Loading amd_fidelityfx_loader_dx12.dll failed.");
    Api loaded;
    resolve(module, loaded.createContext, "ffxCreateContext");
    resolve(module, loaded.destroyContext, "ffxDestroyContext");
    resolve(module, loaded.configure, "ffxConfigure");
    resolve(module, loaded.query, "ffxQuery");
    resolve(module, loaded.dispatch, "ffxDispatch");
    api = loaded;

    return api;
}

void check(ffxReturnCode_t code, const char* what)
{
    if (code != FFX_API_RETURN_OK)
        throw std::runtime_error(std::string(what) + " failed (" + meaning(code) + ", " + std::to_string(code) + ").");
}

std::string version(const Api& functions, ffxContext& context)
{
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (functions.query(&context, &query.header) != FFX_API_RETURN_OK || !query.versionName) return {};

    return query.versionName;
}
}
