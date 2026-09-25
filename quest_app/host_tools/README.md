# ArX VR Bridge (Mac)

The Mac half of ArX VR: desktop video, extra desktops, pointer and keyboard from the headset, and local dictation. One app, `~/Applications/ArX VR Bridge.app`. Closing its window stops the server, the capture and the extra desktops, and releases any held mouse button.

## Build and run

```bash
python3 quest_app/host_tools/build_arxvr_mac.py
```

Compiles the native parts with `clang`, installs the app to `~/Applications` (off any synced folder: sync services damage bundles, and macOS permission grants need a stable path) and signs it. `Start ArX VR.command` and `arxvr.py` build it for you on first run.

```bash
python3 quest_app/host_tools/arxvr_voice_bridge.py
```

The terminal alternative to opening the app. Launched from the app, everything goes to `dist/bridge.log`.

## Permissions and signing

The window shows Screen Recording and Accessibility. Grant both, then restart the bridge: `AXIsProcessTrusted()` can stay cached for the life of a process.

The build signs with the first Apple Development or Developer ID certificate in the keychain (override with `ARXVR_SIGNING_IDENTITY`), and both the app and the inner `ArxMacHost` use `--options runtime`. That flag matters: without the hardened runtime macOS keys Screen Recording on the binary's hash, so every rebuild silently revokes the grant while System Settings still shows it on. The only symptom is capture failing with "no main display". With it, the grant is keyed on identifier and team and survives rebuilds. With no certificate the build falls back to ad-hoc signing and warns: grants then reset on every rebuild.

A grant made against an older signature can be cleared once with `tccutil reset ScreenCapture ai.arvolve.arxvr.macbridge`.

## How it works

- `arxvr_voice_bridge.py`: the server on port 47999. Provisions the headset over USB with the Mac's address and a secret token (kept in `~/Library/Application Support/ArX VR Bridge/token`, so a restart does not orphan a running headset), sets up `adb reverse` fallbacks, relays workspace requests to the native host and serves dictation. Every request must carry the token, compared in constant time.
- `macos/ArxMacHost.m`: native Cocoa process behind a bounded JSON pipe. VR desktops are virtual displays plugged into the Mac while the headset watches them and unplugged when it stops, so macOS moves their windows to the laptop screen as with a real monitor; they come back to where they were. Also posts pointer and keyboard input through CoreGraphics once Accessibility is granted, with a two-second timeout that releases held buttons.
- `macos/ArxVideo.m`: one stream per desktop on port 47997. ScreenCaptureKit capture, VideoToolbox hardware H.264, framed Annex-B. A client sends its token and optionally a slot. A half-second heartbeat re-sends the last picture, since ScreenCaptureKit only delivers on change.
- `macos/ArxMirror.m`: *Show what the headset sees*, on port 47996, open only while that window is.
- `macos/ArxVRLauncher.m`: the app's entry point, starting the Python server and the status window.
- `LocalWhisper` (in the bridge): whisper.cpp on 16 kHz mono WAV, one inference at a time, private temporary files. Takes OpenWispr's model, language and prompt from `~/.config/open-wispr` when present, else the model `arxvr.py` downloads into `~/Library/Application Support/ArX VR Bridge/models`. Optional: without whisper.cpp or a model the bridge runs and the headset is told why dictation is off.

The bridge accepts no shell command, file path, executable, capture target or model from the headset. Workspace operations are allowlisted and native requests validated before dispatch. These checks are tested; they are not a full security audit.

`test_arxvr_video.py` decodes what the encoder sends and counts colours. It must run inside the bridge process (`ARXVR_VIDEO_TEST=1`), because capture permission belongs to the app bundle, not to a shell.

## Attribution

The virtual display declarations were informed by Chromium's BSD-licensed display test utility. The transcription invocation and marker filtering were informed by OpenWispr (MIT). whisper.cpp is an external MIT dependency and is not redistributed. Notices are in `licenses/` and the built app's Resources.
