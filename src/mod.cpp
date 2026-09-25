#include "global.h"

#include "JSystem/J3DGraphBase/J3DShape.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "dolphin/gx/GXGet.h"
#include "dolphin/gx/GXPixel.h"
#include "dolphin/gx/GXTransform.h"
#include "m_Do/m_Do_mtx.h"
#include "m_Do/m_Do_graphic.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"
#include "mods/svc/window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <webgpu/webgpu.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(WindowService, svc_window);

namespace {

constexpr uint32_t kRenderWidth = 640;
constexpr uint32_t kRenderHeight = 360;
constexpr float kTargetDistance = 100.0f;
constexpr float kLookSensitivity = 0.004f;
constexpr float kFastMultiplier = 4.0f;
// The native bloom density is tuned for the game's Twilight presentation. The
// same values are overly bright in ordinary areas, so keep Twilight unchanged
// and use a softer scale everywhere else in Camera 2's presentation pass.
constexpr float kAreaBloomStrengthScale = 0.45f;
constexpr const char* kUpdateRateOptions[] = {
    "Full speed",
    "30 FPS",
    "20 FPS",
    "15 FPS",
};

// WindowService is append-only, but the latest Dusklight main branch still publishes the
// 1.0 header while newer hosts append input and always-on-top functions. Keep the compatibility
// view local so this package can be compiled against 1.0 and take advantage of newer hosts at
// runtime without importing newer fields from an older SDK header.
using WindowCreateFn = decltype(((WindowService*)nullptr)->create_window);
using WindowDestroyFn = decltype(((WindowService*)nullptr)->destroy_window);
using WindowShowFn = decltype(((WindowService*)nullptr)->show_window);
using WindowHideFn = decltype(((WindowService*)nullptr)->hide_window);
using WindowSetTitleFn = decltype(((WindowService*)nullptr)->set_title);
using WindowSetSizeFn = decltype(((WindowService*)nullptr)->set_size);
using WindowGetInfoFn = decltype(((WindowService*)nullptr)->get_info);
using WindowSetRelativeMouseModeFn = ModResult (*)(
    ModContext* ctx, WindowHandle window, bool enabled);
using WindowSetAlwaysOnTopFn = ModResult (*)(
    ModContext* ctx, WindowHandle window, bool enabled);

struct WindowServiceCompatibilityView {
    ServiceHeader header;
    WindowCreateFn create_window;
    WindowDestroyFn destroy_window;
    WindowShowFn show_window;
    WindowHideFn hide_window;
    WindowSetTitleFn set_title;
    WindowSetSizeFn set_size;
    WindowGetInfoFn get_info;
    WindowSetRelativeMouseModeFn set_relative_mouse_mode;
    WindowSetAlwaysOnTopFn set_always_on_top;
};

struct WindowEventCompatibilityView {
    uint32_t struct_size;
    WindowEventType type;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_width;
    uint32_t pixel_height;
    float display_scale;
    int32_t keycode;
    int32_t scancode;
    uint32_t mouse_button;
    float mouse_x;
    float mouse_y;
    float mouse_delta_x;
    float mouse_delta_y;
    bool repeat;
};

constexpr uint16_t kWindowInputMinor = 1;
constexpr uint16_t kWindowAlwaysOnTopMinor = 2;
constexpr auto kWindowEventKeyDown = static_cast<WindowEventType>(7);
constexpr auto kWindowEventKeyUp = static_cast<WindowEventType>(8);
constexpr auto kWindowEventMouseMotion = static_cast<WindowEventType>(9);
constexpr auto kWindowEventMouseButtonDown = static_cast<WindowEventType>(10);

const WindowServiceCompatibilityView* windowServiceCompatibility() {
    return reinterpret_cast<const WindowServiceCompatibilityView*>(svc_window);
}

bool windowServiceSupports(uint16_t minor, size_t memberEnd) {
    const auto* service = windowServiceCompatibility();
    return service != nullptr && service->header.minor_version >= minor &&
        service->header.struct_size >= memberEnd;
}

bool windowServiceHasInputEvents() {
    return windowServiceSupports(kWindowInputMinor,
        offsetof(WindowServiceCompatibilityView, set_relative_mouse_mode) +
            sizeof(WindowSetRelativeMouseModeFn));
}

// SDL scancodes use the USB HID keyboard-page values. Keeping the handful used here local lets
// this mod consume WindowService input without depending on SDL headers or linking SDL itself.
constexpr int32_t kScancodeA = 4;
constexpr int32_t kScancodeD = 7;
constexpr int32_t kScancodeS = 22;
constexpr int32_t kScancodeW = 26;
constexpr int32_t kScancodeEscape = 41;
constexpr int32_t kScancodeSpace = 44;
constexpr int32_t kScancodeLeftCtrl = 224;
constexpr int32_t kScancodeLeftShift = 225;

ConfigVarHandle g_controls = 0;
ConfigVarHandle g_moveSpeed = 0;
ConfigVarHandle g_fov = 0;
ConfigVarHandle g_alwaysOnTop = 0;
ConfigVarHandle g_updateRate = 0;
ConfigVarHandle g_shoulderFollow = 0;
ConfigVarHandle g_shoulderDistance = 0;
ConfigVarHandle g_shoulderSideOffset = 0;
ConfigVarHandle g_shoulderHeight = 0;
ConfigVarHandle g_shoulderAimHeight = 0;
ConfigVarHandle g_shoulderAngle = 0;

WindowHandle g_window = 0;
UiWindowHandle g_settingsWindow = 0;
UiWindowHandle g_shoulderSettingsWindow = 0;
UiMenuTabHandle g_menuTab = 0;
GfxPresentTargetHandle g_presentTarget = 0;
GfxStageHookHandle g_sceneBeginHook = 0;
GfxStageHookHandle g_frameBeforeHudHook = 0;
WGPURenderPipeline g_presentPipeline = nullptr;
WGPUBindGroupLayout g_presentLayout = nullptr;
WGPUTextureFormat g_presentFormat = WGPUTextureFormat_Undefined;
WGPUBuffer g_bloomParamsBuffer = nullptr;

bool g_resetViewRequested = false;
bool g_windowFocused = false;
bool g_mouseCaptured = false;
bool g_windowAlwaysOnTopApplied = false;
bool g_hasRenderedFrame = false;
#if defined(_WIN32)
bool g_windowsCursorCaptured = false;
bool g_windowsEscapeDown = false;
#endif
uint32_t g_stablePlayerModelFrames = 0;
using CameraClock = std::chrono::steady_clock;
CameraClock::time_point g_nextCameraRender;

struct InputState {
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool fast = false;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
};

InputState g_input;

struct CameraState {
    bool initialized = false;
    cXyz eye;
    cXyz center;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovy = 60.0f;
    s16 bank = 0;
};

CameraState g_camera;

bool canRefreshPlayerModelsForCurrentView(const daAlink_c* player) {
    if (player == nullptr) {
        return false;
    }

    // Item-get events (including the rupee slide) keep Link's normal model resources
    // alive and still need a Camera 2 view calculation. Only skip the refresh for
    // procedures that replace or tear down those resources while the auxiliary
    // camera is being rendered.
    switch (player->mProcID) {
    case daAlink_c::PROC_WARP:
    case daAlink_c::PROC_DUNGEON_WARP_READY:
    case daAlink_c::PROC_DUNGEON_WARP:
    case daAlink_c::PROC_DUNGEON_WARP_SCN_START:
    case daAlink_c::PROC_METAMORPHOSE:
    case daAlink_c::PROC_METAMORPHOSE_ONLY:
    case daAlink_c::PROC_TW_GATE:
        return false;
    default:
        break;
    }

    const int roomNo = dComIfGp_roomControl_getStayNo();
    if (roomNo < 0 || dComIfGp_roomControl_checkStatusFlag(roomNo, 0x02 | 0x04 | 0x20)) {
        return false;
    }

    return true;
}

// Keep this model refresh local to the mod so the standalone repository only
// depends on the public Dusklight game headers. It intentionally mirrors the
// stable-model list used by the in-tree build and excludes transition-sensitive
// demo models.
void refreshPlayerModelsForCurrentView(daAlink_c* player, bool includeEquipment) {
    if (player == nullptr) {
        return;
    }

    J3DModel* models[] = {
        player->mpLinkModel, player->mpLinkFaceModel, player->mpLinkHatModel,
        player->mpLinkHandModel,
    };
    for (J3DModel* model : models) {
        if (model != nullptr) {
            model->viewCalc();
        }
    }

    if (includeEquipment) {
        J3DModel* equipmentModels[] = {
            player->mSwordModel, player->mSheathModel, player->mShieldModel,
        };
        for (J3DModel* model : equipmentModels) {
            if (model != nullptr) {
                model->viewCalc();
            }
        }
    }
}

struct PresentPayload {
    WGPUTextureView color;
};

struct BloomParams {
    float threshold;
    float strength;
    float padding[2];
    float blendColor[4];
};

constexpr const char* kPresentShader = R"(
@group(0) @binding(0) var source_color: texture_2d<f32>;
struct BloomParams {
    threshold: f32,
    strength: f32,
    padding0: f32,
    padding1: f32,
    blend_color: vec4f,
};
@group(0) @binding(2) var<uniform> bloom_params: BloomParams;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    var out: VertexOutput;
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

fn bloomSample(coord: vec2<i32>, size: vec2<i32>) -> vec3f {
    let sampleCoord = clamp(coord, vec2<i32>(0i), size - 1i);
    let color = textureLoad(source_color, sampleCoord, 0i).rgb;
    let peak = max(max(color.r, color.g), color.b);
    let amount = max((peak - bloom_params.threshold) /
        max(1.0 - bloom_params.threshold, 0.001), 0.0);
    let tint = max(bloom_params.blend_color.rgb, vec3f(0.18));
    return color * amount * tint;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let size = vec2<i32>(textureDimensions(source_color));
    let texel = clamp(vec2<i32>(in.uv * vec2f(size)), vec2<i32>(0i), size - 1i);
    let base = textureLoad(source_color, texel, 0i).rgb;
    let glow =
        bloomSample(texel + vec2<i32>(-2i, -2i), size) * 0.06
        + bloomSample(texel + vec2<i32>( 0i, -2i), size) * 0.10
        + bloomSample(texel + vec2<i32>( 2i, -2i), size) * 0.06
        + bloomSample(texel + vec2<i32>(-2i,  0i), size) * 0.10
        + bloomSample(texel, size) * 0.16
        + bloomSample(texel + vec2<i32>( 2i,  0i), size) * 0.10
        + bloomSample(texel + vec2<i32>(-2i,  2i), size) * 0.06
        + bloomSample(texel + vec2<i32>( 0i,  2i), size) * 0.10
        + bloomSample(texel + vec2<i32>( 2i,  2i), size) * 0.06
        + bloomSample(texel + vec2<i32>(-8i,  0i), size) * 0.08
        + bloomSample(texel + vec2<i32>( 8i,  0i), size) * 0.08
        + bloomSample(texel + vec2<i32>( 0i, -8i), size) * 0.08
        + bloomSample(texel + vec2<i32>( 0i,  8i), size) * 0.08
        + bloomSample(texel + vec2<i32>(-20i, 0i), size) * 0.04
        + bloomSample(texel + vec2<i32>( 20i, 0i), size) * 0.04
        + bloomSample(texel + vec2<i32>(0i, -20i), size) * 0.04
        + bloomSample(texel + vec2<i32>(0i,  20i), size) * 0.04;
    return vec4f(min(base + glow * bloom_params.strength, vec3f(1.0)), 1.0);
}
)";

float getFov();
float getShoulderDistance();
float getShoulderSideOffset();
float getShoulderHeight();
float getShoulderAimHeight();
float getShoulderAngle();

void updateAngles() {
    const cXyz direction = g_camera.center - g_camera.eye;
    g_camera.yaw = std::atan2(direction.z, direction.x);
    const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    g_camera.pitch = std::atan2(direction.y, horizontal);
}

void updateCenter() {
    const float horizontal = std::cos(g_camera.pitch);
    g_camera.center.x =
        g_camera.eye.x + std::cos(g_camera.yaw) * horizontal * kTargetDistance;
    g_camera.center.y = g_camera.eye.y + std::sin(g_camera.pitch) * kTargetDistance;
    g_camera.center.z =
        g_camera.eye.z + std::sin(g_camera.yaw) * horizontal * kTargetDistance;
}

void updateShoulderCamera(const daAlink_c* player) {
    if (player == nullptr || !canRefreshPlayerModelsForCurrentView(player)) {
        return;
    }

    // Twilight uses sin(angle) for X and cos(angle) for Z. Zero orbit angle is
    // directly behind Link; the configurable angle orbits around him.
    constexpr float kGameAngleToRadians = 3.14159265358979323846f / 32768.0f;
    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    const float orbitAngle = static_cast<float>(player->shape_angle.y) *
            kGameAngleToRadians + getShoulderAngle() * kDegreesToRadians;
    const cXyz forward(std::sin(orbitAngle), 0.0f, std::cos(orbitAngle));
    const cXyz right(std::cos(orbitAngle), 0.0f, -std::sin(orbitAngle));
    const cXyz anchor = player->current.pos;

    g_camera.eye = anchor - (forward * getShoulderDistance()) +
        (right * getShoulderSideOffset()) + cXyz(0.0f, getShoulderHeight(), 0.0f);
    g_camera.center = anchor + cXyz(0.0f, getShoulderAimHeight(), 0.0f);
    g_camera.fovy = getFov();
    g_camera.bank = 0;
    g_camera.initialized = true;
    updateAngles();
}

bool resetFreeCamera() {
    daAlink_c* player = daAlink_getAlinkActorClass();
    if (player == nullptr) {
        return false;
    }

    // Link is used only as a one-time spawn anchor. Start close enough to make
    // the relationship obvious, then leave Camera 2 completely independent.
    g_camera.center = player->current.pos + cXyz(0.0f, 70.0f, 0.0f);
    g_camera.eye = player->current.pos + cXyz(0.0f, 140.0f, 180.0f);
    g_camera.fovy = getFov();
    g_camera.bank = 0;
    g_camera.initialized = true;
    updateAngles();
    return true;
}

bool getControlsEnabled() {
    bool value = false;
    svc_config->get_bool(mod_ctx, g_controls, &value);
    return value;
}

float getMoveSpeed() {
    int64_t value = 900;
    svc_config->get_int(mod_ctx, g_moveSpeed, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 1, 10000));
}

float getFov() {
    int64_t value = 60;
    svc_config->get_int(mod_ctx, g_fov, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 1, 179));
}

bool getShoulderFollow() {
    bool value = false;
    svc_config->get_bool(mod_ctx, g_shoulderFollow, &value);
    return value;
}

float getShoulderDistance() {
    int64_t value = 260;
    svc_config->get_int(mod_ctx, g_shoulderDistance, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 50, 1000));
}

float getShoulderSideOffset() {
    int64_t value = 70;
    svc_config->get_int(mod_ctx, g_shoulderSideOffset, &value);
    return static_cast<float>(std::clamp<int64_t>(value, -300, 300));
}

float getShoulderHeight() {
    int64_t value = 110;
    svc_config->get_int(mod_ctx, g_shoulderHeight, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 0, 600));
}

float getShoulderAimHeight() {
    int64_t value = 70;
    svc_config->get_int(mod_ctx, g_shoulderAimHeight, &value);
    return static_cast<float>(std::clamp<int64_t>(value, 0, 400));
}

float getShoulderAngle() {
    int64_t value = 0;
    svc_config->get_int(mod_ctx, g_shoulderAngle, &value);
    return static_cast<float>(std::clamp<int64_t>(value, -180, 180));
}

CameraClock::duration getCameraRenderPeriod() {
    int64_t value = 0;
    svc_config->get_int(mod_ctx, g_updateRate, &value);
    switch (std::clamp<int64_t>(value, 0, 3)) {
    case 1:
        return std::chrono::duration_cast<CameraClock::duration>(
            std::chrono::duration<double>(1.0 / 30.0));
    case 2:
        return std::chrono::duration_cast<CameraClock::duration>(
            std::chrono::duration<double>(1.0 / 20.0));
    case 3:
        return std::chrono::duration_cast<CameraClock::duration>(
            std::chrono::duration<double>(1.0 / 15.0));
    default:
        return CameraClock::duration::zero();
    }
}

bool cameraRenderDue() {
    const CameraClock::duration period = getCameraRenderPeriod();
    if (period == CameraClock::duration::zero()) {
        return true;
    }

    const CameraClock::time_point now = CameraClock::now();
    return g_nextCameraRender.time_since_epoch().count() == 0 ||
        now >= g_nextCameraRender;
}

void updateControls(float deltaSeconds) {
    if (!getControlsEnabled() || !g_camera.initialized || !g_windowFocused ||
        getShoulderFollow()) {
        g_input.mouseDeltaX = 0.0f;
        g_input.mouseDeltaY = 0.0f;
        return;
    }

    float forward = (g_input.forward ? 1.0f : 0.0f) -
                    (g_input.backward ? 1.0f : 0.0f);
    float right = (g_input.right ? 1.0f : 0.0f) - (g_input.left ? 1.0f : 0.0f);
    float up = (g_input.up ? 1.0f : 0.0f) - (g_input.down ? 1.0f : 0.0f);
    const float lookX = g_input.mouseDeltaX;
    const float lookY = g_input.mouseDeltaY;
    const bool fast = g_input.fast;

    forward = std::clamp(forward, -1.0f, 1.0f);
    right = std::clamp(right, -1.0f, 1.0f);
    const float length = std::sqrt(forward * forward + right * right + up * up);
    if (length > 1.0f) {
        forward /= length;
        right /= length;
        up /= length;
    }

    const float speed = getMoveSpeed() * std::max(deltaSeconds, 0.0f) *
                        (fast ? kFastMultiplier : 1.0f);
    g_camera.eye.x +=
        (forward * std::cos(g_camera.yaw) - right * std::sin(g_camera.yaw)) * speed;
    g_camera.eye.y += up * speed;
    g_camera.eye.z +=
        (forward * std::sin(g_camera.yaw) + right * std::cos(g_camera.yaw)) * speed;
    g_camera.yaw += lookX * kLookSensitivity;
    g_camera.pitch -= lookY * kLookSensitivity;
    g_camera.pitch = std::clamp(g_camera.pitch, -1.553343f, 1.553343f);
    updateCenter();
    g_input.mouseDeltaX = 0.0f;
    g_input.mouseDeltaY = 0.0f;
}

bool drawListsReady() {
    return dComIfGd_getOpaListBG() != nullptr && dComIfGd_getOpaList() != nullptr &&
           dComIfGd_getOpaListDark() != nullptr && dComIfGd_getXluListBG() != nullptr &&
           dComIfGd_getListPacket() != nullptr;
}

void drawSceneLists() {
    dComIfGd_drawOpaListSky();
    dComIfGd_drawXluListSky();
    dComIfGd_drawOpaListBG();
    dComIfGd_drawOpaListDarkBG();
    dComIfGd_drawOpaListMiddle();
    dComIfGd_drawOpaList();
    dComIfGd_drawOpaListDark();
    dComIfGd_drawOpaListPacket();
    dComIfGd_drawXluListBG();
    dComIfGd_drawXluListDarkBG();
    dComIfGd_drawXluList();
    dComIfGd_drawXluListDark();
    dComIfGd_drawXluListZxlu();
}

void restoreGameRenderState(const Mtx savedView, const f32 savedProjection[7],
    const f32 savedViewport[6], const u32 savedScissor[4]) {
    j3dSys.setViewMtx(savedView);
    GXSetProjectionv(savedProjection);
    GXSetViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3],
        savedViewport[4], savedViewport[5]);
    GXSetScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);
    dKy_setLight();
}

void renderCamera2() {
    if (g_presentTarget == 0 || !g_camera.initialized || !drawListsReady()) {
        return;
    }
    if (g_hasRenderedFrame && !cameraRenderDue()) {
        return;
    }

    f32 savedProjection[7];
    GXGetProjectionv(savedProjection);
    f32 savedViewport[6];
    GXGetViewportv(savedViewport);
    u32 savedScissor[4];
    GXGetScissor(&savedScissor[0], &savedScissor[1], &savedScissor[2], &savedScissor[3]);
    Mtx savedView;
    cMtx_copy(j3dSys.getViewMtx(), savedView);

    if (svc_gfx->create_pass(mod_ctx, kRenderWidth, kRenderHeight) != MOD_OK) {
        return;
    }

    daAlink_c* player = daAlink_getAlinkActorClass();
    if (getShoulderFollow()) {
        updateShoulderCamera(player);
    }

    Mtx cameraView;
    Mtx44 cameraProjection;
    cXyz up(0.0f, 1.0f, 0.0f);
    cMtx_lookAt(cameraView, &g_camera.eye, &g_camera.center, &up, g_camera.bank);
    C_MTXPerspective(cameraProjection, getFov(),
        static_cast<float>(kRenderWidth) / static_cast<float>(kRenderHeight), 1.0f, 100000.0f);

    const bool refreshPlayerModels = canRefreshPlayerModelsForCurrentView(player);
    if (refreshPlayerModels) {
        g_stablePlayerModelFrames = std::min(g_stablePlayerModelFrames + 1u, 8u);
    } else {
        g_stablePlayerModelFrames = 0;
    }
    const bool refreshEquipment = refreshPlayerModels && g_stablePlayerModelFrames >= 3;

    j3dSys.setViewMtx(cameraView);
    if (refreshPlayerModels && player != nullptr) {
        refreshPlayerModelsForCurrentView(player, refreshEquipment);
    }
    GXSetProjectionFull(cameraProjection);
    GXSetViewport(0.0f, 0.0f, static_cast<float>(kRenderWidth),
        static_cast<float>(kRenderHeight), 0.0f, 1.0f);
    GXSetViewportRender(0.0f, 0.0f, static_cast<float>(kRenderWidth),
        static_cast<float>(kRenderHeight), 0.0f, 1.0f);
    GXSetScissorRender(0, 0, kRenderWidth, kRenderHeight);
    dKy_setLight();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    J3DShape::resetVcdVatCache();
    drawSceneLists();
    j3dSys.setViewMtx(savedView);
    if (daAlink_c* restoredPlayer = daAlink_getAlinkActorClass();
        refreshPlayerModels && restoredPlayer == player) {
        // The actor can enter an event/transition while this pass is drawing. Do not
        // re-run the safety filter here: the pass already refreshed this exact actor,
        // and leaving its packets on Camera 2's view is what pins Link in front of
        // the second camera during rupee slides.
        refreshPlayerModelsForCurrentView(restoredPlayer, refreshEquipment);
    }
    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restoreGameRenderState(savedView, savedProjection, savedViewport, savedScissor);

    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.depth = false;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr) {
        restoreGameRenderState(savedView, savedProjection, savedViewport, savedScissor);
        return;
    }

    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restoreGameRenderState(savedView, savedProjection, savedViewport, savedScissor);

    const PresentPayload payload{.color = resolved.color};
    if (svc_gfx->push_present(mod_ctx, g_presentTarget, &payload, sizeof(payload)) == MOD_OK) {
        g_hasRenderedFrame = true;
        const CameraClock::duration period = getCameraRenderPeriod();
        g_nextCameraRender = period == CameraClock::duration::zero()
            ? CameraClock::time_point{}
            : CameraClock::now() + period;
    }
}

void releasePresentPipeline() {
    if (g_presentPipeline != nullptr) {
        wgpuRenderPipelineRelease(g_presentPipeline);
        g_presentPipeline = nullptr;
    }
    if (g_presentLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_presentLayout);
        g_presentLayout = nullptr;
    }
    if (g_bloomParamsBuffer != nullptr) {
        wgpuBufferRelease(g_bloomParamsBuffer);
        g_bloomParamsBuffer = nullptr;
    }
    g_presentFormat = WGPUTextureFormat_Undefined;
}

bool ensurePresentPipeline(const GfxPresentContext& ctx) {
    if (g_presentPipeline != nullptr && g_presentLayout != nullptr &&
        g_bloomParamsBuffer != nullptr && g_presentFormat == ctx.target_format) {
        return true;
    }
    releasePresentPipeline();

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {kPresentShader, WGPU_STRLEN};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {"camera 2 present", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(ctx.device, &moduleDesc);
    if (module == nullptr) {
        return false;
    }

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = ctx.target_format;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {"camera 2 present", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.fragment = &fragment;
    g_presentPipeline = wgpuDeviceCreateRenderPipeline(ctx.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_presentPipeline == nullptr) {
        return false;
    }
    g_presentLayout = wgpuRenderPipelineGetBindGroupLayout(g_presentPipeline, 0);
    if (g_presentLayout == nullptr) {
        releasePresentPipeline();
        return false;
    }
    WGPUBufferDescriptor paramsDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDesc.label = {"camera 2 Twilight bloom parameters", WGPU_STRLEN};
    paramsDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDesc.size = sizeof(BloomParams);
    g_bloomParamsBuffer = wgpuDeviceCreateBuffer(ctx.device, &paramsDesc);
    if (g_bloomParamsBuffer == nullptr) {
        releasePresentPipeline();
        return false;
    }
    g_presentFormat = ctx.target_format;
    return true;
}

void onPresent(ModContext*, const GfxPresentContext* ctx, const void* payload,
    size_t payloadSize, void*) {
    WGPUBindGroup bindGroup = nullptr;
    if (payloadSize == sizeof(PresentPayload) && ensurePresentPipeline(*ctx)) {
        PresentPayload data;
        std::memcpy(&data, payload, sizeof(data));
        if (data.color != nullptr) {
            auto* nativeBloom = mDoGph_gInf_c::getBloom();
            const GXColor blendColor = *nativeBloom->getBlendColor();
            const float threshold = std::clamp(
                static_cast<float>(nativeBloom->getPoint()) / 255.0f, 0.08f, 0.45f);
            const float density = static_cast<float>(nativeBloom->getBlureRatio()) / 255.0f;
            const bool twilightVisuals = dKy_darkworld_check() != 0;
            const float strengthScale = twilightVisuals ? 1.0f : kAreaBloomStrengthScale;
            const float strength = nativeBloom->getEnable() != 0
                ? (0.9f + density * 3.6f) * strengthScale
                : 0.0f;
            const BloomParams params{
                .threshold = threshold,
                .strength = strength,
                .padding = {0.0f, 0.0f},
                .blendColor = {
                    static_cast<float>(blendColor.r) / 255.0f,
                    static_cast<float>(blendColor.g) / 255.0f,
                    static_cast<float>(blendColor.b) / 255.0f,
                    static_cast<float>(blendColor.a) / 255.0f,
                },
            };
            wgpuQueueWriteBuffer(ctx->queue, g_bloomParamsBuffer, 0, &params,
                sizeof(params));

            WGPUBindGroupEntry entries[2] = {
                WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
            entries[0].binding = 0;
            entries[0].textureView = data.color;
            entries[1].binding = 2;
            entries[1].buffer = g_bloomParamsBuffer;
            entries[1].size = sizeof(BloomParams);
            WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bindGroupDesc.layout = g_presentLayout;
            bindGroupDesc.entryCount = 2;
            bindGroupDesc.entries = entries;
            bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
        }
    }

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = ctx->target_view;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = WGPUColor{0.02, 0.02, 0.025, 1.0};
    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.label = {"camera 2 present", WGPU_STRLEN};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(ctx->encoder, &passDesc);
    if (bindGroup != nullptr) {
        wgpuRenderPassEncoderSetPipeline(pass, g_presentPipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuBindGroupRelease(bindGroup);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

ModResult closeWindow() {
    if (g_window != 0 && g_mouseCaptured) {
        const auto* service = windowServiceCompatibility();
        if (windowServiceSupports(kWindowInputMinor,
                offsetof(WindowServiceCompatibilityView, set_relative_mouse_mode) +
                    sizeof(WindowSetRelativeMouseModeFn)) &&
            service->set_relative_mouse_mode != nullptr) {
            service->set_relative_mouse_mode(mod_ctx, g_window, false);
        }
        g_mouseCaptured = false;
    }
    if (g_presentTarget != 0) {
        const ModResult result = svc_gfx->unregister_present_target(mod_ctx, g_presentTarget);
        if (result != MOD_OK) {
            return result;
        }
        g_presentTarget = 0;
    }
    if (g_window != 0) {
        const ModResult result = svc_window->destroy_window(mod_ctx, g_window);
        if (result != MOD_OK) {
            return result;
        }
        g_window = 0;
    }
    g_windowFocused = false;
    g_windowAlwaysOnTopApplied = false;
    g_hasRenderedFrame = false;
#if defined(_WIN32)
    g_windowsCursorCaptured = false;
    g_windowsEscapeDown = false;
#endif
    g_nextCameraRender = CameraClock::time_point{};
    g_input = {};
    return MOD_OK;
}

void setKeyState(const int32_t scancode, const bool down) {
    switch (scancode) {
    case kScancodeW:
        g_input.forward = down;
        break;
    case kScancodeS:
        g_input.backward = down;
        break;
    case kScancodeA:
        g_input.left = down;
        break;
    case kScancodeD:
        g_input.right = down;
        break;
    case kScancodeSpace:
        g_input.up = down;
        break;
    case kScancodeLeftCtrl:
        g_input.down = down;
        break;
    case kScancodeLeftShift:
        g_input.fast = down;
        break;
    default:
        break;
    }
}

void activateFreeCameraControls() {
    g_windowFocused = true;
    svc_config->set_bool(mod_ctx, g_controls, true);
}

void onWindowEvent(ModContext*, WindowHandle, const WindowEvent* event, void*) {
    if (event == nullptr) {
        return;
    }

    if (event->type == WINDOW_EVENT_CLOSE_REQUESTED) {
        closeWindow();
    } else if (event->type == WINDOW_EVENT_FOCUS_GAINED) {
        activateFreeCameraControls();
    } else if (event->type == WINDOW_EVENT_FOCUS_LOST) {
        g_windowFocused = false;
        g_input = {};

    } else if (windowServiceHasInputEvents() &&
               event->struct_size >= offsetof(WindowEventCompatibilityView, keycode) +
                   sizeof(int32_t)) {
        const auto* inputEvent = reinterpret_cast<const WindowEventCompatibilityView*>(event);
        if (inputEvent->type == kWindowEventKeyDown) {
            if (inputEvent->scancode == kScancodeEscape) {
                svc_config->set_bool(mod_ctx, g_controls, false);
                g_input = {};
            } else {
                setKeyState(inputEvent->scancode, true);
            }
        } else if (inputEvent->type == kWindowEventKeyUp) {
            setKeyState(inputEvent->scancode, false);
        } else if (inputEvent->type == kWindowEventMouseButtonDown) {
            // A click should recapture controls even if Escape released them while this
            // window remained focused (which does not generate another focus event).
            activateFreeCameraControls();
        } else if (inputEvent->type == kWindowEventMouseMotion && g_mouseCaptured &&
                   event->struct_size >= offsetof(WindowEventCompatibilityView, repeat) +
                       sizeof(bool)) {
            g_input.mouseDeltaX += inputEvent->mouse_delta_x;
            g_input.mouseDeltaY += inputEvent->mouse_delta_y;
        }
    }
}

#if defined(_WIN32)
bool windowsKeyDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

void pollWindowsInput() {
    if (windowServiceHasInputEvents() || g_window == 0 || !g_windowFocused ||
        !g_mouseCaptured || !getControlsEnabled()) {
        g_windowsCursorCaptured = false;
        g_windowsEscapeDown = false;
        return;
    }

    const bool escapeDown = windowsKeyDown(VK_ESCAPE);
    if (escapeDown && !g_windowsEscapeDown) {
        svc_config->set_bool(mod_ctx, g_controls, false);
        g_input = {};
    }
    g_windowsEscapeDown = escapeDown;
    if (!getControlsEnabled()) {
        g_windowsCursorCaptured = false;
        return;
    }

    setKeyState(kScancodeW, windowsKeyDown('W'));
    setKeyState(kScancodeA, windowsKeyDown('A'));
    setKeyState(kScancodeS, windowsKeyDown('S'));
    setKeyState(kScancodeD, windowsKeyDown('D'));
    setKeyState(kScancodeSpace, windowsKeyDown(VK_SPACE));
    setKeyState(kScancodeLeftCtrl, windowsKeyDown(VK_LCONTROL));
    setKeyState(kScancodeLeftShift, windowsKeyDown(VK_LSHIFT));

    WindowInfo info = WINDOW_INFO_INIT;
    if (svc_window->get_info(mod_ctx, g_window, &info) != MOD_OK || info.width == 0 ||
        info.height == 0) {
        g_windowsCursorCaptured = false;
        return;
    }

    POINT center{
        static_cast<LONG>(info.x + static_cast<int32_t>(info.width / 2)),
        static_cast<LONG>(info.y + static_cast<int32_t>(info.height / 2)),
    };
    if (!g_windowsCursorCaptured) {
        SetCursorPos(center.x, center.y);
        g_windowsCursorCaptured = true;
        return;
    }

    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        g_input.mouseDeltaX += static_cast<float>(cursor.x - center.x);
        g_input.mouseDeltaY += static_cast<float>(cursor.y - center.y);
        if (cursor.x != center.x || cursor.y != center.y) {
            SetCursorPos(center.x, center.y);
        }
    }
}
#else
void pollWindowsInput() {}
#endif

void syncMouseCapture() {
    if (g_window == 0) {
        g_mouseCaptured = false;
        return;
    }

    const auto* service = windowServiceCompatibility();
#if defined(_WIN32)
    if (!windowServiceHasInputEvents() || service->set_relative_mouse_mode == nullptr) {
        WindowInfo info = WINDOW_INFO_INIT;
        if (svc_window->get_info(mod_ctx, g_window, &info) == MOD_OK) {
            g_windowFocused = info.focused;
        }
        g_mouseCaptured = g_windowFocused && getControlsEnabled();
        return;
    }
#else
    if (!windowServiceHasInputEvents() || service->set_relative_mouse_mode == nullptr) {
        g_mouseCaptured = false;
        return;
    }
#endif

    const bool wanted = g_windowFocused && getControlsEnabled();
    if (wanted == g_mouseCaptured) {
        return;
    }
    if (service->set_relative_mouse_mode(mod_ctx, g_window, wanted) == MOD_OK) {
        g_mouseCaptured = wanted;
    }
}

void syncAlwaysOnTop() {
    if (g_window == 0) {
        return;
    }

    const auto* service = windowServiceCompatibility();
    if (!windowServiceSupports(kWindowAlwaysOnTopMinor,
            offsetof(WindowServiceCompatibilityView, set_always_on_top) +
                sizeof(WindowSetAlwaysOnTopFn)) ||
        service->set_always_on_top == nullptr) {
        return;
    }

    bool wanted = false;
    svc_config->get_bool(mod_ctx, g_alwaysOnTop, &wanted);
    if (wanted == g_windowAlwaysOnTopApplied) {
        return;
    }

    if (service->set_always_on_top(mod_ctx, g_window, wanted) == MOD_OK) {
        g_windowAlwaysOnTopApplied = wanted;
    }
}

ModResult openWindow() {
    if (g_window != 0) {
        return MOD_CONFLICT;
    }

    // Each newly enabled Camera 2 session starts near the current gameplay Link
    // instead of reusing a stale free-camera position from a previous window.
    g_resetViewRequested = true;
    g_hasRenderedFrame = false;
    g_nextCameraRender = CameraClock::time_point{};

    WindowDesc windowDesc = WINDOW_DESC_INIT;
    // Keep the title version-neutral; the loader's mod list is the authoritative version
    // indicator, so this cannot become stale when the package is updated.
    windowDesc.title = "Freecam+";
    windowDesc.width = kRenderWidth;
    windowDesc.height = kRenderHeight;
    bool alwaysOnTop = false;
    svc_config->get_bool(mod_ctx, g_alwaysOnTop, &alwaysOnTop);
    if (alwaysOnTop) {
        windowDesc.flags |= WINDOW_FLAG_ALWAYS_ON_TOP;
    }
    g_windowAlwaysOnTopApplied = alwaysOnTop;
    windowDesc.on_event = onWindowEvent;
    ModResult result = svc_window->create_window(mod_ctx, &windowDesc, &g_window);
    if (result != MOD_OK) {
        return result;
    }

    GfxPresentTargetDesc presentDesc = GFX_PRESENT_TARGET_DESC_INIT;
    presentDesc.label = "Freecam+ surface";
    presentDesc.render = onPresent;
    result = svc_gfx->register_window_present_target(
        mod_ctx, g_window, &presentDesc, &g_presentTarget);
    if (result != MOD_OK) {
        closeWindow();
        return result;
    }
    result = svc_window->show_window(mod_ctx, g_window);
    if (result != MOD_OK) {
        closeWindow();
    }
    return result;
}

void onToggleWindow(ModContext*, void*) {
    const ModResult result = g_window == 0 ? openWindow() : closeWindow();
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, g_window == 0 ? "failed to open Freecam+ window" :
                                                  "failed to close Freecam+ window");
    }
}

void onResetView(ModContext*, void*) {
    g_resetViewRequested = true;
}

void onSceneBegin(ModContext*, const GfxStageContext* stageCtx, void*) {
    if (stageCtx == nullptr || stageCtx->game_view == nullptr) {
        return;
    }
    if (!g_camera.initialized || g_resetViewRequested) {
        if (resetFreeCamera()) {
            g_resetViewRequested = false;
        }
    }
}

void onFrameBeforeHud(ModContext*, const GfxStageContext*, void*) {
    renderCamera2();
}

void addToggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

void addNumber(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

void addSelect(UiElementHandle pane, const char* label, ConfigVarHandle cvar,
    const char* const* options, size_t optionCount, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.options = options;
    control.option_count = optionCount;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

ModResult buildShoulderSettingsTab(ModContext*, UiWindowHandle, UiElementHandle leftPane,
    UiElementHandle, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, leftPane, "Over-the-Shoulder Camera");
    addNumber(leftPane, "Shoulder Distance", g_shoulderDistance, 50, 1000, 10, " units",
        "Distance behind Link in over-the-shoulder mode.");
    addNumber(leftPane, "Shoulder Side Offset", g_shoulderSideOffset, -300, 300, 10, " units",
        "Moves the camera left or right relative to Link. Positive values use Link's right shoulder.");
    addNumber(leftPane, "Shoulder Height", g_shoulderHeight, 0, 600, 10, " units",
        "Height of the over-the-shoulder camera above Link's position.");
    addNumber(leftPane, "Shoulder Aim Height", g_shoulderAimHeight, 0, 400, 10, " units",
        "Height on Link that Camera 2 looks toward.");
    addNumber(leftPane, "Shoulder Orbit Angle", g_shoulderAngle, -180, 180, 5, " degrees",
        "Orbits the camera around Link. Zero is behind him; 180 is in front.");
    return MOD_OK;
}

void onShoulderSettingsWindowClosed(ModContext*, UiWindowHandle, void*) {
    g_shoulderSettingsWindow = 0;
}

void onOpenShoulderSettings(ModContext*, void*) {
    if (g_shoulderSettingsWindow != 0) {
        return;
    }

    UiTabDesc tab = UI_TAB_DESC_INIT;
    tab.title = "Over-the-Shoulder";
    tab.build = buildShoulderSettingsTab;

    UiWindowDesc window = UI_WINDOW_DESC_INIT;
    window.tabs = &tab;
    window.tab_count = 1;
    window.on_closed = onShoulderSettingsWindowClosed;
    if (svc_ui->window_push(mod_ctx, &window, &g_shoulderSettingsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open over-the-shoulder settings");
    }
}

ModResult buildCameraControls(UiElementHandle pane) {
    svc_ui->pane_add_section(mod_ctx, pane, "Freecam+ Window");
    UiControlDesc windowControl = UI_CONTROL_DESC_INIT;
    windowControl.kind = UI_CONTROL_BUTTON;
    windowControl.label = "Open / Close Freecam+";
    windowControl.on_pressed = onToggleWindow;
    svc_ui->pane_add_control(mod_ctx, pane, &windowControl, nullptr);

    UiControlDesc resetControl = UI_CONTROL_DESC_INIT;
    resetControl.kind = UI_CONTROL_BUTTON;
    resetControl.label = "Reset Freecam+";
    resetControl.help_rml =
        "Spawns Freecam+ just above and behind Link once; it remains independent afterward.";
    resetControl.on_pressed = onResetView;
    svc_ui->pane_add_control(mod_ctx, pane, &resetControl, nullptr);
    addToggle(pane, "Control Freecam+", g_controls,
        "Freecam+ is an independent free camera. Click its window to capture input. WASD moves, "
        "mouse looks, Space/Ctrl move vertically, Shift speeds up, and Escape releases the mouse.");
    addNumber(pane, "Move Speed", g_moveSpeed, 1, 10000, 50, nullptr,
        "Freecam+ movement speed in world units per second.");
    addNumber(pane, "Field of View", g_fov, 1, 179, 1, " degrees",
        "Freecam+ vertical field of view.");
    addSelect(pane, "Camera 2 Update Rate", g_updateRate, kUpdateRateOptions,
        std::size(kUpdateRateOptions),
        "Controls only how often Freecam+ captures a new Camera 2 scene. Lower rates reduce "
        "performance impact while leaving the main game render rate unchanged.");
    addToggle(pane, "Always on Top", g_alwaysOnTop,
        "Keeps the Freecam+ window above other windows and updates while it is open.");
    addToggle(pane, "Over-the-Shoulder Follow", g_shoulderFollow,
        "Makes Camera 2 follow Link's world position and facing direction without changing the main camera.");

    UiControlDesc shoulderSettingsControl = UI_CONTROL_DESC_INIT;
    shoulderSettingsControl.kind = UI_CONTROL_BUTTON;
    shoulderSettingsControl.label = "Open Over-the-Shoulder Settings";
    shoulderSettingsControl.help_rml =
        "Open the detailed follow-camera settings in a separate submenu.";
    shoulderSettingsControl.on_pressed = onOpenShoulderSettings;
    svc_ui->pane_add_control(mod_ctx, pane, &shoulderSettingsControl, nullptr);
    return MOD_OK;
}

ModResult buildPanel(ModContext*, UiElementHandle panel, void*, ModError*) {
    return buildCameraControls(panel);
}

ModResult buildSettingsTab(ModContext*, UiWindowHandle, UiElementHandle leftPane,
    UiElementHandle, void*, ModError*) {
    return buildCameraControls(leftPane);
}

void onSettingsWindowClosed(ModContext*, UiWindowHandle, void*) {
    g_settingsWindow = 0;
}

void onCameraMenuSelected(ModContext*, void*) {
    if (g_settingsWindow != 0) {
        return;
    }

    UiTabDesc tab = UI_TAB_DESC_INIT;
    tab.title = "Freecam+";
    tab.build = buildSettingsTab;

    UiWindowDesc window = UI_WINDOW_DESC_INIT;
    window.tabs = &tab;
    window.tab_count = 1;
    window.on_closed = onSettingsWindowClosed;
    if (svc_ui->window_push(mod_ctx, &window, &g_settingsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open Freecam+ settings");
    }
}

ModResult registerBool(const char* name, bool defaultValue, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Freecam+ option");
    }
    return MOD_OK;
}

ModResult registerInt(const char* name, int64_t defaultValue, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Freecam+ option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = registerBool("controlsEnabled", false, g_controls, error);
    if (result != MOD_OK) return result;
    result = registerInt("moveSpeed", 900, g_moveSpeed, error);
    if (result != MOD_OK) return result;
    result = registerInt("fov", 60, g_fov, error);
    if (result != MOD_OK) return result;
    result = registerInt("updateRate", 0, g_updateRate, error);
    if (result != MOD_OK) return result;
    result = registerBool("alwaysOnTop", false, g_alwaysOnTop, error);
    if (result != MOD_OK) return result;
    result = registerBool("shoulderFollow", false, g_shoulderFollow, error);
    if (result != MOD_OK) return result;
    result = registerInt("shoulderDistance", 260, g_shoulderDistance, error);
    if (result != MOD_OK) return result;
    result = registerInt("shoulderSideOffset", 70, g_shoulderSideOffset, error);
    if (result != MOD_OK) return result;
    result = registerInt("shoulderHeight", 110, g_shoulderHeight, error);
    if (result != MOD_OK) return result;
    result = registerInt("shoulderAimHeight", 70, g_shoulderAimHeight, error);
    if (result != MOD_OK) return result;
    result = registerInt("shoulderAngle", 0, g_shoulderAngle, error);
    if (result != MOD_OK) return result;

    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = onSceneBegin;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_BEGIN, &stageDesc, &g_sceneBeginHook) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Freecam+ scene hook");
    }
    stageDesc.callback = onFrameBeforeHud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &stageDesc, &g_frameBeforeHudHook) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Freecam+ frame hook");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = buildPanel;
    if (svc_ui->register_mods_panel(mod_ctx, &panelDesc) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Freecam+ controls");
    }

    UiMenuTabDesc menuDesc = UI_MENU_TAB_DESC_INIT;
    menuDesc.label = "Freecam+";
    menuDesc.on_selected = onCameraMenuSelected;
    if (svc_ui->register_menu_tab(mod_ctx, &menuDesc, &g_menuTab) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Freecam+ menu tab");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    syncAlwaysOnTop();
    syncMouseCapture();
    pollWindowsInput();
    updateControls(1.0f / 60.0f);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (g_menuTab != 0) {
        svc_ui->unregister_menu_tab(mod_ctx, g_menuTab);
        g_menuTab = 0;
    }
    if (g_settingsWindow != 0) {
        svc_ui->window_close(mod_ctx, g_settingsWindow);
        g_settingsWindow = 0;
    }
    if (g_shoulderSettingsWindow != 0) {
        svc_ui->window_close(mod_ctx, g_shoulderSettingsWindow);
        g_shoulderSettingsWindow = 0;
    }
    if (g_sceneBeginHook != 0) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_sceneBeginHook);
        g_sceneBeginHook = 0;
    }
    if (g_frameBeforeHudHook != 0) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_frameBeforeHudHook);
        g_frameBeforeHudHook = 0;
    }
    closeWindow();
    releasePresentPipeline();
    g_controls = g_moveSpeed = g_fov = g_updateRate = g_alwaysOnTop = 0;
    g_shoulderFollow = g_shoulderDistance = g_shoulderSideOffset = 0;
    g_shoulderHeight = g_shoulderAimHeight = g_shoulderAngle = 0;
    return MOD_OK;
}

}
