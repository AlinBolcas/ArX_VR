package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.os.Process;
import android.preference.PreferenceManager;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import static com.limelight.binding.video.XrShared.*;

/**
 * Presents the decoded stream in an OpenXR session. Same input contract as
 * GlPassthroughRenderer: the decoder renders into our SurfaceTexture, and we
 * consume it from the frame loop thread. All OpenXR work happens in native
 * code, this class owns the thread and the SurfaceTexture plumbing.
 */
public class XrRenderer implements SurfaceTexture.OnFrameAvailableListener {

    static {
        System.loadLibrary("xr-renderer");
    }

    // Averaged over this many inferences before hitting logcat
    private static final int DEPTH_STATS_INTERVAL = 30;
    private static final int DEPTH_AGE_INTERVAL = 300;

    private static final float OVERLAY_TEXT_SIZE = 22.0f;
    private static final float OVERLAY_LINE_HEIGHT = 28.0f;

    // Written by the frame loop thread and read by whichever thread reports the
    // stats, so the write has to be visible across them
    private volatile long nativeCtx;
    // Held around every native call made off the frame loop, and by the frame
    // loop while it frees the context, so no thread can reach a context that
    // is halfway through being destroyed
    private final Object nativeLock = new Object();
    private Thread renderThread;
    private Thread depthThread;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;
    // The frame loop reads the SurfaceTexture every frame, so it cannot be
    // released out from under it. If cleanup arrives while the loop is still
    // running it leaves a note instead, and the loop releases both on its way
    // out.
    private final Object teardownLock = new Object();
    private boolean renderThreadDone;
    private boolean releaseOnExit;

    private final AtomicInteger pendingFrames = new AtomicInteger(0);
    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;
    private long videoFrameIndex;

    // Handoff to the depth thread. The frame loop fills the model input and
    // sets pending, the depth thread runs inference and uploads the result.
    // If it is still busy when the next frame is due, the frame loop skips
    // rather than waits, so depth just runs at whatever rate it manages.
    private final Object depthLock = new Object();
    private boolean depthPending;
    private boolean depthBusy;
    private boolean depthExit;
    private int skippedFrames;
    private volatile boolean depthReady;
    private volatile long lastCaptureNs;

    // How far behind the picture the depth map is. The map warping a frame was
    // computed from an earlier one, and then reused until the next inference
    // lands, so during camera motion it is spatially offset from the colour it
    // is warping. Measured rather than assumed: these are the frame index and
    // clock reading of the frame the live depth map came from.
    private long captureFrameIndex;
    private long captureFrameNs;
    private volatile long publishedFrameIndex;
    private volatile long publishedFrameNs;

    // Stats overlay. Text is drawn to a bitmap on whichever thread reports the
    // stats, then handed to the frame loop, which owns the GL context. Two
    // buffers so the drawing side never writes one the renderer is reading.
    private final AtomicReference<ByteBuffer> pendingOverlay = new AtomicReference<>();
    private ByteBuffer[] overlayBuffers;
    private int overlayBufferIndex;
    private Bitmap overlayBitmap;
    private Canvas overlayCanvas;
    private Paint overlayPaint;
    private volatile float lastInferenceMs;
    private volatile float lastDepthAgeMs;
    private volatile int lastDepthSkips;

    // Controller pointer. The native side does the ray maths and hands back a
    // hit point and a button mask, this side turns that into host events. The
    // slots in that array and the ids the panel reports are the IN_ and
    // SETTING_ values in XrShared, so both sides read them off the same file.
    private final float[] inputState = new float[IN_SLOTS];
    private int heldButtons;
    private boolean voiceHeld;
    private InputListener inputListener;
    private Context prefsContext;
    private PreferenceConfiguration prefConfig;

    // The 360 photo shown behind the screen. Decoded off the frame loop and
    // picked up whenever it is ready, so a slow decode cannot delay the first
    // frame and hang the shell on its loading screen.
    private final AtomicReference<ByteBuffer> pendingBackground = new AtomicReference<>();
    private volatile int backgroundWidth;
    private volatile int backgroundHeight;

    // The baked room that ships with the app, mesh and texture atlas
    private static final String ROOM_DIR = "rooms";
    private static final String ROOM_MESH_FILE = "psx_cinema.room";
    private static final String ROOM_TEXTURE_FILE = "psx_cinema.png";
    // The layout as it shipped before the bands, kept only to read an old saved
    // cell as the environment it meant at the time
    private static final int[] LEGACY_CELL_IDS = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH,
            PreferenceConfiguration.VR_ENV_VOID,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 1,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 2,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 3,
            PreferenceConfiguration.VR_ENV_MINIMAL_ROOM,
            PreferenceConfiguration.VR_ENV_PSX_CINEMA,
    };

    // Panel art on its way to the GPU. XrPanels draws it on the loader thread
    // and it waits here for the frame loop, which owns the GL context.
    private final AtomicReference<ByteBuffer> pendingKbLower = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbUpper = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbSymbols = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbButton = new AtomicReference<>();
    // Built next to the art and read on the frame loop when it uploads
    private volatile float[] kbKeyRects;
    private volatile int[] kbCodesLower;
    private volatile int[] kbCodesUpper;
    private volatile int[] kbCodesSymbols;

    private final AtomicReference<ByteBuffer> pendingExitButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitPlain = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitHot = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCancelHot = new AtomicReference<>();

    private final AtomicReference<ByteBuffer> pendingPickerArt = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingEnvButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingDock = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingGuide = new AtomicReference<>();
    private volatile boolean guideOnFirstRun;
    private final AtomicReference<ByteBuffer> pendingCogScreenTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogDisplayTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCog3dTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogRoomTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockShut = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockOpen = new AtomicReference<>();
    // The baked room, read on the same thread as the art above. The native side
    // shows the minimal room in its place until both of these have landed.
    private final AtomicReference<ByteBuffer> pendingRoomMesh = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingRoomTexture = new AtomicReference<>();
    private volatile int roomMeshBytes;
    private volatile int roomTextureWidth;
    private volatile int roomTextureHeight;
    private String[] environmentFiles = new String[0];
    // The splat world the native side holds, so picking it again costs nothing
    private volatile String loadedWorld;
    // Worlds shown as their 360 panorama instead of splats: picking the world you are
    // already in switches it, and it is remembered
    private static final String WORLD_PANORAMA_PREF = "arxvr_world_panorama";
    private volatile boolean worldPanorama;
    // Each world's ambient loop, quiet under everything else
    private final XrAmbient ambient = new XrAmbient();
    private XrPanels panels;
    private volatile int environmentChoice = ENV_CELL_VOID;
    private volatile boolean passthroughOn;
    // Which photo is in the background swapchain, so switching back to one
    // already loaded costs nothing and the old one stays up during a decode
    private volatile int loadedPhoto = -1;
    private volatile int pendingPhoto = -1;
    private volatile boolean backgroundArrived;
    private final AtomicInteger photoRequest = new AtomicInteger();

    /**
     * Pointer events out of the VR session. Called on the frame loop thread.
     * Buttons are 0 left, 1 right, 2 middle.
     */
    public interface InputListener {
        void onVrPointerMove(float u, float v);
        // The pointer on any desktop, 0 the main one. Buttons and scroll are shared,
        // so a press held across two desktops carries a window from one to the other.
        default void onVrPointer(int screen, float u, float v) { if (screen == 0) onVrPointerMove(u, v); }
        // Sideways scroll clicks, positive to the right
        default void onVrHScroll(int clicks) { }
        // Dictation held down (B, Y or the ring finger to the thumb) and let go
        default void onVrVoice(boolean down) { }
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // A key from the in world keyboard. Unicode with the shift already
        // applied, or backspace, tab, enter and space as their control codes.
        void onVrKey(int code);
        // The exit prompt was confirmed, so the session is to end
        void onVrExit();
        void onVrWorkspace(int slot, float u, float v, int buttons, int scroll, int action);
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    /**
     * Told when a VR session could not be started at all, so the activity can
     * do something visible about it rather than stream into a window the
     * headset's shell never shows. Called off the main thread.
     */
    public interface SessionListener {
        void onVrUnavailable();
    }

    private static native void nativeSetFileLog(String path, int level);
    // envResTier is the EnvResTier the room renders at: 0 low, 1 standard,
    // 2 high, 3 ultra
    private native long nativeInit(Activity activity, int width, int height, int stereoMode,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, boolean perfOverlay,
                                   boolean ambilight, int ambiLevel, boolean roomLight,
                                   int envResTier);
    private final java.util.concurrent.atomic.AtomicBoolean arxShowFeedback = new java.util.concurrent.atomic.AtomicBoolean();
    private native void nativeShowFeedback(long ctx, boolean visible);
    private native void nativeUploadDock(long ctx, ByteBuffer art, float[] rects, int[] codes);
    private native void nativeUploadGuide(long ctx, ByteBuffer art, boolean open);
    private native void nativeUploadPie(long ctx, ByteBuffer art, int[] codes);
    private native void nativeSetMirror(long ctx, Surface surface, int width, int height);

    /** The encoder surface the Mac's mirror window is fed from, or null to stop drawing into it. */
    public void setMirror(Surface surface, int width, int height) {
        if (nativeCtx != 0) nativeSetMirror(nativeCtx, surface, width, height);
    }
    private final AtomicReference<ByteBuffer> pendingPie = new AtomicReference<>();
    // The slice lit last time the pie was drawn, so it is redrawn only when that changes
    private int pieHover = -1;

    private void redrawPie() {
        if (panels != null) pendingPie.set(panels.buildPie(pieHover, dockDictating, dockExitArmed,
                prefConfig != null && prefConfig.vrHeadLocked));
    }

    /** The Head lock slice: every screen follows the head, or stops following it. Returns the new state. */
    public boolean toggleHeadLock() {
        boolean on = !(prefConfig != null && prefConfig.vrHeadLocked);
        applySetting(SETTING_HEAD_LOCK, on ? 1 : 0);
        redrawPie();
        return on;
    }
    private volatile boolean arxFeedbackVisible;

    private volatile boolean dockDictating, dockExitArmed;
    // Relights the dock's Dictate action while the microphone is recording
    public void setDictating(boolean on) {
        dockDictating = on;
        if (panels != null) pendingDock.set(panels.buildDock(dockDictating, dockExitArmed));
        redrawPie();
    }
    // Shows "Tap again" on Exit while the renderer holds it armed
    public void setExitArmed(boolean on) {
        dockExitArmed = on;
        if (panels != null) pendingDock.set(panels.buildDock(dockDictating, dockExitArmed));
        redrawPie();
    }

    // The status pill in the screen's bottom left corner: one short line of status
    public void showArxFeedback(String text) {
        arxFeedbackVisible = !text.isEmpty();
        drawPill(text, null);
        arxShowFeedback.set(true);
    }
    // Or, while dictating, the microphone's level as a live waveform, newest on the right
    public void showArxLevels(float[] levels) {
        arxFeedbackVisible = true;
        drawPill(null, levels);
        arxShowFeedback.set(true);
    }

    private final Paint pillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final android.graphics.RectF pillRect = new android.graphics.RectF();
    private int pillBuffer;
    private synchronized void drawPill(String text, float[] levels) {
        if (nativeCtx == 0) return;
        if (overlayBitmap == null) {
            overlayBitmap = Bitmap.createBitmap(OVERLAY_WIDTH, OVERLAY_HEIGHT, Bitmap.Config.ARGB_8888);
            overlayCanvas = new Canvas(overlayBitmap);
        }
        // A few buffers taken in turn: the frame loop has picked one up long before it
        // comes round again, and the waveform redraws twenty times a second
        if (overlayBuffers == null || overlayBuffers.length < 3) {
            overlayBuffers = new ByteBuffer[3];
            for (int i = 0; i < 3; i++) overlayBuffers[i] = ByteBuffer.allocateDirect(OVERLAY_WIDTH * OVERLAY_HEIGHT * 4).order(ByteOrder.nativeOrder());
        }
        Canvas c = overlayCanvas;
        float w = OVERLAY_WIDTH, h = OVERLAY_HEIGHT, round = (h - 8) * 0.5f;
        c.drawColor(0, PorterDuff.Mode.CLEAR);
        // Texture rows run bottom up, so draw mirrored and let the upload put it back
        c.save();
        c.translate(0.0f, h);
        c.scale(1.0f, -1.0f);
        Paint p = pillPaint;
        pillRect.set(4, 4, w - 4, h - 4);
        p.setStyle(Paint.Style.FILL); p.setColor(0xE60C1118);
        c.drawRoundRect(pillRect, round, round, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2.0f); p.setColor(0xFF2A3544);
        c.drawRoundRect(pillRect, round, round, p);
        p.setStyle(Paint.Style.FILL);
        p.setColor(levels != null ? 0xFFD6453A : 0xFF6F9BDB);
        c.drawCircle(h * 0.5f, h * 0.5f, 11.0f, p);
        float left = h * 0.85f, right = w - h * 0.4f;
        p.setColor(0xFFE8EDF4);
        if (levels != null) {
            float step = (right - left) / levels.length, bar = step * 0.55f, mid = h * 0.5f, tallest = h - 44.0f;
            for (int i = 0; i < levels.length; i++) {
                float half = Math.max(3.0f, levels[i] * tallest * 0.5f), x = left + i * step;
                pillRect.set(x, mid - half, x + bar, mid + half);
                c.drawRoundRect(pillRect, bar * 0.5f, bar * 0.5f, p);
            }
        }
        else {
            p.setTextSize(38.0f);
            p.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            String line = text.split("\n")[0];
            if (p.measureText(line) > right - left) {
                while (line.length() > 1 && p.measureText(line + "...") > right - left) line = line.substring(0, line.length() - 1);
                line = line.trim() + "...";
            }
            Paint.FontMetrics metrics = p.getFontMetrics();
            c.drawText(line, left, h * 0.5f - (metrics.ascent + metrics.descent) * 0.5f, p);
        }
        c.restore();
        ByteBuffer buf = overlayBuffers[pillBuffer];
        pillBuffer = (pillBuffer + 1) % overlayBuffers.length;
        buf.rewind();
        overlayBitmap.copyPixelsToBuffer(buf);
        buf.rewind();
        pendingOverlay.set(buf);
    }

    // Extra desktops, 1 to ARX_MAX_EXTRA. Each gets its own SurfaceTexture on an
    // OES texture the frame loop makes, fed by its own decoder, exactly like the
    // main picture. Created and destroyed on the frame loop, which owns the GL context.
    private static final class Desktop {
        final int slot, width, height;
        final java.util.concurrent.CompletableFuture<Surface> ready = new java.util.concurrent.CompletableFuture<>();
        final AtomicInteger pending = new AtomicInteger();
        SurfaceTexture texture;
        Surface input;
        Desktop(int slot, int width, int height) { this.slot = slot; this.width = width; this.height = height; }
        void release() {
            if (input != null) input.release();
            if (texture != null) texture.release();
        }
    }
    private final Desktop[] desktops = new Desktop[ARX_MAX_EXTRA + 1];
    private final java.util.concurrent.ConcurrentLinkedQueue<Desktop> desktopRequests = new java.util.concurrent.ConcurrentLinkedQueue<>();
    private final java.util.concurrent.ConcurrentLinkedQueue<Integer> desktopRemovals = new java.util.concurrent.ConcurrentLinkedQueue<>();
    private final float[] desktopMatrix = new float[16];
    private native int nativeDesktopCreate(long ctx, int slot, int width, int height);
    private native void nativeDesktopFrame(long ctx, int slot, float[] texMatrix);
    private native void nativeDesktopDestroy(long ctx, int slot, boolean forget);
    private native void nativeUploadClose(long ctx, ByteBuffer art);
    private final AtomicReference<ByteBuffer> pendingClose = new AtomicReference<>();

    /** Opens a desktop at the stream's size, placed beside the others. The surface its decoder renders into, or null. */
    public Surface addDesktop(int slot, int width, int height) {
        if (slot < 1 || slot > ARX_MAX_EXTRA) return null;
        Desktop desktop = new Desktop(slot, width, height);
        desktopRequests.add(desktop);
        try { return desktop.ready.get(3, TimeUnit.SECONDS); }
        catch (Exception timedOut) { removeDesktop(slot, false); return null; }
    }

    // forget: its place in the room goes too; otherwise it comes back where it was
    public void removeDesktop(int slot, boolean forget) {
        if (slot >= 1 && slot <= ARX_MAX_EXTRA) desktopRemovals.add(forget ? -slot : slot);
    }

    // On the frame loop: removals, then new desktops, then every desktop with a new frame
    private void serviceDesktops() {
        Integer gone;
        while ((gone = desktopRemovals.poll()) != null) {
            int slot = Math.abs(gone);
            Desktop desktop = desktops[slot];
            desktops[slot] = null;
            nativeDesktopDestroy(nativeCtx, slot, gone < 0);
            if (desktop != null) desktop.release();
        }
        Desktop wanted;
        while ((wanted = desktopRequests.poll()) != null) {
            Desktop previous = desktops[wanted.slot];
            if (previous != null) previous.release();
            int texture = nativeDesktopCreate(nativeCtx, wanted.slot, wanted.width, wanted.height);
            if (texture == 0) { desktops[wanted.slot] = null; wanted.ready.complete(null); continue; }
            final Desktop desktop = wanted;
            desktop.texture = new SurfaceTexture(texture);
            desktop.texture.setDefaultBufferSize(desktop.width, desktop.height);
            desktop.texture.setOnFrameAvailableListener(st -> desktop.pending.incrementAndGet());
            desktop.input = new Surface(desktop.texture);
            desktops[desktop.slot] = desktop;
            desktop.ready.complete(desktop.input);
        }
        for (Desktop desktop : desktops) {
            if (desktop == null || desktop.pending.getAndSet(0) == 0) continue;
            desktop.texture.updateTexImage();
            desktop.texture.getTransformMatrix(desktopMatrix);
            nativeDesktopFrame(nativeCtx, desktop.slot, desktopMatrix);
        }
    }
    private native void nativeSetCaptureDir(long ctx, String dir);
    private native void nativeSetLayoutFile(long ctx, String path);
    private native int nativeGetTexId(long ctx);
    private native ByteBuffer nativeGetModelInput(long ctx);
    private native ByteBuffer nativeGetModelOutput(long ctx);
    private native long nativeCaptureDepthInput(long ctx, float[] texMatrix);
    private native long nativeFinishDepthCapture(long ctx);
    private native long nativeUploadDepth(long ctx);
    private native boolean nativeBindDepthContext(long ctx);
    private native void nativeUnbindDepthContext(long ctx);
    private native int nativeWaitBeginFrame(long ctx);
    private native void nativeEndFrame(long ctx, boolean newFrame, float[] texMatrix,
                                       float distance, float quadWidth, float curvature,
                                       boolean headLocked, float separation, boolean eyeSwap,
                                       boolean passthrough);
    private native void nativeUpdateInput(long ctx, float distance, float quadWidth,
                                          float curvature, boolean headLocked,
                                          boolean pointerEnabled, boolean gazeEnabled,
                                          float[] out);
    private native void nativeSetScreenPose(long ctx, float[] pose);
    private native void nativeUploadBackground(long ctx, ByteBuffer pixels, int width, int height);
    private native void nativeUploadRoomModel(long ctx, ByteBuffer mesh, int length);
    private native void nativeUploadRoomTexture(long ctx, ByteBuffer pixels, int width, int height);
    private native boolean nativeUploadSplats(long ctx, ByteBuffer splats, int length);
    private native void nativeUploadPicker(long ctx, ByteBuffer grid, ByteBuffer button);
    private native void nativeUploadCog(long ctx, ByteBuffer screenTab, ByteBuffer displayTab,
                                        ByteBuffer tab3d, ByteBuffer roomTab, ByteBuffer button);
    private native void nativeUploadKeyboard(long ctx, ByteBuffer lower, ByteBuffer upper,
                                             ByteBuffer symbols, ByteBuffer buttonIcon,
                                             float[] keyRects, int[] codesLower,
                                             int[] codesUpper, int[] codesSymbols);
    private native void nativeUploadExit(long ctx, ByteBuffer button, ByteBuffer promptPlain,
                                         ByteBuffer promptExitHot, ByteBuffer promptCancelHot);
    private native boolean nativeGetCylinderSupported(long ctx);
    private native void nativeUploadLock(long ctx, ByteBuffer shut, ByteBuffer open);
    private native void nativeSetEnvironment(long ctx, int choice, boolean backgroundOn, int roomKind);
    private native void nativeUploadOverlay(long ctx, ByteBuffer pixels, int width, int height);
    private native float nativeGetWarpGpuMs(long ctx);
    private native void nativeDestroy(long ctx);

    public boolean start(final Activity activity, final int videoWidth, final int videoHeight,
                         final PreferenceConfiguration prefs) {
        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        renderThread = new Thread() {
            @Override
            public void run() {
                try {
                    runSession();
                } finally {
                    finishRenderThread();
                }
            }

            private void runSession() {
                // Submission has to land inside the compositor's frame window,
                // so this thread cannot sit behind the decoder or the depth
                // worker the way an unprioritised thread would. Thread's own
                // setPriority only changes the JVM's bookkeeping, not the
                // Linux scheduler, so the real call goes through Process.
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);

                // Before init, so everything the session setup finds ends up
                // in the log too
                nativeSetFileLog(FileLog.getLogPath(), FileLog.getLevel());

                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.vrDepthMode,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.enablePerfOverlay,
                        prefs.vrAmbilight, prefs.vrAmbilightLevel, prefs.vrRoomLight,
                        prefs.vrEnvResTier);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
                }

                prefsContext = activity.getApplicationContext();
                // Held on to rather than only read here: the stats toggle on
                // the panel writes back to this same instance, which is the one
                // the decoder's stats path checks
                prefConfig = prefs;
                restoreScreenPose();
                startEnvironment(prefs);

                // Where you left each desktop, so a restart or an update puts them back
                nativeSetLayoutFile(nativeCtx, new File(activity.getFilesDir(), "desktop_layout.txt").getAbsolutePath());

                File captureDir = activity.getExternalFilesDir(null);
                if (captureDir != null) {
                    nativeSetCaptureDir(nativeCtx, captureDir.getAbsolutePath());
                }

                // The EGL context is current on this thread now, so the
                // SurfaceTexture attaches to it here
                surfaceTexture = new SurfaceTexture(nativeGetTexId(nativeCtx));
                surfaceTexture.setDefaultBufferSize(videoWidth, videoHeight);
                surfaceTexture.setOnFrameAvailableListener(XrRenderer.this);
                inputSurface = new Surface(surfaceTexture);

                if (prefs.vrDepthMode == DEPTH_MODE_MODEL) {
                    startDepthThread(activity);
                }

                initOk[0] = true;
                initLatch.countDown();

                runFrameLoop(prefs);

                stopDepthThread();

                // Tear down on the same thread that owns the GL context, and
                // under the lock so a stats report cannot land on a context
                // that is halfway through being freed. The SurfaceTexture
                // and Surface stay alive for the codec until cleanup().
                synchronized (nativeLock) {
                    long ctx = nativeCtx;
                    nativeCtx = 0;
                    nativeDestroy(ctx);
                }
            }
        };
        renderThread.setName("Video - XR Renderer");
        renderThread.start();

        boolean initFinished;
        try {
            // Session setup can take a moment on a cold runtime
            initFinished = initLatch.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            initFinished = false;
        }

        if (!initFinished || !initOk[0]) {
            LimeLog.severe("XR renderer init failed");
            prepareForStop();
            cleanup();
            return false;
        }

        LimeLog.info("XR renderer initialized at "+videoWidth+"x"+videoHeight);
        return true;
    }

    /**
     * Inference is longer than a display frame, so it lives on its own
     * thread with its own context in the render context's share group. The
     * frame loop hands over a captured frame and carries on submitting.
     */
    private void startDepthThread(final Activity activity) {
        depthThread = new Thread() {
            @Override
            public void run() {
                // A little above the default so a busy system does not starve
                // inference behind everything else, but deliberately not
                // BACKGROUND: that cpuset is little cores only on this SoC and
                // would make a model run slower in wall clock, not faster
                Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);

                if (!nativeBindDepthContext(nativeCtx)) {
                    return;
                }

                DepthSource source = null;
                try {
                    ByteBuffer input = nativeGetModelInput(nativeCtx);
                    ByteBuffer output = nativeGetModelOutput(nativeCtx);
                    if (input == null || output == null) {
                        LimeLog.severe("Depth staging buffers missing");
                        return;
                    }

                    source = new MidasDepthSource();
                    if (!source.initialize(activity, input, output)) {
                        // The depth texture keeps the flat map it was
                        // initialized with, so zero disparity, and the
                        // stream stays watchable
                        LimeLog.severe("Depth source init failed, stereo will be flat");
                        return;
                    }

                    depthReady = true;
                    runDepthLoop(source);
                } finally {
                    depthReady = false;
                    if (source != null) {
                        source.release();
                    }
                    nativeUnbindDepthContext(nativeCtx);
                }
            }
        };
        depthThread.setName("Video - XR Depth");
        depthThread.start();
    }

    private void runDepthLoop(DepthSource source) {
        long runs = 0, skipped = 0;
        long inferenceNs = 0, uploadNs = 0, captureNs = 0, worstNs = 0;

        while (true) {
            synchronized (depthLock) {
                while (!depthPending && !depthExit) {
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
                if (depthExit) {
                    return;
                }
                depthPending = false;
                depthBusy = true;
            }

            long start = System.nanoTime();
            long upload = 0;
            // The frame loop only queued the readback. This is where it is
            // waited on and turned into the model input, on the thread that
            // has no frame to miss.
            long finish = nativeFinishDepthCapture(nativeCtx);
            boolean ok = finish >= 0 && source.estimate();
            if (ok) {
                upload = nativeUploadDepth(nativeCtx);
                publishedFrameIndex = captureFrameIndex;
                publishedFrameNs = captureFrameNs;
            }

            synchronized (depthLock) {
                depthBusy = false;
                skipped += skippedFrames;
                skippedFrames = 0;
            }

            if (!ok) {
                continue;
            }

            captureNs += lastCaptureNs + finish;
            inferenceNs += (long)(source.getLastInferenceMs() * 1000000.0f);
            lastInferenceMs = source.getLastInferenceMs();
            uploadNs += upload;
            long total = System.nanoTime() - start;
            if (total > worstNs) {
                worstNs = total;
            }
            if (++runs == DEPTH_STATS_INTERVAL) {
                LimeLog.info("Depth stage ("+(source.isGpuAccelerated() ? "GPU" : "CPU")
                        +"): capture "+msPer(captureNs, runs)
                        +" ms, inference "+msPer(inferenceNs, runs)
                        +" ms, upload "+msPer(uploadNs, runs)
                        +" ms, worst "+msPer(worstNs, 1)
                        +" ms, frames skipped while busy "+skipped);
                lastDepthSkips = (int)skipped;
                runs = 0;
                skipped = 0;
                captureNs = inferenceNs = uploadNs = worstNs = 0;
            }
        }
    }

    private void stopDepthThread() {
        if (depthThread == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        // The context is freed the moment this returns, and the thread uses
        // it, so a slow inference is waited out however long it takes rather
        // than left running on memory that is about to go
        boolean interrupted = false;
        try {
            depthThread.join(2000);
        } catch (InterruptedException e) {
            interrupted = true;
        }
        if (depthThread.isAlive()) {
            LimeLog.warning("XR depth thread did not stop in time, waiting for it");
            while (depthThread.isAlive()) {
                try {
                    depthThread.join();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        depthThread = null;
    }

    private void runFrameLoop(PreferenceConfiguration prefs) {
        float distance = prefs.vrDistance / 10.0f;
        float quadWidth = prefs.vrScreenSize / 10.0f;
        float curvature = prefs.vrCurvature / 100.0f;
        // Stored as tenths of a percent of frame width
        float separation = prefs.vrStereoSeparation / 1000.0f;
        boolean eyeSwap = prefs.vrEyeSwap;
        boolean pointer = true;
        boolean gaze = prefs.vrGaze;
        int cadence = Math.max(1, prefs.vrInferenceCadence);

        long ageFrames = 0, ageNs = 0, ageSamples = 0, worstAgeNs = 0;

        while (!stopping) {
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                break;
            }
            if (r == FRAME_IDLE) {
                // Native side slept already while the session is not running
                continue;
            }

            // Read fresh each frame rather than once on the way in: the panel's
            // row writes it back to this same object, and the space is picked
            // from it on both sides of the frame, so a press takes effect on
            // the next one with no native state to keep in step.
            boolean headLocked = prefs.vrHeadLocked;

            serviceDesktops();
            if (arxShowFeedback.getAndSet(false)) nativeShowFeedback(nativeCtx, arxFeedbackVisible);
            nativeUpdateInput(nativeCtx, distance, quadWidth, curvature, headLocked,
                    pointer, gaze, inputState);
            dispatchInput();

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady) {
                    if ((videoFrameIndex % cadence) == 0) {
                        startDepthCapture();
                    }
                    if (publishedFrameNs != 0) {
                        long age = System.nanoTime() - publishedFrameNs;
                        // Smoothed for the overlay, the raw value swings a lot
                        // between one inference landing and the next
                        float ageMs = age / 1000000.0f;
                        lastDepthAgeMs = lastDepthAgeMs == 0.0f ? ageMs
                                : lastDepthAgeMs * 0.95f + ageMs * 0.05f;
                        ageFrames += videoFrameIndex - publishedFrameIndex;
                        ageNs += age;
                        ageSamples++;
                        if (age > worstAgeNs) {
                            worstAgeNs = age;
                        }
                        if (ageSamples == DEPTH_AGE_INTERVAL) {
                            LimeLog.info("Depth age: "+String.format("%.1f", ageFrames
                                    / (double)ageSamples)+" video frames, "
                                    +msPer(ageNs, ageSamples)+" ms avg, "
                                    +msPer(worstAgeNs, 1)+" ms worst");
                            ageFrames = ageNs = ageSamples = worstAgeNs = 0;
                        }
                    }
                }
                videoFrameIndex++;
            }
            // Upload here rather than from the reporting thread, since this is
            // the thread that owns the GL context
            ByteBuffer overlay = pendingOverlay.getAndSet(null);
            if (overlay != null) {
                nativeUploadOverlay(nativeCtx, overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT);
            }

            ByteBuffer dock = pendingDock.getAndSet(null);
            if (dock != null) {
                nativeUploadDock(nativeCtx, dock, XrPanels.DOCK_RECTS, XrPanels.DOCK_CODES);
            }
            ByteBuffer pie = pendingPie.getAndSet(null);
            if (pie != null) {
                nativeUploadPie(nativeCtx, pie, XrPanels.PIE_CODES);
            }
            ByteBuffer close = pendingClose.getAndSet(null);
            if (close != null) {
                nativeUploadClose(nativeCtx, close);
            }
            ByteBuffer guide = pendingGuide.getAndSet(null);
            if (guide != null) {
                nativeUploadGuide(nativeCtx, guide, guideOnFirstRun);
            }

            ByteBuffer screenTab = pendingCogScreenTab.getAndSet(null);
            ByteBuffer displayTab = pendingCogDisplayTab.getAndSet(null);
            ByteBuffer tab3d = pendingCog3dTab.getAndSet(null);
            ByteBuffer roomTab = pendingCogRoomTab.getAndSet(null);
            ByteBuffer cog = pendingCogButton.getAndSet(null);
            if (screenTab != null || displayTab != null || tab3d != null
                    || roomTab != null || cog != null) {
                nativeUploadCog(nativeCtx, screenTab, displayTab, tab3d, roomTab, cog);
            }

            ByteBuffer kbLower = pendingKbLower.getAndSet(null);
            ByteBuffer kbUpper = pendingKbUpper.getAndSet(null);
            ByteBuffer kbSymbols = pendingKbSymbols.getAndSet(null);
            ByteBuffer kbButton = pendingKbButton.getAndSet(null);
            if (kbLower != null || kbUpper != null || kbSymbols != null || kbButton != null) {
                nativeUploadKeyboard(nativeCtx, kbLower, kbUpper, kbSymbols, kbButton,
                        kbKeyRects, kbCodesLower, kbCodesUpper, kbCodesSymbols);
            }

            ByteBuffer exitButton = pendingExitButton.getAndSet(null);
            ByteBuffer exitPlain = pendingExitPlain.getAndSet(null);
            ByteBuffer exitHot = pendingExitHot.getAndSet(null);
            ByteBuffer cancelHot = pendingCancelHot.getAndSet(null);
            if (exitButton != null || exitPlain != null || exitHot != null || cancelHot != null) {
                nativeUploadExit(nativeCtx, exitButton, exitPlain, exitHot, cancelHot);
            }

            ByteBuffer shut = pendingLockShut.getAndSet(null);
            ByteBuffer open = pendingLockOpen.getAndSet(null);
            if (shut != null && open != null) {
                nativeUploadLock(nativeCtx, shut, open);
            }

            // The World grid and its button. Drawn once at start; the grid holds the worlds on the headset.
            ByteBuffer grid = pendingPickerArt.getAndSet(null);
            ByteBuffer envButton = pendingEnvButton.getAndSet(null);
            if (grid != null || envButton != null) {
                nativeUploadPicker(nativeCtx, grid, envButton);
            }

            ByteBuffer roomMesh = pendingRoomMesh.getAndSet(null);
            if (roomMesh != null) {
                nativeUploadRoomModel(nativeCtx, roomMesh, roomMeshBytes);
            }
            ByteBuffer roomTexture = pendingRoomTexture.getAndSet(null);
            if (roomTexture != null) {
                nativeUploadRoomTexture(nativeCtx, roomTexture, roomTextureWidth,
                        roomTextureHeight);
            }

            ByteBuffer background = pendingBackground.getAndSet(null);
            if (background != null) {
                nativeUploadBackground(nativeCtx, background, backgroundWidth, backgroundHeight);
                loadedPhoto = pendingPhoto;
                backgroundArrived = true;
                // Only now is there something to show, so this is where a
                // freshly picked environment actually comes up
                nativeSetEnvironment(nativeCtx, environmentChoice, backgroundVisible(), roomKindFor(environmentChoice));
            }

            nativeEndFrame(nativeCtx, newFrame, texMatrix, distance, quadWidth, curvature,
                    headLocked, separation, eyeSwap, passthroughOn);
        }
    }

    /**
     * Settles on a starting environment, then hands the slow half to another
     * thread: a 4096x2048 photo takes long enough to decode that doing it here
     * would hold up the first frame and hang the shell on its loading screen.
     */
    private void startEnvironment(PreferenceConfiguration prefs) {
        // Your own room and Calm, then the splat worlds on the headset in the cells after
        // them, a folder name each with the marking slash on the end
        String[] worlds = XrPanels.listWorlds(prefsContext);
        environmentFiles = new String[Math.min(worlds.length, XrPanels.MAX_PHOTOS)];
        for (int i = 0; i < environmentFiles.length; i++) {
            environmentFiles[i] = worlds[i].isEmpty() ? "" : worlds[i] + "/";
        }
        panels = new XrPanels(prefsContext, environmentFiles);
        worldPanorama = PreferenceManager.getDefaultSharedPreferences(prefsContext)
                .getBoolean(WORLD_PANORAMA_PREF, false);
        panels.worldPanorama = worldPanorama;

        SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        int id = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, -1);
        if (id < 0) {
            // An install from before the ids has a cell instead, which only
            // means anything read against the layout it was written under. The
            // old key is left where it is, since nothing costs less than a
            // stale int and an older build can still start on it.
            int legacy = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, -1);
            if (legacy >= 0 && legacy < LEGACY_CELL_IDS.length) {
                id = LEGACY_CELL_IDS[legacy];
                saved.edit()
                        .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, id)
                        .apply();
            }
        }

        // Whatever was chosen last time, and your own room until something else is
        int cell = cellForId(id);
        if (!cellExists(cell)) cell = ENV_CELL_PASSTHROUGH;
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;
        nativeSetEnvironment(nativeCtx, cell, backgroundVisible(), roomKindFor(cell));
        if (cell >= ENV_CELL_FIRST_PHOTO) {
            final int entry = cell - ENV_CELL_FIRST_PHOTO;
            new Thread(() -> loadEntry(entry), "Video - XR Environment").start();
        }


        Thread loader = new Thread() {
            @Override
            public void run() {
                buildPanelArt();
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    // Every panel, drawn once and parked for the frame loop
    private void buildPanelArt() {
        pendingDock.set(panels.buildDock(false, false));
        pendingPie.set(panels.buildPie(-1, false, false));
        pendingPickerArt.set(panels.buildPickerGrid());
        pendingEnvButton.set(panels.buildEnvButton());
        pendingClose.set(panels.buildClose());
        SharedPreferences seen = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        // Versioned with the guide's content, so a new vocabulary opens once for everyone
        guideOnFirstRun = !seen.getBoolean("arx_guide_seen_controls_6", false);
        if (guideOnFirstRun) seen.edit().putBoolean("arx_guide_seen_controls_6", true).apply();
        pendingGuide.set(panels.buildGuide());
        ByteBuffer[] locks = panels.buildLockIcons();
        if (locks != null) {
            pendingLockShut.set(locks[0]);
            pendingLockOpen.set(locks[1]);
        }

        // Curvature needs a layer type the runtime may not offer, and a slider
        // that cannot do anything is better shown greyed than hidden
        boolean curveOk;
        synchronized (nativeLock) {
            curveOk = nativeCtx != 0 && nativeGetCylinderSupported(nativeCtx);
        }
        // Same for the 3D rows with stereo turned off in settings
        boolean stereoOk = prefConfig != null && prefConfig.vrDepthMode != DEPTH_MODE_OFF;
        ByteBuffer[] tabs = panels.buildCogTabs(curveOk, stereoOk);
        pendingCogScreenTab.set(tabs[0]);
        pendingCogDisplayTab.set(tabs[1]);
        pendingCog3dTab.set(tabs[2]);
        pendingCogRoomTab.set(tabs[3]);
        pendingCogButton.set(panels.buildCogButton());

        XrPanels.Keyboard keyboard = panels.buildKeyboard();
        kbKeyRects = keyboard.keyRects;
        kbCodesLower = keyboard.codesLower;
        kbCodesUpper = keyboard.codesUpper;
        kbCodesSymbols = keyboard.codesSymbols;
        pendingKbLower.set(keyboard.lower);
        pendingKbUpper.set(keyboard.upper);
        pendingKbSymbols.set(keyboard.symbols);
        pendingKbButton.set(keyboard.button);

        ByteBuffer[] exit = panels.buildExitArt();
        pendingExitButton.set(exit[0]);
        pendingExitPlain.set(exit[1]);
        pendingExitHot.set(exit[2]);
        pendingCancelHot.set(exit[3]);
    }

    // A cell that is a fully 3d room rather than a photo or a plain background
    private boolean isRoomCell(int cell) {
        return cell == ENV_CELL_MINIMAL_ROOM || cell == ENV_CELL_PSX_CINEMA || isWorldCell(cell);
    }

    // A generated world: a baked dome on the headset rather than a picture at infinity
    private boolean isWorldCell(int cell) {
        int entry = cell - ENV_CELL_FIRST_PHOTO;
        return entry >= 0 && entry < environmentFiles.length && environmentFiles[entry].endsWith("/");
    }

    private String worldName(int cell) {
        String entry = environmentFiles[cell - ENV_CELL_FIRST_PHOTO];
        return entry.substring(0, entry.length() - 1);
    }

    // What kind of room the renderer should be drawing for this cell, if any
    private int roomKindFor(int cell) {
        if (cell == ENV_CELL_VOID) return ROOM_STYLE_CALM;
        // A world as its panorama is no room at all: only the 360 photo layer
        if (isWorldCell(cell)) return worldPanorama ? 0 : ROOM_STYLE_SPLAT;
        if (cell == ENV_CELL_MINIMAL_ROOM) return ROOM_STYLE_MINIMAL;
        if (cell == ENV_CELL_PSX_CINEMA) return ROOM_STYLE_PSX;
        return 0;
    }

    // A cell is worth switching to if it is one of the fixed ones or a photo
    // that actually shipped in the assets. The fixed cells come first, so one
    // bound covers both.
    // Your own room, Calm, and the splat worlds on the headset. The built rooms, photos
    // and generated domes are gone.
    private boolean cellExists(int cell) {
        return cell == ENV_CELL_PASSTHROUGH || cell == ENV_CELL_VOID || isWorldCell(cell);
    }

    // The two places where cells and saved ids meet. Everything else in here
    // works in cells, and only the preference speaks ids.
    private static int idForCell(int cell) {
        if (cell >= ENV_CELL_FIRST_PHOTO
                && cell < ENV_CELL_FIRST_PHOTO + XrPanels.MAX_PHOTOS) {
            return PreferenceConfiguration.VR_ENV_FIRST_PHOTO + (cell - ENV_CELL_FIRST_PHOTO);
        }
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return PreferenceConfiguration.VR_ENV_PASSTHROUGH;
            case ENV_CELL_VOID: return PreferenceConfiguration.VR_ENV_VOID;
            case ENV_CELL_MINIMAL_ROOM: return PreferenceConfiguration.VR_ENV_MINIMAL_ROOM;
            case ENV_CELL_PSX_CINEMA: return PreferenceConfiguration.VR_ENV_PSX_CINEMA;
            default: return -1;
        }
    }

    private static int cellForId(int id) {
        if (id >= PreferenceConfiguration.VR_ENV_FIRST_PHOTO
                && id < PreferenceConfiguration.VR_ENV_FIRST_PHOTO + XrPanels.MAX_PHOTOS) {
            return ENV_CELL_FIRST_PHOTO + (id - PreferenceConfiguration.VR_ENV_FIRST_PHOTO);
        }
        switch (id) {
            case PreferenceConfiguration.VR_ENV_PASSTHROUGH: return ENV_CELL_PASSTHROUGH;
            case PreferenceConfiguration.VR_ENV_VOID: return ENV_CELL_VOID;
            case PreferenceConfiguration.VR_ENV_MINIMAL_ROOM: return ENV_CELL_MINIMAL_ROOM;
            case PreferenceConfiguration.VR_ENV_PSX_CINEMA: return ENV_CELL_PSX_CINEMA;
            default: return -1;
        }
    }

    private boolean backgroundVisible() {
        // Calm is drawn by the renderer itself, and a splat world by itself. Only a world
        // switched to its panorama hangs a picture.
        return isWorldCell(environmentChoice) && worldPanorama;
    }


    /**
     * A cell was picked in the grid. Switching between two photos keeps the
     * old one up until the new one has been decoded, so the room does not
     * blink to black on the way.
     */
    private void chooseEnvironment(int cell) {
        if (!cellExists(cell)) {
            return;
        }
        // The world you are in, picked again: between splats and its panorama
        if (cell == environmentChoice && isWorldCell(cell)) {
            worldPanorama = !worldPanorama;
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(WORLD_PANORAMA_PREF, worldPanorama).apply();
            panels.worldPanorama = worldPanorama;
            pendingPickerArt.set(panels.buildPickerGrid());
            if (!worldPanorama) {
                final String world = worldName(cell);
                new Thread(() -> loadWorld(world), "Video - XR World").start();
            }
            nativeSetEnvironment(nativeCtx, cell, backgroundVisible(), roomKindFor(cell));
            LimeLog.info("World shown as " + (worldPanorama ? "its panorama" : "splats"));
            return;
        }
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;
        if (!isWorldCell(cell)) ambient.play(null);

        final int entry = cell - ENV_CELL_FIRST_PHOTO;
        if (entry >= 0 && (isWorldCell(cell) || entry != loadedPhoto)) {
            Thread loader = new Thread() {
                @Override
                public void run() {
                    loadEntry(entry);
                }
            };
            loader.setName("Video - XR Environment");
            loader.start();
        }
        nativeSetEnvironment(nativeCtx, cell, backgroundVisible(), roomKindFor(cell));

        // The grid is a second way to reach the passthrough switch, so the
        // setting follows it rather than disagreeing with what is on screen
        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, idForCell(cell))
                .putBoolean(PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING, passthroughOn)
                .apply();
    }

    // A cell past the fixed ones: either a 360 picture to hang at infinity or a
    // world to build around you
    private void loadEntry(int entry) {
        if (entry < 0 || entry >= environmentFiles.length) return;
        if (environmentFiles[entry].endsWith("/")) {
            String world = environmentFiles[entry].substring(0, environmentFiles[entry].length() - 1);
            ambient.play(XrPanels.worldAmbient(prefsContext, world));
            // As a panorama it needs no splats; they load when it is switched to 3D
            if (!worldPanorama) loadWorld(world);
            // Its panorama goes behind it, the same path a 360 photo takes
            decodePhoto(entry);
        }
        else decodePhoto(entry);
    }

    /** The headset came off or went back on: the ambience follows it */
    public void ambientPause() { ambient.pause(); }
    public void ambientResume() { ambient.resume(); }

    /**
     * A splat world, read straight into one direct buffer (tens of megabytes, so never
     * through the heap) and packed by the native side on this loader thread. The frame
     * loop picks it up from there.
     */
    private void loadWorld(String world) {
        // Picked again: it is already up, and packing it twice would only cost seconds
        if (world.equals(loadedWorld)) return;
        File file = XrPanels.worldSplat(prefsContext, world);
        long size = file.length();
        if (!file.isFile() || size <= 0 || size > Integer.MAX_VALUE) {
            LimeLog.warning("World " + world + " has no world.splat");
            return;
        }
        try (java.io.FileInputStream in = new java.io.FileInputStream(file)) {
            ByteBuffer data = ByteBuffer.allocateDirect((int)size);
            java.nio.channels.FileChannel channel = in.getChannel();
            while (data.hasRemaining() && channel.read(data) > 0) { }
            data.rewind();
            boolean ok = nativeUploadSplats(nativeCtx, data, (int)size);
            if (ok) loadedWorld = world;
            LimeLog.info("World " + world + (ok ? " loaded, " + size / 32 + " splats" : " did not parse"));
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("World " + world + " failed: " + e);
        }
    }

    private void decodePhoto(int photo) {
        if (photo < 0 || photo >= environmentFiles.length) {
            return;
        }
        // Picking about quickly can leave more than one of these running, and
        // only the last one asked for should reach the swapchain
        int ticket = photoRequest.incrementAndGet();

        InputStream in = null;
        try {
            // A very large panorama is downsampled on decode: past 4096 across
            // the swapchain gains nothing and the raw bitmap can reach the
            // gigabyte that kills the process
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            in = XrPanels.openEnvironment(prefsContext, environmentFiles[photo]);
            BitmapFactory.decodeStream(in, null, bounds);
            XrPanels.closeQuietly(in);

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = 1;
            while (Math.max(bounds.outWidth, bounds.outHeight) / opts.inSampleSize > 4096) {
                opts.inSampleSize *= 2;
            }

            in = XrPanels.openEnvironment(prefsContext, environmentFiles[photo]);
            Bitmap bitmap = BitmapFactory.decodeStream(in, null, opts);
            if (bitmap == null || photoRequest.get() != ticket) {
                return;
            }

            ByteBuffer pixels = ByteBuffer.allocateDirect(
                    bitmap.getWidth() * bitmap.getHeight() * 4);
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();

            backgroundWidth = bitmap.getWidth();
            backgroundHeight = bitmap.getHeight();
            bitmap.recycle();
            pendingPhoto = photo;
            pendingBackground.set(pixels);
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Environment " + environmentFiles[photo] + " failed: " + e);
        } finally {
            XrPanels.closeQuietly(in);
        }
    }

    /**
     * The baked room and its texture atlas. Both are parked for the frame loop
     * to hand over, since that thread owns the GL context and is the one that
     * builds the geometry. Either failing leaves the pair unset, and the cell
     * shows the minimal room instead of anything broken.
     */
    private void loadRoomAssets() {
        ByteBuffer mesh = readAsset(ROOM_DIR + "/" + ROOM_MESH_FILE);
        if (mesh == null) {
            return;
        }

        InputStream in = null;
        try {
            in = prefsContext.getAssets().open(ROOM_DIR + "/" + ROOM_TEXTURE_FILE);
            Bitmap atlas = BitmapFactory.decodeStream(in);
            if (atlas == null) {
                LimeLog.warning("Room texture " + ROOM_TEXTURE_FILE + " did not decode");
                return;
            }
            roomTextureWidth = atlas.getWidth();
            roomTextureHeight = atlas.getHeight();
            ByteBuffer pixels = XrPanels.toBuffer(atlas);
            atlas.recycle();

            roomMeshBytes = mesh.remaining();
            pendingRoomMesh.set(mesh);
            pendingRoomTexture.set(pixels);
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Room texture " + ROOM_TEXTURE_FILE + " failed: " + e);
        } finally {
            XrPanels.closeQuietly(in);
        }
    }

    // A whole asset in a direct buffer, which is the only kind the native side
    // can read without a copy
    private ByteBuffer readAsset(String path) {
        try {
            return readStream(prefsContext.getAssets().open(path));
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Asset " + path + " failed: " + e);
            return null;
        }
    }

    // A whole stream in a direct buffer, the only kind the native side reads without a copy
    private ByteBuffer readStream(InputStream source) {
        InputStream in = source;
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[16384];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
            byte[] all = out.toByteArray();
            ByteBuffer buffer = ByteBuffer.allocateDirect(all.length);
            buffer.put(all);
            buffer.rewind();
            return buffer;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Reading a world or an asset failed: " + e);
            return null;
        } finally {
            XrPanels.closeQuietly(in);
        }
    }

    // Moves the pointer before any press, so a click lands where the user is
    // pointing rather than where they pointed last frame
    private void dispatchInput() {
        int pie = (int)inputState[IN_PIE];
        if (pie >= -1 && pie != pieHover) {
            pieHover = pie;
            redrawPie();
        }
        // The screen placement and the environment grid are ours either way,
        // only the host events need somewhere to go
        if (inputListener != null) {
            int screen = (int)inputState[IN_WORKSPACE];
            if (inputState[IN_WORKSPACE_ACTION] == 1.0f && screen > 0) {
                inputListener.onVrWorkspace(screen, 0, 0, 0, 0, 1);
            }
            if (inputState[IN_HIT] != 0.0f) {
                inputListener.onVrPointer(screen, inputState[IN_U], inputState[IN_V]);
            }

            int buttons = (int)inputState[IN_BUTTONS];
            int changed = buttons ^ heldButtons;
            if (changed != 0) {
                for (int i = 0; i < 3; i++) {
                    int mask = 1 << i;
                    if ((changed & mask) != 0) {
                        inputListener.onVrButton(i, (buttons & mask) != 0);
                    }
                }
                heldButtons = buttons;
            }

            int clicks = (int)inputState[IN_SCROLL];
            if (clicks != 0) {
                inputListener.onVrScroll(clicks);
            }
            int sideways = (int)inputState[IN_HSCROLL];
            if (sideways != 0) {
                inputListener.onVrHScroll(sideways);
            }
            boolean voice = inputState[IN_VOICE] != 0.0f;
            if (voice != voiceHeld) {
                voiceHeld = voice;
                inputListener.onVrVoice(voice);
            }

            // Every real code is 8 or more, so anything at zero or above is a
            // key rather than the sentinel
            int key = (int)inputState[IN_KEY];
            if (key >= 0) {
                inputListener.onVrKey(key);
            }

            // Cleared here as well as being written once natively, so a frame
            // that lands while the activity is on its way out cannot ask twice
            if (inputState[IN_EXIT] != 0.0f) {
                inputState[IN_EXIT] = 0.0f;
                inputListener.onVrExit();
            }
        }

        // A 3d room forces the picture onto its wall, so what comes back while
        // one is on is the wall's placement rather than the user's. Writing it
        // would lose where they had the screen in every other environment.
        // A world leaves the screen where you put it, so only the built rooms skip saving it
        if (inputState[IN_POSE_DIRTY] != 0.0f && roomKindFor(environmentChoice) != ROOM_STYLE_MINIMAL
                && roomKindFor(environmentChoice) != ROOM_STYLE_PSX) {
            saveScreenPose();
        }

        int pick = (int)inputState[IN_PICKER_PICK];
        if (pick >= 0) {
            chooseEnvironment(pick);
        }

        int setting = (int)inputState[IN_SETTING];
        if (setting >= 0) {
            applySetting(setting, (int)inputState[IN_SETTING_VALUE]);
        }
    }

    /**
     * A row on the panel's display or 3D tab was pressed. The native side has
     * already applied it to the running session, this end only has to make it
     * stick and tell whatever else in the app cares.
     */
    private void applySetting(int setting, int value) {
        if (prefsContext == null) {
            return;
        }

        if (setting == SETTING_SHARPEN) {
            String choice = value == 2 ? "quality" : (value == 1 ? "normal" : "off");
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putString(PreferenceConfiguration.VR_SHARPENING_PREF_STRING, choice)
                    .apply();
        }
        else if (setting == SETTING_STATS) {
            boolean on = value != 0;
            // The decoder reads this off the same configuration object every
            // time it is about to report, so stats stop or resume at the next
            // one second window with nothing to restart
            if (prefConfig != null) {
                prefConfig.enablePerfOverlay = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.ENABLE_PERF_OVERLAY_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_AMBILIGHT) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrAmbilight = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_AMBILIGHT_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_ROOM_LIGHT) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrRoomLight = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_ROOM_LIGHT_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_HEAD_LOCK) {
            boolean on = value != 0;
            // The frame loop reads this off the same configuration object every
            // frame and passes it down, so the screen follows the head, or
            // stops following it, on the next one. A room ignores it either
            // way, which is why the row stays live in one rather than greying.
            if (prefConfig != null) {
                prefConfig.vrHeadLocked = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_HEAD_LOCKED_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_AMBI_LEVEL) {
            if (prefConfig != null) {
                prefConfig.vrAmbilightLevel = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_AMBILIGHT_LEVEL_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_SEPARATION) {
            // The frame loop read its copy once and keeps passing that stale
            // one down, but the native panel value overrides it for the rest of
            // the session, so this write is only for next time
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_CONVERGENCE) {
            if (prefConfig != null) {
                prefConfig.vrConvergence = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_RESET_3D) {
            // Both at once, since the reset button moved both
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = PreferenceConfiguration.DEFAULT_VR_SEPARATION;
                prefConfig.vrConvergence = PreferenceConfiguration.DEFAULT_VR_CONVERGENCE;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING,
                            PreferenceConfiguration.DEFAULT_VR_SEPARATION)
                    .putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING,
                            PreferenceConfiguration.DEFAULT_VR_CONVERGENCE)
                    .apply();
        }
    }

    // Written once when a grab ends, so the screen is where it was left next
    // time. Cleared by the reset in settings.
    private void saveScreenPose() {
        if (prefsContext == null) {
            return;
        }

        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < POSE_VALUES; i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append(inputState[IN_POSE + i]);
        }

        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putString(PreferenceConfiguration.VR_SCREEN_POSE_PREF_STRING, sb.toString())
                .apply();
    }

    // Each session opens the screen straight ahead. A pose stored in local space goes stale whenever the
    // headset's world origin resets, which left the screen 8 m wide and off to one side.
    private void restoreScreenPose() {
    }

    /**
     * Asks the GPU for a downscaled copy of the frame just latched and wakes
     * the depth thread. Only this stays on the frame loop, since it has to
     * sample the video texture this context owns, and it only queues work:
     * the depth thread waits for the pixels itself, in nativeFinishDepthCapture.
     */
    private void startDepthCapture() {
        synchronized (depthLock) {
            if (depthPending || depthBusy) {
                skippedFrames++;
                return;
            }
        }

        lastCaptureNs = nativeCaptureDepthInput(nativeCtx, texMatrix);
        captureFrameIndex = videoFrameIndex;
        captureFrameNs = System.nanoTime();

        synchronized (depthLock) {
            depthPending = true;
            depthLock.notify();
        }
    }

    /**
     * Draws the stats into the overlay layer. Called about once a second from
     * whichever thread produced them, never from the frame loop, so the
     * bitmap work cannot stall frame submission.
     *
     * The renderer appends its own numbers, since decode and network stats
     * come from the decoder but warp, inference and depth age only exist here.
     */
    public synchronized void setOverlayText(String text) {
        if (nativeCtx == 0) {
            return;
        }
        // The previous one has not been picked up yet, so skip this update
        // rather than write a buffer the frame loop may be reading

        if (overlayBitmap == null) {
            overlayBitmap = Bitmap.createBitmap(OVERLAY_WIDTH, OVERLAY_HEIGHT,
                    Bitmap.Config.ARGB_8888);
            overlayCanvas = new Canvas(overlayBitmap);
            overlayPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            overlayPaint.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
            overlayPaint.setTextSize(OVERLAY_TEXT_SIZE);
            overlayPaint.setColor(Color.WHITE);
            overlayBuffers = new ByteBuffer[2];
            for (int i = 0; i < overlayBuffers.length; i++) {
                overlayBuffers[i] = ByteBuffer.allocateDirect(OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
                overlayBuffers[i].order(ByteOrder.nativeOrder());
            }
        }

        // Dark backing so the text stays readable over any content
        overlayCanvas.drawColor(0xF0101A29, PorterDuff.Mode.SRC);
        // Texture rows run bottom up, so draw mirrored and let the upload put
        // it back the right way round
        overlayCanvas.save();
        overlayCanvas.translate(0.0f, OVERLAY_HEIGHT);
        overlayCanvas.scale(1.0f, -1.0f);
        float y = OVERLAY_LINE_HEIGHT;
        for (String line : text.split("\n")) {
            overlayCanvas.drawText(line, 8.0f, y, overlayPaint);
            y += OVERLAY_LINE_HEIGHT;
            if (y > OVERLAY_HEIGHT) {
                break;
            }
        }
        overlayCanvas.restore();

        ByteBuffer buf = ByteBuffer.allocateDirect(OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
        buf.rewind();
        overlayBitmap.copyPixelsToBuffer(buf);
        buf.rewind();
        pendingOverlay.set(buf);
    }

    private String rendererStats() {
        float warpMs;
        synchronized (nativeLock) {
            if (nativeCtx == 0) {
                return "";
            }
            warpMs = nativeGetWarpGpuMs(nativeCtx);
        }
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("Warp GPU: %.2f ms", warpMs));
        if (depthReady) {
            sb.append('\n').append(String.format("Depth inference: %.1f ms", lastInferenceMs));
            sb.append('\n').append(String.format("Depth age: %.0f ms", lastDepthAgeMs));
            sb.append('\n').append("Depth frames skipped: ").append(lastDepthSkips);
        }
        return sb.toString();
    }

    private static String msPer(long totalNs, long count) {
        return String.format("%.2f", totalNs / (double)count / 1000000.0);
    }

    public Surface getInputSurface() {
        return inputSurface;
    }

    // May run on any thread, the frame loop picks the counter up on its own
    @Override
    public void onFrameAvailable(SurfaceTexture st) {
        pendingFrames.incrementAndGet();
    }

    /**
     * Stops the frame loop and destroys the OpenXR session. The codec-facing
     * surface stays valid until cleanup(). The join is bounded by one
     * xrWaitFrame period plus teardown.
     */
    public void prepareForStop() {
        stopping = true;

        if (renderThread != null) {
            try {
                renderThread.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (renderThread.isAlive()) {
                LimeLog.warning("XR render thread did not stop in time");
            }
        }
    }

    /**
     * Releases the surface handed to MediaCodec. Only call after the codec
     * has been released. A frame loop that has not stopped yet is still
     * reading the SurfaceTexture, so in that case the release is left for it
     * to do on its way out.
     */
    public void cleanup() {
        ambient.release();
        boolean releaseNow;
        synchronized (teardownLock) {
            releaseNow = renderThread == null || renderThreadDone;
            if (!releaseNow) {
                releaseOnExit = true;
            }
        }
        if (releaseNow) {
            releaseSurfaces();
        }
    }

    // The last thing the frame loop thread does, whichever way it ended
    private void finishRenderThread() {
        boolean release;
        synchronized (teardownLock) {
            renderThreadDone = true;
            release = releaseOnExit;
        }
        if (release) {
            releaseSurfaces();
        }
    }

    private void releaseSurfaces() {
        for (int i = 0; i < desktops.length; i++) {
            if (desktops[i] != null) { desktops[i].release(); desktops[i] = null; }
        }
        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }
        if (surfaceTexture != null) {
            surfaceTexture.release();
            surfaceTexture = null;
        }
    }
}
