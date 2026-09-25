# ArX VR, developer notes

The Quest app, the Mac bridge and their tools. For setup and use, see the [main README](../README.md).

## One entry point

```bash
python3 quest_app/arxvr.py
```

Interactive menu: start a session, status, build the Quest app, install it, build the Mac bridge, stop, headset view (a screenshot plus the app log, saved to `dist/headset/`), and the world tools. Enter alone starts a session, which is what `Start ArX VR.command` runs.

A session opens the bridge, waits for the Quest on USB and for the bridge to provision it, installs the newest verified build if the headset is behind, wakes it and launches the app. USB provisions the headset and carries control and dictation; the desktop video goes over Wi-Fi, with the USB loopback as fallback.

When something does not come up, read `host_tools/dist/bridge.log` first. A bridge launched from the app has no terminal, so everything it says goes there.

## Build, test and install

```bash
python3 quest_app/tests/setup_arxvr_toolchain.py
```

```bash
python3 quest_app/tests/build_arxvr_workspace.py
```

```bash
python3 quest_app/tests/install_arxvr_quest.py
```

The toolchain is isolated in `~/.arxvr_toolchain` (override with `ARXVR_TOOLCHAIN_ROOT`): Java 17, Gradle 8.9, Android SDK 35, NDK 27.2 and CMake. Nothing system-wide changes.

The build runs the Java tests, the native C tests, Android lint, signing and APK checks, then writes `dist/arxvr_v<version>_debug.apk` and `dist/workspace-build.json` with its hash. The installer and `arxvr.py` refuse an APK whose hash does not match. In the headset the app is under **Library, Unknown Sources**.

Other tests:

```bash
python3 quest_app/tests/arxvr_bridge_test.py && python3 quest_app/tests/arxvr_device_lab_test.py && python3 quest_app/tests/arxvr_world_test.py
```

`tests/arxvr_device_lab.py` is the device lab: live USB mirror, capture and diagnostics bundles under `dist/diagnostics`. Wear the headset during capture; a sleeping display returns no frames.

## Layout

- `streaming/`: the Quest app, adopted from Moonlight XR (see [its notes](streaming/README.md)). The OpenXR renderer, panels, input and the splat renderer are native C in `streaming/src/main/jni/xr-renderer/`.
- `host_tools/`: the Mac bridge (see [its notes](host_tools/README.md)).
- `host_tools/worlds/`: turning pictures and splat files into headset worlds (see [SPLAT_WORLDS.md](../docs/SPLAT_WORLDS.md)).
- `tests/`: toolchain, builds, install, device lab and unit tests.

## Gotchas already hit, do not reintroduce

- Mac status flags must be JSON booleans (`@YES`/`@NO`, never a boxed C ternary, which becomes `1`). The headset parses them with `ArxWorkspace.truthy`.
- A status reply must not carry an empty `error` key: callers read the key's presence as failure.
- A C array of file descriptors must be initialised to `-1`. `0` is a real descriptor, and the encoder would write frames into the process's own stdout.
- `adb shell` swallows piped stdin, so scripted runs pass `stdin=DEVNULL`.
- Never write a file in the same expression that reads it: `open(p, 'w')` truncates before the read happens.

Provenance and licences: [THIRD_PARTY.md](THIRD_PARTY.md).
