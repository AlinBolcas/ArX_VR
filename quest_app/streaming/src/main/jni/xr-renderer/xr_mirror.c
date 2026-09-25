// The mirror: a flat 16:9 view of what the headset shows, for the Mac to watch
// and record. After each frame is handed to the compositor, the same list of
// layers is drawn again from a smoothed head camera into the encoder's surface.
// Nothing here runs unless Java has handed over a surface, which it only does
// while the Mac has its mirror window open.
#include "xr_renderer.h"
#include <android/native_window_jni.h>
#include <EGL/eglext.h>
#include <math.h>

#define MIRROR_MAX_CHAINS 64
#define MIRROR_CYL_SEGMENTS 48
// Vertical field of view, a little under the headset's so text stays readable
#define MIRROR_FOV_Y 1.05f
// How much of the way to the head the camera moves each drawn frame: steady video, no lag you notice
#define MIRROR_FOLLOW 0.25f

// The texture each swapchain showed last, since an art chain's image changes with every upload
static struct { XrSwapchain chain; GLuint tex; int w, h; } seen[MIRROR_MAX_CHAINS];
static int seenCount;

static pthread_mutex_t pendingLock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow* pendingWindow;
static int pendingSet, pendingW, pendingH;

static EGLSurface surface = EGL_NO_SURFACE;
static ANativeWindow* window;
static int width, height;
static GLuint program;
static GLint uViewProj, uTex, uRect, uSrgb, uOpaque;
static XrPosef cam;
static int camValid;
static long frameCount, frameIndex;
static PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime;
static int linkMirrorProgram(void);

static const char* const MIRROR_VERTEX_SRC =
    "#version 300 es\n"
    "in vec3 a_position;\n"
    "in vec2 a_uv;\n"
    "uniform mat4 u_viewproj;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = u_viewproj * vec4(a_position, 1.0); }\n";

// Four taps across each pixel's footprint, since a desktop is drawn much smaller
// than its swapchain and one bilinear tap shimmers on text. The chains are sRGB,
// so what comes back is linear and goes out re-encoded for the plain encoder surface.
static const char* const MIRROR_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_rect;\n"
    "uniform int u_srgb;\n"
    "uniform int u_opaque;\n"
    "out vec4 fragColor;\n"
    "vec4 tap(vec2 uv) { return texture(u_tex, u_rect.xy + uv * u_rect.zw); }\n"
    "void main() {\n"
    "    vec2 dx = dFdx(v_uv) * 0.25, dy = dFdy(v_uv) * 0.25;\n"
    "    vec4 c = (tap(v_uv + dx + dy) + tap(v_uv - dx + dy) + tap(v_uv + dx - dy) + tap(v_uv - dx - dy)) * 0.25;\n"
    "    if (u_srgb == 1) {\n"
    "        vec3 lo = c.rgb * 12.92, hi = 1.055 * pow(c.rgb, vec3(1.0 / 2.4)) - 0.055;\n"
    "        c.rgb = mix(lo, hi, step(vec3(0.0031308), c.rgb));\n"
    "    }\n"
    "    if (u_opaque == 1) c.a = 1.0;\n"
    "    fragColor = c;\n"
    "}\n";

void mirrorNote(XrSwapchain chain, GLuint tex, int w, int h) {
    for (int i = 0; i < seenCount; i++) {
        if (seen[i].chain == chain) { seen[i].tex = tex; seen[i].w = w; seen[i].h = h; return; }
    }
    if (seenCount < MIRROR_MAX_CHAINS) {
        seen[seenCount].chain = chain; seen[seenCount].tex = tex; seen[seenCount].w = w; seen[seenCount].h = h;
        seenCount++;
    }
}

static int lookup(XrSwapchain chain, GLuint* tex, int* w, int* h) {
    for (int i = 0; i < seenCount; i++) {
        if (seen[i].chain == chain) { *tex = seen[i].tex; *w = seen[i].w; *h = seen[i].h; return 1; }
    }
    return 0;
}

// From Java, on any thread: the encoder's surface, or NULL to stop. Picked up by the render thread.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetMirror(JNIEnv* env, jobject thiz, jlong handle,
        jobject javaSurface, jint w, jint h) {
    ANativeWindow* next = javaSurface != NULL ? ANativeWindow_fromSurface(env, javaSurface) : NULL;
    pthread_mutex_lock(&pendingLock);
    if (pendingSet && pendingWindow != NULL) ANativeWindow_release(pendingWindow);
    pendingWindow = next; pendingW = w; pendingH = h; pendingSet = 1;
    pthread_mutex_unlock(&pendingLock);
}

static void dropSurface(XrCtx* ctx) {
    if (surface != EGL_NO_SURFACE) {
        eglDestroySurface(ctx->eglDisplay, surface);
        surface = EGL_NO_SURFACE;
        LOGI("mirror stopped after %ld frames", frameCount);
    }
    if (window != NULL) { ANativeWindow_release(window); window = NULL; }
}

static void takePending(XrCtx* ctx) {
    pthread_mutex_lock(&pendingLock);
    int changed = pendingSet;
    ANativeWindow* next = pendingWindow;
    int w = pendingW, h = pendingH;
    pendingSet = 0; pendingWindow = NULL;
    pthread_mutex_unlock(&pendingLock);
    if (!changed) return;
    dropSurface(ctx);
    if (next == NULL) return;
    if (program == 0 && !linkMirrorProgram()) { ANativeWindow_release(next); return; }
    surface = eglCreateWindowSurface(ctx->eglDisplay, ctx->eglConfig, next, NULL);
    if (surface == EGL_NO_SURFACE) {
        LOGE("mirror surface refused: %d", eglGetError());
        ANativeWindow_release(next);
        return;
    }
    window = next; width = w; height = h; camValid = 0; frameCount = 0;
    if (presentationTime == NULL) {
        presentationTime = (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress("eglPresentationTimeANDROID");
    }
    LOGI("mirror started at %dx%d", w, h);
}

static int linkMirrorProgram(void) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, MIRROR_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, MIRROR_FRAGMENT_SRC);
    if (!vs || !fs) return 0;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { LOGE("mirror program failed to link"); glDeleteProgram(program); program = 0; return 0; }
    uViewProj = glGetUniformLocation(program, "u_viewproj");
    uTex = glGetUniformLocation(program, "u_tex");
    uRect = glGetUniformLocation(program, "u_rect");
    uSrgb = glGetUniformLocation(program, "u_srgb");
    uOpaque = glGetUniformLocation(program, "u_opaque");
    return 1;
}

// Column major, like the rest: out = projection * view
static void viewProjection(float* out, XrPosef eye, float aspect) {
    float f = 1.0f / tanf(MIRROR_FOV_Y * 0.5f), n = 0.05f, far = 100.0f;
    float proj[16] = { f / aspect, 0, 0, 0,  0, f, 0, 0,  0, 0, (far + n) / (n - far), -1,  0, 0, 2 * far * n / (n - far), 0 };
    XrQuaternionf inv = quatConj(eye.orientation);
    Vec3 x = quatRotate(inv, (Vec3){ 1, 0, 0 }), y = quatRotate(inv, (Vec3){ 0, 1, 0 }), z = quatRotate(inv, (Vec3){ 0, 0, 1 });
    Vec3 p = quatRotate(inv, (Vec3){ -eye.position.x, -eye.position.y, -eye.position.z });
    float view[16] = { x.x, x.y, x.z, 0,  y.x, y.y, y.z, 0,  z.x, z.y, z.z, 0,  p.x, p.y, p.z, 1 };
    matMul(out, proj, view);
}

static Vec3 place(XrPosef pose, float x, float y, float z) {
    Vec3 r = quatRotate(pose.orientation, (Vec3){ x, y, z });
    return (Vec3){ r.x + pose.position.x, r.y + pose.position.y, r.z + pose.position.z };
}

static XrQuaternionf nlerp(XrQuaternionf a, XrQuaternionf b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    XrQuaternionf q = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= len; q.y /= len; q.z /= len; q.w /= len;
    return q;
}

// A layer's pose in local space: layers live in local or in view space
static int localPose(XrCtx* ctx, XrSpace space, XrPosef pose, XrPosef head, XrPosef* out) {
    if (space == ctx->localSpace) { *out = pose; return 1; }
    if (space == ctx->viewSpace) {
        out->orientation = quatMul(head.orientation, pose.orientation);
        Vec3 p = place(head, pose.position.x, pose.position.y, pose.position.z);
        out->position.x = p.x; out->position.y = p.y; out->position.z = p.z;
        return 1;
    }
    return 0;
}

static void blendFor(XrCompositionLayerFlags flags, int* opaque) {
    *opaque = !(flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
    if (*opaque) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    if (flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static int bindImage(const XrSwapchainSubImage* sub) {
    GLuint tex; int w, h;
    if (!lookup(sub->swapchain, &tex, &w, &h) || w <= 0 || h <= 0) return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform4f(uRect, (float)sub->imageRect.offset.x / w, (float)sub->imageRect.offset.y / h,
                (float)sub->imageRect.extent.width / w, (float)sub->imageRect.extent.height / h);
    return 1;
}

static void drawQuad(XrCtx* ctx, const XrCompositionLayerQuad* q, XrPosef head) {
    XrPosef pose;
    if (!localPose(ctx, q->space, q->pose, head, &pose) || !bindImage(&q->subImage)) return;
    int opaque;
    blendFor(q->layerFlags, &opaque);
    glUniform1i(uOpaque, opaque);
    float hw = q->size.width * 0.5f, hh = q->size.height * 0.5f;
    Vec3 c[4] = { place(pose, -hw, -hh, 0), place(pose, hw, -hh, 0), place(pose, -hw, hh, 0), place(pose, hw, hh, 0) };
    float v[20] = { c[0].x, c[0].y, c[0].z, 0, 0,  c[1].x, c[1].y, c[1].z, 1, 0,
                    c[2].x, c[2].y, c[2].z, 0, 1,  c[3].x, c[3].y, c[3].z, 1, 1 };
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, v + 3);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void drawCylinder(XrCtx* ctx, const XrCompositionLayerCylinderKHR* cyl, XrPosef head) {
    XrPosef pose;
    if (!localPose(ctx, cyl->space, cyl->pose, head, &pose) || !bindImage(&cyl->subImage)) return;
    if (cyl->aspectRatio <= 0.0f || cyl->centralAngle <= 0.0f) return;
    int opaque;
    blendFor(cyl->layerFlags, &opaque);
    glUniform1i(uOpaque, opaque);
    float hh = cyl->radius * cyl->centralAngle / cyl->aspectRatio * 0.5f;
    float v[(MIRROR_CYL_SEGMENTS + 1) * 2 * 5];
    for (int i = 0; i <= MIRROR_CYL_SEGMENTS; i++) {
        float u = (float)i / MIRROR_CYL_SEGMENTS, a = (u - 0.5f) * cyl->centralAngle;
        float x = cyl->radius * sinf(a), z = -cyl->radius * cosf(a);
        Vec3 lo = place(pose, x, -hh, z), hi = place(pose, x, hh, z);
        float* o = v + i * 10;
        o[0] = lo.x; o[1] = lo.y; o[2] = lo.z; o[3] = u; o[4] = 0;
        o[5] = hi.x; o[6] = hi.y; o[7] = hi.z; o[8] = u; o[9] = 1;
    }
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, v + 3);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, (MIRROR_CYL_SEGMENTS + 1) * 2);
}

void mirrorFrame(XrCtx* ctx, const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    takePending(ctx);
    if (surface == EGL_NO_SURFACE) return;
    // Every other frame: 36 to 45 a second is smooth video and costs the headset half as much
    if ((frameIndex++ & 1) != 0) return;

    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    if (!XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, ctx->localSpace, ctx->predictedDisplayTime, &head))
            || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        return;
    }
    if (!camValid) { cam = head.pose; camValid = 1; }
    cam.orientation = nlerp(cam.orientation, head.pose.orientation, MIRROR_FOLLOW);
    cam.position.x += (head.pose.position.x - cam.position.x) * MIRROR_FOLLOW;
    cam.position.y += (head.pose.position.y - cam.position.y) * MIRROR_FOLLOW;
    cam.position.z += (head.pose.position.z - cam.position.z) * MIRROR_FOLLOW;

    if (!eglMakeCurrent(ctx->eglDisplay, surface, surface, ctx->eglContext)) {
        LOGE("mirror surface lost: %d", eglGetError());
        dropSurface(ctx);
        eglMakeCurrent(ctx->eglDisplay, ctx->eglPbuffer, ctx->eglPbuffer, ctx->eglContext);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // Arvolve charcoal where the headset would show the room or the real world
    glClearColor(0.039f, 0.039f, 0.047f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    float vp[16];
    viewProjection(vp, cam, (float)width / (float)height);
    glUniformMatrix4fv(uViewProj, 1, GL_FALSE, vp);
    glUniform1i(uTex, 0);
    glUniform1i(uSrgb, ctx->swapchainFormat == GL_SRGB8_ALPHA8);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    // In submission order, as the compositor stacks them. Only what the left eye sees.
    for (uint32_t i = 0; i < count; i++) {
        const XrCompositionLayerBaseHeader* layer = layers[i];
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)layer;
            if (q->eyeVisibility != XR_EYE_VISIBILITY_RIGHT) drawQuad(ctx, q, head.pose);
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            const XrCompositionLayerCylinderKHR* c = (const XrCompositionLayerCylinderKHR*)layer;
            if (c->eyeVisibility != XR_EYE_VISIBILITY_RIGHT) drawCylinder(ctx, c, head.pose);
        }
    }
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (presentationTime != NULL) presentationTime(ctx->eglDisplay, surface, (EGLnsecsANDROID)nowNs());
    eglSwapBuffers(ctx->eglDisplay, surface);
    frameCount++;
    eglMakeCurrent(ctx->eglDisplay, ctx->eglPbuffer, ctx->eglPbuffer, ctx->eglContext);
}

void mirrorDestroy(XrCtx* ctx) {
    takePending(ctx);
    dropSurface(ctx);
    if (program != 0) { glDeleteProgram(program); program = 0; }
    seenCount = 0;
}
