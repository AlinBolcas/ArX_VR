package com.limelight;

import android.content.Context;
import android.content.Intent;
import android.util.Base64;
import org.json.JSONObject;

/** Store only the last successfully connected target, including its existing pinned certificate. */
final class ArxReconnect {
    private static final String[] STRINGS={Game.EXTRA_HOST,Game.EXTRA_APP_NAME,Game.EXTRA_UNIQUEID,Game.EXTRA_PC_UUID,Game.EXTRA_PC_NAME};
    private static final String[] INTS={Game.EXTRA_PORT,Game.EXTRA_HTTPS_PORT,Game.EXTRA_APP_ID};
    static void save(Context context, Intent intent) {
        byte[] certificate=intent.getByteArrayExtra(Game.EXTRA_SERVER_CERT);
        if(certificate==null || certificate.length==0) return;
        try {
            JSONObject value=new JSONObject();
            for(String key:STRINGS) value.put(key,intent.getStringExtra(key));
            for(String key:INTS) value.put(key,intent.getIntExtra(key,0));
            value.put(Game.EXTRA_SERVER_CERT,Base64.encodeToString(certificate,Base64.NO_WRAP));
            value.put(Game.EXTRA_APP_HDR,intent.getBooleanExtra(Game.EXTRA_APP_HDR,false));
            context.getSharedPreferences("arx_connection",Context.MODE_PRIVATE).edit().putString("last",value.toString()).apply();
        } catch(Exception ignored) { }
    }
    static Intent last(Context context) {
        try {
            JSONObject value=new JSONObject(context.getSharedPreferences("arx_connection",Context.MODE_PRIVATE).getString("last",""));
            if(value.getString(Game.EXTRA_HOST).isEmpty()) return null;
            Intent intent=new Intent(context,GameXR.class);
            for(String key:STRINGS) intent.putExtra(key,value.getString(key));
            for(String key:INTS) intent.putExtra(key,value.getInt(key));
            intent.putExtra(Game.EXTRA_SERVER_CERT,Base64.decode(value.getString(Game.EXTRA_SERVER_CERT),Base64.NO_WRAP));
            intent.putExtra(Game.EXTRA_APP_HDR,value.getBoolean(Game.EXTRA_APP_HDR));
            // Exit ends the session rather than dropping into the pairing list
            intent.putExtra(Game.EXTRA_RETURN_TO_PC_VIEW,false);
            String current=bridgeHost(context);
            if(current!=null) intent.putExtra(Game.EXTRA_HOST,current);
            return intent;
        } catch(Exception ignored) { return null; }
    }
    // The Mac's current LAN address, written by the bridge over USB, so a changed DHCP lease is not a dead end
    static String bridgeHost(Context context) {
        try {
            JSONObject config=new JSONObject(new String(java.nio.file.Files.readAllBytes(
                new java.io.File(context.getFilesDir(),"arx_voice_bridge.json").toPath()),java.nio.charset.StandardCharsets.UTF_8));
            String host=config.optString("host","");
            return host.matches("\\d{1,3}(\\.\\d{1,3}){3}") ? host : null;
        } catch(Exception ignored) { return null; }
    }
}
