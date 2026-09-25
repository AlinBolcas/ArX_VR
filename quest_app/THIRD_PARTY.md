# Third-party components, v0.1.0

- Khronos OpenXR Android loader 1.1.49: Maven artifact org.khronos.openxr:openxr_loader_for_android. Its unmodified license is included in src/main/assets/licenses/OpenXR-LICENSE.txt and packaged in the APK.
- Android native app glue: compiled from Android NDK 27.2.12479018. Apache 2.0 notice is included in src/main/assets/licenses/Android-native-glue-NOTICE.txt and packaged in the APK.
- Android/Gradle/NDK are build dependencies, recorded in build.gradle and the toolchain instructions.

The application session/panel implementation is original code using the documented OpenXR API. Moonlight XR was inspected as reference; no Moonlight streaming or UI code is included in this APK. The panel font and icon are original. Freebird is not a dependency of this app.

References: Meta's OpenXR Android setup, Android manifest and passthrough documentation; the Khronos OpenXR API. Future adoption from Moonlight must add its source provenance and applicable licenses here before distribution.

## Workspace 0.2.0 adoption

The statement above about no Moonlight code applies only to the preserved 0.1.0 baseline. The streaming/ module is a derivative of Moonlight XR, not an independent rewrite.

- Source: Gilleece/moonlight-android-xr, revision a33b8dbcce20092c25ba5f08caa1c905864b426b. Imported app Java/resources, XR renderer, native binding and ARM64 Opus/OpenSSL libraries as one connected client boundary. GPLv3 terms apply to the upstream-derived implementation. Full text is packaged in streaming/src/main/assets/licenses/Moonlight-GPL-3.0.txt. No ArX-wide licence was changed.
- Protocol: moonlight-common-c 874ac9548f1bd6f095ef2b435c42cdde460e7821; ENet aca87840b57f045a1f7f9299e4b1b9b8e2a5e2f1; NanoRS b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a. Their licence files accompany their sources and are packaged as notices.
- Binary dependencies: OpenSSL header version 4.0.2 and upstream-provided ARM64 libcrypto.a; upstream-provided ARM64 libopus.a. These archives were not rebuilt locally. Licence texts are packaged; upstream build scripts are retained.
- Build dependencies are pinned in streaming/build.gradle. OkHttp is adapted to 4.12.0 for Android 35 compatibility; newer Android-only keyboard and producer-throttling APIs are omitted. LiteRT support code remains but model inference is disabled and model weights are absent.
- PSX Cinema: "VR Cinema Environment" by fangzhangmnm, https://skfb.ly/6VuIX, CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). Upstream baked the mesh and extracted its texture. Those room assets are included unchanged, with credits in the app. Panoramic photographs are not included.
- ArX changes: workspace home/tutorial, passthrough and mono-video defaults, build compatibility, Shot capture/metadata, reviewed capability-gated dictation, bounded capture slots, local device tools and disabled upstream report submission. Original namespace retained for JNI compatibility. Upstream reference directories remain immutable.

The exact initial imported-file hashes are in streaming/adoption.json. Subsequent ArX edits and exclusions are described in streaming/README.md. On distribution, provide the corresponding modified source and required notices under the applicable upstream terms; renaming the app does not remove those obligations. No distribution/publishing was performed in this batch.

## Mac workspace 0.3.0

`host_tools/README.md` records the new companion boundary, Chromium BSD virtual-display reference (inspected blob a160fd6eb8ae00717f421eace6b0627b9967c096), OpenWispr MIT transcription reference and existing external whisper.cpp MIT executable. Licence texts accompany the Mac app. This host adapter does not modify Sunshine or OpenWispr. The Quest client remains derived from Moonlight XR under the terms above.
