# Blender VR on the Mac

Sculpting in Blender with the Quest on, from the Mac, with no Windows PC and no Air Link.

## Proven on 1 Oct 2026

- **Why it never worked:** ArX VR streams the Mac's desktops as video. Blender's VR mode needs an OpenXR runtime on the computer, a service that renders each eye and takes the head pose back. macOS had none.
- **Blender is ready.** Blender 5.2.1 for Mac is built with VR over Metal (`XR_KHR_metal_enable`, `GHOST_XrGraphicsBindingMetal`).
- **The runtime: OXRSys (OpenXR-OSX)** by Yannick Comte, MPL-2.0, [github.com/demonixis/OpenXR-OSX](https://github.com/demonixis/OpenXR-OSX).
  - It's a Mac OpenXR runtime plus a Quest client, linked over USB `adb reverse` on ports 9944, 9945, 9946 and 9948. ArX VR uses 47996 to 47999, so there is no clash.
  - Built from source in `~/arxVR_deps/OpenXR-OSX`; its 4 runtime tests pass.
- **Result:** Blender with Freebird 2.15 started a VR session through it.
  - The Quest 2 showed the scene with Freebird's controllers.
  - 72 fps, H.265 at 2272x1264, about 12 ms decode per frame on the headset, render poses matched 100%.
- **Test launcher:** `~/arxVR_deps/blender_vr_test.sh` (install the client, set the ports, start the client, open Blender with `XR_RUNTIME_JSON`). Scene and auto-start: `~/arxVR_deps/blender_vr_scene.py`, which calls `bpy.ops.freebird.xr_toggle('INVOKE_DEFAULT')`.

## Problems seen

- **Jank.** The Mac reports about 29 ms total per encoded frame against a 13.9 ms budget, with occasional encoder drops. Blender renders 1512x1680 per eye.
- **Wrong scale and height.** The world looks too big and the viewer floats above it, so the reference space or floor height is off. Freebird resets `base_scale` and `base_pose_location` in `freebird/ui/main_menu.py`.
- **Easy to break.** The client pauses whenever the headset is off your head. If it starts paused, it only shows its waiting grid.

## Found and fixed (1 Oct 2026, afternoon)

- **The crashes are in the runtime.** Every headset reconnect built a new hardware encoder; 42 reconnects left about 30 dead encoder sessions.
  - Blender's draw loop waited on the encoder lock during a stalled teardown (crash at 09:59).
  - VideoToolbox emitted a frame into an already freed session (crash at 10:11).
- **Patched in our copy (`~/arxVR_deps/OpenXR-OSX`), each marked `ArX:`.**
  - `SendFrame` uses `try_lock`, so the app's render thread skips a frame instead of waiting.
  - The old encoder is retired outside `encoderMutex_`.
  - `VideoEncoder::Shutdown` drains in-flight frames before it invalidates and releases the session.
- **The real crash cause, found and fixed with a fake headset.**
  - The frame-done callback captured the encoder (`shared_ptr`), only to read a dropped-frame count for logging.
  - After a reconnect that callback was often the last owner, so `~VideoEncoder` ran on VideoToolbox's own callback queue and `VTCompressionSessionCompleteFrames` waited on that same queue forever.
  - One stuck encoder per reconnect wedged the VideoToolbox service, the control thread jammed in `SetBitrate`, and Blender crashed minutes later.
  - Fix: the callback captures no encoder; frames are counted in the telemetry struct.
  - `~/arxVR_deps/fake_headset` (C++, uses the real `Protocol.h`) does the USB handshake on localhost and cycles connect and disconnect without a headset. It hung by cycle 12 before the fix; after it, 30 of 30 cycles were clean, and a `sample` showed 0 stuck encoders.
- **Hands drive Blender as controllers** (runtime patches, all marked `ArX:`). This works for any app that only binds controllers, Blender included.
  - When a hand's controller is put down and the hand is tracked, it presents the controller profile (with the Oculus Touch fallback), and controller bindings count as live.
  - Trigger = thumb-to-index pinch, and grip = fist, both with dead zones (0.6 up), so a relaxed hand presses nothing.
  - Grip and aim poses come from the hand joints.
  - Tests are updated. Proven in Blender with the fake headset: the right grip followed the hand, the trigger went 1.0 on pinch and 0.0 open, and the squeeze stayed 0.0 with an open hand.
- **Capture renders the headset's view.** The `mirror_xr_session` flag has no effect here, so the harness renders an offscreen image from the live viewer pose (`render_vr_view`, 96 deg vertical field of view) with the same scene, shading and add-on drawing.
- **The room faces you on connect.** The harness watches `~/Library/Application Support/OXRSys/runtime_status.json`; on each switch to `streaming` it clears navigation and turns and centres the world, so the clay is 0.6 m ahead at real floor height.
- **Mac cost per frame is now about 11 ms** to encode (budget 13.9 ms at 72 Hz), down from 29 ms on the first run.
- **There is no runtime registry on macOS.** The Khronos loader has no default runtime path on Apple ("failed to determine active runtime file path for this environment"). `~/.config/openxr` and `~/.local/share/openxr` are both ignored, and only `XR_RUNTIME_JSON` works.
  - The fix: set it inside Blender just before VR starts (`os.environ.setdefault('XR_RUNTIME_JSON', ...)`), because the loader only reads it then.
  - A plain Blender launch then works. This belongs in our add-on.
- **"Up in the sky" is Freebird's own default.** `freebird/ui/__init__.py` sets `base_scale = 10` on start, so you begin as a giant overlooking the scene; its menu has "Reset world scale". Our add-on starts at 1:1.
- **Visual loop.**
  - The harness mirrors the headset view in the desktop viewport (`SpaceView3D.mirror_xr_session`).
  - `~/arxVR_deps/grab.sh` saves that view, plus the VR pose and scale numbers, from inside Blender (`screen.screenshot_area`). It needs no screen permission and can't capture other windows.

## Plan

1. **Comfort first.** Fix the height and scale (reference space, floor offset, Freebird's base pose). Cut the jank: profile Blender's frame time against encode time, then try resolution scale 0.6 and the faster encoder preset.
2. **One click from the Mac.** Our add-on sets `XR_RUNTIME_JSON` itself and sets the USB ports when you press Start VR, so a normally launched Blender finds the headset. (Proven 1 Oct: there is no system-wide registry on macOS.)
3. **One app on the headset.** Fold the OXRSys client's decode and projection path into ArX VR as a Blender mode. When Blender starts VR, the world becomes Blender's view and the desktops stay where they are. Keep the MPL-2.0 files under their licence, with notices.
4. **Make the add-on ours.** Freebird is GPL-2, so it can be forked into the ArX VR Blender add-on (`../ArX_VR_Blender`), then extended for sculpting in our own interaction style.
