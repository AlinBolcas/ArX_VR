package com.limelight;

import android.app.Activity;
import com.limelight.binding.video.XrRenderer;
import com.limelight.binding.video.XrShared;
import org.json.JSONObject;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.Supplier;

/**
 * Mac control and every VR desktop, all through the bridge. A VR desktop is a
 * monitor plugged into the Mac while the headset shows it: when the headset
 * stops showing it the Mac unplugs it and its windows come home to the laptop
 * screen, and when the headset is back the desktop and its windows return.
 * Which desktops you had is kept here, so they come back after a restart too.
 */
final class ArxWorkspace implements AutoCloseable {
    static final int MAX = XrShared.ARX_MAX_EXTRA;
    private final Activity activity;
    private final Consumer<String> feedback;
    private final Supplier<XrRenderer> renderer;
    private final ScheduledExecutorService controls = Executors.newSingleThreadScheduledExecutor();
    private final AtomicReference<JSONObject> pointer = new AtomicReference<>();
    // Index 0 unused: the main desktop's stream belongs to the session
    private final ArxStream[] streams = new ArxStream[MAX + 1];
    private volatile ArxBridge bridge;
    private volatile boolean ready, closed, paused, adding, suspended, restoring = true;
    // Which of your own monitors each slot shows, and the ones you closed this session
    private final long[] monitorShown = new long[MAX + 1];
    private final java.util.Set<Long> monitorsHidden = java.util.concurrent.ConcurrentHashMap.newKeySet();
    private int buttons, pointerSlot;
    private int deliveredButtons;
    private float u, v;
    private String lastFailure = "";
    private long lastReported;
    ArxWorkspace(Activity activity, Consumer<String> feedback, Supplier<XrRenderer> renderer) {
        this.activity = activity; this.feedback = feedback; this.renderer = renderer;
        controls.scheduleWithFixedDelay(this::check, 0, 3, TimeUnit.SECONDS);
        controls.scheduleWithFixedDelay(() -> {
            JSONObject latest = pointer.getAndSet(null);
            if (latest != null && ready && !paused) sendMotion(latest);
        }, 0, 25, TimeUnit.MILLISECONDS);
    }
    private void tell(String text) { if (!closed) activity.runOnUiThread(() -> { if (!closed) feedback.accept(text); }); }
    private void failed(Exception error) {
        String message = error.getMessage() == null ? "Mac bridge disconnected" : error.getMessage();
        long now = System.currentTimeMillis();
        if (!message.equals(lastFailure) || now-lastReported > 15000) { lastFailure=message; lastReported=now; tell(message); }
    }
    private void check() {
        if (closed) return;
        try {
            bridge = new ArxBridge(activity);
            JSONObject health = bridge.json("/health", null);
            boolean wasReady = ready;
            ready = truthy(health.opt("accessibility")) && !health.has("host_error");
            if (ready && !wasReady) tell("Mac connected");
            if (!ready && wasReady) tell("ArX VR Bridge is off on your Mac");
            org.json.JSONArray live = health.optJSONArray("desktop_slots");
            if (live != null && !adding) reconcile(live);
            if (ready && restoring) { restoring = false; for (int slot : kept()) open(slot); }
            if (ready && !suspended) showMonitors(health.optJSONArray("monitors"));
            testRequest();
        } catch (Exception error) { if (ready) tell("ArX VR Bridge is off on your Mac"); ready = false; }
    }
    // Lets the Mac drive a test over the cable: `adb shell run-as` can write this file on a
    // debug build only, so a release build has no way in. "add" or "remove <slot>".
    private void testRequest() {
        java.io.File request = new java.io.File(activity.getFilesDir(), "arx_test_request");
        if (!BuildConfig.DEBUG || !request.exists()) return;
        try {
            String line = new String(java.nio.file.Files.readAllBytes(request.toPath())).trim();
            request.delete();
            FileLog.event("ARX_TEST " + line);
            if (line.equals("add")) add();
            else if (line.startsWith("remove ")) remove(Integer.parseInt(line.substring(7).trim()));
        } catch (Exception ignored) { request.delete(); }
    }
    // The Mac is the truth about which desktops are plugged in. One it unplugged while
    // this session was showing it, after a long network drop say, closes here and is
    // opened again, so its windows come back with it.
    private void reconcile(org.json.JSONArray live) {
        boolean[] onMac = new boolean[MAX + 1];
        for (int i = 0; i < live.length(); i++) { int slot = live.optInt(i); if (slot >= 1 && slot <= MAX) onMac[slot] = true; }
        for (int slot = 1; slot <= MAX; slot++) {
            if (streams[slot] == null || onMac[slot] || paused) continue;
            // One of your own monitors is not a VR desktop: it is shown again as a monitor,
            // never re-created in its slot as a desktop
            if (monitorShown[slot] != 0) { monitorShown[slot] = 0; closeLocally(slot, true); continue; }
            closeLocally(slot, false);
            open(slot);
        }
    }

    // The desktops you had, kept on the headset so a restart or a return puts them back
    private java.util.List<Integer> kept() {
        java.util.List<Integer> slots = new java.util.ArrayList<>();
        for (String part : activity.getSharedPreferences("arx_workspace", Activity.MODE_PRIVATE).getString("desktops", "").split(",")) {
            try { int slot = Integer.parseInt(part.trim()); if (slot >= 1 && slot <= MAX) slots.add(slot); } catch (NumberFormatException ignored) { }
        }
        return slots;
    }
    private void keep(int slot, boolean on) {
        java.util.List<Integer> slots = kept();
        slots.remove(Integer.valueOf(slot));
        if (on) slots.add(slot);
        StringBuilder text = new StringBuilder();
        for (int s : slots) text.append(text.length() == 0 ? "" : ",").append(s);
        activity.getSharedPreferences("arx_workspace", Activity.MODE_PRIVATE).edit().putString("desktops", text.toString()).apply();
    }
    // Hosts serialise flags as true or 1 depending on the platform type; accept both
    static boolean truthy(Object value) {
        if (value instanceof Boolean) return (Boolean)value;
        if (value instanceof Number) return ((Number)value).intValue() != 0;
        return "true".equalsIgnoreCase(String.valueOf(value));
    }
    boolean controlsMac() { return ready; }
    private static JSONObject object(Object... values) {
        JSONObject result = new JSONObject();
        try { for (int i=0;i<values.length;i+=2) result.put((String)values[i], values[i+1]); }
        catch (org.json.JSONException e) { throw new IllegalArgumentException(e); }
        return result;
    }
    private void send(JSONObject request) {
        if (closed) return;
        try {
            bridge.json("/workspace", request);
            if ("pointer".equals(request.optString("kind"))) deliveredButtons=request.optInt("buttons");
            else if ("release".equals(request.optString("kind"))) deliveredButtons=0;
        } catch (Exception error) { ready=false; deliveredButtons=0; failed(error); }
    }
    private void sendMotion(JSONObject request) {
        // A newer cursor sample must not overtake a queued button edge.
        try { request.put("buttons",deliveredButtons); }
        catch(org.json.JSONException error) { return; }
        send(request);
    }
    private void ordered(JSONObject request) {
        if (closed) return;
        long queued=android.os.SystemClock.uptimeMillis();
        JSONObject latest=pointer.getAndSet(null);
        controls.execute(() -> {
            if (paused || android.os.SystemClock.uptimeMillis()-queued>1000) return;
            if (!ready) return;
            if (latest != null) sendMotion(latest);
            if (ready) send(request);
        });
    }
    // Any desktop, 0 the main one. Buttons are shared, so a press held while the
    // pointer crosses to another desktop drags the window across with it.
    synchronized void point(int slot, float nextU, float nextV) {
        if (paused || !ready) return;
        pointerSlot=slot; u=nextU; v=nextV;
        pointer.set(pointerEvent(0));
    }
    private JSONObject pointerEvent(int scroll) { return pointerEvent(scroll, 0); }
    private JSONObject pointerEvent(int scroll, int hscroll) {
        return object("op","input","kind","pointer","slot",pointerSlot,"u",u,"v",v,"buttons",buttons,"scroll",scroll,"hscroll",hscroll);
    }
    synchronized void button(int button, boolean down) {
        if (((buttons & (1<<button)) != 0) == down) return;
        buttons=down ? buttons | (1<<button) : buttons & ~(1<<button);
        ordered(pointerEvent(0));
    }
    synchronized void scroll(int clicks) { ordered(pointerEvent(clicks)); }
    synchronized void hscroll(int clicks) { ordered(pointerEvent(0, clicks)); }
    void text(String words) { ordered(object("op","input","kind","text","text",words)); }
    void key(int key, int modifiers) { ordered(object("op","input","kind","key","key",key,"modifiers",modifiers)); }

    void add() {
        if (closed) return;
        controls.execute(() -> {
            if (bridge == null || !ready) { tell("Mac bridge not ready"); return; }
            int slot = 0;
            for (int i = 1; i <= MAX && slot == 0; i++) if (streams[i] == null) slot = i;
            if (slot == 0) { tell("All " + (MAX + 1) + " desktops are open"); return; }
            if (open(slot)) tell("Desktop " + (slot + 1) + " plugged in. Drag windows onto it.");
        });
    }

    // Plugs a desktop into the Mac, or finds it still plugged in, and shows it. On the
    // controls thread. The stream says the size, and the panel keeps its place.
    private boolean open(int slot) {
        // Already showing: a second stream on the same desktop would fight the first over
        // its one surface, each tearing down the other's picture every second or so
        if (streams[slot] != null) return true;
        adding = true;
        try {
            bridge.json("/workspace", object("op","add","slot",slot));
            streams[slot] = new ArxStream(activity, slot, (w, h, fps) -> {
                        XrRenderer r = renderer.get();
                        return r == null ? null : r.addDesktop(slot, w, h);
                    },
                    status -> { if (!status.isEmpty()) tell("Desktop " + (slot + 1) + ": " + status); });
            keep(slot, true);
            FileLog.event("ARX_DESKTOP " + (slot + 1) + " open");
            return true;
        } catch (Exception error) { failed(error); return false; }
        finally { adding = false; }
    }

    // Every monitor plugged into the Mac shows up in VR too, beside the others, so
    // nothing on the Mac is out of sight. One you close stays closed this session.
    private void showMonitors(org.json.JSONArray ids) {
        if (ids == null) return;
        for (int i = 0; i < ids.length(); i++) {
            long id = ids.optLong(i);
            if (monitorsHidden.contains(id)) continue;
            boolean shown = false;
            for (int slot = 1; slot <= MAX; slot++) if (monitorShown[slot] == id && streams[slot] != null) shown = true;
            if (shown) continue;
            int slot = 0;
            for (int s = 1; s <= MAX && slot == 0; s++) if (streams[s] == null && !kept().contains(s)) slot = s;
            if (slot == 0) return;
            try {
                bridge.json("/workspace", object("op","monitor","slot",slot,"display",id));
                final int shownSlot = slot;
                monitorShown[slot] = id;
                streams[slot] = new ArxStream(activity, slot, (w, h, fps) -> {
                            XrRenderer r = renderer.get();
                            return r == null ? null : r.addDesktop(shownSlot, w, h);
                        },
                        status -> { if (!status.isEmpty()) tell("Monitor: " + status); });
                FileLog.event("ARX_MONITOR " + id + " shown in slot " + slot);
            } catch (Exception error) { failed(error); }
        }
    }

    // Mission Control on the Mac: every app and desktop at once, as the three finger swipe up gives
    void overview() { ordered(object("op","overview")); }
    // Tells the Mac when the headset's microphone is live, so the Bridge window can say so
    private volatile boolean micReported;
    void mic(boolean on) {
        if (on == micReported) return;
        micReported = on;
        controls.execute(() -> { try { if (bridge != null) bridge.json("/workspace", object("op","mic","on",on)); } catch (Exception ignored) { } });
    }

    // Closed on purpose: unplugged now, its windows come home and stay there. One of your
    // own monitors only stops being shown.
    void remove(int slot) {
        if (slot < 1 || slot > MAX || closed) return;
        if (monitorShown[slot] != 0) { monitorsHidden.add(monitorShown[slot]); monitorShown[slot] = 0; }
        controls.execute(() -> {
            closeLocally(slot, true);
            keep(slot, false);
            try { bridge.json("/workspace", object("op","remove","slot",slot)); }
            catch (Exception error) { failed(error); }
        });
    }

    // forget: the panel's place goes too, rather than being kept for its return
    private void closeLocally(int slot, boolean forget) {
        ArxStream stream = streams[slot];
        streams[slot] = null;
        if (stream != null) stream.close();
        XrRenderer r = renderer.get();
        if (r != null) r.removeDesktop(slot, forget);
    }

    // The headset has stopped showing anything, taken off or put to sleep. The streams
    // stop; the Mac sees nobody watching and unplugs the desktops, windows remembered.
    void suspend() {
        suspended = true;
        controls.execute(() -> { for (int slot = 1; slot <= MAX; slot++) if (streams[slot] != null) closeLocally(slot, false); });
    }
    // And back: the desktops you had are plugged in again and their windows return
    void restore() { suspended = false; restoring = true; }

    synchronized void pause() {
        paused=true; buttons=0; pointer.set(null);
        if(bridge!=null && !closed) controls.execute(() -> send(object("op","input","kind","release")));
    }
    void resume() {
        paused=false;
        for (ArxStream stream : streams) if (stream != null) stream.refresh();
    }
    @Override public void close() {
        pause();
        controls.execute(() -> {
            // Leaving is the same as taking the headset off: the Mac unplugs the desktops
            // once nobody watches, and the next session brings them back
            for (int slot = 1; slot <= MAX; slot++) if (streams[slot] != null) closeLocally(slot, false);
            closed=true;
        });
        controls.shutdown();
    }
}
