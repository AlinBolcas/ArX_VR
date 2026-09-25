package com.limelight.binding.video;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

import com.limelight.LimeLog;

import java.io.File;
import java.io.IOException;

/**
 * A world's ambient music: its loop fades in when the world is picked and out when
 * another place is, and it pauses while the headset is off. Quiet on purpose, it sits
 * under whatever the Mac is playing.
 */
final class XrAmbient {
    private static final float VOLUME = 0.35f;
    private static final int FADE_MS = 1500;
    private static final int STEP_MS = 50;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private MediaPlayer player;
    private String playing;
    private boolean paused;

    /** The loop in file, or silence for null or a world without one. */
    synchronized void play(File file) {
        String key = file != null && file.isFile() ? file.getPath() : null;
        if (key == null ? playing == null : key.equals(playing)) return;
        MediaPlayer old = player;
        player = null;
        playing = key;
        if (old != null) fade(old, VOLUME, 0.0f, old::release);
        if (key == null) return;
        try {
            MediaPlayer next = new MediaPlayer();
            next.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            next.setDataSource(key);
            next.setLooping(true);
            next.setVolume(0.0f, 0.0f);
            next.prepare();
            if (!paused) next.start();
            player = next;
            fade(next, 0.0f, VOLUME, null);
        } catch (IOException | RuntimeException e) {
            LimeLog.warning("Ambient " + key + " failed: " + e);
            playing = null;
        }
    }

    synchronized void pause() {
        paused = true;
        if (player != null && player.isPlaying()) player.pause();
    }

    synchronized void resume() {
        paused = false;
        if (player != null && !player.isPlaying()) player.start();
    }

    synchronized void release() {
        handler.removeCallbacksAndMessages(null);
        if (player != null) player.release();
        player = null;
        playing = null;
    }

    private void fade(MediaPlayer target, float from, float to, Runnable done) {
        long start = SystemClock.uptimeMillis();
        handler.post(new Runnable() {
            @Override
            public void run() {
                float t = Math.min(1.0f, (SystemClock.uptimeMillis() - start) / (float)FADE_MS);
                float volume = from + (to - from) * t;
                try {
                    target.setVolume(volume, volume);
                } catch (IllegalStateException released) {
                    return;
                }
                if (t < 1.0f) handler.postDelayed(this, STEP_MS);
                else if (done != null) done.run();
            }
        });
    }
}
