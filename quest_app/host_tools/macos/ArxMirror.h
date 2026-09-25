// The mirror window: what the headset shows, streamed back from the Quest so it can
// be watched, made full screen and recorded on the Mac.
#ifndef ARX_MIRROR_H
#define ARX_MIRROR_H

#import <Cocoa/Cocoa.h>

// Opens the window and listens for the headset until the window is closed
void arxMirrorShow(NSString *token);
// "closed", "open, waiting for the headset" or "showing the headset"
NSString *arxMirrorState(void);

#endif
