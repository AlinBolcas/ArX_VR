// A Gaussian splat world, drawn into the room's own image.
//
// The file arrives from Java on a loader thread and is packed there (xr_splat_data.c).
// The frame loop uploads it as one integer texture, and a sort thread keeps an order
// of the splats in front of the head by distance, furthest first. Only a cone around
// where the head points is kept, wide enough to cover the eyes plus a turn while the
// next order is made, so what is behind you is neither sorted nor drawn. Moving a
// centimetre or turning ten degrees asks for a new order.
//
// A view of the world costs more than one frame can spare on this headset, so each is
// built over several: a slice of the order a frame, into an image of its own, sized by
// how much of the view the slice covers. When the last slice is in, the view goes to
// the room's swapchain with the poses it was drawn from, and the compositor turns it
// with the head at the display's own rate until the next one lands.
//
// Under that sharp view sits a cube of the same splats round each eye, smaller and
// refreshed more slowly, which the compositor also turns at the display's rate. The
// view is drawn a little wider than the eyes and its rim fades into the cube, so a
// quick turn past the view's edge finds the same world, softer, never black.
#include "xr_renderer.h"
#include "xr_splat_data.h"
#include "xr_splat_shaders.h"

// Frames land on the display's beat, about 13.9 ms, or miss one and take twice that.
// A frame is sliced by how much of the view it covers, not by how many splats: the far
// ones are specks and the last ones, round you, fill the eye. A missed frame cuts the
// budget and remembers where the ceiling is; every frame on time grows it back.
#define SPLAT_FRAME_MISSED_NS 20000000L
#define SPLAT_BUDGET_START 20.0f
#define SPLAT_BUDGET_MIN 0.2f
// The view is built at this share of the room image's size and scaled into it
#define SPLAT_BUILD_SCALE 1.0f
// Each view is drawn this much wider than the eyes, so a head turning while the next
// view is built sees sharp world a little past the edge. Every degree of it spreads the
// same pixels wider, so it is kept small: past it the cube carries on.
#define SPLAT_OVERSCAN_H_RAD 0.28f   // 16 degrees: turning sideways is the quick one
#define SPLAT_OVERSCAN_V_RAD 0.12f   // 7 degrees
// The strokes take this outer share of the margin; inside it the view is whole
#define SPLAT_RIM 0.7f
// How much of each frame's budget goes to the cube underneath; the view gets the rest
#define SPLAT_CUBE_SHARE 0.25f
#define SPLAT_FOV_MAX_RAD 1.40f      // 80 degrees

// Room for the face lists: most splats land on one or two faces, big and near ones on more
#define SPLAT_FACE_SLOTS 4
// The cube is re-sorted at most this often; the view's own order is sorted every time
#define SPLAT_CUBE_RESORT_NS 1500000000L

// How far the head moves, or turns, before a new order is worth asking for
#define SPLAT_RESORT_M 0.01f
#define SPLAT_RESORT_COS 0.985f   // 10 degrees
// Half angle of the cone that is kept: a Quest 2 eye reaches about 55 degrees off
// centre, and the rest is room for the head to turn before the next order lands
#define SPLAT_CONE_COS 0.1736f    // 80 degrees

struct SplatWorld {
    // The frame loop's side
    GLuint program;
    GLuint texture;
    GLuint orderBuffer;
    GLuint vao;
    GLint viewUniform, projUniform, focalUniform, viewportUniform, rim0Uniform, rim1Uniform, boundUniform;
    int count;
    int drawCount;
    int ordered;
    float askedEye[3];
    float askedForward[3];
    int asked;
    // The view being built: its poses, how much of its order is in, and the image
    GLuint buildFbo, buildTexture, presentProgram;
    int targetWidth, targetHeight;
    int building, drawn, viewCount, viewFrames, slice, viewsLogged;
    XrView viewViews[ROOM_EYES];
    long viewStartNs, lastFrameNs;
    // The view's running draw cost along its order, and how much of it a frame gets
    float* viewCost;
    float budget, ceiling;

    // Shared with the sort thread, under lock
    pthread_t thread;
    int threadRunning;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int quit;
    int want;
    float wantEye[3];
    float wantForward[3];
    int fresh, freshCube;
    uint32_t* result;
    float* resultCost;
    int resultCount;
    int resultViewCount;
    int workViewCount;
    // The cube's face lists, sorted far less often, in buffers of their own
    uint32_t* cubeWork;
    uint32_t* cubeResult;
    float* cubeWorkCost;
    float* cubeResultCost;
    int cubeWorkCount, cubeResultCount;
    long cubeSortedNs;

    // The sort thread's own
    float* centers;
    float* radii;
    uint32_t* work;
    float* workCost;
    uint32_t* picked;
    uint32_t* keys;
    uint32_t* counts;
    // Cube mode: the whole sphere is sorted, then split by face into work
    int cubeMode;
    uint32_t* sorted;
    int workOffsets[SPLAT_FACES + 1];
    int resultOffsets[SPLAT_FACES + 1];

    // The cube round each eye: faces are built one at a time in a scratch image,
    // copied into a cube that always holds a whole world, and that cube into the
    // swapchain the layer shows
    int cubeSize;
    GLuint buildCube[2], scratch[2], cubeFbo;
    XrSwapchainImageOpenGLESKHR* cubeImages[2];
    uint32_t cubeImageCount[2];
    int faceBuilt[SPLAT_FACES];
    long faceNs[SPLAT_FACES];
    int face, faceDrawn, faceCount;
    long roundStartNs;
    float faceEye[2][3];
    int viewOffsets[SPLAT_FACES + 1];
    GLuint faceBuffer;
    float* faceCost;
    int cubeOrdered;
    // The cube's own progress, apart from the view's
    int cubeBuilding, cubeFrames;
    long cubeStartNs;
};

// Java hands a parsed world over here; the frame loop takes it
static pthread_mutex_t pendingLock = PTHREAD_MUTEX_INITIALIZER;

static void* sortLoop(void* arg) {
    struct SplatWorld* w = arg;
    int logged = 0;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->want && !w->quit) pthread_cond_wait(&w->wake, &w->lock);
        if (w->quit) break;
        float eye[3] = { w->wantEye[0], w->wantEye[1], w->wantEye[2] };
        float forward[3] = { w->wantForward[0], w->wantForward[1], w->wantForward[2] };
        w->want = 0;
        pthread_mutex_unlock(&w->lock);

        long start = nowNs();
        // The view's order: only the cone in front, which is quick
        int kept = splatSortByDistance(w->centers, w->count, eye, forward, SPLAT_CONE_COS,
                                       w->work, w->picked, w->keys, w->counts);
        splatOrderCost(w->centers, w->radii, w->work, kept, eye, w->workCost);
        w->workViewCount = kept;
        // The cube's: everything, split by face, now and then; it is only the backdrop
        int cubeDue = w->cubeMode && (w->cubeSortedNs == 0 || nowNs() - w->cubeSortedNs > SPLAT_CUBE_RESORT_NS);
        if (cubeDue) {
            int all = splatSortByDistance(w->centers, w->count, eye, forward, -1.0f,
                                          w->sorted, w->picked, w->keys, w->counts);
            w->cubeWorkCount = splatSplitFaces(w->centers, w->radii, w->sorted, all, eye, w->cubeWork,
                                               w->count * SPLAT_FACE_SLOTS, w->workOffsets);
            for (int f = 0; f < SPLAT_FACES; f++) {
                int from = w->workOffsets[f];
                splatOrderCost(w->centers, w->radii, w->cubeWork + from, w->workOffsets[f + 1] - from, eye,
                               w->cubeWorkCost + from);
            }
            w->cubeSortedNs = nowNs();
        }
        if (logged < 5) {
            logged++;
            LOGI("splat sort: %d of %d in view, %.1f ms", kept, w->count, (nowNs() - start) / 1e6);
        }

        pthread_mutex_lock(&w->lock);
        uint32_t* done = w->work;
        w->work = w->result;
        w->result = done;
        float* doneCost = w->workCost;
        w->workCost = w->resultCost;
        w->resultCost = doneCost;
        w->resultCount = kept;
        w->resultViewCount = w->workViewCount;
        w->fresh = 1;
        if (cubeDue) {
            uint32_t* doneCube = w->cubeWork;
            w->cubeWork = w->cubeResult;
            w->cubeResult = doneCube;
            float* doneCubeCost = w->cubeWorkCost;
            w->cubeWorkCost = w->cubeResultCost;
            w->cubeResultCost = doneCubeCost;
            w->cubeResultCount = w->cubeWorkCount;
            memcpy(w->resultOffsets, w->workOffsets, sizeof(w->workOffsets));
            w->freshCube = 1;
        }
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static void stopSorter(struct SplatWorld* w) {
    if (w->threadRunning) {
        pthread_mutex_lock(&w->lock);
        w->quit = 1;
        pthread_cond_signal(&w->wake);
        pthread_mutex_unlock(&w->lock);
        pthread_join(w->thread, NULL);
        w->threadRunning = 0;
        w->quit = 0;
    }
    w->want = 0;
    w->fresh = 0;
    w->freshCube = 0;
    free(w->faceCost);
    free(w->cubeWork);
    free(w->cubeResult);
    free(w->cubeWorkCost);
    free(w->cubeResultCost);
    w->faceCost = w->cubeWorkCost = w->cubeResultCost = NULL;
    w->cubeWork = w->cubeResult = NULL;
    w->cubeSortedNs = 0;
    free(w->centers);
    free(w->radii);
    free(w->work);
    free(w->result);
    free(w->workCost);
    free(w->resultCost);
    free(w->viewCost);
    free(w->picked);
    free(w->keys);
    free(w->counts);
    free(w->sorted);
    w->centers = w->radii = w->workCost = w->resultCost = w->viewCost = NULL;
    w->work = w->result = w->picked = w->keys = w->counts = w->sorted = NULL;
}

static int linkSplat(struct SplatWorld* w) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, SPLAT_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, SPLAT_FRAGMENT_SRC);
    if (!vs || !fs) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("splat program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    w->program = program;
    w->viewUniform = glGetUniformLocation(program, "u_view");
    w->projUniform = glGetUniformLocation(program, "u_proj");
    w->focalUniform = glGetUniformLocation(program, "u_focal");
    w->viewportUniform = glGetUniformLocation(program, "u_viewport");
    w->boundUniform = glGetUniformLocation(program, "u_bound");
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_splats"), 0);
    return 1;
}

// A new world onto the GPU and a sorter started for it; the last one's go
static void adoptCloud(XrCtx* ctx, SplatCloud* cloud) {
    struct SplatWorld* w = ctx->splat;
    stopSorter(w);
    w->count = 0;
    w->drawCount = 0;
    w->ordered = 0;
    w->cubeOrdered = 0;
    w->cubeBuilding = 0;
    w->building = 0;
    w->asked = 0;
    w->budget = SPLAT_BUDGET_START;
    w->ceiling = 1e9f;

    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (cloud->texHeight > maxSize) {
        LOGE("splat world of %d is too tall for a %d texture", cloud->count, maxSize);
        splatFree(cloud);
        return;
    }
    if (w->texture == 0) glGenTextures(1, &w->texture);
    glBindTexture(GL_TEXTURE_2D, w->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, SPLAT_TEX_WIDTH, cloud->texHeight, 0,
                 GL_RGBA_INTEGER, GL_UNSIGNED_INT, cloud->texels);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (w->orderBuffer == 0) glGenBuffers(1, &w->orderBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, w->orderBuffer);
    w->cubeMode = ctx->cubeSupported;
    size_t slots = (size_t)cloud->count;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)slots * 4, NULL, GL_DYNAMIC_DRAW);
    if (w->vao == 0) glGenVertexArrays(1, &w->vao);
    glBindVertexArray(w->vao);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 4, (const void*)0);
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // The sorter keeps the centres and reaches; the packed texels were only for the upload
    w->centers = cloud->centers;
    w->radii = cloud->radii;
    cloud->centers = NULL;
    cloud->radii = NULL;
    size_t n = (size_t)cloud->count;
    w->count = cloud->count;
    splatFree(cloud);
    w->work = malloc(slots * 4);
    w->result = malloc(slots * 4);
    w->workCost = malloc(slots * 4);
    w->resultCost = malloc(slots * 4);
    w->viewCost = malloc(slots * 4);
    w->sorted = malloc(n * 4);
    w->faceCost = malloc(n * SPLAT_FACE_SLOTS * 4);
    if (w->cubeMode) {
        w->cubeWork = malloc(n * SPLAT_FACE_SLOTS * 4);
        w->cubeResult = malloc(n * SPLAT_FACE_SLOTS * 4);
        w->cubeWorkCost = malloc(n * SPLAT_FACE_SLOTS * 4);
        w->cubeResultCost = malloc(n * SPLAT_FACE_SLOTS * 4);
    }
    if (w->cubeMode) {
        if (w->faceBuffer == 0) glGenBuffers(1, &w->faceBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, w->faceBuffer);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * SPLAT_FACE_SLOTS * 4, NULL, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    memset(w->faceBuilt, 0, sizeof(w->faceBuilt));
    ctx->splatCubeReady = 0;
    w->picked = malloc(n * 4);
    w->keys = malloc(n * 4);
    w->counts = malloc(SPLAT_SORT_BUCKETS * 4);
    if (!w->work || !w->result || !w->workCost || !w->resultCost || !w->viewCost || !w->sorted || !w->faceCost
            || (w->cubeMode && (!w->cubeWork || !w->cubeResult || !w->cubeWorkCost || !w->cubeResultCost))
            || !w->picked || !w->keys || !w->counts
            || pthread_create(&w->thread, NULL, sortLoop, w) != 0) {
        LOGE("splat sorter could not start");
        stopSorter(w);
        w->count = 0;
        return;
    }
    pthread_setname_np(w->thread, "XR splat sort");
    w->threadRunning = 1;
    LOGI("splat world up: %d splats, %dx%d texture", w->count, SPLAT_TEX_WIDTH,
         (w->count + SPLAT_PER_ROW - 1) / SPLAT_PER_ROW);
}

// From prepareRoom, before the frame's timer opens: a world Java has parsed goes up
void splatPrepare(XrCtx* ctx) {
    pthread_mutex_lock(&pendingLock);
    SplatCloud* cloud = ctx->splatPending;
    ctx->splatPending = NULL;
    pthread_mutex_unlock(&pendingLock);
    if (cloud == NULL) return;
    if (ctx->splat == NULL) {
        ctx->splat = calloc(1, sizeof(struct SplatWorld));
        if (ctx->splat == NULL) { splatFree(cloud); return; }
        pthread_mutex_init(&ctx->splat->lock, NULL);
        pthread_cond_init(&ctx->splat->wake, NULL);
    }
    adoptCloud(ctx, cloud);
}

// Asks the sort thread for a new order when the head has moved or turned enough
static void askForOrder(struct SplatWorld* w, const XrView views[ROOM_EYES]) {
    // Halfway between the eyes is where the order is taken from, and the eyes'
    // forward averaged is where the head points
    float eye[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = 0.5f * ((&views[0].pose.position.x)[i] + (&views[1].pose.position.x)[i]);
    }
    Vec3 f0 = quatRotate(views[0].pose.orientation, (Vec3){ 0, 0, -1 });
    Vec3 f1 = quatRotate(views[1].pose.orientation, (Vec3){ 0, 0, -1 });
    float forward[3] = { f0.x + f1.x, f0.y + f1.y, f0.z + f1.z };
    float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (int i = 0; i < 3; i++) forward[i] /= length > 1e-6f ? length : 1.0f;
    float dx = eye[0] - w->askedEye[0], dy = eye[1] - w->askedEye[1], dz = eye[2] - w->askedEye[2];
    float turned = forward[0] * w->askedForward[0] + forward[1] * w->askedForward[1] + forward[2] * w->askedForward[2];
    int moved = !w->asked || dx * dx + dy * dy + dz * dz > SPLAT_RESORT_M * SPLAT_RESORT_M
            || turned < SPLAT_RESORT_COS;
    pthread_mutex_lock(&w->lock);
    if (moved && !w->want) {
        memcpy(w->wantEye, eye, sizeof(eye));
        memcpy(w->askedEye, eye, sizeof(eye));
        memcpy(w->wantForward, forward, sizeof(forward));
        memcpy(w->askedForward, forward, sizeof(forward));
        w->want = 1;
        w->asked = 1;
        pthread_cond_signal(&w->wake);
    }
    pthread_mutex_unlock(&w->lock);
}

// The shader that copies a finished view into the room's swapchain
static const char* const PRESENT_VERTEX_SRC =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

// The rim of each eye's view dissolves into the cube of the same world under it in
// rough strokes, noise stretched along the horizontal and warped by itself like a dry
// brush. It lives only in the outer part of the margin the view was widened by, so it
// shows on a quick turn and never with the head still. Premultiplied, as the layer
// blends it.
static const char* const PRESENT_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_image;\n"
    "uniform vec4 u_rim0;\n"  // each eye's margin as a share of its image: left, right, down, up
    "uniform vec4 u_rim1;\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
    "float vnoise(vec2 p) { vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y); }\n"
    "float fbm(vec2 p) { float s = 0.0, a = 0.5; for (int i = 0; i < 4; i++) { s += a * vnoise(p); p = p * 2.07 + 13.1; a *= 0.5; } return s; }\n"
    "void main() {\n"
    "  vec3 c = texture(u_image, v_uv).rgb;\n"
    "  vec2 e = vec2(fract(v_uv.x * 2.0), v_uv.y);\n"
    "  vec4 rim = max(v_uv.x < 0.5 ? u_rim0 : u_rim1, vec4(1e-3));\n"
    "  vec4 d = vec4(e.x, 1.0 - e.x, e.y, 1.0 - e.y) / rim;\n"
    "  float edge = min(min(d.x, d.y), min(d.z, d.w));\n"
    "  vec2 q = vec2(e.x * 14.0, e.y * 70.0);\n"
    "  float strokes = fbm(q + 2.2 * vec2(fbm(q * 0.35), fbm(q * 0.35 + 7.3)));\n"
    "  float grain = hash(floor(v_uv * vec2(700.0, 350.0)));\n"
    "  float n = 0.8 * strokes + 0.2 * grain;\n"
    "  float a = clamp(edge * 1.4 - 0.45 + (n - 0.5) * 1.6, 0.0, 1.0);\n"
    "  a = a * a * (3.0 - 2.0 * a);\n"
    "  fragColor = vec4(c * a, a);\n"
    "}\n";

// The image a view is built up in, and the program that presents it
static int ensureTarget(XrCtx* ctx, struct SplatWorld* w) {
    int eyeWidth = (int)(ctx->roomEyeWidth * SPLAT_BUILD_SCALE);
    int width = eyeWidth * ROOM_EYES, height = (int)(ctx->roomEyeHeight * SPLAT_BUILD_SCALE);
    if (w->buildFbo && w->presentProgram && w->targetWidth == width && w->targetHeight == height) return 1;
    if (w->buildTexture == 0) glGenTextures(1, &w->buildTexture);
    glBindTexture(GL_TEXTURE_2D, w->buildTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (w->buildFbo == 0) glGenFramebuffers(1, &w->buildFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, w->buildFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->buildTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("splat build target incomplete: 0x%x", status);
        return 0;
    }
    if (w->presentProgram == 0) {
        GLuint vs = compileShader(GL_VERTEX_SHADER, PRESENT_VERTEX_SRC);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, PRESENT_FRAGMENT_SRC);
        if (!vs || !fs) return 0;
        w->presentProgram = glCreateProgram();
        glAttachShader(w->presentProgram, vs);
        glAttachShader(w->presentProgram, fs);
        glBindAttribLocation(w->presentProgram, 0, "a_pos");
        glLinkProgram(w->presentProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glUseProgram(w->presentProgram);
        glUniform1i(glGetUniformLocation(w->presentProgram, "u_image"), 0);
        w->rim0Uniform = glGetUniformLocation(w->presentProgram, "u_rim0");
        w->rim1Uniform = glGetUniformLocation(w->presentProgram, "u_rim1");
    }
    w->targetWidth = width;
    w->targetHeight = height;
    w->building = 0;
    LOGI("splat build target %dx%d", width, height);
    return 1;
}

static void drawCubeBackdrop(XrCtx* ctx, struct SplatWorld* w);

// A new view begins: the newest order goes up, the eyes are fixed (and widened) for
// every slice of it, and the image starts from the cube of the same world (or black)
static void startView(XrCtx* ctx, struct SplatWorld* w, const XrView views[ROOM_EYES]) {
    pthread_mutex_lock(&w->lock);
    if (w->fresh) {
        glBindBuffer(GL_ARRAY_BUFFER, w->orderBuffer);
        if (w->resultViewCount > 0) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)w->resultViewCount * 4, w->result);
            memcpy(w->viewCost, w->resultCost, (size_t)w->resultViewCount * sizeof(float));
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        w->drawCount = w->resultViewCount;
        w->fresh = 0;
        w->ordered = 1;
    }
    pthread_mutex_unlock(&w->lock);
    if (!w->ordered) return;
    memcpy(w->viewViews, views, sizeof(w->viewViews));
    for (int e = 0; e < ROOM_EYES; e++) {
        XrFovf* f = &w->viewViews[e].fov;
        f->angleLeft = fmaxf(f->angleLeft - SPLAT_OVERSCAN_H_RAD, -SPLAT_FOV_MAX_RAD);
        f->angleRight = fminf(f->angleRight + SPLAT_OVERSCAN_H_RAD, SPLAT_FOV_MAX_RAD);
        f->angleDown = fmaxf(f->angleDown - SPLAT_OVERSCAN_V_RAD, -SPLAT_FOV_MAX_RAD);
        f->angleUp = fminf(f->angleUp + SPLAT_OVERSCAN_V_RAD, SPLAT_FOV_MAX_RAD);
    }
    w->drawn = 0;
    w->viewCount = w->drawCount;
    w->viewStartNs = nowNs();
    glBindFramebuffer(GL_FRAMEBUFFER, w->buildFbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (ctx->splatCubeReady) drawCubeBackdrop(ctx, w);
    w->building = 1;
}

// The next slice of the order, furthest first, over the image as it stands
static void drawSlice(struct SplatWorld* w, int slice) {
    glBindFramebuffer(GL_FRAMEBUFFER, w->buildFbo);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(w->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, w->texture);
    glBindVertexArray(w->vao);
    glBindBuffer(GL_ARRAY_BUFFER, w->orderBuffer);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 4, (const void*)((size_t)w->drawn * 4));
    glUniform1f(w->boundUniform, 1.2f);
    int eyeWidth = w->targetWidth / ROOM_EYES, eyeHeight = w->targetHeight;
    for (int e = 0; e < ROOM_EYES; e++) {
        glViewport(e * eyeWidth, 0, eyeWidth, eyeHeight);
        float proj[16];
        float view[16];
        projectionFromFov(proj, w->viewViews[e].fov, 0.05f, 200.0f);
        viewFromPose(view, w->viewViews[e].pose);
        glUniformMatrix4fv(w->viewUniform, 1, GL_FALSE, view);
        glUniformMatrix4fv(w->projUniform, 1, GL_FALSE, proj);
        glUniform2f(w->focalUniform, proj[0] * eyeWidth * 0.5f, proj[5] * eyeHeight * 0.5f);
        glUniform2f(w->viewportUniform, (float)eyeWidth, (float)eyeHeight);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, slice);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    w->drawn += slice;
}

// A finished view into the room's swapchain, with the poses it was drawn from: the
// compositor turns it with the head at the display's own rate until the next one
static void presentView(XrCtx* ctx, struct SplatWorld* w) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->roomSwapchain, &acquire, &index), "acquire room image")) return;
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->roomSwapchain, &wait);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->roomImages[index].image, 0);
    if (ctx->srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glViewport(0, 0, ctx->roomEyeWidth * ROOM_EYES, ctx->roomEyeHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(w->presentProgram);
    // The dissolve lives only in the margin the view was widened by, side by side,
    // so it is never in sight with the head still: only a quick turn reaches it
    for (int e = 0; e < ROOM_EYES; e++) {
        XrFovf f = w->viewViews[e].fov;
        float l = tanf(-f.angleLeft), r = tanf(f.angleRight), dn = tanf(-f.angleDown), up = tanf(f.angleUp);
        float ml = l - tanf(fmaxf(-f.angleLeft - SPLAT_OVERSCAN_H_RAD, 0.0f));
        float mr = r - tanf(fmaxf(f.angleRight - SPLAT_OVERSCAN_H_RAD, 0.0f));
        float md = dn - tanf(fmaxf(-f.angleDown - SPLAT_OVERSCAN_V_RAD, 0.0f));
        float mu = up - tanf(fmaxf(f.angleUp - SPLAT_OVERSCAN_V_RAD, 0.0f));
        glUniform4f(e == 0 ? w->rim0Uniform : w->rim1Uniform,
                    SPLAT_RIM * ml / (l + r), SPLAT_RIM * mr / (l + r), SPLAT_RIM * md / (dn + up), SPLAT_RIM * mu / (dn + up));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, w->buildTexture);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->roomSwapchain, &release);
    memcpy(ctx->roomViews, w->viewViews, sizeof(w->viewViews));
    ctx->roomViewsValid = 1;
    ctx->roomRendered = 1;
    // The first few views, then one in a hundred, so a long session still reports
    if (w->viewsLogged++ < 5 || w->viewsLogged % 100 == 0) {
        LOGI("splat view: %d splats over %d frames, %.0f ms, budget %.1f of %.1f", w->viewCount, w->viewFrames,
             (nowNs() - w->viewStartNs) / 1e6, w->budget, w->viewCount ? w->viewCost[w->viewCount - 1] : 0.0f);
    }
}

// ---- the cube ------------------------------------------------------------------
//
// Each eye gets a cube round it, faces 640 across, about seven pixels a degree. A
// face is built over frames like a view is, from that eye's position when it started,
// and when it is in, the eye's whole cube goes to its swapchain. The compositor shows
// the two cubes (one per eye) at the display's rate whichever way the head turns, so
// turning never finds an edge; moving refreshes face by face, the one you face first.
#define SPLAT_CUBE_SIZE 640

typedef void (*CopyImageFn)(GLuint, GLenum, GLint, GLint, GLint, GLint,
                            GLuint, GLenum, GLint, GLint, GLint, GLint,
                            GLsizei, GLsizei, GLsizei);
static CopyImageFn copyImage;

// GL's cube faces, +X -X +Y -Y +Z -Z: where each looks and which way is up in it, the
// camera set so its picture lands the right way round in the face's texture
static const float FACE_DIR[SPLAT_FACES][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float FACE_UP[SPLAT_FACES][3] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };

static int makeCubeSwapchain(XrCtx* ctx, struct SplatWorld* w, int eye) {
    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
            | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    info.width = info.height = w->cubeSize;
    info.faceCount = 6;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->splatCube[eye]), "create splat cube")) return 0;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(ctx->splatCube[eye], 0, &n, NULL);
    w->cubeImages[eye] = calloc(n, sizeof(XrSwapchainImageOpenGLESKHR));
    if (n == 0 || w->cubeImages[eye] == NULL) return 0;
    for (uint32_t i = 0; i < n; i++) w->cubeImages[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    if (!checkXr(xrEnumerateSwapchainImages(ctx->splatCube[eye], n, &n,
                                            (XrSwapchainImageBaseHeader*)w->cubeImages[eye]), "splat cube images")) return 0;
    w->cubeImageCount[eye] = n;
    return 1;
}

// Everything the cube needs, made the first time a world is drawn; 0 means no cube,
// and the world is drawn as a view instead
static int ensureCube(XrCtx* ctx, struct SplatWorld* w) {
    if (w->cubeFbo) return 1;
    if (w->cubeSize < 0) return 0;
    w->cubeSize = -1;
    if (copyImage == NULL) copyImage = (CopyImageFn)eglGetProcAddress("glCopyImageSubData");
    if (copyImage == NULL) copyImage = (CopyImageFn)eglGetProcAddress("glCopyImageSubDataEXT");
    if (copyImage == NULL) { LOGE("splat cube: no glCopyImageSubData"); return 0; }
    w->cubeSize = SPLAT_CUBE_SIZE;
    for (int e = 0; e < 2; e++) {
        if (!makeCubeSwapchain(ctx, w, e)) { w->cubeSize = -1; return 0; }
        glGenTextures(1, &w->buildCube[e]);
        glBindTexture(GL_TEXTURE_CUBE_MAP, w->buildCube[e]);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, (GLenum)ctx->swapchainFormat, w->cubeSize, w->cubeSize);
        glGenTextures(1, &w->scratch[e]);
        glBindTexture(GL_TEXTURE_2D, w->scratch[e]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w->cubeSize, w->cubeSize);
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &w->cubeFbo);
    LOGI("splat cube %dx%d a face, both eyes", w->cubeSize, w->cubeSize);
    return 1;
}

// The face most worth building next: any not yet drawn, the one faced first; after
// that the longest waiting, weighted hard towards where the head points
static int pickFace(struct SplatWorld* w, const float forward[3], long now) {
    int best = 0;
    float bestScore = -1e30f;
    for (int f = 0; f < SPLAT_FACES; f++) {
        float facing = FACE_DIR[f][0] * forward[0] + FACE_DIR[f][1] * forward[1] + FACE_DIR[f][2] * forward[2];
        float score = !w->faceBuilt[f] ? 1e9f + facing
                : (now - w->faceNs[f]) / 1e9f * (0.25f + fminf(fmaxf(facing + 0.3f, 0.0f), 1.3f));
        if (score > bestScore) { bestScore = score; best = f; }
    }
    return best;
}

static void clearScratch(struct SplatWorld* w) {
    glBindFramebuffer(GL_FRAMEBUFFER, w->cubeFbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    for (int e = 0; e < 2; e++) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->scratch[e], 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

// A face begins: the newest order goes up, the face and the eyes it is seen from are
// fixed for every slice of it
static void startFace(struct SplatWorld* w, const XrView views[ROOM_EYES], const float forward[3]) {
    // A round is all six faces from one place with one order, swapped in together, so
    // the cube is always a single picture and has no seams to see. Only the first face
    // of a round takes a new order and where the eyes are.
    int roundStart = 1;
    for (int f = 0; f < SPLAT_FACES; f++) {
        if (w->faceBuilt[f]) roundStart = 0;
    }
    if (roundStart) pthread_mutex_lock(&w->lock);
    if (roundStart && w->freshCube) {
        int faces = w->cubeResultCount;
        glBindBuffer(GL_ARRAY_BUFFER, w->faceBuffer);
        if (faces > 0) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)faces * 4, w->cubeResult);
            memcpy(w->faceCost, w->cubeResultCost, (size_t)faces * sizeof(float));
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        memcpy(w->viewOffsets, w->resultOffsets, sizeof(w->viewOffsets));
        w->freshCube = 0;
        w->cubeOrdered = 1;
    }
    if (roundStart) pthread_mutex_unlock(&w->lock);
    if (!w->cubeOrdered) return;
    w->face = pickFace(w, forward, nowNs());
    w->faceDrawn = 0;
    w->faceCount = w->viewOffsets[w->face + 1] - w->viewOffsets[w->face];
    if (roundStart) {
        for (int e = 0; e < 2; e++) {
            memcpy(w->faceEye[e], &views[e].pose.position, sizeof(w->faceEye[e]));
        }
        w->roundStartNs = nowNs();
    }
    w->cubeStartNs = nowNs();
    w->cubeFrames = 0;
    clearScratch(w);
    w->cubeBuilding = 1;
}

// The view from an eye out through a cube face, column major, the camera down its -z
static void faceView(float* m, const float eye[3], const float* f, const float* u) {
    float r[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
    m[0] = r[0];  m[4] = r[1];  m[8] = r[2];   m[12] = -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]);
    m[1] = u[0];  m[5] = u[1];  m[9] = u[2];   m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[3] = 0.0f;  m[7] = 0.0f;  m[11] = 0.0f;  m[15] = 1.0f;
}

static void drawFaceSlice(struct SplatWorld* w, int slice) {
    XrFovf square = { -0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f };
    float proj[16];
    projectionFromFov(proj, square, 0.05f, 200.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, w->cubeFbo);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(w->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, w->texture);
    glBindVertexArray(w->vao);
    glBindBuffer(GL_ARRAY_BUFFER, w->faceBuffer);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 4,
                           (const void*)((size_t)(w->viewOffsets[w->face] + w->faceDrawn) * 4));
    glViewport(0, 0, w->cubeSize, w->cubeSize);
    // A big splat close by reaches onto a face its centre is well past the edge of,
    // the floor round you most of all; dropped, it would leave the face black there
    glUniform1f(w->boundUniform, 3.0f);
    glUniformMatrix4fv(w->projUniform, 1, GL_FALSE, proj);
    glUniform2f(w->focalUniform, proj[0] * w->cubeSize * 0.5f, proj[5] * w->cubeSize * 0.5f);
    glUniform2f(w->viewportUniform, (float)w->cubeSize, (float)w->cubeSize);
    for (int e = 0; e < 2; e++) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->scratch[e], 0);
        float view[16];
        faceView(view, w->faceEye[e], FACE_DIR[w->face], FACE_UP[w->face]);
        glUniformMatrix4fv(w->viewUniform, 1, GL_FALSE, view);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, slice);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    w->faceDrawn += slice;
}

// A face is in: into each eye's whole cube, and once every face has been drawn, each
// cube into its swapchain for the layers
static void finishFace(XrCtx* ctx, struct SplatWorld* w) {
    int n = w->cubeSize;
    for (int e = 0; e < 2; e++) {
        copyImage(w->scratch[e], GL_TEXTURE_2D, 0, 0, 0, 0,
                  w->buildCube[e], GL_TEXTURE_CUBE_MAP, 0, 0, 0, w->face, n, n, 1);
    }
    w->faceBuilt[w->face] = 1;
    w->faceNs[w->face] = nowNs();
    for (int f = 0; f < SPLAT_FACES; f++) {
        if (!w->faceBuilt[f]) return;
    }
    for (int e = 0; e < 2; e++) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (!checkXr(xrAcquireSwapchainImage(ctx->splatCube[e], &acquire, &index), "acquire splat cube")) return;
        XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(ctx->splatCube[e], &wait);
        copyImage(w->buildCube[e], GL_TEXTURE_CUBE_MAP, 0, 0, 0, 0,
                  w->cubeImages[e][index].image, GL_TEXTURE_CUBE_MAP, 0, 0, 0, 0, n, n, SPLAT_FACES);
        XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(ctx->splatCube[e], &release);
    }
    ctx->splatCubeReady = 1;
    memset(w->faceBuilt, 0, sizeof(w->faceBuilt));
    if (w->viewsLogged++ < 8 || w->viewsLogged % 50 == 0) {
        LOGI("splat cube: all six faces in %.0f ms, budget %.1f", (nowNs() - w->roundStartNs) / 1e6, w->budget);
    }
}

// The low cube painted behind a sharp view from the direction each pixel looks along, so
// anything the view's splats leave open is the same world, softer, never black
static const char* const BACKDROP_VERTEX_SRC =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "out vec2 v_ndc;\n"
    "void main() { v_ndc = a_pos; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char* const BACKDROP_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform samplerCube u_cube;\n"
    "uniform mat3 u_rot;\n"      // eye orientation, view to world
    "uniform vec4 u_tan;\n"      // tan of the left, right, down, up half angles
    "uniform float u_encode;\n"  // 1 when the cube is sRGB and reads back linear
    "in vec2 v_ndc;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec2 t = 0.5 * (v_ndc + 1.0);\n"
    "  vec3 d = u_rot * normalize(vec3(mix(u_tan.x, u_tan.y, t.x), mix(u_tan.z, u_tan.w, t.y), -1.0));\n"
    "  vec3 c = textureLod(u_cube, d, 0.0).rgb;\n"
    "  fragColor = vec4(u_encode > 0.5 ? pow(c, vec3(1.0 / 2.2)) : c, 1.0);\n"
    "}\n";

static GLuint backdropProgram;
static GLint backdropRotUniform, backdropTanUniform, backdropEncodeUniform;

static void drawCubeBackdrop(XrCtx* ctx, struct SplatWorld* w) {
    if (backdropProgram == 0) {
        GLuint vs = compileShader(GL_VERTEX_SHADER, BACKDROP_VERTEX_SRC);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, BACKDROP_FRAGMENT_SRC);
        if (!vs || !fs) return;
        backdropProgram = glCreateProgram();
        glAttachShader(backdropProgram, vs);
        glAttachShader(backdropProgram, fs);
        glBindAttribLocation(backdropProgram, 0, "a_pos");
        glLinkProgram(backdropProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glUseProgram(backdropProgram);
        glUniform1i(glGetUniformLocation(backdropProgram, "u_cube"), 0);
        backdropRotUniform = glGetUniformLocation(backdropProgram, "u_rot");
        backdropTanUniform = glGetUniformLocation(backdropProgram, "u_tan");
        backdropEncodeUniform = glGetUniformLocation(backdropProgram, "u_encode");
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(backdropProgram);
    glUniform1f(backdropEncodeUniform, ctx->swapchainFormat == GL_SRGB8_ALPHA8 ? 1.0f : 0.0f);
    int eyeWidth = w->targetWidth / ROOM_EYES;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glActiveTexture(GL_TEXTURE0);
    for (int e = 0; e < ROOM_EYES; e++) {
        glBindTexture(GL_TEXTURE_CUBE_MAP, w->buildCube[e]);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glViewport(e * eyeWidth, 0, eyeWidth, w->targetHeight);
        XrQuaternionf o = w->viewViews[e].pose.orientation;
        Vec3 x = quatRotate(o, (Vec3){ 1, 0, 0 });
        Vec3 y = quatRotate(o, (Vec3){ 0, 1, 0 });
        Vec3 z = quatRotate(o, (Vec3){ 0, 0, 1 });
        float rot[9] = { x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z };
        glUniformMatrix3fv(backdropRotUniform, 1, GL_FALSE, rot);
        XrFovf f = w->viewViews[e].fov;
        glUniform4f(backdropTanUniform, tanf(f.angleLeft), tanf(f.angleRight), tanf(f.angleDown), tanf(f.angleUp));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableVertexAttribArray(0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, w->buildFbo);
}

// One slice of the face being built this frame, as far as budget reaches. Returns
// whether the cube is on show.
static int renderCube(XrCtx* ctx, struct SplatWorld* w, const XrView views[ROOM_EYES], float budget) {
    Vec3 f0 = quatRotate(views[0].pose.orientation, (Vec3){ 0, 0, -1 });
    float forward[3] = { f0.x, f0.y, f0.z };
    if (!w->cubeBuilding) {
        startFace(w, views, forward);
        if (!w->cubeBuilding) return ctx->splatCubeReady;
    }
    int base = w->viewOffsets[w->face];
    float target = (w->faceDrawn > 0 ? w->faceCost[base + w->faceDrawn - 1] : 0.0f) + budget;
    int lo = base + w->faceDrawn, hi = base + w->faceCount;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (w->faceCost[mid] <= target) lo = mid + 1;
        else hi = mid;
    }
    int slice = lo - base - w->faceDrawn;
    if (slice < 1) slice = w->faceCount > w->faceDrawn ? 1 : 0;
    if (slice > 0) drawFaceSlice(w, slice);
    w->cubeFrames++;
    if (w->faceDrawn >= w->faceCount) {
        finishFace(ctx, w);
        w->cubeBuilding = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ctx->splatCubeReady;
}

// The room for a splat world, called every frame instead of the usual pass: a slice of
// a cube face, or where there is no cube, a slice of a view. Returns 0 while nothing
// of the world is ready to show, so Calm shows.
int splatRenderRoom(XrCtx* ctx) {
    struct SplatWorld* w = ctx->splat;
    if (w == NULL || w->count == 0) return 0;
    if (w->program == 0 && !linkSplat(w)) { w->count = 0; return 0; }

    // How long the last frame took decides this frame's budget. A late frame is not
    // always ours (the desktops decode on the same GPU), so the cut is gentle and the
    // ceiling it leaves lifts again within a second or so.
    long now = nowNs();
    if (w->lastFrameNs) {
        long frame = now - w->lastFrameNs;
        if (frame > SPLAT_FRAME_MISSED_NS) {
            w->ceiling = w->budget * 0.9f;
            w->budget *= 0.75f;
        }
        else {
            w->budget = fminf(w->budget * 1.05f, w->ceiling);
            w->ceiling *= 1.01f;
        }
    }
    w->lastFrameNs = now;
    if (w->budget < SPLAT_BUDGET_MIN) w->budget = SPLAT_BUDGET_MIN;

    XrView views[ROOM_EYES];
    for (int e = 0; e < ROOM_EYES; e++) { views[e].type = XR_TYPE_VIEW; views[e].next = NULL; }
    if (!locateEyes(ctx, views)) return ctx->splatCubeReady || ctx->roomRendered;
    askForOrder(w, views);
    float viewBudget = w->budget;
    if (w->cubeMode && ensureCube(ctx, w)) {
        renderCube(ctx, w, views, w->budget * SPLAT_CUBE_SHARE);
        viewBudget = w->budget * (1.0f - SPLAT_CUBE_SHARE);
    }
    if (!ensureTarget(ctx, w)) return ctx->splatCubeReady;

    if (!w->building) {
        startView(ctx, w, views);
        if (!w->building) return ctx->roomRendered || ctx->splatCubeReady;
        w->viewFrames = 0;
    }
    // As far along the order as this frame's budget reaches, found in the running cost
    float target = (w->drawn > 0 ? w->viewCost[w->drawn - 1] : 0.0f) + viewBudget;
    int lo = w->drawn, hi = w->viewCount;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (w->viewCost[mid] <= target) lo = mid + 1;
        else hi = mid;
    }
    int slice = lo - w->drawn;
    if (slice < 1) slice = w->viewCount > w->drawn ? 1 : 0;
    w->slice = slice;
    if (slice > 0) drawSlice(w, slice);
    w->viewFrames++;
    if (w->drawn >= w->viewCount) {
        presentView(ctx, w);
        w->building = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

void splatDestroy(XrCtx* ctx) {
    pthread_mutex_lock(&pendingLock);
    splatFree(ctx->splatPending);
    ctx->splatPending = NULL;
    pthread_mutex_unlock(&pendingLock);
    struct SplatWorld* w = ctx->splat;
    if (w == NULL) return;
    stopSorter(w);
    if (w->program) glDeleteProgram(w->program);
    if (w->texture) glDeleteTextures(1, &w->texture);
    if (w->orderBuffer) glDeleteBuffers(1, &w->orderBuffer);
    if (w->vao) glDeleteVertexArrays(1, &w->vao);
    if (w->buildFbo) glDeleteFramebuffers(1, &w->buildFbo);
    if (w->buildTexture) glDeleteTextures(1, &w->buildTexture);
    if (w->presentProgram) glDeleteProgram(w->presentProgram);
    for (int e = 0; e < 2; e++) {
        if (ctx->splatCube[e] != XR_NULL_HANDLE) xrDestroySwapchain(ctx->splatCube[e]);
        ctx->splatCube[e] = XR_NULL_HANDLE;
        free(w->cubeImages[e]);
        if (w->buildCube[e]) glDeleteTextures(1, &w->buildCube[e]);
        if (w->scratch[e]) glDeleteTextures(1, &w->scratch[e]);
    }
    if (w->cubeFbo) glDeleteFramebuffers(1, &w->cubeFbo);
    if (w->faceBuffer) glDeleteBuffers(1, &w->faceBuffer);
    if (backdropProgram) { glDeleteProgram(backdropProgram); backdropProgram = 0; }
    ctx->splatCubeReady = 0;
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->wake);
    free(w);
    ctx->splat = NULL;
}

// A .splat file read whole on Java's loader thread. Packed right here on that thread,
// which keeps the seconds a big world takes off the frame loop.
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadSplats(JNIEnv* env, jobject thiz,
                                                               jlong handle, jobject buffer,
                                                               jint length) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL) return JNI_FALSE;
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) return JNI_FALSE;
    long start = nowNs();
    SplatCloud* cloud = splatParse(data, (size_t)length);
    if (cloud == NULL) {
        LOGE("splat world of %d bytes did not parse", length);
        return JNI_FALSE;
    }
    LOGI("splat world packed: %d splats in %.0f ms", cloud->count, (nowNs() - start) / 1e6);
    pthread_mutex_lock(&pendingLock);
    splatFree(ctx->splatPending);
    ctx->splatPending = cloud;
    pthread_mutex_unlock(&pendingLock);
    return JNI_TRUE;
}
