package com.limelight;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;
import org.json.JSONObject;

/**
 * Tap to record, tap to finish and type, or hold to talk. The whole recording goes
 * to the Mac once you stop and is typed in one go, into whatever has focus then,
 * so nothing lands in the wrong place while you move around. The audio is written
 * to a file as you talk and only deleted once its words are typed: if the Mac
 * cannot take it, the next press of Dictate sends that same recording again.
 */
final class ArxVoice {
    static final int PERMISSION_REQUEST = 4701;
    private static final int RATE = 16000;
    // The bridge's MAX_SECONDS, four hours: past this the recording stops and is transcribed as it stands
    private static final long MAX_BYTES = RATE * 2L * 14400;
    // The Mac accepts this much text per typing request, so a long transcript goes in a few
    private static final int TYPE_CHUNK = 8000;
    // How many recent levels the waveform shows, one every 50 ms
    static final int LEVELS = 32;
    private final Activity activity;
    private final Consumer<String> status, type;
    private final Consumer<float[]> levels;
    private final AtomicInteger generation = new AtomicInteger();
    private volatile boolean recording;
    private boolean busy;
    // The recording in progress, or one the Mac could not take yet, as raw 16 kHz PCM
    private final java.io.File pending;
    ArxVoice(Activity activity, Consumer<String> status, Consumer<String> type, Consumer<float[]> levels) {
        this.activity = activity; this.status = status; this.type = type; this.levels = levels;
        pending = new java.io.File(activity.getFilesDir(), "dictation_pending.pcm");
    }
    // Let go before the microphone was even up: stop the moment it is, rather than cancel
    private volatile boolean released;
    // A press shorter than this is a tap, which toggles; a longer one is push to talk
    private static final long TAP_MS = 400;
    private long pressedAt;
    private boolean ignoreRelease;
    /**
     * The dictation buttons, the way OpenWispr's hotkey works: tap to start and tap
     * again to stop, or hold to talk and let go to type. A tap used to be read as a
     * hold too short to hear anything.
     */
    void hold(boolean down) {
        long now = android.os.SystemClock.uptimeMillis();
        FileLog.event("ARX_VOICE " + (down ? "down" : "up") + " recording=" + recording + " busy=" + busy);
        if (down) {
            // Already listening: this press finishes it, and its release means nothing
            if (recording) { ignoreRelease = true; press(); return; }
            // Still typing the last one out: leave it be
            if (busy) { ignoreRelease = true; return; }
            ignoreRelease = false; released = false; pressedAt = now;
            press();
            return;
        }
        if (ignoreRelease) { ignoreRelease = false; return; }
        // A tap: keep listening until the next tap
        if (now - pressedAt < TAP_MS) return;
        // Held: letting go finishes it
        if (recording) press();
        else if (busy) released = true;
    }
    void press() {
        if (recording) { recording = false; status.accept("Transcribing..."); return; }
        // Cancelled on purpose, so its recording goes too
        if (busy) { cancel(); pending.delete(); status.accept("Dictation cancelled."); return; }
        if (activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            activity.requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, PERMISSION_REQUEST);
            status.accept("Allow the microphone, then press Dictate again"); return;
        }
        final ArxBridge bridge;
        try { bridge = new ArxBridge(activity); }
        catch (Exception error) { status.accept(error.getMessage()); return; }
        int ticket = generation.incrementAndGet(); busy = true;
        // A recording the Mac never took: send that before anything new is recorded
        if (pending.length() >= 3200) {
            status.accept("Sending your last recording again...");
            new Thread(() -> transcribe(bridge, ticket), "ArXVR-dictation").start();
            return;
        }
        status.accept("Starting microphone...");
        new Thread(() -> record(bridge, ticket), "ArXVR-dictation").start();
    }
    private void publish(int ticket, String message) {
        activity.runOnUiThread(() -> { if (generation.get() == ticket) status.accept(message); });
    }
    @SuppressWarnings("MissingPermission")
    private void record(ArxBridge bridge, int ticket) {
        AudioRecord microphone = null;
        try {
            bridge.json("/health", null);
            if (generation.get() != ticket) return;
            int size = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (size <= 0) throw new Exception("Headset microphone does not support 16 kHz audio.");
            microphone = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, RATE,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, Math.max(size * 2, 8192));
            if (microphone.getState() != AudioRecord.STATE_INITIALIZED) throw new Exception("Headset microphone could not start.");
            java.io.FileOutputStream audio = new java.io.FileOutputStream(pending);
            long recorded = 0;
            // 50 ms at a time, which is how often the waveform moves
            byte[] buffer = new byte[1600];
            float[] history = new float[LEVELS];
            synchronized (generation) {
                if (generation.get() != ticket) return;
                recording = true;
            }
            microphone.startRecording();
            if (microphone.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) throw new Exception("Microphone is in use by another app.");
            publish(ticket, "Listening");
            while (recording && !released && generation.get() == ticket) {
                int n = microphone.read(buffer, 0, buffer.length);
                if (n < 0) throw new Exception("Microphone disconnected. Please retry.");
                if (n == 0) continue;
                audio.write(buffer, 0, n);
                recorded += n;
                float level = level(buffer, n);
                System.arraycopy(history, 1, history, 0, LEVELS - 1);
                history[LEVELS - 1] = level;
                float[] shown = history.clone();
                activity.runOnUiThread(() -> { if (generation.get() == ticket && recording) levels.accept(shown); });
                if (recorded >= MAX_BYTES) break;
            }
            audio.close();
            microphone.stop(); microphone.release(); microphone = null;
            synchronized(generation) { if (generation.get() != ticket) return; recording = false; }
            FileLog.event("ARX_VOICE stopped after " + recorded / (RATE * 2.0f) + " s" + (released ? ", released" : ""));
            if (recorded < 3200) { pending.delete(); throw new Exception("Too short, try again"); }
        } catch (Exception error) {
            activity.runOnUiThread(() -> {
                if (generation.get() != ticket) return;
                busy = false; recording = false;
                status.accept("Dictation: " + (error.getMessage() == null ? "connection failed" : error.getMessage()));
            });
            return;
        } finally {
            if (microphone != null) { try { microphone.stop(); } catch (RuntimeException ignored) { } microphone.release(); }
        }
        transcribe(bridge, ticket);
    }
    // Sends the saved recording and types its words. The file goes only once they are typed.
    private void transcribe(ArxBridge bridge, int ticket) {
        try {
            long length = pending.length();
            float seconds = length / (RATE * 2.0f);
            publish(ticket, "Transcribing " + Math.round(seconds) + " s...");
            // Whisper on the Mac runs many times faster than speech; this leaves it plenty
            int timeout = (int)((90 + seconds) * 1000);
            // Straight from the file, so however long it is it never has to fit in memory
            ArxBridge.Body body = new ArxBridge.Body() {
                public long length() { return 44 + length; }
                public void writeTo(java.io.OutputStream output) throws java.io.IOException {
                    output.write(wavHeader(length));
                    java.nio.file.Files.copy(pending.toPath(), output);
                }
            };
            byte[] response = bridge.request("/transcribe", body, "audio/wav", timeout);
            String words = new JSONObject(new String(response, StandardCharsets.UTF_8)).getString("text").trim();
            activity.runOnUiThread(() -> {
                if (generation.get() != ticket) return;
                busy = false;
                for (int at = 0; at < words.length(); at += TYPE_CHUNK) {
                    type.accept(words.substring(at, Math.min(words.length(), at + TYPE_CHUNK)));
                }
                pending.delete();
                status.accept(words.isEmpty() ? "No speech heard" : "Typed");
            });
        } catch (Exception error) {
            FileLog.event("ARX_VOICE send failed, recording kept: " + error);
            activity.runOnUiThread(() -> {
                if (generation.get() != ticket) return;
                busy = false;
                status.accept("Mac didn't take it (" + (error.getMessage() == null ? "connection failed" : error.getMessage())
                        + "). Recording kept: press Dictate to send it again");
            });
        }
    }
    // Loudness of one 50 ms read, 0 to 1 on a decibel scale, as the waveform shows it
    private static float level(byte[] pcm, int length) {
        double sum = 0;
        int count = length / 2;
        for (int i = 0; i + 1 < length; i += 2) {
            int sample = (short)((pcm[i] & 0xFF) | (pcm[i + 1] << 8));
            sum += (double)sample * sample;
        }
        double rms = Math.sqrt(sum / Math.max(count, 1)) / 32768.0;
        double db = 20.0 * Math.log10(rms + 1e-6);
        return (float)Math.max(0.0, Math.min(1.0, (db + 55.0) / 40.0));
    }
    static byte[] wavHeader(long pcmLength) {
        ByteBuffer out = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN);
        out.put("RIFF".getBytes(StandardCharsets.US_ASCII)).putInt((int)(36 + pcmLength));
        out.put("WAVEfmt ".getBytes(StandardCharsets.US_ASCII)).putInt(16).putShort((short)1).putShort((short)1);
        out.putInt(RATE).putInt(RATE * 2).putShort((short)2).putShort((short)16);
        out.put("data".getBytes(StandardCharsets.US_ASCII)).putInt((int)pcmLength);
        return out.array();
    }
    boolean cancel() { synchronized(generation) { boolean active = busy; generation.incrementAndGet(); recording = false; busy = false; return active; } }
    void close() { cancel(); }
}
