package com.limelight;

import android.content.Context;
import org.json.JSONObject;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The Mac bridge, over Wi-Fi first and the USB loopback second. Paired once over the
 * cable, which hands over the Mac's address and the secret; after that no cable is
 * needed. No discovery, no cloud.
 */
public final class ArxBridge {
    private static final String LOOPBACK = "http://127.0.0.1:47999";
    // Whichever route answered last, so every request after the first goes straight there
    private static volatile String preferred;
    private final String token;
    private final String lan;
    public ArxBridge(Context context) throws IOException {
        try {
            JSONObject config = new JSONObject(new String(Files.readAllBytes(
                new File(context.getFilesDir(), "arx_voice_bridge.json").toPath()), StandardCharsets.UTF_8));
            if (!"http://127.0.0.1:47999".equals(config.getString("url"))) throw new IOException("Invalid bridge address");
            token = config.getString("token");
            if (token.length() < 32 || token.contains("\n")) throw new IOException("Invalid bridge token");
            String host = config.optString("host", "");
            lan = host.matches("\\d{1,3}(\\.\\d{1,3}){3}") ? "http://" + host + ":47999" : null;
        } catch (Exception e) { throw new IOException("Plug the headset into your Mac once so ArX VR Bridge can pair it.", e); }
    }
    /** The same secret authorises the video socket, so the headset proves itself there too. */
    String token() { return token; }
    // Opens and connects, and nothing more: a route that cannot be reached fails here,
    // before anything is sent, so trying the other one can never send a request twice
    /** A request body written straight to the socket, so a long recording never sits in memory. */
    interface Body { long length(); void writeTo(OutputStream output) throws IOException; }
    private static Body bytes(byte[] data) {
        return data == null ? null : new Body() {
            public long length() { return data.length; }
            public void writeTo(OutputStream output) throws IOException { output.write(data); }
        };
    }
    private HttpURLConnection open(String base, String path, Body body, String contentType, int timeout) throws IOException {
        HttpURLConnection connection = (HttpURLConnection)new URL(base + path).openConnection();
        connection.setConnectTimeout(base.equals(LOOPBACK) ? 2000 : 1500); connection.setReadTimeout(timeout);
        connection.setRequestProperty("Authorization", "Bearer " + token);
        if (body != null) {
            connection.setRequestMethod("POST"); connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", contentType);
            connection.setFixedLengthStreamingMode(body.length());
        }
        connection.connect();
        return connection;
    }
    public byte[] request(String path, byte[] body, String contentType, int timeout) throws IOException {
        return request(path, bytes(body), contentType, timeout);
    }
    byte[] request(String path, Body body, String contentType, int timeout) throws IOException {
        String first = preferred != null ? preferred : lan != null ? lan : LOOPBACK;
        String second = first.equals(LOOPBACK) ? lan : LOOPBACK;
        String route = first;
        HttpURLConnection connection;
        try { connection = open(first, path, body, contentType, timeout); }
        catch (IOException unreachable) {
            if (second == null) throw unreachable;
            route = second;
            connection = open(second, path, body, contentType, timeout);
        }
        preferred = route;
        try {
            if (body != null) {
                try (OutputStream output = connection.getOutputStream()) { body.writeTo(output); }
            }
            int code = connection.getResponseCode();
            InputStream source = code == 200 ? connection.getInputStream() : connection.getErrorStream();
            ByteArrayOutputStream result = new ByteArrayOutputStream();
            if (source != null) try (InputStream input = source) {
                byte[] chunk = new byte[8192]; int n;
                while ((n = input.read(chunk)) != -1) {
                    if (result.size() + n > 4 * 1024 * 1024) throw new IOException("Bridge response too large");
                    result.write(chunk, 0, n);
                }
            }
            if (code != 200) {
                String reason = "Mac bridge returned " + code;
                try { reason = new JSONObject(result.toString("UTF-8")).optString("error", reason); }
                catch (Exception ignored) { }
                throw new IOException(reason);
            }
            return result.toByteArray();
        } finally { connection.disconnect(); }
    }
    public JSONObject json(String path, JSONObject body) throws IOException {
        try { return new JSONObject(new String(request(path, body == null ? null : body.toString().getBytes(StandardCharsets.UTF_8),
            "application/json", 8000), StandardCharsets.UTF_8)); }
        catch (org.json.JSONException error) { throw new IOException("Invalid bridge response", error); }
    }
}
