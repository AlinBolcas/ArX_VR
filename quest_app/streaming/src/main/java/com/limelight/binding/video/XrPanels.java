package com.limelight.binding.video;

import android.graphics.Typeface;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.graphics.Shader;

import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;

import static com.limelight.binding.video.XrShared.*;

/**
 * The flat panels reachable from inside the session: the environment picker,
 * the settings sheets, the keyboard and the exit prompt, and the buttons that
 * open them. Java is the only place Android will lay out text, so their art
 * is drawn to bitmaps here and handed back as pixels for the frame loop to
 * upload, since that thread owns the GL context. Nothing in here touches the
 * session, so it can run on whichever thread has the time.
 */
final class XrPanels {

    // Environment picker, a grid of thumbnails reachable from inside the
    // session. One band per category: a header strip carrying its name, then a
    // row of cells under it. The rooms are the first band, the photos from the
    // assets folder in name order are the second. A cell is a place in the
    // grid and nothing more, what gets saved is the stable id it maps to. The
    // grid and its cells are the PICKER_ and ENV_CELL_ values in XrShared,
    // which is what the native side hit tests against.
    static final String ENVIRONMENT_DIR = "environments";

    /**
     * A world is a 360 picture, and one can be put on the headset long after the app
     * was built, so the headset's own files are looked at first and the assets are
     * only what shipped with it.
     */
    static java.io.InputStream openEnvironment(Context context, String fileName) throws IOException {
        java.io.File world = fileName.endsWith("/") ? worldTexture(context, fileName.substring(0, fileName.length() - 1)) : null;
        if (world != null) return new java.io.FileInputStream(world);
        java.io.File local = new java.io.File(new java.io.File(context.getFilesDir(), ENVIRONMENT_DIR), fileName);
        if (local.isFile()) return new java.io.FileInputStream(local);
        return context.getAssets().open(ENVIRONMENT_DIR + "/" + fileName);
    }

    static final String WORLD_DIR = "worlds";
    static final String WORLD_MESH = "model.room";
    static final String WORLD_SPLAT = "world.splat";
    static final String WORLD_LAYOUT = "layout.txt";

    /**
     * The Gaussian splat worlds on the headset, one per picker cell after Calm. A world is
     * a folder holding world.splat and a picture for its tile, made on the Mac and pushed
     * over. worlds/layout.txt, if there, places them: one folder name a line, "-" for an
     * empty cell. Without it they go in name order. An empty string is an empty cell.
     */
    static String[] listWorlds(Context context) {
        java.io.File dir = new java.io.File(context.getFilesDir(), WORLD_DIR);
        java.util.TreeSet<String> names = new java.util.TreeSet<>();
        java.io.File[] found = dir.listFiles();
        if (found != null) for (java.io.File world : found) {
            if (world.isDirectory() && new java.io.File(world, WORLD_SPLAT).isFile()) names.add(world.getName());
        }
        java.io.File layout = new java.io.File(dir, WORLD_LAYOUT);
        if (!layout.isFile()) return names.toArray(new String[0]);
        java.util.List<String> placed = new java.util.ArrayList<>();
        try {
            for (String line : java.nio.file.Files.readAllLines(layout.toPath())) {
                String name = line.trim();
                if (name.isEmpty()) continue;
                placed.add(names.remove(name) ? name : "");
            }
        } catch (IOException e) {
            LimeLog.warning("World layout unreadable: " + e);
        }
        // Anything pushed but not in the layout still shows, after it
        placed.addAll(names);
        return placed.toArray(new String[0]);
    }

    /** A world's ambient loop, if it has one */
    static java.io.File worldAmbient(Context context, String world) {
        return new java.io.File(new java.io.File(new java.io.File(context.getFilesDir(), WORLD_DIR), world), "ambient.ogg");
    }

    static java.io.File worldSplat(Context context, String world) {
        return new java.io.File(new java.io.File(new java.io.File(context.getFilesDir(), WORLD_DIR), world), WORLD_SPLAT);
    }

    /** The picture a world is painted with, which is also what its tile previews */
    static java.io.File worldTexture(Context context, String world) {
        java.io.File dir = new java.io.File(new java.io.File(context.getFilesDir(), WORLD_DIR), world);
        // The full panorama first: it is what hangs behind the world, and the tile scales it down
        for (String name : new String[]{ "panorama.jpg", "texture.png", "texture.jpg", "texture.jpeg" }) {
            java.io.File file = new java.io.File(dir, name);
            if (file.isFile()) return file;
        }
        return null;
    }

    static java.io.File worldMesh(Context context, String world) {
        return new java.io.File(new java.io.File(new java.io.File(context.getFilesDir(), WORLD_DIR), world), WORLD_MESH);
    }

    /** Every world the headset has, the ones put there after the build included, in name order */
    static String[] listEnvironments(Context context) {
        java.util.TreeSet<String> names = new java.util.TreeSet<>();
        try {
            String[] shipped = context.getAssets().list(ENVIRONMENT_DIR);
            if (shipped != null) names.addAll(java.util.Arrays.asList(shipped));
        } catch (IOException none) { /* none shipped */ }
        java.io.File[] added = new java.io.File(context.getFilesDir(), ENVIRONMENT_DIR).listFiles();
        if (added != null) for (java.io.File f : added) {
            String name = f.getName().toLowerCase();
            if (f.isFile() && (name.endsWith(".jpg") || name.endsWith(".jpeg") || name.endsWith(".png"))) {
                names.add(f.getName());
            }
        }
        return names.toArray(new String[0]);
    }
    private static final String IMAGE_DIR = "images";
    private static final int PICKER_CELL_W = PICKER_TEX_W / PICKER_COLS;
    // One per band, drawn in the strip above its cells
    private static final String[] PICKER_HEADERS = { "Around you", "" };
    // Whether worlds show as their 360 panorama rather than as splats, which the header
    // under the first row says, with how to switch
    boolean worldPanorama;
    // The photos take whatever the rooms leave, so how many fit is a question
    // for the layout rather than a count kept here
    static final int MAX_PHOTOS = PICKER_CELLS - ENV_CELL_FIRST_PHOTO;

    // The settings panel behind the cog button. Drawn here, placed and dragged
    // natively, so the layout is agreed between the two through the COG_
    // values in XrShared. Three tabs, a texture each, all uploaded once so
    // switching is free, and a fourth sheet handed over after them: the screen
    // tab as it reads while a 3d room hangs the picture, which the native side
    // picks for itself.
    private static final String[] COG_TABS = { "Screen", "Display", "3D" };
    private static final String[] COG_SLIDER_ROWS =
            { "Distance", "Height", "Tilt", "Rotate", "Curve", "Size" };
    // What stands in for those rows in a room, where the wall decides both the
    // placement and the size
    private static final String COG_ROOM_NOTICE =
            "Screen size cannot be changed in a 3D environment. "
                    + "Please choose a different environment to customise the screen size.";
    // Display tab: a label and a row of cells, one of which is in force, and
    // the glow level track under them. Head locked sits with the picture rows
    // so the two light rows and the level track they belong with stay together
    // at the bottom. Screen light is the wash the picture throws over a 3d
    // room, which only shows in one, and head lock is ignored in one, but both
    // stay live here like the rest: the picker can put a room up at any moment.
    private static final String[] COG_OPTION_ROWS =
            { "Sharpen", "Stats", "Head locked", "Glow", "Screen light" };
    private static final String[][] COG_OPTION_CELLS = {
            { "Off", "Normal", "Quality" },
            { "Off", "On" },
            { "Off", "On" },
            { "Off", "On" },
            { "Off", "On" }
    };
    // 3D tab: two sliders, drawn the same way the screen tab's are. Only
    // values that take effect the moment they move belong on the panel, which
    // is why the depth source itself stays in the 2d settings.
    private static final String[] COG_SLIDER3D_ROWS = { "Depth", "Convergence" };
    // Where the measured comfort cap, which is also the shipped default, falls
    // along the separation track
    private static final float COG_SEP_CAP_T =
            PreferenceConfiguration.DEFAULT_VR_SEPARATION / (float)COG_SEP_STEPS;

    // The in world keyboard. Three sheets of the same layout, one per state,
    // handed over in state order, along with the geometry that goes with them:
    // the native side is given key rectangles and codes and knows nothing else
    // about it. The sheet size and the code values are the KB_ values in
    // XrShared.
    // Key widths per row, in units where a plain key is 1, and where each row
    // starts. One table for all three states, so every state has to lay its
    // keys out the same way.
    // Five plain rows. Workspace actions live on the dock, not here.
    private static final float[][] KB_ROW_WIDTHS = {
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1.5f, 1, 1, 1, 1, 1, 1, 1, 1.5f },
            { 1.5f, 1.25f, 1.25f, 3, 1, 2 }
    };
    // Only the home row is inset, the way it is on a real keyboard
    private static final float[] KB_ROW_INDENT = { 0.0f, 0.0f, 0.5f, 0.0f, 0.0f };
    private static final float KB_ROW_UNITS = 10.0f;
    // Margins and the gap between two keys, all as fractions of the panel
    private static final float KB_PAD_U = 0.012f;
    private static final float KB_PAD_V = 0.030f;
    private static final float KB_GAP_U = 0.005f;
    private static final float KB_GAP_V = 0.014f;

    private static final String[][] KB_LABELS_LOWER = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
            { "a", "s", "d", "f", "g", "h", "j", "k", "l" },
            { "Shift", "z", "x", "c", "v", "b", "n", "m", "Delete" },
            { "?123", "Cmd", "Ctrl", "space", ".", "Return" }
    };
    private static final int[][] KB_CODES_LOWER = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' },
            { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l' },
            { KB_CODE_SHIFT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', 8 },
            { KB_CODE_SYMBOLS, KB_CODE_CMD, KB_CODE_CTRL, 32, '.', 13 }
    };
    private static final String[][] KB_LABELS_UPPER = {
            { "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" },
            { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
            { "A", "S", "D", "F", "G", "H", "J", "K", "L" },
            { "Shift", "Z", "X", "C", "V", "B", "N", "M", "Delete" },
            { "?123", "Cmd", "Ctrl", "space", ",", "Return" }
    };
    private static final int[][] KB_CODES_UPPER = {
            { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' },
            { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },
            { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L' },
            { KB_CODE_SHIFT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 8 },
            { KB_CODE_SYMBOLS, KB_CODE_CMD, KB_CODE_CTRL, 32, ',', 13 }
    };
    // Navigation lives on the symbols page: Esc, Tab and the four arrows
    private static final String[][] KB_LABELS_SYMBOLS = {
            { "Esc", "Tab", "←", "↓", "↑", "→", "~", "`", "|", "^" },
            { "@", "#", "$", "%", "&", "*", "-", "+", "(", ")" },
            { "!", "\"", "'", ":", ";", "/", "?", "_", "=" },
            { ",", "<", ">", "[", "]", "{", "}", "\\", "Delete" },
            { "ABC", "Cmd", "Ctrl", "space", ".", "Return" }
    };
    private static final int[][] KB_CODES_SYMBOLS = {
            { 27, 9, 65020, 65023, 65022, 65021, '~', '`', '|', '^' },
            { '@', '#', '$', '%', '&', '*', '-', '+', '(', ')' },
            { '!', '"', '\'', ':', ';', '/', '?', '_', '=' },
            { ',', '<', '>', '[', ']', '{', '}', '\\', 8 },
            { KB_CODE_SYMBOLS, KB_CODE_CMD, KB_CODE_CTRL, 32, '.', 13 }
    };

    // The dock: six equal actions across one strip. No settings panel: good defaults,
    // and distance and curve are set by hand while holding a panel
    static final String[] DOCK_LABELS = { "Keyboard", "Dictate", "Add desktop", "Overview", "Guide", "Exit" };
    static final int[] DOCK_CODES = { KB_CODE_KEYBOARD, KB_CODE_VOICE, KB_CODE_SCREEN, KB_CODE_OVERVIEW, KB_CODE_GUIDE, KB_CODE_EXIT };
    static final float[] DOCK_RECTS = buildDockRects();

    // The pie menu that replaced the dock on screen, clockwise from the top:
    // Overview up, Exit down, new desktop and keyboard right, guide and dictate left,
    // the two panels on the diagonals
    // Settings is gone: the screens are placed by hand now, and the one setting left worth a
    // switch, head lock, has a slice of its own
    static final String[] PIE_LABELS = { "Overview", "Add desktop", "Keyboard", "Head lock", "Exit", "Dictate", "Guide", "World" };
    static final int[] PIE_CODES = { KB_CODE_OVERVIEW, KB_CODE_SCREEN, KB_CODE_KEYBOARD, KB_CODE_HEADLOCK,
            KB_CODE_EXIT, KB_CODE_VOICE, KB_CODE_GUIDE, KB_CODE_ENVIRONMENT };

    private static float[] buildDockRects() {
        int n = DOCK_CODES.length;
        float pad = 0.012f, w = (1.0f - 2 * pad) / n;
        float[] rects = new float[n * 4];
        for (int i = 0; i < n; i++) {
            rects[i * 4] = pad + i * w + 0.004f;
            rects[i * 4 + 1] = 0.10f;
            rects[i * 4 + 2] = pad + (i + 1) * w - 0.004f;
            rects[i * 4 + 3] = 0.90f;
        }
        return rects;
    }

    // The button that ends the stream and the prompt it opens. One sheet per
    // lit button, in zone order, so which one shows is a swapchain handle on
    // the native side rather than an upload. The sheet and where its buttons
    // sit on it are the EXIT_ values in XrShared.
    private static final String EXIT_QUESTION = "Exit the stream?";

    private final Context context;
    // The photos in the assets folder, in the order the picker shows them
    private final String[] environmentFiles;

    XrPanels(Context context, String[] environmentFiles) {
        this.context = context;
        this.environmentFiles = environmentFiles;
    }

    // Everything the keyboard hands over: the three sheets in state order, the
    // button that opens it, and the geometry they were all drawn from
    static final class Keyboard {
        final ByteBuffer lower;
        final ByteBuffer upper;
        final ByteBuffer symbols;
        final ByteBuffer button;
        final float[] keyRects;
        final int[] codesLower;
        final int[] codesUpper;
        final int[] codesSymbols;

        Keyboard(ByteBuffer lower, ByteBuffer upper, ByteBuffer symbols, ByteBuffer button,
                 float[] keyRects, int[] codesLower, int[] codesUpper, int[] codesSymbols) {
            this.lower = lower;
            this.upper = upper;
            this.symbols = symbols;
            this.button = button;
            this.keyRects = keyRects;
            this.codesLower = codesLower;
            this.codesUpper = codesUpper;
            this.codesSymbols = codesSymbols;
        }
    }

    // Where a cell sits in the picker texture: along to its column, then down
    // past the headers of its own band and the ones above it. The native side
    // ends up at the same place from the PICKER_ constants.
    private static RectF pickerTile(int cell, float pad) {
        float left = (cell % PICKER_COLS) * PICKER_CELL_W;
        float top = (cell / PICKER_COLS) * PICKER_BAND_PX + PICKER_HEADER_PX;
        return new RectF(left + pad, top + pad,
                left + PICKER_CELL_W - pad, top + PICKER_CELL_PX - pad);
    }

    /**
     * Draws the grid. Java is the only place Android will lay out text, so the
     * labels have to be baked into the texture here rather than drawn in the
     * shader.
     */
    ByteBuffer buildPickerGrid() {
        final float pad = 7.0f;
        // Matches the radius of the hover ring drawn over it, which is a
        // fraction of the cell rather than a pixel count
        final float radius = PICKER_CELL_W * 0.125f;

        Bitmap grid = Bitmap.createBitmap(PICKER_TEX_W, PICKER_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(grid);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xE0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, PICKER_TEX_W - 1.0f, PICKER_TEX_H - 1.0f),
                radius * 0.6f, radius * 0.6f, paint);

        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setColor(Color.WHITE);
        label.setTextSize(21.0f);
        label.setTextAlign(Paint.Align.CENTER);

        // The category names, quieter than the tile labels so they read as
        // headings rather than as another row of things to press
        Paint header = new Paint(Paint.ANTI_ALIAS_FLAG);
        header.setColor(0xB0FFFFFF);
        header.setTextSize(22.0f);
        header.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics metrics = header.getFontMetrics();
        float baseline = (PICKER_HEADER_PX - (metrics.descent - metrics.ascent)) * 0.5f
                - metrics.ascent;
        for (int band = 0; band < PICKER_ROWS && band < PICKER_HEADERS.length; band++) {
            String text = band == 1
                    ? (worldPanorama ? "Worlds as 360 panoramas · pick yours again for 3D"
                                     : "Worlds in 3D · pick yours again for its 360 panorama")
                    : PICKER_HEADERS[band];
            canvas.drawText(text, PICKER_TEX_W * 0.5f, band * PICKER_BAND_PX + baseline, header);
        }

        for (int cell = 0; cell < PICKER_CELLS; cell++) {
            RectF tile = pickerTile(cell, pad);

            String name;
            Bitmap thumb = null;
            if (cell == ENV_CELL_PASSTHROUGH) {
                name = "Passthrough";
                paint.setColor(0xFF2A3540);
            }
            else if (cell == ENV_CELL_VOID) {
                name = "Calm";
                paint.setColor(0xFF141A2A);
            }
            else if (cell - ENV_CELL_FIRST_PHOTO < environmentFiles.length
                    && !environmentFiles[cell - ENV_CELL_FIRST_PHOTO].isEmpty()) {
                name = labelFor(environmentFiles[cell - ENV_CELL_FIRST_PHOTO]);
                thumb = decodeThumb(environmentFiles[cell - ENV_CELL_FIRST_PHOTO], (int)tile.height());
                paint.setColor(0xFF1E1E20);
            }
            else {
                continue;
            }

            if (thumb != null) {
                // Scaled to cover and centred, so the middle of the panorama
                // becomes the preview rather than a squashed whole sphere
                BitmapShader shader = new BitmapShader(thumb, Shader.TileMode.CLAMP,
                                                       Shader.TileMode.CLAMP);
                float scale = Math.max(tile.width() / thumb.getWidth(),
                                       tile.height() / thumb.getHeight());
                Matrix m = new Matrix();
                m.setScale(scale, scale);
                m.postTranslate(tile.centerX() - thumb.getWidth() * scale * 0.5f,
                                tile.centerY() - thumb.getHeight() * scale * 0.5f);
                shader.setLocalMatrix(m);
                paint.setShader(shader);
            }
            paint.setStyle(Paint.Style.FILL);
            canvas.drawRoundRect(tile, radius, radius, paint);
            paint.setShader(null);
            if (thumb != null) {
                thumb.recycle();
            }
            if (cell == ENV_CELL_MINIMAL_ROOM) {
                drawRoomTile(canvas, paint, tile, radius);
            }
            else if (cell == ENV_CELL_PSX_CINEMA) {
                drawCinemaTile(canvas, paint, tile, radius);
            }

            // Dark band under the label, clipped to the bottom of the tile so
            // it keeps the rounded corners it sits in
            canvas.save();
            canvas.clipRect(tile.left, tile.bottom - 44.0f, tile.right, tile.bottom);
            paint.setColor(0xC0000000);
            canvas.drawRoundRect(tile, radius, radius, paint);
            canvas.restore();

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2.0f);
            paint.setColor(0x50FFFFFF);
            canvas.drawRoundRect(tile, radius, radius, paint);
            paint.setStyle(Paint.Style.FILL);

            canvas.drawText(name, tile.centerX(), tile.bottom - 15.0f, label);
        }

        ByteBuffer pixels = toBuffer(grid);
        grid.recycle();
        return pixels;
    }

    /**
     * The thumbnail for a room cell, drawn rather than photographed: a lit
     * screen on the wall of a bare dark room, with a faint line low down where
     * the floor meets it.
     */
    private void drawRoomTile(Canvas canvas, Paint paint, RectF tile, float radius) {
        canvas.save();
        // Clipped to the tile so nothing leaks past the rounded corners
        Path clip = new Path();
        clip.addRoundRect(tile, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);

        final float w = tile.width();
        final float h = tile.height();

        // A 16:9 screen sitting in the upper middle, with a wider soft rect
        // behind it standing in for the light it throws on the wall
        float screenW = w * 0.62f;
        float screenH = screenW * 9.0f / 16.0f;
        float screenTop = tile.top + h * 0.24f;
        RectF screen = new RectF(tile.centerX() - screenW * 0.5f, screenTop,
                tile.centerX() + screenW * 0.5f, screenTop + screenH);

        RectF halo = new RectF(screen);
        halo.inset(-w * 0.07f, -h * 0.07f);
        paint.setColor(0x38A6C4F0);
        canvas.drawRoundRect(halo, radius * 0.7f, radius * 0.7f, paint);
        paint.setColor(0xFFDCE6F4);
        canvas.drawRect(screen, paint);

        // Where the floor meets the wall, faint enough to read as a room
        // rather than as a line across the tile
        paint.setColor(0x28FFFFFF);
        float floorY = tile.top + h * 0.78f;
        canvas.drawRect(new RectF(tile.left, floorY, tile.right, floorY + 1.5f), paint);

        canvas.restore();
    }

    /**
     * The thumbnail for the cinema cell: a lit screen between the deep red side
     * curtains, which is about all of that room that reads at this size.
     */
    private void drawCinemaTile(Canvas canvas, Paint paint, RectF tile, float radius) {
        canvas.save();
        Path clip = new Path();
        clip.addRoundRect(tile, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);

        final float w = tile.width();
        final float h = tile.height();

        // The picture, narrower than the bare room's since the curtains take
        // the sides of the tile
        float screenW = w * 0.50f;
        float screenH = screenW * 9.0f / 16.0f;
        float screenTop = tile.top + h * 0.27f;
        RectF screen = new RectF(tile.centerX() - screenW * 0.5f, screenTop,
                tile.centerX() + screenW * 0.5f, screenTop + screenH);

        RectF halo = new RectF(screen);
        halo.inset(-w * 0.07f, -h * 0.07f);
        paint.setColor(0x34C4D6F0);
        canvas.drawRoundRect(halo, radius * 0.7f, radius * 0.7f, paint);
        paint.setColor(0xFFE2E9F6);
        canvas.drawRect(screen, paint);

        // Curtains over the ends of that halo, so the light reads as coming
        // from behind them
        final float curtainW = w * 0.21f;
        paint.setColor(0xFF7C1319);
        canvas.drawRect(new RectF(tile.left, tile.top, tile.left + curtainW, tile.bottom), paint);
        canvas.drawRect(new RectF(tile.right - curtainW, tile.top, tile.right, tile.bottom), paint);

        // Three pleats apiece, which is what says curtain rather than red panel
        final float pleatW = w * 0.013f;
        paint.setColor(0xFF4A0B10);
        for (int i = 1; i < 4; i++) {
            float along = curtainW * (i / 4.0f);
            float left = tile.left + along;
            canvas.drawRect(new RectF(left, tile.top, left + pleatW, tile.bottom), paint);
            float right = tile.right - curtainW + along;
            canvas.drawRect(new RectF(right, tile.top, right + pleatW, tile.bottom), paint);
        }

        // The front of the stage, faint enough to read as the dark of the room
        // rather than as a line across the tile
        paint.setColor(0x20FFFFFF);
        float stageY = tile.top + h * 0.76f;
        canvas.drawRect(new RectF(tile.left + curtainW, stageY,
                tile.right - curtainW, stageY + 1.5f), paint);

        canvas.restore();
    }

    // The padlocks and the cog ship as PNGs. Colour carries the state, so there
    // is nothing to tint or dim here, just a decode and a downscale to whatever
    // the swapchain it is headed for wants.
    private Bitmap loadIcon(String fileName, int size) {
        InputStream in = null;
        try {
            in = context.getAssets().open(IMAGE_DIR + "/" + fileName);
            Bitmap full = BitmapFactory.decodeStream(in);
            if (full == null) {
                LimeLog.warning("Icon " + fileName + " did not decode");
                return null;
            }
            if (full.getWidth() == size && full.getHeight() == size) {
                return full;
            }
            Bitmap scaled = Bitmap.createScaledBitmap(full, size, size, true);
            if (scaled != full) {
                full.recycle();
            }
            return scaled;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Icon " + fileName + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    // A framed landscape, which is about as much as reads at this size
    ByteBuffer buildEnvButton() {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(46.0f, 46.0f, 9.0f, paint);

        Path hills = new Path();
        hills.moveTo(26.0f, 100.0f);
        hills.lineTo(54.0f, 58.0f);
        hills.lineTo(73.0f, 84.0f);
        hills.lineTo(84.0f, 70.0f);
        hills.lineTo(102.0f, 100.0f);
        hills.close();
        canvas.drawPath(hills, paint);

        return toBuffer(button);
    }

    // The padlock shut, then open, or nothing at all
    ByteBuffer[] buildLockIcons() {
        Bitmap shut = loadIcon("handtracking_locked.png", LOCK_TEX);
        Bitmap open = loadIcon("handtracking_unlocked.png", LOCK_TEX);
        // Both or neither, since one on its own would leave the button blank
        // in half its states
        ByteBuffer[] icons = null;
        if (shut != null && open != null) {
            icons = new ByteBuffer[] { toBuffer(shut), toBuffer(open) };
        }
        if (shut != null) {
            shut.recycle();
        }
        if (open != null) {
            open.recycle();
        }
        return icons;
    }

    /**
     * The settings panel. A texture per tab and one more for the screen tab in
     * a room, all drawn once here, so changing tab in the session picks
     * another swapchain rather than redrawing anything. Only the labels, tracks
     * and cells live in the texture: thumbs and selection rings are quads of
     * their own, so using the panel costs no upload.
     */
    ByteBuffer[] buildCogTabs(boolean curveOk, boolean stereoOk) {
        Bitmap screenTab = buildCogTab(COG_TAB_SCREEN, curveOk, stereoOk);
        ByteBuffer screen = toBuffer(screenTab);
        screenTab.recycle();

        Bitmap displayTab = buildCogTab(COG_TAB_DISPLAY, curveOk, stereoOk);
        ByteBuffer display = toBuffer(displayTab);
        displayTab.recycle();

        Bitmap tab3d = buildCogTab(COG_TAB_3D, curveOk, stereoOk);
        ByteBuffer sheet3d = toBuffer(tab3d);
        tab3d.recycle();

        Bitmap roomTab = buildCogRoomTab();
        ByteBuffer room = toBuffer(roomTab);
        roomTab.recycle();

        return new ByteBuffer[] { screen, display, sheet3d, room };
    }

    // The cog that opens the settings panel
    ByteBuffer buildCogButton() {
        // Never blank: the drawn gear stands in if the art does not decode
        Bitmap button = loadIcon("settings_icon.png", BUTTON_TEX);
        if (button == null) {
            button = buildCogFallback();
        }
        ByteBuffer pixels = toBuffer(button);
        button.recycle();
        return pixels;
    }

    // The screen tab as it reads inside a 3d room: the same chrome, and a note
    // where the rows would be, since the room hangs and sizes the picture
    // itself. The native side shows this sheet in place of the screen tab
    // while a room is on.
    private Bitmap buildCogRoomTab() {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, COG_TAB_SCREEN);
        drawCogRoomNotice(canvas);
        return bitmap;
    }

    private Bitmap buildCogTab(int tab, boolean curveOk, boolean stereoOk) {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, tab);
        if (tab == COG_TAB_SCREEN) {
            drawCogSliderRows(canvas, curveOk);
        }
        else if (tab == COG_TAB_3D) {
            drawCog3dRows(canvas, stereoOk);
        }
        else {
            drawCogOptionRows(canvas);
        }
        return bitmap;
    }

    // Background and tab bar, the part both tabs have in common. The tab this
    // texture belongs to is the one drawn as current.
    private void drawCogChrome(Canvas canvas, int tab) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0101A29);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, COG_TEX_W - 1.0f, COG_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(25.0f);
        text.setTextAlign(Paint.Align.CENTER);

        final float barB = COG_TAB_BAR_B * COG_TEX_H;
        final float slotW = COG_TEX_W / (float)COG_TABS.length;
        for (int i = 0; i < COG_TABS.length; i++) {
            boolean current = i == tab;
            RectF slot = new RectF(i * slotW + 12.0f, 12.0f, (i + 1) * slotW - 12.0f, barB - 8.0f);

            if (current) {
                paint.setColor(0x28FFFFFF);
                canvas.drawRoundRect(slot, 14.0f, 14.0f, paint);
            }

            text.setColor(current ? Color.WHITE : 0x60FFFFFF);
            canvas.drawText(COG_TABS[i], slot.centerX(),
                    slot.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

            if (current) {
                // The underline is what carries at a glance, the fill alone is
                // too subtle at this size
                paint.setColor(0xEEFFFFFF);
                canvas.drawRect(slot.left + 24.0f, slot.bottom - 4.0f,
                        slot.right - 24.0f, slot.bottom, paint);
            }
        }

        paint.setColor(0x30FFFFFF);
        canvas.drawRect(20.0f, barB, COG_TEX_W - 20.0f, barB + 2.0f, paint);
    }

    // Screen tab: a label and a track per row, and the reset button under them
    private void drawCogSliderRows(Canvas canvas, boolean curveOk) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);

        for (int row = 0; row < COG_SLIDER_ROWS.length; row++) {
            boolean live = row != COG_SLIDER_CURVE || curveOk;
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;

            text.setColor(live ? Color.WHITE : 0x30FFFFFF);
            // Centred on the row rather than sitting on it, so the label lines
            // up with the track beside it
            canvas.drawText(COG_SLIDER_ROWS[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            track.setColor(live ? 0x66FFFFFF : 0x30FFFFFF);
            canvas.drawLine(COG_TRACK_L * COG_TEX_W, y, COG_TRACK_R * COG_TEX_W, y, track);

            if (row == COG_SLIDER_TILT || row == COG_SLIDER_ROTATE) {
                // Marks level, which is where the middle of these two tracks
                // snaps to. The rows that do not snap stay unmarked.
                float midX = (COG_TRACK_L + COG_TRACK_R) * 0.5f * COG_TEX_W;
                float tickHalf = COG_CELL_HALF * COG_TEX_H;
                canvas.drawRect(midX - 2.0f, y - tickHalf, midX + 2.0f, y + tickHalf, tick);
            }
        }

        // A way back for a screen dragged somewhere unrecoverable
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Reset", reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    // What the screen tab carries in a room instead of its rows: the reason
    // there are none, centred in the body under the tab bar
    private void drawCogRoomNotice(Canvas canvas) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(0xC0FFFFFF);

        String[] lines = wrapText(COG_ROOM_NOTICE, text, 0.80f * COG_TEX_W);
        float step = (text.descent() - text.ascent()) * 1.4f;
        float middle = (COG_TAB_BAR_B * COG_TEX_H + COG_TEX_H) * 0.5f;
        float y = middle - (lines.length - 1) * step * 0.5f
                - (text.ascent() + text.descent()) * 0.5f;
        for (String line : lines) {
            canvas.drawText(line, COG_TEX_W * 0.5f, y, text);
            y += step;
        }
    }

    // Greedy word wrap, which is all one fixed sentence on a fixed panel needs
    private static String[] wrapText(String message, Paint paint, float width) {
        ArrayList<String> lines = new ArrayList<>();
        StringBuilder line = new StringBuilder();
        for (String word : message.split(" ")) {
            if (line.length() > 0 && paint.measureText(line + " " + word) > width) {
                lines.add(line.toString());
                line.setLength(0);
            }
            if (line.length() > 0) {
                line.append(' ');
            }
            line.append(word);
        }
        if (line.length() > 0) {
            lines.add(line.toString());
        }
        return lines.toArray(new String[0]);
    }

    // 3D tab: the two values worth reaching mid stream. Depth runs past the
    // comfortable range on purpose, with the far end marked, since where that
    // range ends is a matter of eyes rather than of hardware.
    private void drawCog3dRows(Canvas canvas, boolean stereoOk) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(stereoOk ? Color.WHITE : 0x30FFFFFF);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(stereoOk ? 0xCCFFFFFF : 0x30FFFFFF);

        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        final float tickHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < COG_SLIDER3D_ROWS.length; row++) {
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(COG_SLIDER3D_ROWS[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            // The default sits a third along the depth track and halfway along
            // convergence, and a tick says so on both
            float markT = row == 0 ? COG_SEP_CAP_T : 0.5f;
            float markX = trackL + markT * (trackR - trackL);

            if (row == 0) {
                // Measured on device: past 0.5 percent the depth stops growing
                // and only the strain does, so the rest of the track is drawn
                // as a place you can go rather than one you should
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, markX, y, track);
                track.setColor(stereoOk ? 0x66FFB74D : 0x30FFB74D);
                canvas.drawLine(markX, y, trackR, y, track);

                Paint caption = new Paint(Paint.ANTI_ALIAS_FLAG);
                caption.setTextSize(15.0f);
                caption.setTextAlign(Paint.Align.CENTER);
                caption.setColor(stereoOk ? 0xA0FFB74D : 0x30FFB74D);
                // Just above the next row's hit band, which starts 0.055 down
                // now the rows sit closer together
                canvas.drawText("harder on the eyes", (markX + trackR) * 0.5f,
                        y + 0.04f * COG_TEX_H, caption);
            }
            else {
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, trackR, y, track);
            }

            canvas.drawRect(markX - 2.0f, y - tickHalf, markX + 2.0f, y + tickHalf, tick);
        }

        // A way back from a pair of values that turned out to be unwatchable
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Reset", reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

        if (!stereoOk) {
            // Otherwise two dead sliders with no explanation
            Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
            hint.setTextSize(17.0f);
            hint.setTextAlign(Paint.Align.CENTER);
            hint.setColor(0x50FFFFFF);
            canvas.drawText("3D is off in settings", COG_TEX_W * 0.5f,
                    0.62f * COG_TEX_H, hint);
        }
    }

    // Display tab: a label and a row of cells, one press wide each. Which cell
    // is in force and which is under the ray are rings the native side puts
    // over them, so nothing here has to be redrawn when one is chosen.
    private void drawCogOptionRows(Canvas canvas) {
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextSize(22.0f);
        label.setTextAlign(Paint.Align.LEFT);
        label.setColor(Color.WHITE);

        Paint cellText = new Paint(Paint.ANTI_ALIAS_FLAG);
        cellText.setTextSize(19.0f);
        cellText.setTextAlign(Paint.Align.CENTER);
        cellText.setColor(Color.WHITE);

        Paint cell = new Paint(Paint.ANTI_ALIAS_FLAG);

        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        final float cellHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < COG_OPTION_ROWS.length; row++) {
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(COG_OPTION_ROWS[row], 0.06f * COG_TEX_W,
                    y - (label.ascent() + label.descent()) * 0.5f, label);

            String[] names = COG_OPTION_CELLS[row];
            float span = (trackR - trackL) / names.length;
            for (int i = 0; i < names.length; i++) {
                // Inset so neighbours read as separate buttons rather than one
                // long strip
                RectF box = new RectF(trackL + i * span + 3.0f, y - cellHalf,
                        trackL + (i + 1) * span - 3.0f, y + cellHalf);

                cell.setStyle(Paint.Style.FILL);
                cell.setColor(0x28FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, cell);
                cell.setStyle(Paint.Style.STROKE);
                cell.setStrokeWidth(2.0f);
                cell.setColor(0x50FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, cell);

                canvas.drawText(names[i], box.centerX(),
                        box.centerY() - (cellText.ascent() + cellText.descent()) * 0.5f,
                        cellText);
            }
        }

        // How strong the glow is, a track under the cells and the only row on
        // this tab that is dragged rather than pressed
        float y = (COG_ROW_V0 + COG_DISPLAY_SLIDER_ROW * COG_ROW_STEP) * COG_TEX_H;
        canvas.drawText("Glow level", 0.06f * COG_TEX_W,
                y - (label.ascent() + label.descent()) * 0.5f, label);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);
        track.setColor(0x66FFFFFF);
        canvas.drawLine(trackL, y, trackR, y, track);

        // Marks the default, halfway, the same way the 3D tab marks its two
        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);
        float midX = (trackL + trackR) * 0.5f;
        canvas.drawRect(midX - 2.0f, y - cellHalf, midX + 2.0f, y + cellHalf, tick);
    }

    // The fallback cog, drawn only when the icon asset is missing. About as
    // much of a gear as reads at this size.
    private Bitmap buildCogFallback() {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = BUTTON_TEX * 0.5f;
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(10.0f);
        canvas.drawCircle(mid, mid, 34.0f, paint);

        paint.setStrokeWidth(12.0f);
        paint.setStrokeCap(Paint.Cap.ROUND);
        for (int tooth = 0; tooth < 8; tooth++) {
            double angle = tooth * Math.PI / 4.0;
            float dx = (float)Math.cos(angle);
            float dy = (float)Math.sin(angle);
            canvas.drawLine(mid + dx * 34.0f, mid + dy * 34.0f,
                            mid + dx * 48.0f, mid + dy * 48.0f, paint);
        }

        // A ring rather than a filled dot, which reads as a hole through the
        // middle of the gear the way a real one does
        paint.setStrokeCap(Paint.Cap.BUTT);
        paint.setStrokeWidth(8.0f);
        canvas.drawCircle(mid, mid, 14.0f, paint);

        return button;
    }

    /**
     * The in world keyboard: one sheet of art per state, the button that opens
     * it, and the layout the native side hit tests against. All three sheets
     * share one set of key rectangles, so the art and the hit test are built
     * from the same numbers and cannot drift apart.
     */
    // ArX palette: charcoal glass, hairlines, off-white ink, one deep blue accent
    private static final int PANEL_FILL = 0xEB0C1118;
    private static final int CARD_FILL = 0xFF131A24;
    private static final int KEY_FILL = 0xFF1B2430;
    private static final int KEY_MOD_FILL = 0xFF121922;
    private static final int HAIRLINE = 0xFF2A3544;
    private static final int INK = 0xFFF2F5F9;
    private static final int INK_SOFT = 0xFFA9B6C7;
    private static final int ACCENT = 0xFF1F55A6;
    // The accent lifted for small type on the dark panel, where the full blue sinks
    private static final int ACCENT_SOFT = 0xFF6F9BDB;
    private static final int RECORD = 0xFFD6453A;

    private static Paint ink(float size, int color, boolean medium) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(color);
        p.setTextSize(size);
        p.setTypeface(Typeface.create(medium ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        return p;
    }

    /** The dock strip. Dictating lights the Dictate action while the microphone is live. */
    ByteBuffer buildDock(boolean dictating, boolean exitArmed) {
        Bitmap bitmap = Bitmap.createBitmap(DOCK_TEX_W, DOCK_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setColor(PANEL_FILL);
        RectF body = new RectF(1, 1, DOCK_TEX_W - 1, DOCK_TEX_H - 1);
        canvas.drawRoundRect(body, 36, 36, fill);
        Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        edge.setStyle(Paint.Style.STROKE);
        edge.setStrokeWidth(1.5f);
        edge.setColor(HAIRLINE);
        canvas.drawRoundRect(body, 36, 36, edge);

        Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3.5f);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        Paint label = ink(22, INK_SOFT, true);
        label.setTextAlign(Paint.Align.CENTER);

        for (int i = 0; i < DOCK_CODES.length; i++) {
            RectF cell = new RectF(DOCK_RECTS[i * 4] * DOCK_TEX_W, DOCK_RECTS[i * 4 + 1] * DOCK_TEX_H,
                    DOCK_RECTS[i * 4 + 2] * DOCK_TEX_W, DOCK_RECTS[i * 4 + 3] * DOCK_TEX_H);
            boolean live = dictating && DOCK_CODES[i] == KB_CODE_VOICE;
            boolean armed = exitArmed && DOCK_CODES[i] == KB_CODE_EXIT;
            if (live || armed) {
                fill.setColor(RECORD);
                canvas.drawRoundRect(cell, 24, 24, fill);
            }
            float cx = cell.centerX(), cy = DOCK_TEX_H * 0.40f;
            stroke.setColor(INK);
            drawDockIcon(canvas, stroke, DOCK_CODES[i], cx, cy);
            label.setColor(live || armed ? INK : INK_SOFT);
            canvas.drawText(live ? "Listening" : armed ? "Tap again" : DOCK_LABELS[i], cx, DOCK_TEX_H * 0.80f, label);
        }
        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    // The round close button past an extra desktop's move bar: the dock's panel
    // fill and hairline, with a thin cross in the soft ink, so it reads as part
    // of the same furniture rather than something stuck over the picture
    ByteBuffer buildClose() {
        Bitmap bitmap = Bitmap.createBitmap(CLOSE_TEX, CLOSE_TEX, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        float c = CLOSE_TEX * 0.5f, r = CLOSE_TEX * 0.5f - 3.0f;
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setColor(PANEL_FILL);
        canvas.drawCircle(c, c, r, fill);
        Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        edge.setStyle(Paint.Style.STROKE);
        edge.setStrokeWidth(2.5f);
        edge.setColor(HAIRLINE);
        canvas.drawCircle(c, c, r, edge);
        Paint cross = new Paint(Paint.ANTI_ALIAS_FLAG);
        cross.setStyle(Paint.Style.STROKE);
        cross.setStrokeWidth(7.0f);
        cross.setStrokeCap(Paint.Cap.ROUND);
        cross.setColor(INK_SOFT);
        float arm = r * 0.36f;
        canvas.drawLine(c - arm, c - arm, c + arm, c + arm, cross);
        canvas.drawLine(c + arm, c - arm, c - arm, c + arm, cross);
        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    /**
     * The pie menu: a ring of slices on the dock's charcoal glass, the lit one in the
     * accent, and what it does written in the middle.
     */
    ByteBuffer buildPie(int hover, boolean dictating, boolean exitArmed) {
        return buildPie(hover, dictating, exitArmed, false);
    }

    ByteBuffer buildPie(int hover, boolean dictating, boolean exitArmed, boolean headLocked) {
        Bitmap bitmap = Bitmap.createBitmap(PIE_TEX, PIE_TEX, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        float c = PIE_TEX * 0.5f, outer = c - 4, inner = PIE_TEX * 0.17f;
        int n = PIE_CODES.length;
        float step = 360.0f / n, gap = 1.6f;
        RectF outerBox = new RectF(c - outer, c - outer, c + outer, c + outer);
        RectF innerBox = new RectF(c - inner, c - inner, c + inner, c + inner);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        edge.setStyle(Paint.Style.STROKE);
        edge.setStrokeWidth(1.5f);
        edge.setColor(HAIRLINE);
        Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3.5f);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        stroke.setColor(INK);
        Paint label = ink(21, INK_SOFT, true);
        label.setTextAlign(Paint.Align.CENTER);

        for (int i = 0; i < n; i++) {
            // Canvas angles run clockwise from the right, so the top is -90
            float mid = -90.0f + i * step;
            Path slice = new Path();
            slice.arcTo(outerBox, mid - step / 2 + gap, step - 2 * gap);
            slice.arcTo(innerBox, mid + step / 2 - gap, -(step - 2 * gap));
            slice.close();
            boolean lit = i == hover;
            boolean live = dictating && PIE_CODES[i] == KB_CODE_VOICE;
            boolean armed = exitArmed && PIE_CODES[i] == KB_CODE_EXIT;
            fill.setColor(live || armed ? RECORD : lit ? ACCENT : PANEL_FILL);
            canvas.drawPath(slice, fill);
            canvas.drawPath(slice, edge);
            double a = Math.toRadians(mid);
            float r = (outer + inner) * 0.5f + 6;
            float x = c + (float)(Math.cos(a) * r), y = c + (float)(Math.sin(a) * r);
            drawDockIcon(canvas, stroke, PIE_CODES[i], x, y - 16);
            label.setColor(lit || live || armed ? INK : INK_SOFT);
            boolean lockLit = headLocked && PIE_CODES[i] == KB_CODE_HEADLOCK;
            String text = live ? "Listening" : armed ? "Again to exit" : lockLit ? "Head lock on" : PIE_LABELS[i];
            canvas.drawText(text, x, y + 36, label);
        }

        // The middle: the lit action in words, or how to use the menu
        fill.setColor(PANEL_FILL);
        canvas.drawCircle(c, c, inner - 8, fill);
        canvas.drawCircle(c, c, inner - 8, edge);
        Paint centre = ink(hover >= 0 ? 26 : 19, hover >= 0 ? INK : INK_SOFT, hover >= 0);
        centre.setTextAlign(Paint.Align.CENTER);
        if (hover >= 0 && hover < n) {
            canvas.drawText(PIE_LABELS[hover], c, c + 9, centre);
        } else {
            canvas.drawText("Tilt and", c, c - 4, centre);
            canvas.drawText("let go", c, c + 20, centre);
        }
        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    private static void drawDockIcon(Canvas c, Paint p, int code, float cx, float cy) {
        if (code == KB_CODE_HEADLOCK) {
            // A padlock
            c.drawRoundRect(new RectF(cx - 16, cy - 4, cx + 16, cy + 20), 5, 5, p);
            c.drawArc(new RectF(cx - 10, cy - 22, cx + 10, cy + 2), 180, 180, false, p);
            c.drawLine(cx, cy + 5, cx, cy + 11, p);
            return;
        }
        if (code == KB_CODE_ENVIRONMENT) {
            // A horizon: hills under a sky, for the world around you
            c.drawCircle(cx, cy, 20, p);
            c.drawLine(cx - 19, cy + 6, cx + 19, cy + 6, p);
            Path hills = new Path();
            hills.moveTo(cx - 14, cy + 6);
            hills.lineTo(cx - 5, cy - 6);
            hills.lineTo(cx + 2, cy + 2);
            hills.lineTo(cx + 8, cy - 4);
            hills.lineTo(cx + 16, cy + 6);
            c.drawPath(hills, p);
            return;
        }
        if (code == KB_CODE_OVERVIEW) {
            // Mission Control's own picture: windows spread out in a grid
            c.drawRoundRect(new RectF(cx - 24, cy - 16, cx - 3, cy - 2), 4, 4, p);
            c.drawRoundRect(new RectF(cx + 3, cy - 16, cx + 24, cy - 2), 4, 4, p);
            c.drawRoundRect(new RectF(cx - 24, cy + 2, cx - 3, cy + 16), 4, 4, p);
            c.drawRoundRect(new RectF(cx + 3, cy + 2, cx + 24, cy + 16), 4, 4, p);
            return;
        }
        if (code == KB_CODE_KEYBOARD) {
            c.drawRoundRect(new RectF(cx - 25, cy - 16, cx + 25, cy + 16), 6, 6, p);
            for (int row = 0; row < 2; row++)
                for (int k = 0; k < 5; k++) {
                    float x = cx - 16 + k * 8, y = cy - 7 + row * 8;
                    c.drawPoint(x, y, p);
                }
            c.drawLine(cx - 10, cy + 9, cx + 10, cy + 9, p);
        } else if (code == KB_CODE_VOICE) {
            c.drawRoundRect(new RectF(cx - 8, cy - 22, cx + 8, cy + 5), 8, 8, p);
            c.drawArc(new RectF(cx - 16, cy - 12, cx + 16, cy + 13), 0, 180, false, p);
            c.drawLine(cx, cy + 13, cx, cy + 21, p);
            c.drawLine(cx - 8, cy + 21, cx + 8, cy + 21, p);
        } else if (code == KB_CODE_SCREEN) {
            c.drawRoundRect(new RectF(cx - 25, cy - 18, cx + 25, cy + 11), 4, 4, p);
            c.drawLine(cx, cy + 11, cx, cy + 19, p);
            c.drawLine(cx - 11, cy + 20, cx + 11, cy + 20, p);
            c.drawLine(cx - 7, cy - 4, cx + 7, cy - 4, p);
            c.drawLine(cx, cy - 11, cx, cy + 3, p);
        } else if (code == KB_CODE_GUIDE) {
            c.drawCircle(cx, cy, 19, p);
            Paint q = ink(26, INK, true);
            q.setTextAlign(Paint.Align.CENTER);
            c.drawText("?", cx, cy + 9, q);
        } else if (code == KB_CODE_SETTINGS) {
            float[] knob = { -8, 10, -2 };
            for (int row = 0; row < 3; row++) {
                float y = cy - 13 + row * 13;
                c.drawLine(cx - 23, y, cx + 23, y, p);
                Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
                dot.setColor(PANEL_FILL | 0xFF000000);
                c.drawCircle(cx + knob[row], y, 5.5f, dot);
                c.drawCircle(cx + knob[row], y, 5.5f, p);
            }
        } else if (code == KB_CODE_EXIT) {
            c.drawArc(new RectF(cx - 18, cy - 18, cx + 18, cy + 18), -60, 300, false, p);
            c.drawLine(cx, cy - 22, cx, cy - 4, p);
        }
    }

    /** The guide card: six short tiles, closed by any press. */
    ByteBuffer buildGuide() {
        Bitmap bitmap = Bitmap.createBitmap(GUIDE_TEX_W, GUIDE_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setColor(0xF20C1118);
        RectF body = new RectF(1, 1, GUIDE_TEX_W - 1, GUIDE_TEX_H - 1);
        canvas.drawRoundRect(body, 40, 40, fill);
        Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        edge.setStyle(Paint.Style.STROKE);
        edge.setStrokeWidth(1.5f);
        edge.setColor(HAIRLINE);
        canvas.drawRoundRect(body, 40, 40, edge);

        Paint accent = new Paint(Paint.ANTI_ALIAS_FLAG);
        accent.setColor(ACCENT);
        canvas.drawRoundRect(new RectF(70, 58, 76, 132), 3, 3, accent);
        canvas.drawText("Controls", 98, 98, ink(44, INK, true));
        canvas.drawText("Both controllers work the same, and every desktop works the same.", 98, 132, ink(24, INK_SOFT, false));

        // The whole vocabulary: what you want, then the controller way and the hands way.
        // Kept in step with the bindings in xr_input.c, which is where each of these is made.
        String[][] rows = {
                { "Click, hold to drag", "Trigger", "Pinch index finger" },
                { "Right click", "B or Y", "Pinch middle finger" },
                { "Dictate", "Tap A or X, or hold to talk", "Dictate in the pie menu" },
                { "Scroll", "Thumbstick, any direction", "Pinch little finger, move hand" },
                { "Move a desktop", "Grip anywhere on it, swing", "Make a fist on it, swing" },
                { "Distance and curve", "Grip, then the thumbstick", "Reach out or draw back" },
                { "Resize", "Grip a corner", "Fist on a corner" },
                { "Window to another desktop", "Hold trigger, point across", "Hold the pinch, point across" },
                { "Pie menu", "Click a thumbstick, tilt, let go", "Pinch in empty air" },
                { "Keyboard, Overview, new desktop", "In the pie menu", "In the pie menu" },
                { "Desktops in front of you", "Menu button", "Palm up, hold the pinch" },
        };
        float left = 70, right = GUIDE_TEX_W - 70;
        float col1 = left + 28, col2 = left + 494, col3 = left + 872;
        float top = 172, rowH = 44;
        Paint head = ink(20, ACCENT_SOFT, true);
        canvas.drawText("TO", col1, top + 30, head);
        canvas.drawText("CONTROLLER", col2, top + 30, head);
        canvas.drawText("HANDS", col3, top + 30, head);
        Paint action = ink(24, INK, true);
        Paint how = ink(21, INK_SOFT, false);
        for (int i = 0; i < rows.length; i++) {
            float y = top + 48 + i * rowH;
            if (i % 2 == 0) {
                fill.setColor(CARD_FILL);
                canvas.drawRoundRect(new RectF(left, y, right, y + rowH - 4), 14, 14, fill);
            }
            float baseline = y + rowH * 0.5f + 7;
            canvas.drawText(rows[i][0], col1, baseline, action);
            canvas.drawText(rows[i][1], col2, baseline, how);
            canvas.drawText(rows[i][2], col3, baseline, how);
        }
        Paint note = ink(21, INK_SOFT, false);
        note.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("VR desktops unplug when you take the headset off; their windows move to your laptop, and come back.",
                GUIDE_TEX_W / 2f, top + 48 + rows.length * rowH + 34, note);
        Paint footer = ink(22, INK_SOFT, false);
        footer.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Press anywhere to close. Guide in the dock brings this back.", GUIDE_TEX_W / 2f, GUIDE_TEX_H - 30, footer);
        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    Keyboard buildKeyboard() {
        float[] keyRects = buildKeyRects();
        int[] codesLower = flatten(KB_CODES_LOWER);
        int[] codesUpper = flatten(KB_CODES_UPPER);
        int[] codesSymbols = flatten(KB_CODES_SYMBOLS);

        Bitmap lower = buildKeyboardSheet(KB_LABELS_LOWER, keyRects);
        ByteBuffer lowerPixels = toBuffer(lower);
        lower.recycle();

        Bitmap upper = buildKeyboardSheet(KB_LABELS_UPPER, keyRects);
        ByteBuffer upperPixels = toBuffer(upper);
        upper.recycle();

        Bitmap symbols = buildKeyboardSheet(KB_LABELS_SYMBOLS, keyRects);
        ByteBuffer symbolsPixels = toBuffer(symbols);
        symbols.recycle();

        Bitmap button = buildKeyboardButton();
        ByteBuffer buttonPixels = toBuffer(button);
        button.recycle();

        return new Keyboard(lowerPixels, upperPixels, symbolsPixels, buttonPixels,
                keyRects, codesLower, codesUpper, codesSymbols);
    }

    // Left, top, right and bottom of every key as fractions of the panel, rows
    // top down, keys left to right. The row widths are in key units, so this is
    // where they turn into a place on the texture.
    private static float[] buildKeyRects() {
        int keys = 0;
        for (float[] row : KB_ROW_WIDTHS) {
            keys += row.length;
        }

        float[] rects = new float[keys * 4];
        float unit = (1.0f - 2.0f * KB_PAD_U) / KB_ROW_UNITS;
        float rowHeight = (1.0f - 2.0f * KB_PAD_V) / KB_ROW_WIDTHS.length;
        int at = 0;
        for (int row = 0; row < KB_ROW_WIDTHS.length; row++) {
            float x = KB_ROW_INDENT[row];
            for (int key = 0; key < KB_ROW_WIDTHS[row].length; key++) {
                float w = KB_ROW_WIDTHS[row][key];
                rects[at++] = KB_PAD_U + x * unit + KB_GAP_U * 0.5f;
                rects[at++] = KB_PAD_V + row * rowHeight + KB_GAP_V * 0.5f;
                rects[at++] = KB_PAD_U + (x + w) * unit - KB_GAP_U * 0.5f;
                rects[at++] = KB_PAD_V + (row + 1) * rowHeight - KB_GAP_V * 0.5f;
                x += w;
            }
        }
        return rects;
    }

    private static int[] flatten(int[][] rows) {
        int keys = 0;
        for (int[] row : rows) {
            keys += row.length;
        }

        int[] flat = new int[keys];
        int at = 0;
        for (int[] row : rows) {
            for (int code : row) {
                flat[at++] = code;
            }
        }
        return flat;
    }

    // One state's worth of keys, drawn as caps on the same dark rounded panel
    // the settings use
    private Bitmap buildKeyboardSheet(String[][] labels, float[] rects) {
        Bitmap bitmap = Bitmap.createBitmap(KB_TEX_W, KB_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(PANEL_FILL);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, KB_TEX_W - 1.0f, KB_TEX_H - 1.0f),
                28.0f, 28.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        text.setColor(Color.WHITE);

        int at = 0;
        for (String[] row : labels) {
            for (String label : row) {
                RectF box = new RectF(rects[at] * KB_TEX_W, rects[at + 1] * KB_TEX_H,
                        rects[at + 2] * KB_TEX_W, rects[at + 3] * KB_TEX_H);
                at += 4;

                paint.setStyle(Paint.Style.FILL);
                boolean glyph = label.length() == 1;
                paint.setColor(label.equals("Return") ? ACCENT : glyph || label.equals("space") ? KEY_FILL : KEY_MOD_FILL);
                canvas.drawRoundRect(box, 12.0f, 12.0f, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(1.5f);
                paint.setColor(HAIRLINE);
                canvas.drawRoundRect(box, 12.0f, 12.0f, paint);

                // A single character is what the key types, so it gets the
                // room. The named keys are wordier and have to fit.
                text.setColor(glyph ? INK : INK_SOFT);
                text.setTextSize(glyph ? 34.0f : 23.0f);
                canvas.drawText(label, box.centerX(),
                        box.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
            }
        }

        return bitmap;
    }

    // A keyboard outline with a few keys in it, which is about as much as reads
    // at this size
    private Bitmap buildKeyboardButton() {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(12.0f, 28.0f, 116.0f, 100.0f), 14.0f, 14.0f, paint);

        paint.setStyle(Paint.Style.FILL);
        for (int row = 0; row < 2; row++) {
            float y = 42.0f + row * 16.0f;
            for (int key = 0; key < 4; key++) {
                float x = 26.0f + key * 20.0f;
                canvas.drawRoundRect(new RectF(x, y, x + 14.0f, y + 12.0f), 3.0f, 3.0f, paint);
            }
        }
        canvas.drawRoundRect(new RectF(44.0f, 74.0f, 84.0f, 86.0f), 3.0f, 3.0f, paint);

        return button;
    }

    /**
     * The button that ends the stream and the prompt it opens. The prompt is
     * drawn three times, once plain and once with each of its buttons lit, so
     * hovering one in the session picks another sheet rather than costing an
     * upload.
     */
    ByteBuffer[] buildExitArt() {
        Bitmap button = buildExitButton();
        ByteBuffer buttonPixels = toBuffer(button);
        button.recycle();

        Bitmap plain = buildExitPrompt(EXIT_ZONE_NONE);
        ByteBuffer plainPixels = toBuffer(plain);
        plain.recycle();

        Bitmap exitHot = buildExitPrompt(EXIT_ZONE_EXIT);
        ByteBuffer exitHotPixels = toBuffer(exitHot);
        exitHot.recycle();

        Bitmap cancelHot = buildExitPrompt(EXIT_ZONE_CANCEL);
        ByteBuffer cancelHotPixels = toBuffer(cancelHot);
        cancelHot.recycle();

        return new ByteBuffer[] { buttonPixels, plainPixels, exitHotPixels, cancelHotPixels };
    }

    // A power symbol, in the same weight and colour as the buttons either side
    // of it: a ring open at the top with a bar standing in the gap
    private Bitmap buildExitButton() {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = BUTTON_TEX * 0.5f;
        final float radius = 36.0f;
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(9.0f);
        paint.setStrokeCap(Paint.Cap.ROUND);

        // Starts a little past the top on one side and comes back round to the
        // same place on the other, which leaves the gap centred
        RectF ring = new RectF(mid - radius, mid - radius, mid + radius, mid + radius);
        canvas.drawArc(ring, -60.0f, 300.0f, false, paint);

        canvas.drawLine(mid, mid - radius - 10.0f, mid, mid - 2.0f, paint);

        return button;
    }

    // The prompt sheet: the question, and the two buttons under it. The zone
    // passed in is the one drawn lit, or none of them.
    private Bitmap buildExitPrompt(int hot) {
        Bitmap bitmap = Bitmap.createBitmap(EXIT_TEX_W, EXIT_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);

        // The same dark sheet the settings panel and the keyboard sit on
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0101A29);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, EXIT_TEX_W - 1.0f, EXIT_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        text.setColor(Color.WHITE);
        text.setTextSize(34.0f);
        float questionY = EXIT_TEX_H * 0.30f;
        canvas.drawText(EXIT_QUESTION, EXIT_TEX_W * 0.5f,
                questionY - (text.ascent() + text.descent()) * 0.5f, text);

        // Leaving is the destructive half, so it is the one that reads red.
        // Both are the same shape, so neither is the easier target.
        drawExitChoice(canvas, paint, text, EXIT_EXIT_L, EXIT_EXIT_R, "Exit",
                0xFFE05A5A, hot == EXIT_ZONE_EXIT);
        drawExitChoice(canvas, paint, text, EXIT_CANCEL_L, EXIT_CANCEL_R, "Cancel",
                0xEEFFFFFF, hot == EXIT_ZONE_CANCEL);

        return bitmap;
    }

    // One of the prompt's buttons. Hovering fills it, which is what says which
    // of the two a press would land on.
    private void drawExitChoice(Canvas canvas, Paint paint, Paint text, float left, float right,
                                String label, int colour, boolean hot) {
        RectF box = new RectF(left * EXIT_TEX_W, EXIT_BTN_T * EXIT_TEX_H,
                right * EXIT_TEX_W, EXIT_BTN_B * EXIT_TEX_H);

        if (hot) {
            paint.setStyle(Paint.Style.FILL);
            // The button's own colour, kept faint enough to read as a wash
            // behind the label rather than as a filled block
            paint.setColor((colour & 0x00FFFFFF) | 0x38000000);
            canvas.drawRoundRect(box, 16.0f, 16.0f, paint);
        }

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(hot ? 5.0f : 3.0f);
        paint.setColor(colour);
        canvas.drawRoundRect(box, 16.0f, 16.0f, paint);
        paint.setStyle(Paint.Style.FILL);

        text.setColor(colour);
        text.setTextSize(30.0f);
        canvas.drawText(label, box.centerX(),
                box.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    static ByteBuffer toBuffer(Bitmap bitmap) {
        ByteBuffer pixels = ByteBuffer.allocateDirect(
                bitmap.getWidth() * bitmap.getHeight() * 4);
        bitmap.copyPixelsToBuffer(pixels);
        pixels.rewind();
        return pixels;
    }

    // Sampled down on the way out of the JPEG, since a full 4096x2048 decode
    // for a 240 pixel tile would cost 32 MB apiece
    private Bitmap decodeThumb(String fileName, int wanted) {
        InputStream in = null;
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            in = openEnvironment(context, fileName);
            BitmapFactory.decodeStream(in, null, bounds);
            closeQuietly(in);

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = 1;
            while (bounds.outHeight / (opts.inSampleSize * 2) >= wanted) {
                opts.inSampleSize *= 2;
            }

            in = openEnvironment(context, fileName);
            Bitmap thumb = BitmapFactory.decodeStream(in, null, opts);
            // A square panorama is top/bottom stereo: thumb from the top half,
            // or the crop lands on the seam between the two eyes
            if (thumb != null && thumb.getWidth() == thumb.getHeight()) {
                Bitmap top = Bitmap.createBitmap(thumb, 0, 0,
                        thumb.getWidth(), thumb.getHeight() / 2);
                thumb.recycle();
                return top;
            }
            return thumb;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Thumbnail " + fileName + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    // spaichingen_hill.jpg becomes Spaichingen Hill
    static String labelFor(String fileName) {
        // A world is named by its folder, so it arrives with the marking slash on the end
        if (fileName.endsWith("/")) fileName = fileName.substring(0, fileName.length() - 1);
        int dot = fileName.lastIndexOf('.');
        String base = dot > 0 ? fileName.substring(0, dot) : fileName;
        StringBuilder out = new StringBuilder(base.length());
        boolean wordStart = true;
        for (int i = 0; i < base.length(); i++) {
            char c = base.charAt(i) == '_' ? ' ' : base.charAt(i);
            out.append(wordStart ? Character.toUpperCase(c) : c);
            wordStart = c == ' ';
        }
        return out.toString();
    }

    static void closeQuietly(InputStream in) {
        if (in != null) {
            try {
                in.close();
            } catch (IOException ignored) {
            }
        }
    }
}
