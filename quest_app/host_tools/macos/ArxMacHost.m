// ArX VR Mac companion. Loopback transport is owned by arxvr_voice_bridge.py.
#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import "ArxVideo.h"
#import "ArxMirror.h"
#import <ApplicationServices/ApplicationServices.h>

// Private CoreGraphics declarations informed by Chromium's BSD-licensed display test utility.
// The companion detects availability and fails visibly if macOS changes this API.
@interface NSObject (ArxVirtualDisplay)
- (id)initWithDescriptor:(id)descriptor;
- (id)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)rate;
- (BOOL)applySettings:(id)settings;
- (unsigned int)displayID;
@end

// A VR desktop is a monitor plugged into the Mac for as long as the headset is
// watching it, and unplugged when it stops: macOS then brings its windows onto the
// laptop screen, exactly as it does when a real monitor's cable comes out. When
// the desktop comes back, so do the windows that were on it.
#define ARX_DESKTOP_W 1920
#define ARX_DESKTOP_H 1080
#define ARX_DESKTOP_KBPS 20000
// Seconds a desktop may go unwatched before it is unplugged. Covers a Wi-Fi blip or a
// stream rebuilding; anything longer is the headset asleep, closed or gone.
#define ARX_UNWATCHED_S 5

static NSMutableDictionary<NSNumber *, id> *displays;
// Your own other monitors, shown in VR under a slot like a desktop. They are real and
// stay plugged in: when the headset stops watching one, only its stream stops.
static NSMutableDictionary<NSNumber *, NSNumber *> *monitors;
static NSMutableSet<NSNumber *> *creating;
static NSMutableDictionary<NSNumber *, NSNumber *> *unwatched;
// Which windows sat on each desktop when it was unplugged, and where on it
static NSMutableDictionary<NSNumber *, NSArray<NSDictionary *> *> *windowMemory;
static NSWindow *window;
static NSTextField *statusLabel;
static CGPoint pointer;
static int heldButtons;
static NSUInteger clickCount;
static double lastClickTime;
static CGPoint lastClickPoint;
static CGDirectDisplayID mainDisplay;
static double lastInputTime;
static BOOL questConnected;
// The headset says when its microphone is live, so this window can say so too
static BOOL micOn;

// Every monitor plugged into the Mac other than its main screen and the VR desktops
static NSArray<NSNumber *> *physicalMonitors(void) {
    CGDirectDisplayID online[16]; uint32_t count = 0;
    NSMutableArray *found = [NSMutableArray array];
    if (CGGetOnlineDisplayList(16, online, &count) != kCGErrorSuccess) return found;
    for (uint32_t i = 0; i < count; i++) {
        if (online[i] == mainDisplay || CGDisplayMirrorsDisplay(online[i]) != kCGNullDirectDisplay) continue;
        BOOL ours = NO;
        for (id display in displays.allValues) if ([display displayID] == online[i]) ours = YES;
        if (!ours) [found addObject:@(online[i])];
    }
    return found;
}
static void reply(NSDictionary *value) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (data) { fwrite(data.bytes, 1, data.length, stdout); fputc('\n', stdout); fflush(stdout); }
}
static NSDictionary *status(void) {
    // @YES/@NO literals: a C ternary promotes YES/NO to int, and AXIsProcessTrusted() is a CF Boolean,
    // both of which box to JSON 1 instead of true
    return @{ @"quest_connected": questConnected ? @YES : @NO, @"screen_recording": CGPreflightScreenCaptureAccess() ? @YES : @NO, @"accessibility": AXIsProcessTrusted() ? @YES : @NO,
        @"desktop_slots": [displays.allKeys arrayByAddingObjectsFromArray:monitors.allKeys], @"main_display": @(mainDisplay),
        @"monitors": physicalMonitors() };
}
static void updateStatus(void) {
    // Exactly what the bridge is doing right now, in plain words
    NSDictionary *video = arxVideoStatus();
    BOOL watching = [video[@"clients"] intValue] > 0;
    // A headset that went away mid-dictation is not listening any more
    if (!watching) micOn = NO;
    BOOL controlling = NSDate.timeIntervalSinceReferenceDate - lastInputTime < 2.0;
    NSString *permissions = !CGPreflightScreenCaptureAccess() ? @"Screen capture permission needed"
        : !AXIsProcessTrusted() ? @"Mac control permission needed" : @"Permissions: all granted";
    statusLabel.stringValue = [NSString stringWithFormat:
        @"Headset: %@\nControlling your Mac: %@\nMicrophone: %@\nVR desktops: %@\nMirror: %@\n%@\n\nClose this window to switch the bridge off. Nothing keeps running after that.",
        watching ? @"connected, showing your Mac" : @"not connected",
        controlling ? @"yes, right now" : @"no",
        micOn ? @"listening" : @"off",
        displays.count ? [NSString stringWithFormat:@"%lu plugged in", (unsigned long)displays.count] : @"none",
        arxMirrorState(),
        permissions];
}
static CGDirectDisplayID displayForSlot(int slot) {
    if (slot == 0) return mainDisplay;
    if (monitors[@(slot)]) return monitors[@(slot)].unsignedIntValue;
    return [displays[@(slot)] displayID];
}
// A window's frame in global points now, or a null rect once it has closed
static CGRect windowBounds(CGWindowID windowID, pid_t *owner) {
    CGRect bounds = CGRectNull;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, windowID);
    if (list && CFArrayGetCount(list) > 0) {
        NSDictionary *info = ((__bridge NSArray *)list)[0];
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(__bridge NSString *)kCGWindowBounds], &bounds);
        if (owner) *owner = [info[(__bridge NSString *)kCGWindowOwnerPID] intValue];
    }
    if (list) CFRelease(list);
    return bounds;
}
// Accessibility knows windows by frame rather than number, so the one whose frame matches is the one
static AXUIElementRef accessibleWindow(pid_t pid, CGRect bounds) {
    AXUIElementRef found = NULL;
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    CFArrayRef windows = NULL;
    if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows) == kAXErrorSuccess && windows) {
        for (id item in (__bridge NSArray *)windows) {
            AXUIElementRef w = (__bridge AXUIElementRef)item;
            CFTypeRef position = NULL, size = NULL;
            CGPoint at = CGPointZero; CGSize extent = CGSizeZero;
            if (AXUIElementCopyAttributeValue(w, kAXPositionAttribute, &position) == kAXErrorSuccess) { AXValueGetValue(position, kAXValueCGPointType, &at); CFRelease(position); }
            if (AXUIElementCopyAttributeValue(w, kAXSizeAttribute, &size) == kAXErrorSuccess) { AXValueGetValue(size, kAXValueCGSizeType, &extent); CFRelease(size); }
            if (fabs(at.x - bounds.origin.x) < 2 && fabs(at.y - bounds.origin.y) < 2
                    && fabs(extent.width - bounds.size.width) < 2 && fabs(extent.height - bounds.size.height) < 2) {
                found = (AXUIElementRef)CFRetain(w);
                break;
            }
        }
        CFRelease(windows);
    }
    CFRelease(app);
    return found;
}
static CGEventFlags flagsFor(int mask) {
    CGEventFlags f = 0;
    if (mask & 1) f |= kCGEventFlagMaskShift;
    if (mask & 2) f |= kCGEventFlagMaskControl;
    if (mask & 4) f |= kCGEventFlagMaskAlternate;
    if (mask & 8) f |= kCGEventFlagMaskCommand;
    return f;
}
static int macKey(int vk) {
    static NSDictionary *keys;
    if (!keys) keys = @{@65:@0,@66:@11,@67:@8,@68:@2,@69:@14,@70:@3,@71:@5,@72:@4,@73:@34,@74:@38,@75:@40,@76:@37,@77:@46,@78:@45,@79:@31,@80:@35,@81:@12,@82:@15,@83:@1,@84:@17,@85:@32,@86:@9,@87:@13,@88:@7,@89:@16,@90:@6,
        @48:@29,@49:@18,@50:@19,@51:@20,@52:@21,@53:@23,@54:@22,@55:@26,@56:@28,@57:@25,
        @8:@51,@9:@48,@13:@36,@27:@53,@32:@49,@37:@123,@38:@126,@39:@124,@40:@125,@46:@117,@36:@115,@35:@119,@33:@116,@34:@121,
        @16:@56,@17:@59,@18:@58,@91:@55};
    NSNumber *value = keys[@(vk)]; return value ? value.intValue : -1;
}
static void releaseButtons(void) {
    for (int b = 0; b < 3; b++) if (heldButtons & (1 << b)) {
        CGEventType t = b == 0 ? kCGEventLeftMouseUp : b == 1 ? kCGEventRightMouseUp : kCGEventOtherMouseUp;
        CGMouseButton button = b == 0 ? kCGMouseButtonLeft : b == 1 ? kCGMouseButtonRight : kCGMouseButtonCenter;
        CGEventRef e = CGEventCreateMouseEvent(NULL, t, pointer, button); CGEventPost(kCGHIDEventTap, e); CFRelease(e);
    }
    heldButtons = 0;
}
static NSDictionary *input(NSDictionary *r) {
    if (!AXIsProcessTrusted()) return @{@"error": @"Allow ArX VR Bridge in Mac Accessibility settings."};
    lastInputTime = NSDate.timeIntervalSinceReferenceDate;
    NSString *kind = r[@"kind"];
    if ([kind isEqual:@"release"]) { releaseButtons(); return @{@"ok":@YES}; }
    if ([kind isEqual:@"pointer"]) {
        int slot = [r[@"slot"] intValue]; CGDirectDisplayID display = displayForSlot(slot);
        if (!display || !CGDisplayIsOnline(display)) { releaseButtons(); return @{@"error": @"Desktop is no longer available."}; }
        CGRect bounds = CGDisplayBounds(display);
        double u = [r[@"u"] doubleValue], v = [r[@"v"] doubleValue];
        if (!isfinite(u) || !isfinite(v)) return @{@"error": @"Invalid pointer coordinates"};
        pointer = CGPointMake(bounds.origin.x + fmax(0, fmin(1, u)) * (bounds.size.width-1), bounds.origin.y + fmax(0, fmin(1, v)) * (bounds.size.height-1));
        int buttons = [r[@"buttons"] intValue] & 7;
        CGEventType move = (heldButtons & 1) ? kCGEventLeftMouseDragged : (heldButtons & 2) ? kCGEventRightMouseDragged : (heldButtons & 4) ? kCGEventOtherMouseDragged : kCGEventMouseMoved;
        CGMouseButton movingButton = (heldButtons & 2) ? kCGMouseButtonRight : (heldButtons & 4) ? kCGMouseButtonCenter : kCGMouseButtonLeft;
        CGEventRef e = CGEventCreateMouseEvent(NULL, move, pointer, movingButton); CGEventPost(kCGHIDEventTap, e); CFRelease(e);
        for (int b = 0; b < 3; b++) if ((buttons ^ heldButtons) & (1 << b)) {
            BOOL down = buttons & (1 << b);
            CGEventType t = b == 0 ? (down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp) : b == 1 ? (down ? kCGEventRightMouseDown : kCGEventRightMouseUp) : (down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp);
            CGMouseButton button = b == 0 ? kCGMouseButtonLeft : b == 1 ? kCGMouseButtonRight : kCGMouseButtonCenter;
            e = CGEventCreateMouseEvent(NULL, t, pointer, button);
            if (b == 0) {
                if (down) { double now = NSDate.timeIntervalSinceReferenceDate;
                    clickCount = now-lastClickTime < NSEvent.doubleClickInterval && hypot(pointer.x-lastClickPoint.x,pointer.y-lastClickPoint.y)<5 ? MIN(clickCount+1,3) : 1;
                    lastClickTime=now; lastClickPoint=pointer;
                }
                CGEventSetIntegerValueField(e,kCGMouseEventClickState,clickCount);
            }
            CGEventPost(kCGHIDEventTap,e); CFRelease(e);
        }
        heldButtons=buttons;
        int scroll = MAX(-20,MIN(20,[r[@"scroll"] intValue])), hscroll = MAX(-20,MIN(20,[r[@"hscroll"] intValue]));
        // The second wheel runs positive to the left, the headset sends positive to the right
        if (scroll || hscroll) { e=CGEventCreateScrollWheelEvent(NULL,kCGScrollEventUnitLine,2,scroll,-hscroll); CGEventPost(kCGHIDEventTap,e); CFRelease(e); }
    } else if ([kind isEqual:@"text"]) {
        NSString *text = r[@"text"];
        if (![text isKindOfClass:NSString.class] || text.length > 16000) return @{@"error":@"Invalid text"};
        for (NSUInteger offset=0;offset<text.length;) {
            NSRange range=NSMakeRange(offset,MIN((NSUInteger)20,text.length-offset));
            range=[text rangeOfComposedCharacterSequencesForRange:range];
            UniChar chars[128]; if (range.length>128) return @{@"error":@"Unsupported character sequence"};
            [text getCharacters:chars range:range];
            CGEventRef e=CGEventCreateKeyboardEvent(NULL,0,true); CGEventKeyboardSetUnicodeString(e,range.length,chars); CGEventPost(kCGHIDEventTap,e);
            CGEventSetType(e,kCGEventKeyUp); CGEventPost(kCGHIDEventTap,e); CFRelease(e); offset=NSMaxRange(range);
        }
    } else if ([kind isEqual:@"key"]) {
        int key=macKey([r[@"key"] intValue]); if (key<0) return @{@"error":@"Unsupported key"};
        for(int down=1;down>=0;down--) { CGEventRef e=CGEventCreateKeyboardEvent(NULL,key,down); CGEventSetFlags(e,flagsFor([r[@"modifiers"] intValue])); CGEventPost(kCGHIDEventTap,e); CFRelease(e); }
    } else return @{@"error":@"Unknown input action"};
    return @{@"ok":@YES};
}
// Right of whatever is rightmost now, so windows cross over in the order the desktops were added
static CGPoint nextDesktopOrigin(void) {
    CGRect main = CGDisplayBounds(mainDisplay);
    double right = CGRectGetMaxX(main);
    for (id display in displays.allValues) right = fmax(right, CGRectGetMaxX(CGDisplayBounds([display displayID])));
    return CGPointMake(right, main.origin.y);
}
// Kept on disk too, so a restart of the bridge does not forget where your windows were
static NSString *memoryPath(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/ArX VR Bridge/windows.json"];
}
static void saveMemory(void) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    for (NSNumber *slot in windowMemory) out[slot.stringValue] = windowMemory[slot];
    NSData *data = [NSJSONSerialization dataWithJSONObject:out options:0 error:nil];
    [data writeToFile:memoryPath() atomically:YES];
}
static void loadMemory(void) {
    NSData *data = [NSData dataWithContentsOfFile:memoryPath()];
    NSDictionary *in = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![in isKindOfClass:NSDictionary.class]) return;
    for (NSString *slot in in) if ([in[slot] isKindOfClass:NSArray.class]) windowMemory[@(slot.intValue)] = in[slot];
}
// Notes every window on a desktop and where on it, just before it is unplugged
static void rememberWindows(int slot) {
    CGRect desk = CGDisplayBounds(displayForSlot(slot));
    NSMutableArray *kept = [NSMutableArray array];
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    for (NSDictionary *info in (__bridge NSArray *)list) {
        if ([info[(__bridge NSString *)kCGWindowLayer] intValue] != 0) continue;
        CGRect w = CGRectNull;
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(__bridge NSString *)kCGWindowBounds], &w);
        if (!CGRectContainsPoint(desk, CGPointMake(CGRectGetMidX(w), CGRectGetMidY(w)))) continue;
        [kept addObject:@{ @"id": info[(__bridge NSString *)kCGWindowNumber],
                           @"dx": @(w.origin.x - desk.origin.x), @"dy": @(w.origin.y - desk.origin.y) }];
    }
    if (list) CFRelease(list);
    windowMemory[@(slot)] = kept;
    saveMemory();
    NSLog(@"ArX desktop %d unplugged with %lu windows", slot + 1, (unsigned long)kept.count);
}
// Puts those windows back on the desktop, where they were on it. A window closed in the meantime is skipped.
static void restoreWindows(int slot) {
    NSArray *kept = windowMemory[@(slot)];
    [windowMemory removeObjectForKey:@(slot)];
    saveMemory();
    CGRect desk = CGDisplayBounds(displayForSlot(slot));
    for (NSDictionary *entry in kept) {
        pid_t pid = 0;
        CGRect now = windowBounds([entry[@"id"] unsignedIntValue], &pid);
        if (CGRectIsNull(now)) continue;
        AXUIElementRef w = accessibleWindow(pid, now);
        if (!w) continue;
        CGPoint target = CGPointMake(desk.origin.x + [entry[@"dx"] doubleValue], desk.origin.y + [entry[@"dy"] doubleValue]);
        AXValueRef value = AXValueCreate(kAXValueCGPointType, &target);
        AXUIElementSetAttributeValue(w, kAXPositionAttribute, value);
        CFRelease(value); CFRelease(w);
    }
}
static void addDisplay(int slot, void (^done)(NSDictionary *)) {
    if (slot < 1 || slot >= ARX_VIDEO_MAX_SLOTS || [creating containsObject:@(slot)] || monitors[@(slot)]) { done(@{@"error":@"That desktop slot is in use."}); return; }
    // Already plugged in, the headset only back from a short absence: just make sure it is streaming
    if (displays[@(slot)]) {
        [unwatched removeObjectForKey:@(slot)];
        if ([arxVideoDesktopSlots() containsObject:@(slot)]) { done(@{@"ok":@YES,@"slot":@(slot),@"width":@ARX_DESKTOP_W,@"height":@ARX_DESKTOP_H}); return; }
        arxVideoAttachDisplay(slot, [displays[@(slot)] displayID], ARX_DESKTOP_KBPS, ^(NSString *failure) {
            done(failure ? @{@"error":failure} : @{@"ok":@YES,@"slot":@(slot),@"width":@ARX_DESKTOP_W,@"height":@ARX_DESKTOP_H});
        });
        return;
    }
    if (!CGPreflightScreenCaptureAccess()) { done(@{@"error":@"Allow ArX VR Bridge in Screen & System Audio Recording on your Mac."}); return; }
    Class descriptorClass=NSClassFromString(@"CGVirtualDisplayDescriptor"), displayClass=NSClassFromString(@"CGVirtualDisplay");
    if (!descriptorClass || !displayClass) { done(@{@"error":@"This macOS version does not expose virtual displays."}); return; }
    @try {
        id descriptor=[descriptorClass new];
        [descriptor setValue:[NSString stringWithFormat:@"ArX VR Desktop %d",slot+1] forKey:@"name"];
        [descriptor setValue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0) forKey:@"queue"];
        [descriptor setValue:[NSValue valueWithPoint:NSMakePoint(0.3125,0.3291)] forKey:@"whitePoint"];
        [descriptor setValue:[NSValue valueWithPoint:NSMakePoint(0.64,0.33)] forKey:@"redPrimary"];
        [descriptor setValue:[NSValue valueWithPoint:NSMakePoint(0.30,0.60)] forKey:@"greenPrimary"];
        [descriptor setValue:[NSValue valueWithPoint:NSMakePoint(0.15,0.06)] forKey:@"bluePrimary"];
        [descriptor setValue:@ARX_DESKTOP_W forKey:@"maxPixelsWide"]; [descriptor setValue:@ARX_DESKTOP_H forKey:@"maxPixelsHigh"];
        [descriptor setValue:[NSValue valueWithSize:NSMakeSize(530,300)] forKey:@"sizeInMillimeters"];
        [descriptor setValue:@0x4158 forKey:@"vendorID"]; [descriptor setValue:@2 forKey:@"productID"];
        [descriptor setValue:@(42000+slot) forKey:@"serialNum"];
        if([descriptor respondsToSelector:NSSelectorFromString(@"setSerialNumber:")]) [descriptor setValue:@(42000+slot) forKey:@"serialNumber"];
        id display=[[displayClass alloc] initWithDescriptor:descriptor];
        id mode=[[NSClassFromString(@"CGVirtualDisplayMode") alloc] initWithWidth:ARX_DESKTOP_W height:ARX_DESKTOP_H refreshRate:60];
        id settings=[NSClassFromString(@"CGVirtualDisplaySettings") new]; [settings setValue:@[mode] forKey:@"modes"]; [settings setValue:@0 forKey:@"hiDPI"];
        if(!display || ![display applySettings:settings]) { done(@{@"error":@"macOS could not create the extra desktop."}); return; }
        CGPoint origin=nextDesktopOrigin();
        displays[@(slot)]=display; [creating addObject:@(slot)];
        CGDisplayConfigRef layout=NULL;
        if(CGBeginDisplayConfiguration(&layout)==kCGErrorSuccess) {
            CGConfigureDisplayOrigin(layout,[display displayID],(int32_t)origin.x,(int32_t)origin.y);
            CGCompleteDisplayConfiguration(layout,kCGConfigureForSession);
        }
        arxVideoAttachDisplay(slot, [display displayID], ARX_DESKTOP_KBPS, ^(NSString *failure) {
            [creating removeObject:@(slot)];
            if(failure) [displays removeObjectForKey:@(slot)];
            // The windows it had last time go back, once the new display has settled
            else if (windowMemory[@(slot)]) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ restoreWindows(slot); });
            updateStatus();
            done(failure ? @{@"error":failure} : @{@"ok":@YES,@"slot":@(slot),@"width":@ARX_DESKTOP_W,@"height":@ARX_DESKTOP_H});
        });
    } @catch(NSException *e) { [displays removeObjectForKey:@(slot)]; [creating removeObject:@(slot)]; done(@{@"error":@"Virtual display API is incompatible with this macOS version."}); }
}
// Unplugged: macOS moves its windows to the laptop screen, as with a real monitor
static void removeDisplay(int slot) {
    arxVideoDetach(slot);
    [displays removeObjectForKey:@(slot)];
    [unwatched removeObjectForKey:@(slot)];
}
// Shows one of your own monitors in VR under a slot
static void showMonitor(int slot, CGDirectDisplayID display, void (^done)(NSDictionary *)) {
    if (slot < 1 || slot >= ARX_VIDEO_MAX_SLOTS || displays[@(slot)]) { done(@{@"error":@"That slot is in use."}); return; }
    if (![physicalMonitors() containsObject:@(display)]) { done(@{@"error":@"That monitor is not connected."}); return; }
    monitors[@(slot)] = @(display);
    [unwatched removeObjectForKey:@(slot)];
    if ([arxVideoDesktopSlots() containsObject:@(slot)]) { done(@{@"ok":@YES,@"slot":@(slot)}); return; }
    arxVideoAttachDisplay(slot, display, ARX_DESKTOP_KBPS, ^(NSString *failure) {
        NSLog(@"ArX monitor %u in slot %d: %@", display, slot, failure ?: @"streaming");
        if (failure) [monitors removeObjectForKey:@(slot)];
        updateStatus();
        done(failure ? @{@"error":failure} : @{@"ok":@YES,@"slot":@(slot)});
    });
}
// Mission Control, the view the three-finger swipe up gives: every app and desktop at once
static void overview(void) {
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:@"/System/Applications/Mission Control.app"]
        configuration:[NSWorkspaceOpenConfiguration configuration] completionHandler:nil];
}
// Once a second: a desktop nobody has watched for a few seconds is unplugged, its windows remembered
static void unplugUnwatched(void) {
    // A monitor that is really there only stops streaming; one unplugged from the Mac goes
    for (NSNumber *slot in monitors.allKeys) {
        BOOL gone = ![physicalMonitors() containsObject:monitors[slot]];
        int idle = arxVideoViewers(slot.intValue) > 0 ? 0 : [unwatched[slot] intValue] + 1;
        unwatched[slot] = @(idle);
        if (!gone && idle < ARX_UNWATCHED_S) continue;
        NSLog(@"ArX monitor %@ in slot %@ no longer shown: %@", monitors[slot], slot, gone ? @"unplugged from the Mac" : @"nobody watching");
        arxVideoDetach(slot.intValue);
        [monitors removeObjectForKey:slot];
        [unwatched removeObjectForKey:slot];
        updateStatus();
    }
    for (NSNumber *slot in displays.allKeys) {
        if ([creating containsObject:slot]) continue;
        int idle = arxVideoViewers(slot.intValue) > 0 ? 0 : [unwatched[slot] intValue] + 1;
        unwatched[slot] = @(idle);
        if (idle < ARX_UNWATCHED_S) continue;
        releaseButtons();
        rememberWindows(slot.intValue);
        removeDisplay(slot.intValue);
        updateStatus();
    }
}
static void command(NSDictionary *r) {
    id identifier=r[@"request_id"] ?: @0;
    void (^done)(NSDictionary *)=^(NSDictionary *value){ NSMutableDictionary *result=[value mutableCopy]; result[@"request_id"]=identifier; reply(result); };
    NSString *op=r[@"op"];
    if([op isEqual:@"status"]) { updateStatus(); done(status()); }
    else if([op isEqual:@"usb"]) { questConnected=[r[@"connected"] boolValue]; updateStatus(); done(@{@"ok":@YES}); }
    else if([op isEqual:@"input"]) done(input(r));
    else if([op isEqual:@"video_start"]) {
        NSString *failure = arxVideoStart([r[@"port"] intValue] ?: 47997, [r[@"fps"] intValue] ?: 60,
                                          [r[@"bitrate"] intValue] ?: 30000, r[@"token"]);
        done(failure ? @{@"error": failure} : arxVideoStatus());
    }
    else if([op isEqual:@"video_stop"]) { arxVideoStop(); done(@{@"ok": @YES}); }
    else if([op isEqual:@"video_status"]) done(arxVideoStatus());
    else if([op isEqual:@"add"]) addDisplay([r[@"slot"] intValue],done);
    else if([op isEqual:@"monitor"]) showMonitor([r[@"slot"] intValue],[r[@"display"] unsignedIntValue],done);
    else if([op isEqual:@"overview"]) { overview(); done(@{@"ok":@YES}); }
    else if([op isEqual:@"mic"]) { micOn=[r[@"on"] boolValue]; updateStatus(); done(@{@"ok":@YES}); }
    else if([op isEqual:@"remove"]) {
        // Closed on purpose: its windows come home to the laptop screen and stay there
        NSNumber *slot=r[@"slot"]; if([creating containsObject:slot]) { done(@{@"error":@"Desktop is still starting."}); return; }
        releaseButtons();
        // One of your monitors only stops being shown; a VR desktop is unplugged
        if (monitors[slot]) { arxVideoDetach(slot.intValue); [monitors removeObjectForKey:slot]; [unwatched removeObjectForKey:slot]; }
        else { [windowMemory removeObjectForKey:slot]; saveMemory(); removeDisplay(slot.intValue); }
        updateStatus(); done(@{@"ok":@YES});
    } else done(@{@"error":@"Unknown bridge command"});
}
@interface ArxDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation ArxDelegate
- (void)capturePermission:(id)sender { CGRequestScreenCaptureAccess(); [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]]; }
- (void)controlPermission:(id)sender { AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{(__bridge NSString*)kAXTrustedCheckOptionPrompt:@YES}); [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]]; }
- (void)quit:(id)sender { [NSApp terminate:nil]; }
- (void)mirror:(id)sender { arxMirrorShow(arxVideoToken()); updateStatus(); }
// This window is the bridge: closing it ends everything, the mirror included
- (void)windowWillClose:(NSNotification *)note { if (note.object == window) [NSApp terminate:nil]; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    // Arvolve charcoal, off-white type, one blue action
    NSApp.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,560,470) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    window.title=@"ArX VR Bridge"; window.delegate=self; [window center];
    window.backgroundColor=[NSColor colorWithSRGBRed:0.04 green:0.04 blue:0.047 alpha:1];
    NSView *content=window.contentView;
    NSImage *logo=[[NSImage alloc] initWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"logo" ofType:@"png"]];
    if (logo) { NSImageView *mark=[NSImageView imageViewWithImage:logo]; mark.frame=NSMakeRect(24,366,92,92); mark.imageScaling=NSImageScaleProportionallyUpOrDown; [content addSubview:mark]; }
    NSTextField *title=[NSTextField labelWithString:@"ArX VR Bridge"]; title.font=[NSFont systemFontOfSize:24 weight:NSFontWeightSemibold];
    title.textColor=[NSColor colorWithSRGBRed:0.96 green:0.96 blue:0.96 alpha:1]; title.frame=NSMakeRect(128,410,400,32); [content addSubview:title];
    NSTextField *subtitle=[NSTextField labelWithString:@"Your Mac, one to one, in your Quest"]; subtitle.font=[NSFont systemFontOfSize:14];
    subtitle.textColor=[NSColor colorWithSRGBRed:0.75 green:0.74 blue:0.73 alpha:1]; subtitle.frame=NSMakeRect(128,386,400,22); [content addSubview:subtitle];
    statusLabel=[[NSTextField alloc] initWithFrame:NSMakeRect(28,138,504,226)]; statusLabel.editable=NO; statusLabel.bezeled=NO; statusLabel.drawsBackground=NO; statusLabel.font=[NSFont systemFontOfSize:15];
    statusLabel.textColor=[NSColor colorWithSRGBRed:0.9 green:0.9 blue:0.9 alpha:1];
    [content addSubview:statusLabel];
    NSButton *mirror=[NSButton buttonWithTitle:@"Show what the headset sees" target:self action:@selector(mirror:)];
    mirror.frame=NSMakeRect(24,86,512,40); mirror.keyEquivalent=@"\r"; mirror.bezelColor=[NSColor colorWithSRGBRed:0.19 green:0.58 blue:0.94 alpha:1];
    [content addSubview:mirror];
    NSArray *titles=@[@"Allow screen capture",@"Allow Mac control",@"Quit"];
    SEL actions[]={@selector(capturePermission:),@selector(controlPermission:),@selector(quit:)};
    for(int i=0;i<3;i++) { NSButton *b=[NSButton buttonWithTitle:titles[i] target:self action:actions[i]]; b.frame=NSMakeRect(24+i*174,40,164,36); [content addSubview:b]; }
    updateStatus(); [window makeKeyAndOrderFront:nil];
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
        if(heldButtons && NSDate.timeIntervalSinceReferenceDate-lastInputTime>2.0) releaseButtons();
    }];
    [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *timer) { unplugUnwatched(); updateStatus(); }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        char *line=NULL; size_t length=0;
        while(getline(&line,&length,stdin)>0) { @autoreleasepool {
            NSData *data=[[NSString stringWithUTF8String:line] dataUsingEncoding:NSUTF8StringEncoding];
            NSDictionary *request=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if([request isKindOfClass:NSDictionary.class]) dispatch_async(dispatch_get_main_queue(), ^{ command(request); });
        }}
        free(line); dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    });
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible { [window makeKeyAndOrderFront:nil]; return YES; }
// Quitting unplugs every desktop, so their windows are remembered first and go back next time
- (void)applicationWillTerminate:(NSNotification *)notification {
    releaseButtons();
    for (NSNumber *slot in displays.allKeys) rememberWindows(slot.intValue);
    arxVideoStop();
    [displays removeAllObjects];
}
@end
int main(void) {
    @autoreleasepool {
        mainDisplay=CGMainDisplayID(); displays=[NSMutableDictionary new]; monitors=[NSMutableDictionary new]; creating=[NSMutableSet new];
        unwatched=[NSMutableDictionary new]; windowMemory=[NSMutableDictionary new]; loadMemory();
        NSApplication *app=[NSApplication sharedApplication]; app.activationPolicy=NSApplicationActivationPolicyRegular;
        ArxDelegate *delegate=[ArxDelegate new]; app.delegate=delegate; [app run];
    } return 0;
}
