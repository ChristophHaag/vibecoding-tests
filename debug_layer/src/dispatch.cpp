// SPDX-License-Identifier: MIT
// dispatch.cpp — Interceptor map population and next-dispatch filling.

#include "dispatch.h"

#include <cstring>
#include <unordered_map>

namespace debug_layer {

// ── Static interceptor map ───────────────────────────────────────────────────

static std::unordered_map<std::string_view, PFN_xrVoidFunction> g_interceptors;

void RegisterAllInterceptors()
{
    if (!g_interceptors.empty())
        return; // already registered

    // Register our xrGetInstanceProcAddr
    g_interceptors["xrGetInstanceProcAddr"] =
        reinterpret_cast<PFN_xrVoidFunction>(Layer_xrGetInstanceProcAddr);

    // Register all intercepted functions via the X-macro
#define REGISTER(func) \
    g_interceptors[#func] = reinterpret_cast<PFN_xrVoidFunction>(Layer_##func);
    LIST_INTERCEPTED_FUNCTIONS(REGISTER)
#undef REGISTER
}

PFN_xrVoidFunction GetInterceptor(const char *name)
{
    auto it = g_interceptors.find(std::string_view(name));
    if (it != g_interceptors.end())
        return it->second;
    return nullptr;
}

// ── Populate NextDispatch ────────────────────────────────────────────────────

void PopulateNextDispatch(NextDispatch &next, XrInstance instance, PFN_xrGetInstanceProcAddr getProc)
{
    next.GetInstanceProcAddr = getProc;

#define POPULATE(func) \
    getProc(instance, #func, reinterpret_cast<PFN_xrVoidFunction *>(&next.func));
    LIST_INTERCEPTED_FUNCTIONS(POPULATE)
#undef POPULATE
}

} // namespace debug_layer
