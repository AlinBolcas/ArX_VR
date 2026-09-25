# Gaussian splat worlds

The picker shows three kinds of world: Passthrough, Calm, and Gaussian splat worlds you make. This file covers the third kind: where the splats come from, how a world gets onto the headset, how the headset draws it, and how to check one.

## Where the splats come from

| Source | What it makes | Verdict |
| --- | --- | --- |
| **World Labs Marble** (World API) | A whole world you stand inside, from text, an image, a panorama or a video. SPZ at 100k, 500k and full resolution, metric scale and a ground height | **Chosen.** The only hosted source that makes an explorable world rather than one object or one view |
| Tencent HY-World 2.0 | Open world model, splats plus mesh | Self-hosted on a 16 to 80 GB GPU. The route if Marble's cost ever matters |
| Apple SHARP | Splats for one perspective view | A picture floating in the dark, nothing behind you |
| TripoSplat, object models | One object as splats | Objects, not places |
| Insta360 Spatial Capture | A real place, from a 360 camera | Good for real rooms, nothing to automate |

**One strong image beats everything else as a prompt.** Make the image first (any good image model, cents and seconds), with the viewer's spot clear and centred. Marble's multi-image input was tried with three views at set azimuths and came out worse: seams and repeated ceilings. A generated video as input is worse again: reconstruction punishes every place the video guessed differently. Video input is for real footage.

Try a prompt as `marble-1.0-draft` (about 230 credits, 30 s) before spending a `marble-1.1` world (about 1,580 credits, 5 minutes).

## The chain

```
picture, sentence, panorama or video
  -> marble_world.py      Marble through worldlabs_client.py; shows the cost and asks first
  -> SPZ or PLY           SPZ v2/v3 read directly; v4 (zstd) falls back to the PLY export
  -> splat_convert.py     OpenCV to headset frame, metric scale, floor at seated eye height, seat under you
  -> splat_merge.py       optional: clusters you cannot tell apart from the seat become one splat
  -> ~/arxVR_worlds/<name>/  world.splat, texture.jpg, panorama.jpg, ambient.ogg
  -> splat_harness.py     the headset's own shaders and packing in WebGL2, beside Spark
  -> arxvr.py "Put a world on the headset"  ->  files/worlds/<name>/ on the Quest
```

`splat_merge.py` works per angular cell, a fixed angle wide at every distance, so near things keep every splat and the far wall loses what it never needed. Moment matching keeps each cluster's light and extent. The shipped worlds used 0.4 degrees; 0.6 is about a third fewer splats again, softer far away. Nothing within 1.5 m is touched.

`~/arxVR_worlds/layout.txt` sets the picker: one folder name per line, `-` for an empty cell.

## How the headset draws a world

`xr_splat.c`, `xr_splat_data.c` and `xr_splat_shaders.h`, in `quest_app/streaming/src/main/jni/xr-renderer/`.

- **Packing.** Java reads `world.splat` into one direct buffer; the loader thread packs it as two RGBA32UI texels a splat: centre and colour, then the 3D covariance as halves scaled by its largest entry, so millimetre splats keep their precision.
- **Sorting.** A sort thread orders splats by distance from the head, furthest first, with a 16 bit counting sort, keeping only those inside an 80 degree cone around the gaze. Distance, not depth: turning never needs a new order, only moving does.
- **The sharp view.** Where you look, both eyes, drawn as instanced quads: EWA projection to a 2D ellipse, three standard deviations, premultiplied alpha, back to front. A Quest 2 cannot draw a whole world in one frame beside the desktop streams, so the view is built over several frames in slices sized by how much of the screen each splat covers. A budget watches for missed frames and shrinks or grows the slices, so the screens keep their full rate. The view overscans 16 degrees sideways and 7 up and down, and its edge dissolves in brush strokes.
- **The surround.** A lower resolution cube (640 px a face, each eye) drawn from one head position at a time, all six faces together so there are no seams, re-sorted every 1.5 s. It sits under the sharp view as an OpenXR cube layer and is painted behind each new view, so a fast turn shows the softer world and the brush-stroke edge, never black.
- **The layer limit.** A Quest 2 composites at most 16 layers a frame, and the keyboard, pie and pointers go up last. When a frame would overflow, it is rebuilt without the cube's two layers, so the controls always show.
- **The toggle.** Picking the current world again swaps the splats for its flat `panorama.jpg`, which costs nearly nothing.
- **Music.** `ambient.ogg` in the world folder loops with fades while the world is up and pauses with the app.

On a Quest 2 a new sharp view takes about 0.15 to 0.3 s and a new cube about 1.5 s. Fewer splats (a bigger merge angle) is the lever when it lags.

## Checking a world on a headset

1. Push it: `arxvr.py`, "Put a world on the headset". Restart ArX VR if it is running.
2. Pie menu, World, and pick its tile. Calm shows for a moment, then the world.
3. Look around slowly: sharp everywhere. Turn fast: soft world and brush strokes at the edge, then sharp again.
4. Lean and stand: nothing pops; the order keeps up.
5. Screens, pie and keyboard stay smooth and on top of the world.
6. The app log carries `splat world packed`, `splat world up` and `splat sort` lines.

## Checking a world on the Mac

`splat_harness.py` draws a world with the headset's own shaders and packing beside Spark 2.2, and `window.compare()` (or C) scores six views, including a lean and an asymmetric Quest 2 lens, as the mean difference per channel out of 255. A correct renderer scores well under 1; a splat size off by half scores 3 to 9.

## To make a first world

1. Get a World Labs API key (platform.worldlabs.ai) and put `WORLD_LABS_API_KEY=...` in the environment or a `.env` at the repo root.
2. `pip install numpy pillow requests`.
3. `python3 quest_app/arxvr.py`, "Make a splat world with Marble". Start with a draft to check orientation and scale.
4. Merge it, check it on the Mac, push it.
