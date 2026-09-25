package com.limelight;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import com.limelight.binding.video.XrRenderer;
import com.limelight.binding.video.XrShared;
import com.limelight.preferences.PreferenceConfiguration;

/**
 * The whole workspace, ours: our video off the Mac companion, our input over the
 * bridge, our renderer. No pairing, no PIN, no third party host.
 */
public final class ArxSession extends Activity implements XrRenderer.InputListener, XrRenderer.SessionListener {
    private PreferenceConfiguration prefs;
    private volatile XrRenderer renderer;
    private ArxWorkspace workspace;
    private ArxStream stream;
    // Only encodes while the Mac has its mirror window open
    private ArxMirror mirror;
    private ArxVoice voice;
    private int modifiers;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = PreferenceConfiguration.readPreferences(this);
        workspace = new ArxWorkspace(this, this::showFeedback, () -> renderer);

        try { new ArxBridge(this); }
        catch (Exception error) { fail(error.getMessage()); return; }
        stream = new ArxStream(this, 0, this::onFormat, this::showFeedback);
        mirror = new ArxMirror(this, () -> renderer);
        ArxHome.markConnected(this);
        FileLog.event("ARX_SESSION start version=" + BuildConfig.VERSION_NAME
                + " host=" + (ArxReconnect.bridgeHost(this) == null ? "usb" : "lan"));
    }

    /** Called on the stream thread the moment the Mac's picture size is known. */
    private Surface onFormat(int width, int height, int fps) {
        XrRenderer current = renderer;
        if (current == null) {
            current = new XrRenderer();
            current.setInputListener(this);
            if (!current.start(this, width, height, prefs)) { onVrUnavailable(); return null; }
            renderer = current;
            LimeLog.info("ArX session at " + width + "x" + height + " " + fps + "fps");
        }
        return current.getInputSurface();
    }

    @Override public void onVrUnavailable() {
        fail("This headset would not open a VR session.");
    }

    private void fail(String reason) {
        FileLog.event("ARX_SESSION failed: " + reason);
        runOnUiThread(() -> {
            getSharedPreferences("arx_connection", MODE_PRIVATE).edit().putBoolean("pending", true).apply();
            finish();
        });
    }

    @Override public void onVrPointerMove(float u, float v) { workspace.point(0, u, v); }
    @Override public void onVrPointer(int screen, float u, float v) { workspace.point(screen, u, v); }
    @Override public void onVrButton(int button, boolean down) { workspace.button(button, down); }
    @Override public void onVrScroll(int clicks) { workspace.scroll(clicks); }
    @Override public void onVrHScroll(int clicks) { workspace.hscroll(clicks); }
    // Held to talk, let go to type it
    @Override public void onVrVoice(boolean down) {
        runOnUiThread(() -> {
            if (isFinishing()) return;
            if (voice == null) voice = new ArxVoice(this, this::showDictation, workspace::text, this::showLevels);
            voice.hold(down);
        });
    }

    // The one panel action left: its close button, which only stops showing the window
    @Override public void onVrWorkspace(int slot, float u, float v, int buttons, int scroll, int action) {
        if (action == 1) workspace.remove(slot);
    }

    @Override public void onVrExit() { runOnUiThread(this::finish); }

    @Override public void onVrKey(int code) {
        if (code == XrShared.KB_CODE_EXIT) {
            XrRenderer r = renderer;
            if (r != null) {
                r.setExitArmed(true);
                getWindow().getDecorView().postDelayed(() -> r.setExitArmed(false), 3000);
            }
            return;
        }
        // The guide opens and closes inside the renderer
        if (code == XrShared.KB_CODE_GUIDE) return;
        if (code == XrShared.KB_CODE_SCREEN) { workspace.add(); return; }
        if (code == XrShared.KB_CODE_OVERVIEW) { workspace.overview(); return; }
        if (code == XrShared.KB_CODE_HEADLOCK) {
            XrRenderer r = renderer;
            if (r != null) {
                boolean on = r.toggleHeadLock();
                runOnUiThread(() -> showFeedback(on ? "Head lock on: the screens follow your head" : "Head lock off"));
            }
            return;
        }
        if (code == XrShared.KB_CODE_CMD || code == XrShared.KB_CODE_CTRL) {
            modifiers ^= code == XrShared.KB_CODE_CMD ? 8 : 2;
            String held = ((modifiers & 8) != 0 ? "Cmd " : "") + ((modifiers & 2) != 0 ? "Ctrl " : "");
            runOnUiThread(() -> showFeedback(held.isEmpty() ? "" : held + "held: press a key"));
            return;
        }
        if (code == XrShared.KB_CODE_VOICE) {
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (voice == null) voice = new ArxVoice(this, this::showDictation, workspace::text, this::showLevels);
                voice.press();
            });
            return;
        }
        runOnUiThread(() -> { if (voice != null && voice.cancel()) showFeedback("Dictation discarded."); });
        if (code == XrShared.KB_CODE_CAPTURE) {
            if (stream != null) stream.refresh();
            runOnUiThread(() -> showFeedback("Screen refreshed."));
            return;
        }
        // The pointer shortcuts the keyboard carries: right click, two scroll steps, select all
        if (code >= 65030 && code <= 65033) {
            if (code == 65030) { workspace.button(1, true); workspace.button(1, false); }
            else if (code == 65031 || code == 65032) workspace.scroll(code == 65031 ? 3 : -3);
            else workspace.key(65, 8);
            return;
        }
        boolean navigation = code >= 65020 && code <= 65023;
        if (navigation) code = new int[]{37, 39, 38, 40}[code - 65020];
        int held = modifiers; modifiers = 0;
        if (held != 0 || code == 8 || code == 9 || code == 13 || code == 27 || navigation)
            workspace.key(code >= 'a' && code <= 'z' ? code - 32 : code, held);
        else workspace.text(String.valueOf((char)code));
    }

    // Status lines are short and clear themselves; nothing stays pinned over the desktop
    private final Runnable clearFeedback = () -> showFeedback("", false);
    private void showFeedback(String text) { showFeedback(text, true); }
    private void showFeedback(String text, boolean autoHide) {
        XrRenderer r = renderer;
        if (r != null) r.showArxFeedback(text);
        View root = getWindow().getDecorView();
        root.removeCallbacks(clearFeedback);
        if (autoHide && !text.isEmpty()) root.postDelayed(clearFeedback, 3500);
    }

    // Dictation lights the dock while the microphone is live, and the pill shows the
    // waveform instead of words, so you can see it hearing you
    private void showDictation(String text) {
        boolean listening = text.startsWith("Listening");
        workspace.mic(listening);
        XrRenderer r = renderer;
        if (r != null) r.setDictating(listening);
        if (listening) { showLevels(new float[ArxVoice.LEVELS]); return; }
        showFeedback(text, !text.startsWith("Transcribing"));
    }
    private void showLevels(float[] levels) {
        getWindow().getDecorView().removeCallbacks(clearFeedback);
        XrRenderer r = renderer;
        if (r != null) r.showArxLevels(levels);
    }

    @Override protected void onPause() {
        super.onPause();
        XrRenderer paused = renderer;
        if (paused != null) paused.ambientPause();
        if (workspace != null) { workspace.pause(); workspace.mic(false); }
        if (voice != null) voice.cancel();
    }

    // Headset off or asleep, and back
    @Override protected void onStop() {
        super.onStop();
        if (workspace != null) workspace.suspend();
    }
    @Override protected void onStart() {
        super.onStart();
        if (workspace != null) workspace.restore();
    }

    @Override protected void onResume() {
        super.onResume();
        XrRenderer resumed = renderer;
        if (resumed != null) resumed.ambientResume();
        if (workspace != null) workspace.resume();
        // Coming back to a session that was hidden, the decoder holds a stale picture
        if (stream != null) stream.refresh();
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        if (stream != null) stream.close();
        if (mirror != null) mirror.close();
        if (workspace != null) workspace.close();
        if (voice != null) voice.close();
        XrRenderer r = renderer;
        if (r != null) { r.prepareForStop(); r.cleanup(); }
        FileLog.event("ARX_SESSION ended, " + (stream == null ? 0 : stream.frames()) + " frames");
    }
}
