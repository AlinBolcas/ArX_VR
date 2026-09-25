package com.limelight;

import android.content.Context;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.view.Surface;
import com.limelight.binding.video.XrRenderer;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.function.Supplier;

/**
 * What the headset shows, sent back to the Mac for its mirror window. The Mac only
 * listens while that window is open, so until then this just knocks every couple of
 * seconds and nothing is encoded. Once in, the renderer draws each frame again into
 * a hardware encoder and the H.264 goes up the socket as it comes out.
 *
 * Wire format, after the token, " mirror" and a newline:
 *   frame   length (4), pts_us (8), payload (Annex-B), all big endian
 */
final class ArxMirror implements AutoCloseable {
    static final int PORT = 47996;
    private static final int WIDTH = 1920, HEIGHT = 1080, FPS = 36, BITRATE = 16_000_000;

    private final Context context;
    private final Supplier<XrRenderer> renderer;
    private final Thread thread;
    private volatile Socket socket;
    private volatile boolean closed;

    ArxMirror(Context context, Supplier<XrRenderer> renderer) {
        this.context = context.getApplicationContext(); this.renderer = renderer;
        thread = new Thread(this::run, "arx-mirror");
        thread.start();
    }

    private void run() {
        while (!closed) {
            String lan = ArxReconnect.bridgeHost(context);
            for (String host : lan == null ? new String[]{"127.0.0.1"} : new String[]{lan, "127.0.0.1"}) {
                if (closed) return;
                try { session(host); break; }
                catch (Exception notWatching) { /* the Mac's mirror window is closed: nothing to do */ }
            }
            try { Thread.sleep(2000); }
            catch (InterruptedException interrupted) { return; }
        }
    }

    private void session(String host) throws Exception {
        XrRenderer r = renderer.get();
        if (r == null) throw new IOException("No picture yet");
        Socket open = new Socket();
        open.setTcpNoDelay(true);
        open.connect(new InetSocketAddress(host, PORT), 1500);
        socket = open;
        MediaCodec codec = null;
        try {
            DataOutputStream out = new DataOutputStream(open.getOutputStream());
            out.write((new ArxBridge(context).token() + " mirror\n").getBytes(StandardCharsets.UTF_8));
            out.flush();

            MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, WIDTH, HEIGHT);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            format.setInteger(MediaFormat.KEY_BIT_RATE, BITRATE);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, FPS);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            Surface input = codec.createInputSurface();
            codec.start();
            r.setMirror(input, WIDTH, HEIGHT);
            FileLog.event("ARX_MIRROR watching from " + host);

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            byte[] chunk = new byte[0];
            while (!closed) {
                int index = codec.dequeueOutputBuffer(info, 100_000);
                if (index < 0) continue;
                ByteBuffer data = codec.getOutputBuffer(index);
                if (data != null && info.size > 0) {
                    if (chunk.length < info.size) chunk = new byte[info.size];
                    data.position(info.offset);
                    data.get(chunk, 0, info.size);
                    out.writeInt(info.size);
                    out.writeLong(info.presentationTimeUs);
                    out.write(chunk, 0, info.size);
                    out.flush();
                }
                codec.releaseOutputBuffer(index, false);
            }
        } finally {
            // The Mac closed its window or went away: stop drawing and encoding at once
            XrRenderer current = renderer.get();
            if (current != null) current.setMirror(null, 0, 0);
            if (codec != null) { try { codec.stop(); } catch (RuntimeException ignored) { } codec.release(); }
            try { open.close(); } catch (IOException ignored) { }
            socket = null;
            FileLog.event("ARX_MIRROR stopped");
        }
    }

    @Override public void close() {
        closed = true;
        Socket s = socket;
        if (s != null) try { s.close(); } catch (IOException ignored) { }
        thread.interrupt();
    }
}
