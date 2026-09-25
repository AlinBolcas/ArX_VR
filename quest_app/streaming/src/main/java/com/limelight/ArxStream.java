package com.limelight;

import android.content.Context;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.view.Surface;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

/**
 * The desktop picture, ours end to end. Framed H.264 from the Mac companion
 * straight into a MediaCodec that decodes onto the XR screen's surface.
 *
 * One stream per desktop: 0 the main one, 1 and up the extras.
 *
 * Wire format, after the token, a space and the desktop, and a newline go up:
 *   header  "ARXV" v1, width, height, fps          (16 bytes, big endian)
 *   frame   length, flags (1 = keyframe), pts_us, payload   (Annex-B)
 * One byte 'I' back up the same socket asks for a fresh keyframe.
 */
final class ArxStream implements AutoCloseable {
    static final int PORT = 47997;
    /** Handed the Mac's picture size before anything decodes; returns the surface to decode into. */
    interface Sink { Surface onFormat(int width, int height, int fps); }

    private final Context context;
    private final int slot;
    private final Sink sink;
    private final Consumer<String> status;
    private final Thread thread;
    private volatile Socket socket;
    private volatile boolean closed;
    private volatile long framesDecoded;

    ArxStream(Context context, int slot, Sink sink, Consumer<String> status) {
        this.context = context.getApplicationContext(); this.slot = slot; this.sink = sink; this.status = status;
        thread = new Thread(this::run, "arx-stream-" + slot);
        thread.setPriority(Thread.MAX_PRIORITY);
        thread.start();
    }

    long frames() { return framesDecoded; }

    private volatile boolean wantKeyframe;

    /** Asks for a keyframe, for instance after the screen was hidden. Safe from any thread: the stream thread sends it. */
    void refresh() { wantKeyframe = true; }

    // Wi-Fi first, the USB loopback second: whichever answers wins, and a cable is
    // only ever a fallback. Read fresh on every attempt, as is the secret, so a new
    // DHCP lease or a restarted bridge is picked up without restarting the session.
    private String[] hosts() {
        String lan = ArxReconnect.bridgeHost(context);
        return lan == null ? new String[]{"127.0.0.1"} : new String[]{lan, "127.0.0.1"};
    }

    private void run() {
        int attempt = 0;
        while (!closed) {
            String[] hosts = hosts();
            String host = hosts[attempt % hosts.length];
            try { session(host, new ArxBridge(context).token()); attempt = 0; }
            catch (Exception error) {
                if (closed) return;
                attempt++;
                // Said once, not every few seconds: the bridge being off is a state, not news
                if (attempt == 1) status.accept("Waiting for ArX VR Bridge on your Mac");
            }
            // Slow down only once every host has been tried, so a cable swap recovers at once
            try { Thread.sleep(attempt <= 2 ? 300 : 1500); }
            catch (InterruptedException interrupted) { return; }
        }
    }

    private void session(String host, String token) throws IOException {
        Socket open = new Socket();
        open.setTcpNoDelay(true);
        open.connect(new InetSocketAddress(host, PORT), 2500);
        open.setSoTimeout(8000);
        socket = open;
        MediaCodec codec = null;
        try {
            open.getOutputStream().write((token + " " + slot + "\n").getBytes(StandardCharsets.UTF_8));
            open.getOutputStream().flush();
            DataInputStream input = new DataInputStream(open.getInputStream());
            byte[] header = new byte[16];
            input.readFully(header);
            if (header[0] != 'A' || header[1] != 'R' || header[2] != 'X' || header[3] != 'V')
                throw new IOException("Not the ArX video stream");
            int width = ((header[8] & 0xFF) << 8) | (header[9] & 0xFF);
            int height = ((header[10] & 0xFF) << 8) | (header[11] & 0xFF);
            int fps = ((header[12] & 0xFF) << 8) | (header[13] & 0xFF);
            if (width < 16 || height < 16 || width > 8192 || height > 8192)
                throw new IOException("Mac reported an impossible picture size");

            Surface surface = sink.onFormat(width, height, fps);
            if (surface == null) throw new IOException("No surface to decode into");

            // Wait for a keyframe carrying SPS and PPS before the decoder exists. A frame
            // already in flight when we joined can arrive first, and the Quest decoder,
            // fed a P-frame with nothing to reference, outputs flat grey forever, no error.
            OutputStream upstream = open.getOutputStream();
            upstream.write('I'); upstream.flush();
            byte[] first = readFrame(input);
            ByteBuffer parameterSets = parameterSets(first);
            for (int skipped = 0; parameterSets == null; skipped++) {
                if (skipped == 120) throw new IOException("Mac sent no keyframe");
                first = readFrame(input);
                parameterSets = parameterSets(first);
            }
            MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            format.setByteBuffer("csd-0", parameterSets);
            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            codec.configure(format, surface, null, 0);
            codec.start();
            status.accept("");

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            byte[] payload = first;
            while (!closed) {
                submit(codec, payload);
                drain(codec, info);
                if (wantKeyframe) { wantKeyframe = false; upstream.write('I'); upstream.flush(); }
                payload = readFrame(input);
            }
        } finally {
            socket = null;
            if (codec != null) { try { codec.stop(); } catch (Exception ignored) { } codec.release(); }
            try { open.close(); } catch (IOException ignored) { }
        }
    }

    private byte[] readFrame(DataInputStream input) throws IOException {
        byte[] head = new byte[13];
        input.readFully(head);
        int length = ((head[0] & 0xFF) << 24) | ((head[1] & 0xFF) << 16) | ((head[2] & 0xFF) << 8) | (head[3] & 0xFF);
        if (length <= 0 || length > 8 * 1024 * 1024) throw new IOException("Frame size out of range");
        byte[] payload = new byte[length];
        input.readFully(payload);
        return payload;
    }

    private void submit(MediaCodec codec, byte[] payload) throws IOException {
        int index = codec.dequeueInputBuffer(500000);
        if (index < 0) throw new IOException("Decoder stopped taking frames");
        ByteBuffer buffer = codec.getInputBuffer(index);
        if (buffer == null) throw new IOException("Decoder gave no input buffer");
        buffer.clear();
        buffer.put(payload);
        codec.queueInputBuffer(index, 0, payload.length, System.nanoTime() / 1000, 0);
    }

    private void drain(MediaCodec codec, MediaCodec.BufferInfo info) {
        int index;
        while ((index = codec.dequeueOutputBuffer(info, 0)) >= 0) {
            codec.releaseOutputBuffer(index, true);
            // Logged rarely, so the headset view tool can tell a stalled picture from a stalled stream
            if (++framesDecoded % 600 == 0) FileLog.event("ARX_VIDEO desktop " + (slot + 1) + ": " + framesDecoded + " frames decoded");
        }
    }

    /** SPS and PPS out of an Annex-B keyframe, start codes kept, as the decoder wants them. */
    private static ByteBuffer parameterSets(byte[] frame) {
        int at = 0, end = 0;
        while (at + 5 <= frame.length && startCode(frame, at)) {
            int type = frame[at + 4] & 0x1F;
            if (type != 7 && type != 8) break;
            int next = at + 4;
            while (next + 4 <= frame.length && !startCode(frame, next)) next++;
            at = end = Math.min(next, frame.length);
        }
        return end == 0 ? null : ByteBuffer.wrap(frame, 0, end);
    }

    private static boolean startCode(byte[] frame, int at) {
        return frame[at] == 0 && frame[at + 1] == 0 && frame[at + 2] == 0 && frame[at + 3] == 1;
    }

    @Override public void close() {
        closed = true;
        Socket open = socket;
        if (open != null) try { open.close(); } catch (IOException ignored) { }
        thread.interrupt();
    }
}
