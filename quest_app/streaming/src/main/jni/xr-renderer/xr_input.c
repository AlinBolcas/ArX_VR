// Controllers, hands and gaze: the actions they are read through, the ray
// they aim, and the per frame pass that turns it into hover, grab, panel
// presses and host mouse events.
#include "xr_renderer.h"
#include <time.h>

// A pinch is how these headsets click, but it is not always offered as an
// input to bind to. The joints always are, so it is measured here instead:
// thumb tip to index tip, with a gap between the closing and opening distances
// so a hand held near the threshold does not chatter.
#define PINCH_ON_M  0.020f
#define PINCH_OFF_M 0.032f

static void initJointTracking(XrCtx* ctx) {
    if (!ctx->handTracking) {
        return;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrCreateHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&ctx->pfnCreateHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyHandTrackerEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnDestroyHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrLocateHandJointsEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnLocateHandJoints))
            || ctx->pfnCreateHandTracker == NULL || ctx->pfnLocateHandJoints == NULL) {
        LOGW("hand joint entry points missing");
        ctx->jointTracking = 0;
        return;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrHandTrackerCreateInfoEXT info = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        info.hand = h == HAND_LEFT ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!checkXr(ctx->pfnCreateHandTracker(ctx->session, &info, &ctx->handTrackers[h]),
                     "create hand tracker")) {
            ctx->handTrackers[h] = XR_NULL_HANDLE;
            return;
        }
    }
    ctx->jointTracking = 1;
    LOGI("reading hand joints for pinch");
}

// Which kind of thing is driving each hand. Hands are never still enough for
// the movement gate to mean anything, so they wake the pointer a different way
// and need to be told apart from controllers.
void refreshInputSource(XrCtx* ctx) {
    if (ctx->session == XR_NULL_HANDLE || !ctx->inputReady) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(ctx->session, ctx->handPaths[h], &state))) {
            continue;
        }
        // Without a pinch bound there is nothing to wake the pointer with, so
        // those hands stay on the movement gate rather than becoming unusable
        int hands = ctx->handClickOk && state.interactionProfile != XR_NULL_PATH
                && (state.interactionProfile == ctx->handProfile
                    || state.interactionProfile == ctx->msftHandProfile);
        if (hands != ctx->usingHands[h]) {
            LOGI("hand %d is now driven by %s", h, hands ? "hand tracking" : "a controller");
        }
        ctx->usingHands[h] = hands;
    }
}

static XrPath toPath(XrCtx* ctx, const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(ctx->instance, str, &path);
    return path;
}

static XrAction makeAction(XrCtx* ctx, XrActionType type, const char* name, const char* label) {
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = HAND_COUNT;
    info.subactionPaths = ctx->handPaths;

    XrAction action = XR_NULL_HANDLE;
    if (!checkXr(xrCreateAction(ctx->actionSet, &info, &action), name)) {
        return XR_NULL_HANDLE;
    }
    return action;
}

// One unsupported path rejects a whole profile, so the full set is offered
// first and a runtime that does not recognise this controller falls back to
// aim and trigger, which every profile has.
static void suggestBindings(XrCtx* ctx, const char* profile, int full) {
    XrActionSuggestedBinding b[16];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };
    // x and y on the left controller, a and b on the right, mirrored so either hand
    // does the same thing: the lower button dictates, the upper one right-clicks
    static const char* rightClick[HAND_COUNT] = { "input/y/click", "input/b/click" };
    static const char* dictate[HAND_COUNT] = { "input/x/click", "input/a/click" };
    // Only the left controller has a menu button an app may use; the right one is the system's
    int menu = strstr(profile, "/oculus/") != NULL;
    int simple = strstr(profile, "/khr/") != NULL;

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/input/aim/pose", hands[h]);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h],
                 simple ? "input/select/click" : "input/trigger/value");
        b[n].action = ctx->triggerAction;
        b[n++].binding = toPath(ctx, path);

        if (!full || simple) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", hands[h], rightClick[h]);
        b[n].action = ctx->rightClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h], dictate[h]);
        b[n].action = ctx->voiceAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick", hands[h]);
        b[n].action = ctx->scrollAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick/click", hands[h]);
        b[n].action = ctx->keyboardAction;
        b[n++].binding = toPath(ctx, path);

        if (menu && h == HAND_LEFT) {
            snprintf(path, sizeof(path), "%s/input/menu/click", hands[h]);
            b[n].action = ctx->menuAction;
            b[n++].binding = toPath(ctx, path);
        }

        snprintf(path, sizeof(path), "%s/input/squeeze/value", hands[h]);
        b[n].action = ctx->grabAction;
        b[n++].binding = toPath(ctx, path);
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    XrResult res = xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
    if (XR_SUCCEEDED(res)) {
        LOGI("bindings accepted for %s (%s)", profile, full ? "full" : "reduced");
    }
    else if (full) {
        LOGW("full bindings rejected for %s (%d), trying aim and trigger only", profile, res);
        suggestBindings(ctx, profile, 0);
    }
    else {
        LOGW("bindings rejected for %s (%d)", profile, res);
    }
}

// Hands come in through the same actions the controllers use, so everything
// downstream of here treats them identically: same ray, same handles, same
// picker. Only the paths differ, which is why this is its own function rather
// than another flag on the one above.
static XrResult trySuggestHands(XrCtx* ctx, const char* profile, const char* aim,
                                const char* click, const char* grasp) {
    XrActionSuggestedBinding b[6];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/%s", hands[h], aim);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        if (click != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], click);
            b[n].action = ctx->triggerAction;
            b[n++].binding = toPath(ctx, path);
        }

        if (grasp != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], grasp);
            b[n].action = ctx->grabAction;
            b[n++].binding = toPath(ctx, path);
        }
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    return xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
}

// Runtimes that offer the hand profile do not all implement every input in it,
// and one unsupported path throws out the whole suggestion. So the inputs are
// offered up in falling order of usefulness until a set is accepted. Returns
// whether a pinch ended up bound, since without one the hands cannot wake the
// pointer and are better left to the movement gate.
static int suggestHandBindings(XrCtx* ctx, const char* profile, const char* aim,
                               const char* const* clicks, int clickCount,
                               const char* grasp) {
    XrResult res = XR_SUCCESS;
    for (int c = 0; c < clickCount; c++) {
        if (grasp != NULL) {
            res = trySuggestHands(ctx, profile, aim, clicks[c], grasp);
            if (XR_SUCCEEDED(res)) {
                LOGI("hand bindings accepted for %s (%s and grasp)", profile, clicks[c]);
                return 1;
            }
        }
        res = trySuggestHands(ctx, profile, aim, clicks[c], NULL);
        if (XR_SUCCEEDED(res)) {
            LOGI("hand bindings accepted for %s (%s)", profile, clicks[c]);
            return 1;
        }
    }
    res = trySuggestHands(ctx, profile, aim, NULL, NULL);
    if (XR_SUCCEEDED(res)) {
        LOGW("only the aim pose bound for %s, so hands cannot click", profile);
        return 0;
    }
    LOGW("hand bindings rejected for %s, even the aim pose alone (%d)", profile, res);
    return 0;
}

int initXrInput(XrCtx* ctx) {
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "moonlight", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Moonlight", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!checkXr(xrCreateActionSet(ctx->instance, &setInfo, &ctx->actionSet), "create action set")) {
        return 0;
    }

    ctx->handPaths[HAND_LEFT] = toPath(ctx, "/user/hand/left");
    ctx->handPaths[HAND_RIGHT] = toPath(ctx, "/user/hand/right");

    ctx->aimAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    ctx->triggerAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Left click");
    ctx->rightClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "rightclick", "Right click");
    ctx->voiceAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "dictate", "Hold to dictate");
    ctx->scrollAction = makeAction(ctx, XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll");
    ctx->grabAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "grab", "Move the screen");
    ctx->keyboardAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "keyboard", "Pie menu");
    ctx->menuAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "recentre", "Bring the desktops in front");

    if (ctx->aimAction == XR_NULL_HANDLE || ctx->triggerAction == XR_NULL_HANDLE) {
        return 0;
    }

    suggestBindings(ctx, "/interaction_profiles/khr/simple_controller", 1);
    suggestBindings(ctx, "/interaction_profiles/oculus/touch_controller", 1);
    if (ctx->picoInteraction) {
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico4_controller", 1);
    }

    // Hands. aim_activate is the spec's own name for pointing at something out
    // of reach and pinching to act on it, which is exactly what the ray does.
    // Gaze is its own top level path rather than a hand, so it needs an action
    // of its own. There is no click on it: whatever the runtime reports as a
    // trigger, usually a pinch, does the clicking.
    if (ctx->eyeGaze) {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strncpy(info.actionName, "gaze", XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(info.localizedActionName, "Gaze pointer",
                XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        if (checkXr(xrCreateAction(ctx->actionSet, &info, &ctx->gazeAction), "gaze action")) {
            XrActionSuggestedBinding b;
            b.action = ctx->gazeAction;
            b.binding = toPath(ctx, "/user/eyes_ext/input/gaze_ext/pose");

            XrInteractionProfileSuggestedBinding suggest = {
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
            };
            suggest.interactionProfile = toPath(ctx,
                    "/interaction_profiles/ext/eye_gaze_interaction");
            suggest.countSuggestedBindings = 1;
            suggest.suggestedBindings = &b;
            if (XR_FAILED(xrSuggestInteractionProfileBindings(ctx->instance, &suggest))) {
                LOGW("gaze bindings rejected");
                ctx->gazeAction = XR_NULL_HANDLE;
                ctx->eyeGaze = 0;
            }
        }
        else {
            ctx->gazeAction = XR_NULL_HANDLE;
            ctx->eyeGaze = 0;
        }
    }

    if (ctx->handInteraction) {
        // aim_activate is the spec's own name for the far pointer pinch, and
        // pinch is the plain one. Runtimes vary in which they implement.
        static const char* const clicks[] = {
            "input/aim_activate_ext/value", "input/pinch_ext/value"
        };
        const char* profile = "/interaction_profiles/ext/hand_interaction_ext";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim_ext/pose",
                                                clicks, 2, "input/grasp_ext/value");
        ctx->handProfile = toPath(ctx, profile);
    }
    // Older runtimes that predate the EXT profile. Same idea, fewer inputs.
    if (ctx->msftHandInteraction) {
        static const char* const clicks[] = { "input/select/value" };
        const char* profile = "/interaction_profiles/microsoft/hand_interaction";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim/pose",
                                                clicks, 1, "input/squeeze/value");
        ctx->msftHandProfile = toPath(ctx, profile);
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &ctx->actionSet;
    if (!checkXr(xrAttachSessionActionSets(ctx->session, &attach), "attach action sets")) {
        return 0;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->aimAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[h]),
                     "create aim space")) {
            return 0;
        }
    }

    if (ctx->gazeAction != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->gazeAction;
        spaceInfo.subactionPath = XR_NULL_PATH;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[SRC_GAZE]),
                     "create gaze space")) {
            ctx->aimSpaces[SRC_GAZE] = XR_NULL_HANDLE;
        }
    }

    ctx->inputReady = 1;
    ctx->pointerOn = 1;
    initJointTracking(ctx);
    refreshInputSource(ctx);
    LOGI("controller input ready (pico bindings %s, hand pinch %s)",
         ctx->picoInteraction ? "offered" : "not offered by this runtime",
         ctx->handClickOk ? "bound" : (ctx->jointTracking ? "from joints" : "unavailable"));
    return 1;
}

static float actionFloat(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(ctx->session, &get, &state)) || !state.isActive) {
        return 0.0f;
    }
    return state.currentState;
}

static int actionBool(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(ctx->session, &get, &state)) || !state.isActive) {
        return 0;
    }
    return state.currentState != 0;
}

static XrVector2f actionVec2(XrCtx* ctx, XrAction action, int hand) {
    XrVector2f zero = { 0.0f, 0.0f };
    if (action == XR_NULL_HANDLE) {
        return zero;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(ctx->session, &get, &state)) || !state.isActive) {
        return zero;
    }
    return state.currentState;
}

// A pointer ray from the joints, for runtimes that track hands but never offer
// a pointer pose. Cast from a shoulder rather than from the hand itself: a ray
// along the finger swings wildly with small movements of the wrist, while one
// through the hand from the shoulder is what the arm is actually aiming and is
// steady enough to hold on a target.
static void buildHandRay(XrCtx* ctx, int hand, const XrPosef* head,
                         const XrHandJointLocationEXT* joints) {
    const XrHandJointLocationEXT* knuckle = &joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    if (!(knuckle->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->handRayValid[hand] = 0;
        return;
    }

    Vec3 offset = { hand == HAND_RIGHT ? 0.17f : -0.17f, -0.20f, 0.05f };
    Vec3 shoulder = quatRotate(head->orientation, offset);
    shoulder.x += head->position.x;
    shoulder.y += head->position.y;
    shoulder.z += head->position.z;

    Vec3 origin = { knuckle->pose.position.x, knuckle->pose.position.y,
                    knuckle->pose.position.z };
    Vec3 dir = vecSub(origin, shoulder);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 0.05f) {
        ctx->handRayValid[hand] = 0;
        return;
    }
    dir = vecNorm(dir);

    // A pose points down its own -Z, so the basis is built around that
    Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    Vec3 rayZ = { -dir.x, -dir.y, -dir.z };
    Vec3 rayX = vecCross(worldUp, rayZ);
    float side = sqrtf(rayX.x * rayX.x + rayX.y * rayX.y + rayX.z * rayX.z);
    if (side < 0.01f) {
        Vec3 fallback = { 1.0f, 0.0f, 0.0f };
        rayX = vecCross(fallback, rayZ);
    }
    rayX = vecNorm(rayX);
    Vec3 rayY = vecCross(rayZ, rayX);

    ctx->handRay[hand].orientation = quatFromBasis(rayX, rayY, rayZ);
    ctx->handRay[hand].position = knuckle->pose.position;
    ctx->handRayValid[hand] = 1;
}

static int jointPinching(XrCtx* ctx, int hand, XrSpace space, const XrPosef* head,
                         int headValid) {
    if (!ctx->jointTracking || ctx->handTrackers[hand] == XR_NULL_HANDLE) {
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;

    XrHandJointsLocateInfoEXT locate = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locate.baseSpace = space;
    locate.time = ctx->predictedDisplayTime;
    if (XR_FAILED(ctx->pfnLocateHandJoints(ctx->handTrackers[hand], &locate, &locations))
            || !locations.isActive) {
        ctx->jointPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    if (headValid) {
        buildHandRay(ctx, hand, head, joints);
    }

    const XrHandJointLocationEXT* thumb = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT* index = &joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    if (!(thumb->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            || !(index->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->jointPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        return 0;
    }

    float dx = thumb->pose.position.x - index->pose.position.x;
    float dy = thumb->pose.position.y - index->pose.position.y;
    float dz = thumb->pose.position.z - index->pose.position.z;
    float gap = sqrtf(dx * dx + dy * dy + dz * dz);

    // Where the pinch happened, which is what a drag follows
    ctx->pinchPoint[hand].x = (thumb->pose.position.x + index->pose.position.x) * 0.5f;
    ctx->pinchPoint[hand].y = (thumb->pose.position.y + index->pose.position.y) * 0.5f;
    ctx->pinchPoint[hand].z = (thumb->pose.position.z + index->pose.position.z) * 0.5f;
    ctx->pinchPointValid[hand] = 1;

    ctx->jointPinch[hand] = gap < (ctx->jointPinch[hand] ? PINCH_OFF_M : PINCH_ON_M);

    // The other three fingers each mean one thing against the thumb: middle
    // right-clicks, ring holds dictation, little finger scrolls. Only the one
    // nearest the thumb counts, and none while the index pinches, so a loose
    // hand never fires two at once.
    static const XrHandJointEXT tips[3] = { XR_HAND_JOINT_MIDDLE_TIP_EXT, XR_HAND_JOINT_RING_TIP_EXT,
                                            XR_HAND_JOINT_LITTLE_TIP_EXT };
    int* held[3] = { &ctx->jointMiddlePinch[hand], &ctx->jointRingPinch[hand], &ctx->jointPinkyPinch[hand] };
    float gaps[3];
    int nearest = 0;
    for (int i = 0; i < 3; i++) {
        const XrHandJointLocationEXT* tip = &joints[tips[i]];
        gaps[i] = 1.0f;
        if (tip->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            float fx = thumb->pose.position.x - tip->pose.position.x;
            float fy = thumb->pose.position.y - tip->pose.position.y;
            float fz = thumb->pose.position.z - tip->pose.position.z;
            gaps[i] = sqrtf(fx * fx + fy * fy + fz * fz);
        }
        if (gaps[i] < gaps[nearest]) nearest = i;
    }
    for (int i = 0; i < 3; i++) {
        *held[i] = !ctx->jointPinch[hand] && i == nearest
                && gaps[i] < (*held[i] ? PINCH_OFF_M : PINCH_ON_M);
    }
    ctx->thumbY[hand] = thumb->pose.position.y;
    return ctx->jointPinch[hand];
}

// The sliders place the screen, the grab moves it from there. Moving either
// slider is taken as the user asking for the placement back. Says whether it
// reseeded, since a room holding the screen has to know the placement waiting
// behind it has changed.
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature) {
    int sliderMoved = ctx->sliderSeen
            && (fabsf(distance - ctx->lastDistance) > 1e-4f
                || fabsf(quadWidth - ctx->lastQuadWidth) > 1e-4f);
    int reseeded = !ctx->placementValid || sliderMoved;

    if (reseeded) {
        memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
        ctx->screenPose.orientation.w = 1.0f;
        ctx->screenPose.position.z = -distance;
        ctx->screenWidth = quadWidth;
        // Radius runs from 4x distance (slightly curved) down to the distance
        // itself (wrapped around the viewer) as curvature rises
        ctx->screenRadius = distance * (1.0f + 3.0f * (1.0f - curvature));
        ctx->placementValid = 1;
        ctx->grabMode = GRAB_NONE;
        ctx->poseDirty = 1;
    }

    ctx->lastDistance = distance;
    ctx->lastQuadWidth = quadWidth;
    ctx->sliderSeen = 1;
    return reseeded;
}

// Handed back only when a grab ends, so preferences are written once per move
// rather than every frame of it
static void writeInputPose(XrCtx* ctx, float* out) {
    if (!ctx->poseDirty) {
        return;
    }
    ctx->poseDirty = 0;
    out[IN_POSE_DIRTY] = 1.0f;
    out[IN_POSE + 0] = ctx->screenPose.position.x;
    out[IN_POSE + 1] = ctx->screenPose.position.y;
    out[IN_POSE + 2] = ctx->screenPose.position.z;
    out[IN_POSE + 3] = ctx->screenPose.orientation.x;
    out[IN_POSE + 4] = ctx->screenPose.orientation.y;
    out[IN_POSE + 5] = ctx->screenPose.orientation.z;
    out[IN_POSE + 6] = ctx->screenPose.orientation.w;
    out[IN_POSE + 7] = ctx->screenWidth;
    out[IN_POSE + 8] = ctx->screenRadius;
    out[IN_POSE + 9] = ctx->panelCurve;
}

// Move and resize both work off the handle the ray was over when the grip
// closed. Gripping the picture itself does nothing, which keeps the panel from
// being dragged by accident while pointing at something. Every desktop goes
// through here, so they all move and size the same way: target says which.
static void applyGrab(XrCtx* ctx, XrPosef* aims, const int* valid, int hand,
                      int hover, int corner, int offPicture, int target, int curved, float dt) {
    for (int h = 0; h < HAND_COUNT; h++) {
        int wasDown = ctx->grabDown[h];
        float value = actionFloat(ctx, ctx->grabAction, h);
        ctx->grabDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON);
        ctx->gripEdge[h] = ctx->grabDown[h] && !wasDown;
    }

    if (ctx->grabMode != GRAB_NONE) {
        int stillHeld = ctx->grabByTrigger ? ctx->triggerDown[ctx->grabHand]
                                           : ctx->grabDown[ctx->grabHand];
        if (!stillHeld || !valid[ctx->grabHand] || !screenLive(ctx, ctx->grabTarget)) {
            // Persist where it ended up, not every frame of the drag. Only the
            // main screen's placement is stored between sessions.
            ctx->grabMode = GRAB_NONE;
            if (ctx->grabTarget == 0) ctx->poseDirty = 1;
            else layoutSave(ctx);
            return;
        }
    }

    if (ctx->grabMode == GRAB_NONE) {
        // A 3d room holds the picture on its wall and forces the pose every
        // frame, so a drag could only fight it. Neither handle is drawn there,
        // and the corners are not even hovered.
        if (roomHangsPicture(ctx)) {
            return;
        }
        if (hand < 0 || (hover != HOVER_BAR && hover != HOVER_CORNER && hover != HOVER_SCREEN)) {
            return;
        }

        // The grip takes hold of a desktop anywhere on it, the trigger only on the
        // handles, since on the picture the trigger is a left click and the bottom
        // corners of a desktop are worth clicking.
        int byGrip = ctx->gripEdge[hand];
        int byTrigger = ctx->triggerEdge[hand] && offPicture && hover != HOVER_SCREEN;
        if (!byGrip && !byTrigger) {
            return;
        }
        ScreenRef r = screenRef(ctx, target);
        ctx->grabByTrigger = !byGrip;
        ctx->grabTarget = target;
        ctx->grabHand = hand;
        ctx->grabAim = aims[hand];
        ctx->grabScreen = *r.pose;
        ctx->grabWidth = *r.width;
        ctx->grabHeight = *r.width * r.aspect;
        ctx->grabRadius = *r.radius;
        ctx->grabPush = 0.0f;
        ctx->grabOnPicture = hover == HOVER_SCREEN
                && screenProject(aims[hand], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                                 ctx->grabRadius, curved, &ctx->grabU, &ctx->grabV);

        if (hover == HOVER_BAR || hover == HOVER_SCREEN) {
            ctx->grabMode = GRAB_MOVE;
            // Read once here rather than every frame of the drag. Re-extracting
            // each frame would let the rounding walk the tilt away over a long move.
            ctx->grabPitch = posePitch(ctx->grabScreen);
            ctx->grabRoll = poseRoll(ctx->grabScreen);
            return;
        }

        // The hit itself is not needed any more, but a ray that misses the
        // plane has nothing to measure the drag against
        float u, v;
        if (!screenProject(aims[hand], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                           ctx->grabRadius, curved, &u, &v)) {
            return;
        }

        // The centre is the anchor, and the drag runs along the half diagonal
        // out to the corner being held
        int right = (corner == 1 || corner == 3);
        int bottom = (corner >= 2);
        ctx->grabOppX = (right ? -0.5f : 0.5f) * ctx->grabWidth;
        ctx->grabOppY = (bottom ? 0.5f : -0.5f) * ctx->grabHeight;
        ctx->grabMode = GRAB_RESIZE;
        return;
    }

    ScreenRef r = screenRef(ctx, ctx->grabTarget);
    int h = ctx->grabHand;
    if (ctx->grabMode == GRAB_MOVE) {
        // Where it goes is the rigid attach: the offset from the hand is carried
        // round by the full hand turn, so the screen swings on a lever rather
        // than sliding with the hand, which only travels a few centimetres.
        XrQuaternionf turn = quatMul(aims[h].orientation, quatConj(ctx->grabAim.orientation));
        Vec3 offset = { ctx->grabScreen.position.x - ctx->grabAim.position.x,
                        ctx->grabScreen.position.y - ctx->grabAim.position.y,
                        ctx->grabScreen.position.z - ctx->grabAim.position.z };
        Vec3 moved = quatRotate(turn, offset);

        // The thumbstick, while holding: forward and back push it away and pull it
        // in along the ray, left and right take the curve down and up. Whichever way
        // the stick leans further, so one never nudges the other.
        XrVector2f stick = actionVec2(ctx, ctx->scrollAction, h);
        if (fabsf(stick.y) >= fabsf(stick.x) && fabsf(stick.y) > SCROLL_DEADZONE) {
            float past = (fabsf(stick.y) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
            ctx->grabPush += copysignf(past * GRAB_PUSH_SPEED * dt, stick.y);
        }
        else if (fabsf(stick.x) > SCROLL_DEADZONE) {
            float past = (fabsf(stick.x) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
            ctx->panelCurve = fminf(fmaxf(effectiveCurvature(ctx) + copysignf(past * GRAB_CURVE_SPEED * dt, stick.x), 0.0f), 1.0f);
        }
        float reach = sqrtf(vecDot(offset, offset));
        if (reach > 0.01f) {
            float wanted = fminf(fmaxf(reach + ctx->grabPush, GRAB_MIN_REACH), GRAB_MAX_REACH);
            // Held at the ends rather than winding up past them, so pulling back answers at once
            ctx->grabPush = wanted - reach;
            float scale = wanted / reach;
            moved.x *= scale; moved.y *= scale; moved.z *= scale;
        }

        r.pose->position.x = aims[h].position.x + moved.x;
        r.pose->position.y = aims[h].position.y + moved.y;
        r.pose->position.z = aims[h].position.z + moved.z;

        // Held rigidly, as a real panel would be: it turns with the wrist on every axis,
        // twist included, from the way it faced when it was picked up. Held from the hand,
        // which sits near the head, it still ends up roughly facing you wherever it goes.
        XrQuaternionf held = quatMul(turn, ctx->grabScreen.orientation);
        float n = sqrtf(held.x * held.x + held.y * held.y + held.z * held.z + held.w * held.w);
        if (n > 1e-6f) {
            held.x /= n; held.y /= n; held.z /= n; held.w /= n;
            r.pose->orientation = held;
        }
        // Every arc re-bent about the viewer, for the distance this one is at now and the curve
        applyCurve(ctx);
        return;
    }

    // Resize. Everything is measured against the pose the grab started from,
    // so growing the screen cannot feed back into where the ray lands on it.
    float u, v;
    if (!screenProject(aims[h], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                       ctx->grabRadius, curved, &u, &v)) {
        return;
    }

    // Measured from the centre, since that is what holds. The half diagonal is
    // the held corner's own position, so projecting onto it keeps that corner
    // under the ray.
    float px = (u - 0.5f) * ctx->grabWidth;
    float py = (0.5f - v) * ctx->grabHeight;
    float halfX = -ctx->grabOppX;
    float halfY = -ctx->grabOppY;
    float halfLen = halfX * halfX + halfY * halfY;
    float scale = (px * halfX + py * halfY) / halfLen;
    if (scale < 0.05f) {
        scale = 0.05f;
    }

    float width = ctx->grabWidth * scale;
    if (width < SCREEN_MIN_WIDTH) width = SCREEN_MIN_WIDTH;
    if (width > SCREEN_MAX_WIDTH) width = SCREEN_MAX_WIDTH;

    // Keeping the arc the same shape rather than flattening as it grows
    *r.radius = ctx->grabRadius * (width / ctx->grabWidth);
    *r.width = width;

    // The centre stays where it was, so the screen grows evenly about the spot
    // it was placed on rather than walking off towards one corner
    *r.pose = ctx->grabScreen;
}

// The press was meant for the thing that is open, not for the host behind it.
// Gaze has no button of its own, so swallowing a gaze press means swallowing
// the pinch that stood in for it.
static void swallowTrigger(XrCtx* ctx, int src) {
    if (src < 0 || src >= SRC_COUNT) {
        return;
    }
    ctx->triggerSwallowed[src] = 1;
    if (src == SRC_GAZE) {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->triggerDown[h]) {
                ctx->triggerSwallowed[h] = 1;
            }
        }
    }
}

// Where the ray lands on furniture rather than on the picture. The grid has a
// plane of its own, everything else sits on the screen.
static Vec3 furniturePoint(XrCtx* ctx, int hover, float u, float v, XrPosef screenPose,
                           float width, float height, float radius, int curved) {
    if (hover == HOVER_PICKER) {
        float pickW, pickH;
        XrPosef pose = pickerPose(ctx, &pickW, &pickH);
        return screenPoint(u, v, pose, pickW, pickH, 0.0f, 0);
    }
    if (hover == HOVER_COGPANEL) {
        return screenPoint(u, v, ctx->cogPose, ctx->cogW, ctx->cogH, 0.0f, 0);
    }
    if (hover == HOVER_KBPANEL) {
        return screenPoint(u, v, ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0);
    }
    if (hover == HOVER_EXITPROMPT) {
        return screenPoint(u, v, ctx->exitPose, ctx->exitW, ctx->exitH, 0.0f, 0);
    }
    // The dock's own plane. Read as screen coordinates, as it used to be, its
    // top edge landed the dot on the top of the picture the moment the ray
    // reached the dock. Held to the strip, since its hit band is generous.
    if (hover == HOVER_DOCK) {
        return screenPoint(u, fminf(fmaxf(v, 0.0f), 1.0f), ctx->dockPose, ctx->dockW, ctx->dockH, 0.0f, 0);
    }
    if (hover == HOVER_PIE) {
        return screenPoint(u, v, ctx->piePose, PIE_SIZE_M, PIE_SIZE_M, 0.0f, 0);
    }
    return screenPoint(u, v, screenPose, width, height, radius, curved);
}

void destroyXrInput(XrCtx* ctx) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handTrackers[h] != XR_NULL_HANDLE && ctx->pfnDestroyHandTracker != NULL) {
            ctx->pfnDestroyHandTracker(ctx->handTrackers[h]);
            ctx->handTrackers[h] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->aimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->aimSpaces[h]);
            ctx->aimSpaces[h] = XR_NULL_HANDLE;
        }
    }
    if (ctx->actionSet != XR_NULL_HANDLE) {
        // Takes its actions with it
        xrDestroyActionSet(ctx->actionSet);
        ctx->actionSet = XR_NULL_HANDLE;
    }
    ctx->inputReady = 0;
}

// Everything one pass over the sources works out before deciding what the
// ray is on, shared between the stages below
typedef struct {
    // The IN_ slots handed back to Java
    float* out;
    long now;
    float dt;
    XrSpace space;
    int roomOn;
    int curved;
    float height;
    float radius;
    float width;
    XrPosef screenPose;
    // Which desktop each source's ray is on, and the one being worked on this frame
    int screens[SRC_COUNT];
    int active;
    XrSpaceLocation headLoc;
    int headValid;
    XrPosef aimPoses[SRC_COUNT];
    int aimValid[SRC_COUNT];
    float hitU[SRC_COUNT];
    float hitV[SRC_COUNT];
    int hovers[SRC_COUNT];
    int corners[SRC_COUNT];
    // Tracked separately from the hover, because the lock filter wipes the
    // hovers and this is what says which source to spare
    int atLock[SRC_COUNT];
    int moved;
    int pinching;
    // The source doing the pointing, or -1, and what it is over
    int hand;
    int hover;
} InputFrame;

// Drops whatever the pointer was holding once it stops being watched
static void releaseInput(XrCtx* ctx, float* out) {
    ctx->buttonsDown = 0;
    ctx->beamVisible = 0;
    if (ctx->grabMode != 0) {
        // Dropping focus mid grab has to count as letting go, or the
        // anchor is stale when focus comes back and the screen jumps
        ctx->grabMode = 0;
        ctx->poseDirty = 1;
    }
    if (ctx->cogDragSlider >= 0) {
        // Same for a slider: a drag must not survive the trigger it
        // was being held with going unwatched. Ending it here still
        // writes the value, since out is flushed on the way out.
        cogDragEnded(ctx, out);
    }
}

// Points this frame's geometry at one desktop
static void useScreen(XrCtx* ctx, InputFrame* f, int index) {
    ScreenRef r = screenRef(ctx, index);
    f->active = index;
    f->screenPose = *r.pose;
    f->width = *r.width;
    f->height = *r.width * r.aspect;
    f->radius = *r.radius;
}

// Tests one ray against every desktop. Something real under the ray beats the
// halo around a neighbour, and after that the nearest along the ray wins.
static int pickScreen(XrCtx* ctx, InputFrame* f, int h) {
    int found = 0, bestRank = 9;
    float bestDistance = 1e9f;
    for (int i = 0; i <= ARX_MAX_EXTRA; i++) {
        // A room hangs only the main picture
        if (!screenLive(ctx, i) || (i > 0 && (f->roomOn || !ctx->desktops[i - 1].rendered || !ctx->desktops[i - 1].placed))) continue;
        ScreenRef r = screenRef(ctx, i);
        float width = *r.width, height = width * r.aspect, u, v;
        if (!screenProject(f->aimPoses[h], *r.pose, width, height, *r.radius, f->curved, &u, &v)) continue;
        int corner = 0;
        // Extras carry a close button past the end of the bar, claimed before the bar's wide zone
        int hover = i > 0 && closeHit(u, v, width, height) ? HOVER_CLOSE
                : hoverTest(u, v, width, height, !f->roomOn, &corner);
        if (hover == HOVER_NONE) continue;
        Vec3 point = screenPoint(u, v, *r.pose, width, height, *r.radius, f->curved);
        Vec3 origin = { f->aimPoses[h].position.x, f->aimPoses[h].position.y, f->aimPoses[h].position.z };
        Vec3 delta = vecSub(point, origin);
        float distance = vecDot(delta, delta);
        int rank = hover == HOVER_HALO ? 1 : 0;
        if (rank < bestRank || (rank == bestRank && distance < bestDistance)) {
            bestRank = rank; bestDistance = distance; found = 1;
            f->hovers[h] = hover; f->hitU[h] = u; f->hitV[h] = v;
            f->corners[h] = corner; f->screens[h] = i;
        }
    }
    return found;
}

// Reads every source: the triggers, the aim poses and where each ray lands
static void readSources(XrCtx* ctx, InputFrame* f) {
    for (int h = 0; h < SRC_COUNT; h++) {
        if (h < HAND_COUNT) {
            int wasDown = ctx->triggerDown[h];
            float value = actionFloat(ctx, ctx->triggerAction, h);
            // Either a bound trigger or a measured pinch will do. Runtimes
            // that offer neither leave this at rest, which is what a headset
            // with nothing in its hands should report.
            ctx->triggerDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON)
                    || jointPinching(ctx, h, f->space, &f->headLoc.pose, f->headValid);
            ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;

            // Diagnostics only. A press held from a pinch reads zero here, so
            // a hand click shows as a hold that never leaves 0.00.
            ctx->triggerValue[h] = value;
            if (ctx->triggerEdge[h]) {
                ctx->triggerHoldMin[h] = value;
                ctx->triggerDipFrames[h] = 0;
                ctx->triggerRetaps[h] = 0;
                ctx->triggerDipping[h] = 0;
            }
            else if (ctx->triggerDown[h]) {
                if (value < ctx->triggerHoldMin[h]) {
                    ctx->triggerHoldMin[h] = value;
                }
                if (value < PRESS_ON) {
                    ctx->triggerDipFrames[h]++;
                    ctx->triggerDipping[h] = 1;
                }
                else if (ctx->triggerDipping[h]) {
                    ctx->triggerRetaps[h]++;
                    ctx->triggerDipping[h] = 0;
                }
            }
        }
        else if (!ctx->eyeGaze || !ctx->gazeEnabled
                 || ctx->aimSpaces[SRC_GAZE] == XR_NULL_HANDLE) {
            continue;
        }

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        int located = XR_SUCCEEDED(xrLocateSpace(ctx->aimSpaces[h], f->space,
                                                 ctx->predictedDisplayTime, &loc))
                && (loc.locationFlags & needed) == needed;
        if (!located) {
            // No controller and no pointer pose from the runtime, so the ray
            // built out of the joints stands in. This is what makes hand
            // pointing work on runtimes that refuse the hand profile.
            if (h < HAND_COUNT && ctx->handRayValid[h]) {
                loc.pose = ctx->handRay[h];
            }
            else {
                // Filtering across a tracking gap would sweep the ray in
                // from wherever the hand was last seen
                if (h < HAND_COUNT) {
                    ctx->aimFilterPos[h][0].valid = 0;
                    ctx->aimFilterPos[h][1].valid = 0;
                    ctx->aimFilterPos[h][2].valid = 0;
                    ctx->aimFilterRot[h].valid = 0;
                }
                continue;
            }
        }
        // Hands and controllers go through the pose filter. Gaze does not:
        // eyes move in saccades and the cursor is already smoothed downstream.
        if (h < HAND_COUNT) {
            XrPosef filtered = loc.pose;
            filtered.position.x = euroFilter(&ctx->aimFilterPos[h][0], filtered.position.x,
                                             f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            filtered.position.y = euroFilter(&ctx->aimFilterPos[h][1], filtered.position.y,
                                             f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            filtered.position.z = euroFilter(&ctx->aimFilterPos[h][2], filtered.position.z,
                                             f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            filtered.orientation = euroFilterQuat(&ctx->aimFilterRot[h], filtered.orientation,
                                                  f->dt, ctx->aimMinCutoff, ctx->aimBeta);
            f->aimPoses[h] = filtered;
        }
        else {
            f->aimPoses[h] = loc.pose;
        }
        f->aimValid[h] = 1;
        // The keyboard is not modal, but it does own the ground it covers: it
        // hangs in front of the bar, so a ray that lands on it must not reach
        // the picture or the furniture behind.
        float kbU, kbV;
        int onKeyboard = ctx->kbOpen
                && screenProject(f->aimPoses[h], ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0,
                                 &kbU, &kbV)
                && kbU >= 0.0f && kbU <= 1.0f && kbV >= 0.0f && kbV <= 1.0f;
        // The dock owns its strip the same way
        float dockU, dockV;
        int onDock = dockVisible(ctx)
                && screenProject(f->aimPoses[h], ctx->dockPose, ctx->dockW, ctx->dockH, 0.0f, 0,
                                 &dockU, &dockV)
                // Generous vertically: a tracked hand wobbles, and the strip is a single row
                && dockU >= 0.0f && dockU <= 1.0f && dockV >= -0.4f && dockV <= 1.4f;
        if (ctx->guideOpen) {
            f->hovers[h] = HOVER_GUIDE;
        }
        else if (onDock) {
            f->hovers[h] = HOVER_DOCK;
            f->hitU[h] = dockU;
            f->hitV[h] = dockV;
        }
        else if (onKeyboard) {
            f->hovers[h] = HOVER_KBPANEL;
            f->hitU[h] = kbU;
            f->hitV[h] = kbV;
        }
        else if (pickScreen(ctx, f, h) && f->screens[h] == 0) {
            // Legacy bar buttons are replaced by the dock, so their zones must not claim the ray.
            // They and the padlock belong to the main screen only.
            int legacy = !ctx->dockReady;
            // The button reaches past the left end of the bar's zone, so it is
            // tested here rather than after a hand has been picked. Otherwise
            // the part of it outside that zone belongs to no hand at all.
            if (legacy && (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_BAR)
                    && envButtonHit(ctx, f->hitU[h], f->hitV[h], f->height)) {
                f->hovers[h] = HOVER_ENVBUTTON;
            }
            // The cog is the same button on the other side of the bar, so it
            // is claimed the same way
            if (legacy && (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_BAR)
                    && cogButtonHit(ctx, f->hitU[h], f->hitV[h], f->height)) {
                f->hovers[h] = HOVER_COGBUTTON;
            }
            // And the keyboard is one further out again, far enough out that
            // it sits past the right end of the bar's zone entirely. That is
            // halo ground, so like the padlock on the left it has to claim the
            // halo back or the ray never reaches it.
            if (legacy && (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_BAR
                    || f->hovers[h] == HOVER_HALO)
                    && kbButtonHit(ctx, f->hitU[h], f->hitV[h], f->height)) {
                f->hovers[h] = HOVER_KBBUTTON;
            }
            // The exit button is the same distance out on the left, so it sits
            // past that end of the bar's zone and has to claim the halo back
            // the same way
            if (legacy && (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_BAR
                    || f->hovers[h] == HOVER_HALO)
                    && exitButtonHit(ctx, f->hitU[h], f->hitV[h], f->height)) {
                f->hovers[h] = HOVER_EXITBUTTON;
            }
            // Off the left edge, so the halo owns that ground until the
            // padlock claims it back
            if (ctx->handsEnabled && f->hovers[h] != HOVER_ENVBUTTON
                    && (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_HALO)
                    && lockButtonHit(ctx, f->hitU[h], f->hitV[h], f->height)) {
                f->hovers[h] = HOVER_LOCK;
                f->atLock[h] = 1;
            }
        }

        if (ctx->poseSeen[h] && f->dt > 0.0f) {
            Vec3 now3 = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            Vec3 was3 = { ctx->lastAim[h].position.x, ctx->lastAim[h].position.y,
                          ctx->lastAim[h].position.z };
            Vec3 step = vecSub(now3, was3);
            float speed = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z) / f->dt;

            // Angle between the two orientations, from the dot product of the
            // quaternions, which is half the rotation
            XrQuaternionf a = loc.pose.orientation, b = ctx->lastAim[h].orientation;
            float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            if (dot > 1.0f) {
                dot = 1.0f;
            }
            float turn = 2.0f * acosf(dot) / f->dt;

            // Hands and eyes are never still, so their motion says nothing
            // about intent and the gate would just hold the pointer on forever
            if (!ctx->usingHands[h] && h != SRC_GAZE
                    && (speed > POINTER_MOVE_SPEED || turn > POINTER_TURN_SPEED)) {
                f->moved = 1;
            }
        }
        ctx->lastAim[h] = loc.pose;
        ctx->poseSeen[h] = 1;
    }
}

// Keeps locked hands off everything but the padlock, and gives gaze the pinch
// it clicks with
static void applyHandLock(XrCtx* ctx, InputFrame* f) {
    // Locked hands reach the padlock and nothing else. Everything is dropped
    // at once, the aim as well as the pinch, so there is no ray to chase, no
    // click to land and no grab to start. Controllers are untouched: they
    // never had the problem, and one has to stay able to unlock.
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handsLocked || !ctx->usingHands[h] || f->atLock[h]) {
            continue;
        }
        f->hovers[h] = HOVER_NONE;
        f->aimValid[h] = 0;
        ctx->triggerDown[h] = 0;
        ctx->triggerEdge[h] = 0;
    }

    for (int h = 0; h < SRC_COUNT; h++) {
        if (!f->atLock[h]) {
            ctx->lockArmed[h] = 0;
        }
        else if (!ctx->triggerDown[h]) {
            ctx->lockArmed[h] = 1;
        }
    }

    // Gaze has no button of its own, so a pinch from either hand clicks
    // wherever the eyes have landed
    if (f->aimValid[SRC_GAZE]) {
        ctx->triggerDown[SRC_GAZE] = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
        ctx->triggerEdge[SRC_GAZE] = ctx->triggerEdge[HAND_LEFT] || ctx->triggerEdge[HAND_RIGHT];
        ctx->usingHands[SRC_GAZE] = 1;
    }
    else {
        ctx->triggerDown[SRC_GAZE] = 0;
        ctx->triggerEdge[SRC_GAZE] = 0;
        ctx->usingHands[SRC_GAZE] = 0;
    }
}

// Wakes the pointer on a pinch or a deliberate move and retires it once the
// controller has been still a while
static void updatePointerWake(XrCtx* ctx, InputFrame* f) {
    // A pinch is what a hand has instead of deliberate movement: it turns the
    // pointer on, and keeps it on for as long as pinches keep arriving. The
    // first deliberate pinch also clicks, so the workspace does not require
    // an invisible wake-up gesture.
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        // A pinch on the padlock is always meant as a press. The swallow is
        // there to keep a waking pinch off the host, and the padlock is not
        // the host, so charging the user a pinch for it buys nothing.
        // The padlock and the dock are ours, not the host's, so a pinch there is always a press.
        // Swallowing it only costs the user a gesture and makes the dock feel like it dodges them.
        if (f->atLock[h] || f->hovers[h] == HOVER_DOCK || f->hovers[h] == HOVER_GUIDE) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        if (ctx->triggerDown[h]) {
            f->pinching = 1;
            if (!ctx->pointerAwake) {
                ctx->pointerAwake = 1;
                ctx->pinchSwallowed[h] = 0;
            }
        }
        else {
            ctx->pinchSwallowed[h] = 0;
            // The other fingers are just as deliberate: right click, dictation and
            // scroll all need the pointer where the hand is aiming
            if (h < HAND_COUNT && (ctx->jointMiddlePinch[h] || ctx->jointRingPinch[h]
                                   || ctx->jointPinkyPinch[h])) {
                f->pinching = 1;
                ctx->pointerAwake = 1;
            }
        }
        if (ctx->pinchSwallowed[h]) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }

    // A press taken by the grid, the panel or the keyboard stays taken for as
    // long as it is held, and letting go is what hands the trigger back
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->triggerDown[h]) {
            ctx->triggerSwallowed[h] = 0;
        }
    }

    // Deliberate movement wakes the pointer, a controller put down retires it
    if (f->pinching) {
        // Only the pinch clock matters while hands are in charge
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
    }
    else if (f->moved) {
        ctx->movingFor += f->dt;
        ctx->stillFor = 0.0f;
        if (ctx->movingFor >= ctx->pointerWake) {
            ctx->pointerAwake = 1;
        }
    }
    else {
        ctx->stillFor += f->dt;
        ctx->movingFor = 0.0f;
        if (ctx->stillFor >= ctx->pointerSleep) {
            ctx->pointerAwake = 0;
        }
    }
}

// Chooses the source doing the pointing, and what it is over
static void pickPointingSource(XrCtx* ctx, InputFrame* f) {
    // The hand holding the trigger wins, so a drag is never stolen by the other
    // one drifting across the screen. Right hand otherwise.
    static const int order[SRC_COUNT] = { HAND_RIGHT, HAND_LEFT, SRC_GAZE };
    f->hand = -1;
    for (int i = 0; i < SRC_COUNT; i++) {
        int h = order[i];
        if (f->hovers[h] == HOVER_SCREEN && ctx->triggerDown[h]) {
            f->hand = h;
            break;
        }
    }
    // A hand on something beats one merely near it, so a controller resting in
    // the margin never takes the pointer off the one being aimed
    for (int pass = 0; pass < 2 && f->hand < 0; pass++) {
        for (int i = 0; i < SRC_COUNT && f->hand < 0; i++) {
            int h = order[i];
            if (f->hovers[h] == HOVER_NONE || (pass == 0 && f->hovers[h] == HOVER_HALO)) {
                continue;
            }
            f->hand = h;
        }
    }

    // The padlock is reachable with the pointer asleep, because locking is
    // what put it to sleep and there would otherwise be no way back
    int reachingLock = f->hand >= 0 && f->atLock[f->hand];
    // The dock and the guide stay reachable with the pointer asleep, for the same reason the padlock
    // does: hands only wake the pointer by pinching, and the controls must not dodge the hand
    int reachingOurs = f->hand >= 0 && (f->hovers[f->hand] == HOVER_DOCK || f->hovers[f->hand] == HOVER_GUIDE);
    if (!ctx->pointerAwake && !reachingLock && !reachingOurs && ctx->grabMode == GRAB_NONE) {
        f->hand = -1;
        for (int h = 0; h < SRC_COUNT; h++) {
            f->hovers[h] = HOVER_NONE;
        }
    }

    f->hover = f->hand >= 0 ? f->hovers[f->hand] : HOVER_NONE;
    if (f->hover == HOVER_CORNER) {
        ctx->hoverCorner = f->corners[f->hand];
    }
}

// Puts every button and hover mark back to rest, for this frame to relight
// whichever the ray is on
static void clearHotState(XrCtx* ctx) {
    ctx->pickerHover = -1;
    ctx->envButtonHot = 0;
    ctx->cogButtonHot = 0;
    ctx->cogHoverSlider = -1;
    ctx->cogHoverCell = -1;
    ctx->lockHot = 0;
    ctx->pickerPick = -1;
    ctx->kbButtonHot = 0;
    ctx->kbHoverKey = -1;
    ctx->kbKeyDown = 0;
    ctx->exitButtonHot = 0;
    ctx->exitHoverZone = EXIT_ZONE_NONE;
    ctx->dockHoverKey = -1;
}

static void dockAction(XrCtx* ctx, InputFrame* f, int code);

static void openPie(XrCtx* ctx, XrPosef head) {
    if (!ctx->pieReady || ctx->pieCount <= 0) return;
    ctx->piePose = piePoseAt(ctx, head);
    ctx->pieOpen = 1;
    ctx->pieHover = -1;
    ctx->pieTilted = 0;
    LOGI("pie menu open");
}

// Does what a slice says. Exit asks once more in place: the first choice arms it
// and the menu stays open, the second within three seconds leaves.
static void pieChoose(XrCtx* ctx, InputFrame* f, int slice) {
    ctx->pieTilted = 0;
    if (slice < 0 || slice >= ctx->pieCount) { ctx->pieOpen = 0; return; }
    int code = ctx->pieCodes[slice];
    LOGI("pie menu chose %d", code);
    if (code == KB_CODE_EXIT && !(ctx->exitArmedNs != 0 && f->now - ctx->exitArmedNs < 3000000000LL)) {
        dockAction(ctx, f, code);
        return;
    }
    ctx->pieOpen = 0;
    dockAction(ctx, f, code);
}

// While the pie is open it has the stick and the ray. Tilt the stick onto a slice and
// let it spring back to choose it, or point at a slice and press. A press anywhere
// else closes it, and nothing reaches the Mac underneath.
static void updatePie(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_PIE;
    f->hand = -1;
    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, -1);
    float tilt = sqrtf(stick.x * stick.x + stick.y * stick.y);
    if (tilt > 0.55f) {
        ctx->pieHover = pieSliceAt(ctx, stick.x, stick.y);
        ctx->pieTilted = 1;
    }
    else if (tilt < 0.25f && ctx->pieTilted) {
        pieChoose(ctx, f, ctx->pieHover);
        return;
    }
    for (int h = 0; h < SRC_COUNT; h++) {
        float u, v;
        if (!f->aimValid[h] || !screenProject(f->aimPoses[h], ctx->piePose, PIE_SIZE_M, PIE_SIZE_M, 0.0f, 0, &u, &v)) {
            continue;
        }
        float x = u - 0.5f, y = 0.5f - v, r = sqrtf(x * x + y * y);
        if (r > 0.5f) {
            if (ctx->triggerEdge[h]) { ctx->pieOpen = 0; swallowTrigger(ctx, h); return; }
            continue;
        }
        f->hand = h;
        f->hitU[h] = u;
        f->hitV[h] = v;
        if (!ctx->pieTilted) ctx->pieHover = r > 0.12f ? pieSliceAt(ctx, x, y) : -1;
        if (ctx->triggerEdge[h]) {
            swallowTrigger(ctx, h);
            pieChoose(ctx, f, ctx->pieHover);
        }
        return;
    }
    // Pressed pointing at nothing at all: close
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->triggerEdge[h]) { ctx->pieOpen = 0; swallowTrigger(ctx, h); return; }
    }
}

// The keyboard is picked up like a desktop: grip while pointing at it and it follows the
// hand, turning with it, until the grip opens. Opening the keyboard again always puts it
// back in its place under the main screen, so it can never be lost.
static void grabKeyboard(XrCtx* ctx, InputFrame* f) {
    if (!ctx->kbOpen) {
        ctx->kbGrabHand = -1;
        return;
    }
    int h = ctx->kbGrabHand;
    if (h >= 0) {
        if (!ctx->grabDown[h] || !f->aimValid[h]) {
            ctx->kbGrabHand = -1;
            LOGI("keyboard put down");
            return;
        }
        // The thumbstick pushes it away or pulls it in along the ray, as it does a desktop
        XrVector2f stick = actionVec2(ctx, ctx->scrollAction, h);
        if (fabsf(stick.y) > 0.2f) {
            Vec3 along = { ctx->kbGrabOffset.position.x, ctx->kbGrabOffset.position.y, ctx->kbGrabOffset.position.z };
            float reach = sqrtf(vecDot(along, along));
            float next = fminf(fmaxf(reach * (1.0f + stick.y * 1.2f * f->dt), 0.25f), 3.0f);
            if (reach > 1e-3f) {
                ctx->kbGrabOffset.position.x *= next / reach;
                ctx->kbGrabOffset.position.y *= next / reach;
                ctx->kbGrabOffset.position.z *= next / reach;
            }
        }
        XrPosef aim = f->aimPoses[h];
        Vec3 offset = quatRotate(aim.orientation, (Vec3){ ctx->kbGrabOffset.position.x,
                                 ctx->kbGrabOffset.position.y, ctx->kbGrabOffset.position.z });
        ctx->kbPose.position.x = aim.position.x + offset.x;
        ctx->kbPose.position.y = aim.position.y + offset.y;
        ctx->kbPose.position.z = aim.position.z + offset.z;
        ctx->kbPose.orientation = quatMul(aim.orientation, ctx->kbGrabOffset.orientation);
        return;
    }
    if (ctx->grabMode != GRAB_NONE || f->hand < 0 || !ctx->gripEdge[f->hand]
            || (f->hover != HOVER_KBPANEL && f->hover != HOVER_KBBUTTON)) {
        return;
    }
    XrPosef aim = f->aimPoses[f->hand];
    XrQuaternionf inverse = quatConj(aim.orientation);
    Vec3 local = quatRotate(inverse, (Vec3){ ctx->kbPose.position.x - aim.position.x,
                            ctx->kbPose.position.y - aim.position.y, ctx->kbPose.position.z - aim.position.z });
    ctx->kbGrabOffset.position.x = local.x;
    ctx->kbGrabOffset.position.y = local.y;
    ctx->kbGrabOffset.position.z = local.z;
    ctx->kbGrabOffset.orientation = quatMul(inverse, ctx->kbPose.orientation);
    ctx->kbGrabHand = f->hand;
    LOGI("keyboard picked up");
}

// The guide is modal and any press anywhere closes it, so it can never trap anyone
static void updateGuide(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_GUIDE;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->triggerEdge[h]) {
            ctx->guideOpen = 0;
            swallowTrigger(ctx, h);
            LOGI("guide closed");
            break;
        }
    }
}

// A dock action. Panels the renderer owns are opened here; the rest go to Java as keys.
static void dockAction(XrCtx* ctx, InputFrame* f, int code) {
    if (code == KB_CODE_KEYBOARD) {
        ctx->kbOpen = !ctx->kbOpen;
        if (ctx->kbOpen) {
            ctx->kbState = KB_STATE_LOWER;
            ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
        }
    }
    else if (code == KB_CODE_ENVIRONMENT) {
        ctx->pickerOpen = ctx->pickerReady;
    }
    else if (code == KB_CODE_SETTINGS) {
        ctx->cogTarget = screenLive(ctx, ctx->workScreen) ? ctx->workScreen : 0;
        ctx->cogOpen = 1;
        ctx->cogTab = COG_TAB_SCREEN;
        ctx->cogPose = cogPanelPose(ctx, &ctx->cogW, &ctx->cogH);
    }
    else if (code == KB_CODE_EXIT) {
        // Confirmed in place: a second press within three seconds exits, so nothing jumps elsewhere
        if (ctx->exitArmedNs != 0 && f->now - ctx->exitArmedNs < 3000000000LL) {
            f->out[IN_EXIT] = 1.0f;
            ctx->exitArmedNs = 0;
        }
        else {
            ctx->exitArmedNs = f->now;
            f->out[IN_KEY] = (float)code;
        }
    }
    else if (code == KB_CODE_GUIDE) {
        ctx->guideOpen = ctx->guideReady;
    }
    else {
        f->out[IN_KEY] = (float)code;
    }
    LOGI("dock action %d", code);
}

// The picker is modal: while it is open the ray belongs to it and nothing
// reaches the picture behind
static void updatePicker(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_PICKER;
    // Anything the hands were pointing at before belongs to the screen,
    // and reading those coordinates as grid coordinates would land the
    // ray somewhere it never was
    f->hand = -1;
    float pickW, pickH;
    XrPosef pose = pickerPose(ctx, &pickW, &pickH);
    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!f->aimValid[h] || !ctx->pointerAwake) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], pose, pickW, pickH, 0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        // Each band is a header strip over a row of cells. The strip is a
        // label and nothing else, so pointing at one is pointing at
        // nothing and a press there closes the grid like a press outside.
        int band = (int)(pv * PICKER_ROWS);
        if (band >= PICKER_ROWS) band = PICKER_ROWS - 1;
        float inBand = pv * PICKER_TEX_H - band * PICKER_BAND_PX;
        if (inBand < PICKER_HEADER_PX) {
            continue;
        }
        int col = (int)(pu * PICKER_COLS);
        if (col >= PICKER_COLS) col = PICKER_COLS - 1;
        ctx->pickerHover = band * PICKER_COLS + col;
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;

        if (ctx->triggerEdge[h]) {
            ctx->pickerPick = ctx->pickerHover;
            ctx->pickerChoice = ctx->pickerHover;
            ctx->pickerOpen = 0;
            swallowTrigger(ctx, h);
        }
        break;
    }

    // A press that lands on no cell, whether on a header strip or nowhere
    // near the grid at all, closes it
    if (ctx->pickerOpen && ctx->pickerHover < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->pickerOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Modal in the same way the grid is, and against the pose frozen when
// it opened rather than wherever the screen has since been dragged to
static void updateCogPanel(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_COGPANEL;
    f->hand = -1;

    // A drag keeps the hand that started it, and keeps it even once the
    // ray has wandered off the panel, so a slider can be run to either end
    // in one go. The display tab is cells apart from its one level row.
    if (ctx->cogDragSlider >= 0 && cogScreenLocked(ctx)) {
        // A room took the picture mid drag, which only a debug property
        // can do, and there is nothing left under the thumb to move
        cogDragEnded(ctx, f->out);
    }
    else if (ctx->cogDragSlider >= 0
            && (ctx->cogTab != COG_TAB_DISPLAY
                || ctx->cogDragSlider == COG_DISPLAY_SLIDER_ROW)) {
        int h = ctx->cogDragHand;
        float pu, pv;
        if (h >= 0 && f->aimValid[h] && ctx->triggerDown[h]
                && screenProject(f->aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                                 0.0f, 0, &pu, &pv)) {
            f->hand = h;
            f->hitU[h] = pu;
            f->hitV[h] = pv;
            ctx->cogHoverSlider = ctx->cogDragSlider;
            cogApplySlider(ctx, ctx->cogTab, ctx->cogDragSlider, pu);
        }
        else {
            cogDragEnded(ctx, f->out);
        }
    }

    for (int h = 0; f->hand < 0 && h < SRC_COUNT; h++) {
        float pu, pv;
        if (!f->aimValid[h] || !ctx->pointerAwake) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;

        // The tab bar runs across the top, one even slot per tab. A press
        // up here changes tab and reaches nothing else.
        if (pv < COG_TAB_BAR_B) {
            if (ctx->triggerEdge[h]) {
                int t = (int)(pu * COG_TAB_COUNT);
                if (t >= COG_TAB_COUNT) t = COG_TAB_COUNT - 1;
                ctx->cogTab = t;
                ctx->cogDragSlider = -1;
                ctx->cogDragHand = -1;
            }
            break;
        }

        // Below the tabs the screen tab is a note while a room is on, so
        // rows, tracks and the reset button are all out of reach. The
        // press is still swallowed, since it landed on the panel.
        if (cogScreenLocked(ctx)) {
            ctx->cogHoverSlider = -1;
            ctx->cogHoverCell = -1;
            break;
        }

        int rowCount = cogTabRowCount(ctx->cogTab);
        int row = -1;
        for (int s = 0; s < rowCount; s++) {
            // Curving needs a layer type this runtime may not have, and
            // the row is drawn greyed to say so
            if (ctx->cogTab == COG_TAB_SCREEN && s == COG_SLIDER_CURVE
                    && !ctx->cylinderSupported) {
                continue;
            }
            // With stereo off there is nothing for either 3D row to move,
            // and both are drawn greyed to match
            if (ctx->cogTab == COG_TAB_3D && ctx->stereoMode == DEPTH_MODE_OFF) {
                continue;
            }
            if (fabsf(pv - (COG_ROW_V0 + s * COG_ROW_STEP)) < COG_ROW_HALF) {
                row = s;
                break;
            }
        }

        if (ctx->cogTab == COG_TAB_DISPLAY) {
            if (row == COG_DISPLAY_SLIDER_ROW) {
                // The one track on this tab, handled the way the other
                // tabs' rows are, including the band reaching a little
                // past both ends for the thumb hanging over them
                if (pu <= COG_TRACK_L - 0.04f || pu >= COG_TRACK_R + 0.04f) {
                    row = -1;
                }
                ctx->cogHoverSlider = row;
                ctx->cogHoverCell = -1;
                if (row >= 0 && ctx->triggerEdge[h]) {
                    ctx->cogDragSlider = row;
                    ctx->cogDragHand = h;
                    // Jumps to where the press landed, same as the others
                    cogApplySlider(ctx, ctx->cogTab, row, pu);
                }
                break;
            }

            // Cells, so a press picks one rather than starting a drag
            int cell = row >= 0 ? cogCellAt(pu, cogOptionCells(row)) : -1;
            ctx->cogHoverSlider = cell >= 0 ? row : -1;
            ctx->cogHoverCell = cell;
            if (cell >= 0 && ctx->triggerEdge[h]) {
                int id = cogApplyOption(ctx, row, cell);
                if (id >= 0) {
                    f->out[IN_SETTING] = (float)id;
                    f->out[IN_SETTING_VALUE] = (float)cell;
                }
            }
            break;
        }

        // Sliders. The band reaches a little past both ends of the track,
        // since the thumb hangs over them.
        if (row >= 0 && (pu <= COG_TRACK_L - 0.04f || pu >= COG_TRACK_R + 0.04f)) {
            row = -1;
        }
        ctx->cogHoverSlider = row;

        int onReset = pu >= COG_RESET_L && pu <= COG_RESET_R
                && pv >= COG_RESET_T && pv <= COG_RESET_B;
        if (onReset && ctx->triggerEdge[h] && ctx->cogTab == COG_TAB_3D) {
            // The shipped defaults, 0.5 percent and half convergence, said
            // here rather than read back so the button works the same way
            // whatever the preferences were left on. Still allowed while
            // stereo is off, where it does no harm and keeps the button
            // from being a dead rectangle.
            ctx->panelSeparation = 0.005f;
            ctx->separationCurrent = 0.005f;
            ctx->convergence = 0.5f;
            f->out[IN_SETTING] = (float)SETTING_RESET_3D;
            f->out[IN_SETTING_VALUE] = 0.0f;
            LOGI("3d settings reset from the panel");
        }
        else if (onReset && ctx->triggerEdge[h]) {
            // Hands the curve back to the preference and drops the
            // placement, which is all it takes: updatePlacement reseeds
            // from the preferences on the next frame and marks the pose
            // dirty itself, so the reset persists with nothing else to do.
            // The panel stays open so the jump is visible.
            ctx->panelCurve = -1.0f;
            ctx->placementValid = 0;
            LOGI("screen placement reset from the panel");
        }
        else if (row >= 0 && ctx->triggerEdge[h]) {
            ctx->cogDragSlider = row;
            ctx->cogDragHand = h;
            // Jumps to where the press landed rather than waiting for the
            // first bit of movement
            cogApplySlider(ctx, ctx->cogTab, row, pu);
        }
    }

    // A press that lands off the panel closes it, which is also how the
    // cog button shuts what it opened. One inside that hits nothing is
    // swallowed, so a near miss does not put the panel away.
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->cogOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Modal like the grid and the panel, and against the pose frozen when
// it opened. Ending the stream is not something to do by accident, so
// nothing outside the prompt is reachable while it is up.
static void updateExitPrompt(XrCtx* ctx, InputFrame* f) {
    f->hover = HOVER_EXITPROMPT;
    f->hand = -1;

    for (int h = 0; h < SRC_COUNT; h++) {
        float pu, pv;
        if (!f->aimValid[h] || !ctx->pointerAwake) {
            continue;
        }
        if (!screenProject(f->aimPoses[h], ctx->exitPose, ctx->exitW, ctx->exitH,
                           0.0f, 0, &pu, &pv)) {
            continue;
        }
        if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
            continue;
        }
        f->hand = h;
        f->hitU[h] = pu;
        f->hitV[h] = pv;
        ctx->exitHoverZone = exitPromptZone(pu, pv);

        if (ctx->triggerEdge[h]) {
            if (ctx->exitHoverZone == EXIT_ZONE_EXIT) {
                // Said once. Java takes the session down from here, and
                // the prompt closes either way so a refused exit leaves
                // the button usable.
                f->out[IN_EXIT] = 1.0f;
                LOGI("exit confirmed from the prompt");
            }
            ctx->exitConfirmOpen = 0;
            swallowTrigger(ctx, h);
        }
        break;
    }

    // A press anywhere off the sheet puts it away, the way one off the
    // grid closes that
    if (f->hand < 0) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (ctx->triggerEdge[h]) {
                ctx->exitConfirmOpen = 0;
                swallowTrigger(ctx, h);
            }
        }
    }
}

// Lights whichever piece of furniture the ray is on, and acts on a press there
static void updateFurniture(XrCtx* ctx, InputFrame* f) {
    if (f->hover == HOVER_DOCK) {
        int key = dockKeyAt(ctx, f->hitU[f->hand], f->hitV[f->hand]);
        ctx->dockHoverKey = key;
        if (key >= 0 && ctx->triggerEdge[f->hand]) {
            swallowTrigger(ctx, f->hand);
            dockAction(ctx, f, ctx->dockCodes[key]);
        }
    }
    else if (f->hover == HOVER_ENVBUTTON) {
        ctx->envButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->pickerOpen = 1;
        }
    }
    else if (f->hover == HOVER_COGBUTTON) {
        ctx->cogButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->cogOpen = !ctx->cogOpen;
            if (ctx->cogOpen) {
                // Always opens on the first tab, so the button does the same
                // thing every time
                ctx->cogTab = COG_TAB_SCREEN;
                ctx->cogPose = cogPanelPose(ctx, &ctx->cogW, &ctx->cogH);
            }
        }
    }
    else if (f->hover == HOVER_KBBUTTON) {
        ctx->kbButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->kbOpen = !ctx->kbOpen;
            if (ctx->kbOpen) {
                // Always comes up in lowercase, so the first key is where the
                // eye expects it however it was left last time
                ctx->kbState = KB_STATE_LOWER;
                ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
            }
            LOGI("keyboard %s", ctx->kbOpen ? "open" : "closed");
        }
    }
    else if (f->hover == HOVER_EXITBUTTON) {
        ctx->exitButtonHot = 1;
        if (ctx->triggerEdge[f->hand]) {
            ctx->exitConfirmOpen = 1;
            ctx->exitHoverZone = EXIT_ZONE_NONE;
            ctx->exitPose = exitPromptPose(ctx, &ctx->exitW, &ctx->exitH);
            // The press belonged to the button, not to the host behind it
            swallowTrigger(ctx, f->hand);
            LOGI("exit prompt open");
        }
    }
    else if (f->hover == HOVER_KBPANEL) {
        int key = kbKeyAt(ctx, f->hitU[f->hand], f->hitV[f->hand]);
        ctx->kbHoverKey = key;
        ctx->kbKeyDown = key >= 0 && ctx->triggerDown[f->hand];
        if (key >= 0 && ctx->triggerEdge[f->hand]) {
            int code = ctx->kbCodes[ctx->kbState][key];
            if (code == KB_CODE_SHIFT) {
                // Shift off the symbols page goes to the capitals rather than
                // back where it came from
                ctx->kbState = ctx->kbState == KB_STATE_UPPER
                        ? KB_STATE_LOWER : KB_STATE_UPPER;
            }
            else if (code == KB_CODE_SYMBOLS) {
                ctx->kbState = ctx->kbState == KB_STATE_SYMBOLS
                        ? KB_STATE_LOWER : KB_STATE_SYMBOLS;
            }
            else if (code == KB_CODE_HIDE) {
                ctx->kbOpen = 0;
                LOGI("keyboard closed");
            }
            else if (code > 0) {
                if (code == KB_CODE_CAPTURE && ctx->captureDir[0]) {
                    static unsigned int shot = 0;
                    snprintf(ctx->captureTag, sizeof(ctx->captureTag), "user_%02u", (++shot) % 10);
                    ctx->captureRequested = 1;
                    LOGI("ARX_CAPTURE requested %s", ctx->captureTag);
                }
                // One key a frame, which is as fast as anyone presses them
                f->out[IN_KEY] = (float)code;
                // Shift is one shot over the letters, the way a phone keyboard
                // behaves, and sticky over the punctuation row above them
                if (ctx->kbState == KB_STATE_UPPER && code >= 'A' && code <= 'Z') {
                    ctx->kbState = KB_STATE_LOWER;
                }
            }
        }
    }
    else if (f->hover == HOVER_LOCK) {
        ctx->lockHot = 1;
        if (ctx->triggerEdge[f->hand] && ctx->lockArmed[f->hand]) {
            ctx->lockArmed[f->hand] = 0;
            ctx->handsLocked = !ctx->handsLocked;
            LOGI("hands %s", ctx->handsLocked ? "locked" : "unlocked");
            if (ctx->handsLocked) {
                // Put the ray away and let go of anything held, so locking
                // mid drag does not leave the screen stuck to a hand or a
                // button down on the host
                ctx->pointerAwake = 0;
                ctx->buttonsDown = 0;
                ctx->stillFor = 0.0f;
                ctx->movingFor = 0.0f;
                if (ctx->grabMode != GRAB_NONE) {
                    ctx->grabMode = GRAB_NONE;
                    ctx->poseDirty = 1;
                    layoutSave(ctx);
                }
            }
        }
    }
}

// A press that lands on nothing at all puts the keyboard away, the same way
// one off the grid or the settings panel closes those. Only empty ground
// counts: a press on the picture is a mouse click and stays one, and the
// furniture and the keys themselves keep their own meanings. Every source
// is checked rather than the one doing the pointing, so a second hand can
// dismiss it while the first is still on the screen.
static void dismissKeyboard(XrCtx* ctx, InputFrame* f) {
    if (ctx->kbOpen && !ctx->pickerOpen && !ctx->cogOpen && !ctx->exitConfirmOpen) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (!f->aimValid[h] || !ctx->pointerAwake || !ctx->triggerEdge[h]) {
                continue;
            }
            // The halo is the invisible fringe around the picture, so it reads
            // as empty space too
            if (f->hovers[h] == HOVER_NONE || f->hovers[h] == HOVER_HALO) {
                ctx->kbOpen = 0;
                swallowTrigger(ctx, h);
                LOGI("keyboard closed");
                break;
            }
        }
    }
}

// Hands have no stick to click, so a pinch in empty air opens the pie menu
static void pinchOpensPie(XrCtx* ctx, InputFrame* f) {
    if (ctx->pieOpen || ctx->kbOpen || ctx->guideOpen || ctx->pickerOpen || ctx->cogOpen
            || ctx->exitConfirmOpen || ctx->grabMode != GRAB_NONE || !f->headValid) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->usingHands[h] && f->aimValid[h] && ctx->triggerEdge[h] && f->hovers[h] == HOVER_NONE) {
            swallowTrigger(ctx, h);
            openPie(ctx, f->headLoc.pose);
            break;
        }
    }
}

// One line that says whether gaze is tracking, whether it is the thing
// doing the pointing, and whether a pinch is reaching us at all. Logged
// only when it changes, so it costs nothing while it sits still.
static void logInputSnapshot(XrCtx* ctx, InputFrame* f) {
    int snapshot = (f->aimValid[SRC_GAZE] ? 1 : 0) | (f->hand == SRC_GAZE ? 2 : 0)
            | ((ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) ? 4 : 0)
            | (ctx->pointerAwake ? 8 : 0);
    if (snapshot != ctx->lastSnapshot) {
        ctx->lastSnapshot = snapshot;
        LOGI("input: gaze tracked %d, pointing by gaze %d, pinch %d, awake %d",
             (snapshot & 1) != 0, (snapshot & 2) != 0, (snapshot & 4) != 0,
             (snapshot & 8) != 0);
    }
}

// Nothing goes to the host mid drag, and the ray ends on the handle
// being held rather than wherever it is now pointing
static void beamToHandle(XrCtx* ctx, InputFrame* f) {
    ctx->buttonsDown = 0;
    ctx->scrollCarry = 0.0f;
    ctx->filterU.valid = 0;
    ctx->filterV.valid = 0;

    if (f->headValid) {
        Vec3 local;
        local.z = 0.0f;
        if (ctx->grabMode == GRAB_MOVE && ctx->grabOnPicture) {
            local.x = (ctx->grabU - 0.5f) * f->width;
            local.y = (0.5f - ctx->grabV) * f->height;
        }
        else if (ctx->grabMode == GRAB_MOVE) {
            local.x = 0.0f;
            local.y = -(f->height * 0.5f + (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f)
                        * f->width);
        }
        else {
            // The bracket being held sits a half bracket outside the
            // corner, so the ray has to end out there with it
            float side = f->width * CORNER_FRAC;
            local.x = (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * (f->width + side);
            local.y = (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * (f->height + side);
        }
        curveLocal(&local, f->radius, f->curved, NULL);
        Vec3 handle = quatRotate(f->screenPose.orientation, local);
        ctx->beamStart = f->aimPoses[ctx->grabHand].position;
        ctx->beamEnd.x = f->screenPose.position.x + handle.x;
        ctx->beamEnd.y = f->screenPose.position.y + handle.y;
        ctx->beamEnd.z = f->screenPose.position.z + handle.z;
        ctx->beamVisible = 1;
    }
}

// Ends the ray on the furniture under it, whichever plane that sits on
static void beamToFurniture(XrCtx* ctx, InputFrame* f) {
    Vec3 end = furniturePoint(ctx, f->hover, f->hitU[f->hand], f->hitV[f->hand],
                              f->screenPose, f->width, f->height, f->radius, f->curved);
    ctx->beamStart = f->aimPoses[f->hand].position;
    ctx->beamEnd.x = end.x;
    ctx->beamEnd.y = end.y;
    ctx->beamEnd.z = end.z;
    ctx->beamVisible = f->headValid;
}

// Holds the cursor still through a click. The press pins it where it happened; it stays
// pinned until the aim has moved more than CLICK_SLOP_RAD away, and only then follows,
// so a wobble while pulling the trigger clicks rather than drags. A second press soon
// after, close by, lands on the first one exactly.
static void holdClick(XrCtx* ctx, InputFrame* f, float* u, float* v) {
    int down = ctx->triggerDown[f->hand];
    float reachNow;
    {
        Vec3 at = screenPoint(*u, *v, f->screenPose, f->width, f->height, f->radius, f->curved);
        Vec3 from = { f->aimPoses[f->hand].position.x, f->aimPoses[f->hand].position.y, f->aimPoses[f->hand].position.z };
        Vec3 d = vecSub(at, from);
        reachNow = fmaxf(sqrtf(vecDot(d, d)), 0.3f);
    }
    if (down && !ctx->clickHeld) {
        ctx->clickHeld = 1;
        ctx->clickFree = 0;
        ctx->clickNs = f->now;
        ctx->clickScreen = f->active;
        ctx->clickReach = reachNow;
        ctx->clickU = *u;
        ctx->clickV = *v;
        // The second half of a double click goes where the first one went
        float du = (*u - ctx->lastClickU) * f->width, dv = (*v - ctx->lastClickV) * f->height;
        if (f->now - ctx->lastClickNs < DOUBLE_CLICK_NS
                && sqrtf(du * du + dv * dv) < CLICK_SLOP_RAD * reachNow) {
            ctx->clickU = ctx->lastClickU;
            ctx->clickV = ctx->lastClickV;
        }
    }
    if (!down) {
        if (ctx->clickHeld && !ctx->clickFree) {
            ctx->lastClickU = ctx->clickU;
            ctx->lastClickV = ctx->clickV;
            ctx->lastClickNs = f->now;
        }
        ctx->clickHeld = 0;
        return;
    }
    if (ctx->clickFree || ctx->clickScreen != f->active) return;
    float du = (*u - ctx->clickU) * f->width, dv = (*v - ctx->clickV) * f->height;
    float slop = f->now - ctx->clickNs > CLICK_HOLD_NS ? CLICK_SLOP_RAD : CLICK_SLOP_WIDE_RAD;
    if (sqrtf(du * du + dv * dv) > slop * ctx->clickReach) {
        ctx->clickFree = 1;
        return;
    }
    *u = ctx->clickU;
    *v = ctx->clickV;
}

// Smooths the hit point, hands it to Java and ends the ray on it
static void sendPointer(XrCtx* ctx, InputFrame* f, int hit) {
    if (!hit) {
        return;
    }
    // Filtering across a gap or a change of hands would slide the cursor
    // in from wherever it used to be
    if (f->hand != ctx->lastHand || f->active != ctx->lastScreen
            || f->now - ctx->lastHitNs > POINTER_RESET_NS) {
        ctx->filterU.valid = 0;
        ctx->filterV.valid = 0;
    }
    ctx->lastHand = f->hand;
    ctx->lastScreen = f->active;
    ctx->workScreen = f->active;
    ctx->lastHitNs = f->now;

    float u = euroFilter(&ctx->filterU, f->hitU[f->hand], f->dt, ctx->pointerMinCutoff,
                         ctx->pointerBeta);
    float v = euroFilter(&ctx->filterV, f->hitV[f->hand], f->dt, ctx->pointerMinCutoff,
                         ctx->pointerBeta);
    holdClick(ctx, f, &u, &v);
    f->out[IN_HIT] = 1.0f;
    f->out[IN_U] = u;
    f->out[IN_V] = v;
    f->out[IN_WORKSPACE] = (float)f->active;

    // The ray is only drawn when it lands on something, which is what
    // makes a laser readable rather than a light show
    Vec3 endPoint = screenPoint(u, v, f->screenPose, f->width, f->height, f->radius,
                                f->curved);
    ctx->beamStart = f->aimPoses[f->hand].position;
    ctx->beamEnd.x = endPoint.x;
    ctx->beamEnd.y = endPoint.y;
    ctx->beamEnd.z = endPoint.z;
    ctx->beamVisible = f->headValid;
}

// Works out which host buttons are down, and logs each trigger transition
static void updateButtons(XrCtx* ctx, InputFrame* f, int hit) {
    int mask = 0;
    for (int h = 0; h < HAND_COUNT; h++) {
        // Per hand rather than either hand, so a trigger spent on the grid or a
        // panel is out of the count while the other one still clicks
        if (ctx->triggerDown[h] && !ctx->triggerSwallowed[h]) {
            mask |= VR_BUTTON_LEFT;
        }
    }
    if (actionBool(ctx, ctx->rightClickAction, -1)
            || ctx->jointMiddlePinch[HAND_LEFT] || ctx->jointMiddlePinch[HAND_RIGHT]) {
        mask |= VR_BUTTON_RIGHT;
    }
    // A press only counts while aimed at the screen, but a release always
    // does, so walking the pointer off the edge mid drag still lets go
    ctx->buttonsDown = (ctx->buttonsDown & mask) | (hit ? mask : 0);
    f->out[IN_BUTTONS] = (float)ctx->buttonsDown;

    // Diagnostics for the click path, one line per transition, so an ordinary
    // click costs two. A release carries the low water mark of the analog
    // value and how many frames it spent under the press threshold, which is
    // what tells a genuine pair of taps from two that merged into one press.
    int rawNow = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
    int leftNow = (ctx->buttonsDown & VR_BUTTON_LEFT) ? 1 : 0;
    if (rawNow != ctx->trigLogRaw || leftNow != ctx->trigLogLeft
            || ctx->triggerDown[HAND_LEFT] != ctx->trigLogDown[HAND_LEFT]
            || ctx->triggerDown[HAND_RIGHT] != ctx->trigLogDown[HAND_RIGHT]) {
        char rel[80];
        rel[0] = '\0';
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->trigLogDown[h] && !ctx->triggerDown[h]) {
                snprintf(rel, sizeof(rel),
                         " up hand=%d holdMin=%.2f dipFrames=%d retaps=%d",
                         h, ctx->triggerHoldMin[h], ctx->triggerDipFrames[h],
                         ctx->triggerRetaps[h]);
            }
        }
        LOGI("trig: raw=%d%d val=%.2f,%.2f swal=%d%d edge=%d%d hit=%d awake=%d left=%d%s",
             ctx->triggerDown[HAND_LEFT], ctx->triggerDown[HAND_RIGHT],
             ctx->triggerValue[HAND_LEFT], ctx->triggerValue[HAND_RIGHT],
             ctx->triggerSwallowed[HAND_LEFT], ctx->triggerSwallowed[HAND_RIGHT],
             ctx->triggerEdge[HAND_LEFT], ctx->triggerEdge[HAND_RIGHT],
             hit ? 1 : 0, ctx->pointerAwake, leftNow, rel);

        ctx->trigLogRaw = rawNow;
        ctx->trigLogLeft = leftNow;
        ctx->trigLogDown[HAND_LEFT] = ctx->triggerDown[HAND_LEFT];
        ctx->trigLogDown[HAND_RIGHT] = ctx->triggerDown[HAND_RIGHT];
    }
}

// Winds the thumbstick into scroll clicks while the pointer is on the picture
// Metres the hand travels, little finger pinched to the thumb, for one scroll click
#define PINKY_STEP_M 0.012f

// Winds the thumbstick into scroll clicks while the pointer is on a desktop,
// up and down or sideways, whichever way it leans further. A hand scrolls by
// pinching the little finger to the thumb and moving up or down, like turning
// a wheel.
static void updateScroll(XrCtx* ctx, InputFrame* f, int hit) {
    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, -1);
    int vertical = fabsf(stick.y) >= fabsf(stick.x);
    float along = vertical ? stick.y : stick.x;
    if (hit && fabsf(along) > SCROLL_DEADZONE) {
        float past = (fabsf(along) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
        float* carry = vertical ? &ctx->scrollCarry : &ctx->hscrollCarry;
        *carry += copysignf(past * SCROLL_CLICKS_PER_SEC * f->dt, along);
    }
    else if (!hit) {
        ctx->scrollCarry = 0.0f;
        ctx->hscrollCarry = 0.0f;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!hit || !ctx->jointPinkyPinch[h]) { ctx->pinkyAnchored[h] = 0; continue; }
        if (!ctx->pinkyAnchored[h]) { ctx->pinkyAnchorY[h] = ctx->thumbY[h]; ctx->pinkyAnchored[h] = 1; continue; }
        float steps = truncf((ctx->thumbY[h] - ctx->pinkyAnchorY[h]) / PINKY_STEP_M);
        ctx->pinkyAnchorY[h] += steps * PINKY_STEP_M;
        ctx->scrollCarry += steps;
    }
    float clicks = truncf(ctx->scrollCarry);
    ctx->scrollCarry -= clicks;
    f->out[IN_SCROLL] = clicks;
    float sideways = truncf(ctx->hscrollCarry);
    ctx->hscrollCarry -= sideways;
    f->out[IN_HSCROLL] = sideways;
}

// Aimed at nothing at all, so the ray runs off into the room rather than
// blinking out. A laser that comes and goes is harder to aim than one that
// always shows where the hand is looking, so the only thing that retires it
// is the controller being put down.
static void beamIntoRoom(XrCtx* ctx, InputFrame* f) {
    if (!ctx->beamVisible && ctx->pointerAwake && f->headValid && !ctx->beamGaze) {
        int free = f->hand;
        if (free < 0) {
            free = f->aimValid[HAND_RIGHT] ? HAND_RIGHT
                    : (f->aimValid[HAND_LEFT] ? HAND_LEFT : -1);
        }
        if (free >= 0) {
            Vec3 forward = { 0.0f, 0.0f, -1.0f };
            Vec3 d = quatRotate(f->aimPoses[free].orientation, forward);
            ctx->beamStart = f->aimPoses[free].position;
            ctx->beamEnd.x = ctx->beamStart.x + d.x * FREE_BEAM_M;
            ctx->beamEnd.y = ctx->beamStart.y + d.y * FREE_BEAM_M;
            ctx->beamEnd.z = ctx->beamStart.z + d.z * FREE_BEAM_M;
            ctx->beamVisible = 1;
            // No target, so no cursor. The dot is what says a click would
            // land somewhere.
            ctx->beamFree = 1;
        }
    }
}

// Reads the controllers and works out where they are pointing on the screen.
// Java turns the result into host mouse events, so nothing here knows about
// the connection.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUpdateInput(JNIEnv* env, jobject thiz,
                                                              jlong handle, jfloat distance,
                                                              jfloat quadWidth, jfloat curvature,
                                                              jboolean headLocked,
                                                              jboolean pointerEnabled,
                                                              jboolean gazeEnabled,
                                                              jfloatArray outArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    float out[IN_SLOTS];
    memset(out, 0, sizeof(out));
    if (ctx != NULL) {
        ctx->gazeEnabled = gazeEnabled;
        ctx->prefCurvature = curvature;
    }
    // Zero is a real cell, so "nothing picked" has to be said explicitly. Every
    // early return below would otherwise read as a press on the first one. Same
    // for the setting id, and nothing clears it again once a row has written
    // one, so the early returns carry it out too.
    out[IN_PICKER_PICK] = -1.0f;
    out[IN_SETTING] = -1.0f;
    // Backspace is 8, so a zeroed slot would type one every frame
    out[IN_KEY] = -1.0f;

    // Anything held has to come back up when pointing stops, or the host is
    // left with a stuck button
    if (ctx == NULL || !ctx->inputReady || !pointerEnabled || !ctx->placementValid
            || ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        if (ctx != NULL) {
            releaseInput(ctx, out);
        }
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    XrActiveActionSet active;
    active.actionSet = ctx->actionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(ctx->session, &sync))) {
        ctx->buttonsDown = 0;
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    // A hidden thumbstick toggle must never leave a workspace silently view-only.
    ctx->pointerOn = 1;
    out[IN_POINTER] = 1.0f;

    InputFrame f;
    memset(&f, 0, sizeof(f));
    f.out = out;
    // Both of these have to agree with what endFrame submits, or the ray lands
    // somewhere other than where the picture is drawn. A room world locks it
    // and flattens it whatever the preference and the panel say.
    // Only a room that hangs the picture takes over placement; a world leaves it alone
    f.roomOn = roomHangsPicture(ctx);
    f.space = (headLocked && !f.roomOn) ? ctx->viewSpace : ctx->localSpace;
    f.height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    f.curved = !f.roomOn && effectiveCurvature(ctx) > 0.01f && ctx->cylinderSupported;
    f.radius = ctx->screenRadius;
    f.width = ctx->screenWidth;
    f.screenPose = ctx->screenPose;
    // Hit tests read this before the frame's layers are built
    if (ctx->dockReady) {
        ctx->dockPose = dockPanelPose(ctx, &ctx->dockW, &ctx->dockH);
    }

    f.now = nowNs();
    f.dt = ctx->lastInputNs != 0 ? (f.now - ctx->lastInputNs) / 1e9f : 0.0f;
    ctx->lastInputNs = f.now;
    if (f.dt > 0.1f) {
        f.dt = 0.1f;
    }

    f.headLoc.type = XR_TYPE_SPACE_LOCATION;
    f.headValid = XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, f.space,
                                             ctx->predictedDisplayTime, &f.headLoc))
            && (f.headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (f.headValid) {
        ctx->headPos = f.headLoc.pose.position;
        ctx->headKnown = 1;
        // A desktop that arrived before there was anywhere to put it goes up now
        for (int i = 0; i < ARX_MAX_EXTRA; i++) {
            if (ctx->desktops[i].visible && !ctx->desktops[i].placed && ctx->placementValid) {
                placeOrRestoreDesktop(ctx, i + 1);
            }
        }
    }

    readSources(ctx, &f);
    applyHandLock(ctx, &f);
    updatePointerWake(ctx, &f);
    pickPointingSource(ctx, &f);

    // Buttons that mean the same wherever the pointer is. Dictation goes out as
    // a level, since it can be held; the other two act on the press. Only a
    // button starts the microphone: a hand at rest brushes ring finger to thumb
    // far too easily, and a gesture that did it typed whatever the room said.
    out[IN_VOICE] = actionBool(ctx, ctx->voiceAction, -1) ? 1.0f : 0.0f;
    // The thumbstick click opens the pie menu, and clicks it shut again, choosing
    // whatever slice is lit
    int stickClick = actionBool(ctx, ctx->keyboardAction, -1);
    if (stickClick && !ctx->keyboardPrev && !ctx->guideOpen && !ctx->pickerOpen && !ctx->cogOpen) {
        if (ctx->pieOpen) pieChoose(ctx, &f, ctx->pieHover);
        else if (f.headValid) openPie(ctx, f.headLoc.pose);
    }
    ctx->keyboardPrev = stickClick;
    // The left menu button brings the desktops round to where you are looking
    int menu = actionBool(ctx, ctx->menuAction, -1);
    if (menu && !ctx->menuPrev && f.headValid && ctx->grabMode == GRAB_NONE) {
        recentreDesktops(ctx, f.headLoc.pose);
    }
    ctx->menuPrev = menu;
    // The desktop being worked on: the one held by a grab, else the one under
    // the pointing hand. Everything below measures against it.
    int desk = ctx->grabMode != GRAB_NONE ? ctx->grabTarget
            : (f.hand >= 0 ? f.screens[f.hand] : 0);
    if (!screenLive(ctx, desk)) desk = 0;
    useScreen(ctx, &f, desk);
    clearHotState(ctx);
    if (ctx->guideOpen) {
        updateGuide(ctx, &f);
    }
    else if (ctx->pieOpen) {
        updatePie(ctx, &f);
    }
    else if (ctx->pickerOpen) {
        updatePicker(ctx, &f);
    }
    else if (ctx->cogOpen) {
        updateCogPanel(ctx, &f);
    }
    else if (ctx->exitConfirmOpen) {
        updateExitPrompt(ctx, &f);
    }
    else {
        updateFurniture(ctx, &f);
    }
    pinchOpensPie(ctx, &f);
    dismissKeyboard(ctx, &f);
    logInputSnapshot(ctx, &f);

    // Where the handle is clear of the picture, so a trigger press there cannot
    // have been meant as a click
    int offPicture = f.hand >= 0 && (f.hitU[f.hand] < 0.0f || f.hitU[f.hand] > 1.0f
                                     || f.hitV[f.hand] < 0.0f || f.hitV[f.hand] > 1.0f);
    applyGrab(ctx, f.aimPoses, f.aimValid, f.hand, f.hover, ctx->hoverCorner, offPicture,
              desk, f.curved, f.dt);
    grabKeyboard(ctx, &f);
    if (ctx->grabMode != GRAB_NONE) desk = ctx->workScreen = ctx->grabTarget;
    useScreen(ctx, &f, desk);
    ctx->hoverScreen = desk;

    // The round button past an extra desktop's bar closes it
    if (f.hover == HOVER_CLOSE && f.hand >= 0 && ctx->triggerEdge[f.hand] && desk > 0) {
        out[IN_WORKSPACE] = (float)desk;
        out[IN_WORKSPACE_ACTION] = 1.0f;
        swallowTrigger(ctx, f.hand);
        LOGI("desktop %d close pressed", desk + 1);
    }

    // A handle stays lit while it is being dragged, however far the ray has
    // wandered from it in the meantime
    if (ctx->grabMode == GRAB_MOVE) {
        ctx->hoverKind = HOVER_BAR;
    }
    else if (ctx->grabMode == GRAB_RESIZE) {
        ctx->hoverKind = HOVER_CORNER;
    }
    else {
        ctx->hoverKind = f.hover;
    }

    ctx->screenOrientation = f.screenPose.orientation;
    ctx->beamVisible = 0;
    ctx->beamFree = 0;
    // Eyes aim by looking, so a ray out of the face would be nonsense, and a
    // cursor riding on them shakes too much to be anything but a distraction.
    // Gaze draws nothing: the handle lighting up is the feedback.
    ctx->beamGaze = (ctx->grabMode != GRAB_NONE ? ctx->grabHand : f.hand) == SRC_GAZE;

    if (ctx->grabMode != GRAB_NONE) {
        beamToHandle(ctx, &f);
        writeInputPose(ctx, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    if (!ctx->pointerOn) {
        // The ray still shows on the handles and the grid, so the screen can
        // be tidied and the environment changed with the mouse switched off
        if (f.hand >= 0 && f.hover != HOVER_NONE && f.hover != HOVER_SCREEN) {
            beamToFurniture(ctx, &f);
        }
        ctx->buttonsDown = 0;
        writeInputPose(ctx, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    // The bar, the button and the picker all sit off the picture, so pointing
    // at them must not drag the host cursor to the edge
    int hit = (f.hover == HOVER_SCREEN || f.hover == HOVER_CORNER) && f.hand != SRC_GAZE
            && !ctx->guideOpen;
    if ((f.hover == HOVER_BAR || f.hover == HOVER_ENVBUTTON || f.hover == HOVER_PICKER
            || f.hover == HOVER_LOCK || f.hover == HOVER_HALO || f.hover == HOVER_COGBUTTON
            || f.hover == HOVER_COGPANEL || f.hover == HOVER_KBBUTTON
            || f.hover == HOVER_KBPANEL || f.hover == HOVER_EXITBUTTON
            || f.hover == HOVER_EXITPROMPT || f.hover == HOVER_DOCK || f.hover == HOVER_CLOSE
            || f.hover == HOVER_PIE) && f.headValid && f.hand >= 0) {
        beamToFurniture(ctx, &f);
    }
    sendPointer(ctx, &f, hit);
    updateButtons(ctx, &f, hit);
    updateScroll(ctx, &f, hit);
    beamIntoRoom(ctx, &f);

    writeInputPose(ctx, out);
    out[IN_PICKER_PICK] = (float)ctx->pickerPick;
    out[IN_PIE] = ctx->pieOpen ? (float)ctx->pieHover : -2.0f;
    (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
}

// Puts back a placement saved from a previous session. Marking the sliders as
// already seen stops the first frame taking the screen straight back off it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetScreenPose(JNIEnv* env, jobject thiz,
                                                                jlong handle, jfloatArray poseArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || poseArr == NULL) {
        return;
    }
    float p[POSE_VALUES];
    if ((*env)->GetArrayLength(env, poseArr) < POSE_VALUES) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, poseArr, 0, POSE_VALUES, p);

    if (p[7] < SCREEN_MIN_WIDTH || p[7] > SCREEN_MAX_WIDTH || p[8] <= 0.0f) {
        LOGW("stored screen placement out of range, ignoring it");
        return;
    }
    // Anything below zero means the panel never set a curve, and anything else
    // out of range is not worth trusting either
    ctx->panelCurve = (p[9] >= 0.0f && p[9] <= 1.0f) ? p[9] : -1.0f;

    ctx->screenPose.position.x = p[0];
    ctx->screenPose.position.y = p[1];
    ctx->screenPose.position.z = p[2];
    ctx->screenPose.orientation.x = p[3];
    ctx->screenPose.orientation.y = p[4];
    ctx->screenPose.orientation.z = p[5];
    ctx->screenPose.orientation.w = p[6];
    ctx->screenPose.orientation = quatNorm(ctx->screenPose.orientation);
    ctx->screenWidth = p[7];
    ctx->screenRadius = p[8];
    ctx->placementValid = 1;
    ctx->sliderSeen = 0;
    LOGI("restored screen placement %.2f %.2f %.2f, %.2f m wide",
         p[0], p[1], p[2], p[7]);
}
