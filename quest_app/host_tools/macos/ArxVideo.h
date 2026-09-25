// Screen capture, hardware H.264 and the frame server the headset connects to.
// One stream per display: slot 0 is the Mac's own screen, slots 1 and up the
// desktops that exist while the headset is in use. ScreenCaptureKit and VideoToolbox, owned by this app so the
// capture permission belongs to us.
#ifndef ARX_VIDEO_H
#define ARX_VIDEO_H

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

#define ARX_VIDEO_MAX_SLOTS 5

// Starts the listener and the main display's stream. Returns nil on success, else a reason.
NSString *arxVideoStart(int port, int fps, int bitrateKbps, NSString *token);
void arxVideoStop(void);
// The pairing secret the headset proves itself with, once the bridge has started the video
NSString *arxVideoToken(void);
// Streams a VR desktop's display under a slot, 1 and up; done runs on the main queue with nil or a reason
void arxVideoAttachDisplay(int slot, CGDirectDisplayID display, int bitrateKbps, void (^done)(NSString *failure));
// The desktop slots streaming now, and how many headsets are watching one
NSArray<NSNumber *> *arxVideoDesktopSlots(void);
int arxVideoViewers(int slot);
void arxVideoDetach(int slot);
// The main stream's running, clients, frames encoded/sent, dropped, size, plus how many streams are live
NSDictionary *arxVideoStatus(void);

#endif
