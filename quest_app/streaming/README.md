# Streaming workspace module

The Quest app. A buildable adaptation of the pinned Moonlight XR client, under the application ID `ai.arvolve.arxvr`.

The adopted boundary is the connected client: native protocol, audio and video decoding, XR rendering, input and UI. Splitting these into isolated copied files would break their JNI, callback and lifecycle contracts. The Java package `com.limelight` is kept to preserve JNI names, not to present the app as official Moonlight.

Since 0.5.0 the video comes from the ArX VR bridge (ScreenCaptureKit and VideoToolbox on the Mac, MediaCodec here), so Sunshine, pairing and the PIN are gone. ArX additions live in the `Arx*` and `Xr*` Java classes and in `src/main/jni/xr-renderer/`.

No upstream Git metadata, CI, signing configuration, report collector, model weights or panoramic photographs were imported. Native binaries are ARM64 only. The PSX Cinema room assets are kept with attribution.

`adoption.json` records the original hashes at import, including files later removed or modified. See [../THIRD_PARTY.md](../THIRD_PARTY.md) and the in-app Credits and licences for provenance.
