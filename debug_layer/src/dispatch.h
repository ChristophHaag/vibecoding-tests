// SPDX-License-Identifier: MIT
// dispatch.h — X-macro interceptor list, NextDispatch struct, registration.
#pragma once

#include <openxr/openxr.h>

#include <string_view>
#include <unordered_map>

// We define XR_NO_PROTOTYPES in the build, so function pointers are available
// as PFN_xr* but no prototype declarations exist.  We include the loader
// negotiation header for the layer entry point types.
#include <openxr/openxr_loader_negotiation.h>

// ── X-macro: every OpenXR function we intercept ──────────────────────────────
// Add one line here to intercept a new function (core or extension).
// The macro argument receives the bare function name (e.g. xrDestroyInstance).

#define LIST_INTERCEPTED_FUNCTIONS(X) \
    /* Instance */ \
    X(xrDestroyInstance) \
    X(xrStringToPath) \
    X(xrPathToString) \
    /* Session */ \
    X(xrCreateSession) \
    X(xrDestroySession) \
    X(xrBeginSession) \
    X(xrEndSession) \
    /* Action sets & actions */ \
    X(xrCreateActionSet) \
    X(xrDestroyActionSet) \
    X(xrCreateAction) \
    X(xrDestroyAction) \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) \
    X(xrSyncActions) \
    X(xrGetActionStateBoolean) \
    X(xrGetActionStateFloat) \
    X(xrGetActionStateVector2f) \
    X(xrGetActionStatePose) \
    X(xrGetCurrentInteractionProfile) \
    /* Spaces */ \
    X(xrCreateReferenceSpace) \
    X(xrCreateActionSpace) \
    X(xrDestroySpace) \
    X(xrLocateSpace) \
    X(xrLocateViews) \
    /* Frame */ \
    X(xrWaitFrame) \
    X(xrBeginFrame) \
    X(xrEndFrame)

namespace debug_layer {

// ── NextDispatch: typed function pointers to the next layer/runtime ──────────
// One member per intercepted function, plus xrGetInstanceProcAddr.

struct NextDispatch {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;

#define NEXT_DISPATCH_MEMBER(func) PFN_##func func = nullptr;
    LIST_INTERCEPTED_FUNCTIONS(NEXT_DISPATCH_MEMBER)
#undef NEXT_DISPATCH_MEMBER
};

// Fill all members of `next` by querying `getProc` for each function name.
void PopulateNextDispatch(NextDispatch &next, XrInstance instance, PFN_xrGetInstanceProcAddr getProc);

// ── Interceptor registry ─────────────────────────────────────────────────────

// Must be called once (e.g. from negotiate) before any GetInterceptor call.
void RegisterAllInterceptors();

// Returns our layer's wrapper for `name`, or nullptr if we don't intercept it.
PFN_xrVoidFunction GetInterceptor(const char *name);

// ── Interceptor declarations ─────────────────────────────────────────────────
// Each interceptor has the same signature as the real OpenXR function.

// Forward-declare them all so interceptor .cpp files can reference each other.
// The actual definitions live in src/interceptors/*.cpp.

// Instance
XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance);
XrResult XRAPI_CALL Layer_xrStringToPath(XrInstance instance, const char *pathString, XrPath *path);
XrResult XRAPI_CALL Layer_xrPathToString(XrInstance instance, XrPath path,
                                          uint32_t bufferCapacityInput, uint32_t *bufferCountOutput,
                                          char *buffer);

// Session
XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance,
                                           const XrSessionCreateInfo *createInfo,
                                           XrSession *session);
XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session);
XrResult XRAPI_CALL Layer_xrBeginSession(XrSession session, const XrSessionBeginInfo *beginInfo);
XrResult XRAPI_CALL Layer_xrEndSession(XrSession session);

// Action sets & actions
XrResult XRAPI_CALL Layer_xrCreateActionSet(XrInstance instance,
                                             const XrActionSetCreateInfo *createInfo,
                                             XrActionSet *actionSet);
XrResult XRAPI_CALL Layer_xrDestroyActionSet(XrActionSet actionSet);
XrResult XRAPI_CALL Layer_xrCreateAction(XrActionSet actionSet,
                                          const XrActionCreateInfo *createInfo,
                                          XrAction *action);
XrResult XRAPI_CALL Layer_xrDestroyAction(XrAction action);
XrResult XRAPI_CALL Layer_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding *suggestedBindings);
XrResult XRAPI_CALL Layer_xrAttachSessionActionSets(XrSession session,
                                                     const XrSessionActionSetsAttachInfo *attachInfo);
XrResult XRAPI_CALL Layer_xrSyncActions(XrSession session, const XrActionsSyncInfo *syncInfo);
XrResult XRAPI_CALL Layer_xrGetActionStateBoolean(XrSession session,
                                                    const XrActionStateGetInfo *getInfo,
                                                    XrActionStateBoolean *state);
XrResult XRAPI_CALL Layer_xrGetActionStateFloat(XrSession session,
                                                  const XrActionStateGetInfo *getInfo,
                                                  XrActionStateFloat *state);
XrResult XRAPI_CALL Layer_xrGetActionStateVector2f(XrSession session,
                                                     const XrActionStateGetInfo *getInfo,
                                                     XrActionStateVector2f *state);
XrResult XRAPI_CALL Layer_xrGetActionStatePose(XrSession session,
                                                 const XrActionStateGetInfo *getInfo,
                                                 XrActionStatePose *state);
XrResult XRAPI_CALL Layer_xrGetCurrentInteractionProfile(XrSession session,
                                                          XrPath topLevelUserPath,
                                                          XrInteractionProfileState *interactionProfile);

// Spaces
XrResult XRAPI_CALL Layer_xrCreateReferenceSpace(XrSession session,
                                                   const XrReferenceSpaceCreateInfo *createInfo,
                                                   XrSpace *space);
XrResult XRAPI_CALL Layer_xrCreateActionSpace(XrSession session,
                                               const XrActionSpaceCreateInfo *createInfo,
                                               XrSpace *space);
XrResult XRAPI_CALL Layer_xrDestroySpace(XrSpace space);
XrResult XRAPI_CALL Layer_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time,
                                         XrSpaceLocation *location);
XrResult XRAPI_CALL Layer_xrLocateViews(XrSession session,
                                         const XrViewLocateInfo *viewLocateInfo,
                                         XrViewState *viewState, uint32_t viewCapacityInput,
                                         uint32_t *viewCountOutput, XrView *views);

// Frame
XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo,
                                       XrFrameState *frameState);
XrResult XRAPI_CALL Layer_xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo);
XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo);

// Special: our xrGetInstanceProcAddr (also registered, but handled specially)
XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                  PFN_xrVoidFunction *function);

} // namespace debug_layer
