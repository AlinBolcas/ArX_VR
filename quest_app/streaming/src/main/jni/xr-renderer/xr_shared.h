// Every value the native renderer and the Java side have to agree on: the
// protocol of the per frame input array, the ids of the settings the panel
// reports, and the size and layout of every sheet of art Java draws and this
// side hit tests. The build turns this file into XrShared.java, so a value
// changed here moves both sides together and nothing has to be kept in step
// by hand.
//
// Only plain integer and float constants, and expressions over ones defined
// above them, since that is all the generator reads. No casts, no macros with
// arguments, and nothing that only means something in C.

#ifndef XR_SHARED_H
#define XR_SHARED_H

// Levels of the file log
#define FILE_LOG_OFF 0
#define FILE_LOG_BASIC 1
#define FILE_LOG_VERBOSE 2

// Return codes for waitBeginFrame
#define FRAME_EXIT   -1
#define FRAME_IDLE    0
#define FRAME_RENDER  1

// Synthetic depth patterns for the stereo test path
#define DEPTH_MODE_OFF   0
#define DEPTH_MODE_FLAT  1
#define DEPTH_MODE_RAMP  2
#define DEPTH_MODE_BLOB  3
// Tints each eye instead of warping, so eye routing can be checked by
// closing one eye rather than by judging depth
#define DEPTH_MODE_EYETEST 4
// Draws a synthetic bar through the warp and reads back where it landed in
// each eye, so the shift direction is measured rather than eyeballed
#define DEPTH_MODE_SHIFTTEST 5
// Real depth from the MiDaS model, run in Java on LiteRT
#define DEPTH_MODE_MODEL 6

// The depth model's input and output are square at this size
#define DEPTH_TEX_SIZE 256

// The status pill: one short line, or the dictation waveform, in the screen's corner
#define OVERLAY_WIDTH 640
#define OVERLAY_HEIGHT 128

// Slots in the float array handed back to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// Which setting the panel just changed, or -1. Zero is a real id, so this one
// has to be said explicitly rather than left at the memset.
#define IN_SETTING  7
// x y z, then the orientation quaternion, then width, cylinder radius and the
// curvature the panel asked for, POSE_VALUES in all
#define IN_POSE     8
#define POSE_VALUES 10
// The cell just chosen in the environment grid, or -1
#define IN_PICKER_PICK 18
#define IN_SETTING_VALUE 19
// The key the in world keyboard just typed, or -1. Unicode with the shift
// already applied, plus the four control codes below 32.
#define IN_KEY      20
// Set to 1 the frame the exit prompt is confirmed. Nothing else is meaningful
// here, so a zeroed slot says nothing happened.
#define IN_EXIT     21
// Which desktop the pointer is on, 0 the main one. Every desktop takes the
// same pointer, buttons and scroll, so a drag can carry a window across.
#define IN_WORKSPACE 22
// 1 the frame a desktop's close button is pressed, for the desktop in IN_WORKSPACE
#define IN_WORKSPACE_ACTION 23
// 1 while dictation is held: B or Y, or the ring finger pinched to the thumb
#define IN_VOICE    24
// Sideways scroll clicks this frame, positive to the right
#define IN_HSCROLL  25
// The pie menu: -2 while closed, -1 open with nothing picked out, else the item lit
#define IN_PIE      26
#define IN_SLOTS    27

// Desktops beyond the main one. The Mac companion's ARX_VIDEO_MAX_SLOTS is this plus one.
#define ARX_MAX_EXTRA 4
// The round close button at the end of an extra desktop's bar
#define CLOSE_TEX 128

// Settings the panel can hand back to Java to be applied and stored
#define SETTING_SHARPEN 0
#define SETTING_STATS   1
#define SETTING_SEPARATION 2
#define SETTING_CONVERGENCE 3
#define SETTING_RESET_3D 4
#define SETTING_AMBILIGHT 5
#define SETTING_AMBI_LEVEL 6
#define SETTING_ROOM_LIGHT 7
#define SETTING_HEAD_LOCK 8

// Which Environment Res tier the room draws at
#define ENV_RES_LOW 0
#define ENV_RES_STANDARD 1
#define ENV_RES_HIGH 2
#define ENV_RES_ULTRA 3

// Environment picker. A grid of thumbnails drawn in Java and shown as one
// quad, with the hover and selection marks as separate outline quads so
// pointing around the grid never costs an upload. One band per category: a
// header strip carrying its name, then a row of cells under it.
#define PICKER_COLS 4
#define PICKER_ROWS 2
#define PICKER_CELLS (PICKER_COLS * PICKER_ROWS)
#define PICKER_TEX_W 1024
#define PICKER_HEADER_PX 40
#define PICKER_CELL_PX 256
#define PICKER_BAND_PX (PICKER_HEADER_PX + PICKER_CELL_PX)
#define PICKER_TEX_H (PICKER_BAND_PX * PICKER_ROWS)
// The cells of the grid. The fixed ones come first, then the photos from the
// assets folder in name order take whatever the rooms leave. A cell is a place
// in the grid and nothing more: what gets saved is the stable id it maps to.
#define ENV_CELL_PASSTHROUGH 0
#define ENV_CELL_VOID 1
// The built rooms are retired: their cells go to worlds, and these match no cell
#define ENV_CELL_MINIMAL_ROOM (-2)
#define ENV_CELL_PSX_CINEMA (-3)
#define ENV_CELL_FIRST_PHOTO 2

// Which kind of room the environment choice asks for, 0 for none: 1 is generated
// by the renderer, 2 is a baked model, which is both the cinema and any world made
// on the Mac and put on the headset
#define ROOM_STYLE_MINIMAL 1
#define ROOM_STYLE_PSX 2
// 3 is a world made on the Mac: a baked model like the cinema, but one that is
// only drawn around you. It hangs nothing and moves nothing of yours.
#define ROOM_STYLE_WORLD 3
// 4 is Calm: nothing but a shader, drawn fresh each frame around you. It hangs nothing.
#define ROOM_STYLE_CALM 4
// 5 is a Gaussian splat world made on the Mac, drawn around you. It hangs nothing either.
#define ROOM_STYLE_SPLAT 5

// The buttons under the bar are all drawn at this size
#define BUTTON_TEX 128
// The padlock that locks the hands out
#define LOCK_TEX 384

// Settings panel. Same shape as the picker: the art is drawn in Java and shown
// on one quad, the thumbs are separate little quads so dragging one never costs
// an upload.
#define COG_TEX_W 768
#define COG_TEX_H 640

// Where the tabs, rows and tracks sit in the panel texture, as fractions of it
#define COG_TRACK_L 0.42f
#define COG_TRACK_R 0.93f
// Anything above this is the tab bar, split evenly between the tabs
#define COG_TAB_BAR_B 0.16f
// Six rows on the screen tab, so they start a little higher and sit closer
// together than they did at five
#define COG_ROW_V0 0.25f
#define COG_ROW_STEP 0.11f
// Half height of a row's hit band. Under half the pitch, so neighbouring
// bands stay disjoint.
#define COG_ROW_HALF 0.05f
#define COG_RESET_L 0.35f
#define COG_RESET_R 0.65f
// Clear of the last row, which reaches 0.80 plus the half band
#define COG_RESET_T 0.87f
#define COG_RESET_B 0.97f
// Half height of an option cell, so the ring drawn over one matches the art
#define COG_CELL_HALF 0.045f

// One texture per tab, all uploaded once, so switching costs a swapchain
// handle rather than an upload
#define COG_TAB_SCREEN  0
#define COG_TAB_DISPLAY 1
#define COG_TAB_3D      2
#define COG_TAB_COUNT   3
// And one more sheet than there are tabs: the screen tab has a second face for
// when a room hangs the picture and none of its rows can do anything
#define COG_ART_ROOM_SCREEN 3
#define COG_ART_COUNT       4

// Screen tab rows, in the order they are drawn
#define COG_SLIDER_DISTANCE 0
#define COG_SLIDER_HEIGHT   1
#define COG_SLIDER_TILT     2
#define COG_SLIDER_ROTATE   3
#define COG_SLIDER_CURVE    4
#define COG_SLIDER_SIZE     5
#define COG_SLIDER_COUNT    6

// 3D tab rows, sliders like the screen tab's. Only values that take effect the
// moment they move belong here: the depth source itself is settled when the
// session starts, so it stays in the 2d settings.
#define COG_ROW3D_SEPARATION 0
#define COG_ROW3D_CONVERGENCE 1
#define COG_ROW3D_COUNT 2
// Right hand end of the separation track, as a fraction of frame width. Three
// times the 0.5 percent that phase 6 measured as the useful maximum: past
// there depth stops growing and only the strain does, so the far end of the
// track is drawn marked rather than left off.
#define COG_SEP_MAX 0.015f
// Steps along that track, so a dragged value lands exactly on one of the
// tenths of a percent the preference is stored in
#define COG_SEP_STEPS 15

// Display tab rows. Cells rather than a track, so a press picks one instead of
// dragging a value.
#define COG_OPTION_SHARPEN 0
#define COG_OPTION_STATS   1
#define COG_OPTION_HEAD_LOCK 2
#define COG_OPTION_AMBILIGHT 3
#define COG_OPTION_ROOM_LIGHT 4
#define COG_OPTION_COUNT   5
#define COG_SHARPEN_CELLS 3
#define COG_STATS_CELLS   2
#define COG_HEAD_LOCK_CELLS 2
#define COG_AMBI_CELLS    2
#define COG_ROOM_LIGHT_CELLS 2
// The one row on this tab that is a track rather than cells, under the option
// rows, so the glow can be turned down without leaving the tab it lives on.
// Six rows on this tab now, the same grid the screen tab already fills.
#define COG_DISPLAY_SLIDER_ROW 5

// In world keyboard, for the login boxes and chat windows that turn up mid
// stream. One sheet of art per state, drawn in Java like the other panels, and
// the layout arrives with it: the native side is handed rectangles and codes
// and knows nothing else about what the keys say.
#define KB_TEX_W 1120
#define KB_TEX_H 460
#define KB_STATE_LOWER   0
#define KB_STATE_UPPER   1
#define KB_STATE_SYMBOLS 2
#define KB_STATE_COUNT   3
// Codes under zero change the keyboard instead of typing. Everything at or
// above 8 is sent on as it stands.
#define KB_CODE_SHIFT   -2
#define KB_CODE_SYMBOLS -3
#define KB_CODE_HIDE    -4

// The button that ends the stream and the prompt it opens. The sheet is drawn
// in Java like the other panels, one per lit button, so hovering one is
// another handle in the layer rather than an upload.
#define EXIT_TEX_W 512
#define EXIT_TEX_H 256
// Which zone of the sheet the ray is on, and the sheet drawn with that zone
// lit, so the two share their numbering
#define EXIT_ZONE_NONE   0
#define EXIT_ZONE_EXIT   1
#define EXIT_ZONE_CANCEL 2
#define EXIT_ART_COUNT   3
// Where the two buttons sit on the sheet, as fractions of it
#define EXIT_BTN_T 0.56f
#define EXIT_BTN_B 0.86f
#define EXIT_EXIT_L 0.08f
#define EXIT_EXIT_R 0.46f
#define EXIT_CANCEL_L 0.54f
#define EXIT_CANCEL_R 0.92f

// ArX workspace actions, never forwarded as host character codes.
#define KB_CODE_GUIDE 65002
#define KB_CODE_SCREEN 65003
#define KB_CODE_CMD 65010
#define KB_CODE_CTRL 65011
#define KB_CODE_VOICE 65000
#define KB_CODE_CAPTURE 65001
// Dock actions the renderer handles itself rather than passing to Java
#define KB_CODE_KEYBOARD 65004
#define KB_CODE_SETTINGS 65005
#define KB_CODE_EXIT 65006
// Mission Control on the Mac, every app and desktop at once
#define KB_CODE_OVERVIEW 65007
// The grid of worlds to put around you
#define KB_CODE_ENVIRONMENT 65008
// Head lock on or off: every screen follows your head, keeping its place around you
#define KB_CODE_HEADLOCK 65009

// The pie menu that replaces the dock: art drawn in Java, one item per slice,
// clockwise from the top
#define PIE_TEX 640
#define PIE_MAX_ITEMS 8

// ArX dock: one always-visible strip of actions under the main screen. Java
// draws the art and hands over one rectangle and code per action, like the
// keyboard, so the layout lives in one place.
#define DOCK_TEX_W 1200
#define DOCK_TEX_H 150
#define DOCK_MAX_KEYS 8

// The guide card that opens in front of the screen. Any press closes it.
#define GUIDE_TEX_W 1400
#define GUIDE_TEX_H 820

#endif
