package com.limelight;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;

/**
 * Goes straight into the Mac workspace. There is nothing to pair: the bridge hands
 * this app its address and its token over the cable, and the session opens on that.
 */
public final class ArxHome extends Activity {
    private static final String STATE = "arx_connection";
    private LinearLayout content;

    // Set before a launch, cleared once the session is up, so a failed attempt is visible next time
    static void markConnected(Context context) {
        context.getSharedPreferences(STATE, MODE_PRIVATE).edit().putBoolean("pending", false).apply();
    }

    private boolean paired() { return new File(getFilesDir(), "arx_voice_bridge.json").exists(); }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        boolean lastFailed = getSharedPreferences(STATE, MODE_PRIVATE).getBoolean("pending", false);
        if (paired() && !lastFailed) { launch(); return; }

        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(Color.rgb(12, 17, 24));
        content.setPadding(dp(40), dp(36), dp(40), dp(36));
        setContentView(content);
        text("ArX Workspace", 32, 0xFFF2F5F9, true);
        if (paired()) {
            text("Could not reach your Mac last time. Check that ArX VR Bridge is running there, then retry.",
                    19, 0xFFA9B6C7, false);
        } else {
            text("Run arxvr on your Mac and plug the headset in once. The bridge hands this app everything it needs, and from then on the workspace opens on its own.",
                    19, 0xFFA9B6C7, false);
        }
        button("Open my workspace", true, this::launch);
        FileLog.event("ARX_HOME version=" + BuildConfig.VERSION_NAME + " paired=" + paired() + " lastFailed=" + lastFailed);
    }

    private void launch() {
        getSharedPreferences(STATE, MODE_PRIVATE).edit().putBoolean("pending", true).apply();
        startActivity(new Intent(this, ArxSession.class));
        finish();
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    private void text(String value, int size, int color, boolean heading) {
        TextView view = new TextView(this);
        view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setPadding(0, dp(6), 0, dp(18));
        if (heading) view.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        content.addView(view);
    }

    private void button(String value, boolean primary, Runnable action) {
        Button button = new Button(this);
        button.setText(value); button.setAllCaps(false); button.setTextSize(19);
        button.setTextColor(0xFFF2F5F9);
        GradientDrawable background = new GradientDrawable();
        background.setColor(primary ? 0xFF1F55A6 : 0xFF1B2430);
        background.setCornerRadius(dp(14));
        button.setBackground(background);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, dp(58));
        params.setMargins(0, dp(6), 0, dp(10));
        content.addView(button, params);
        button.setOnClickListener(v -> action.run());
    }
}
