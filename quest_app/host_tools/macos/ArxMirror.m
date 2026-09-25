// The headset's view, sent back from the Quest for a window you can watch, make full
// screen and record. The port is only open while the window is: close it and the
// listener goes, the headset stops encoding, and nothing is left running.
//
// Wire format, after "<token> mirror\n" comes up:
//   frame   length (4), pts_us (8), payload (Annex-B), all big endian
#import "ArxMirror.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <netinet/in.h>
#import <netinet/tcp.h>
#import <pthread.h>
#import <sys/socket.h>
#import <unistd.h>

#define MIRROR_PORT 47996
#define MIRROR_MAX_FRAME (8 * 1024 * 1024)

static NSWindow *mirrorWindow;
static AVSampleBufferDisplayLayer *displayLayer;
static NSTextField *hint;
static NSString *mirrorToken;
static int listenFd = -1, clientFd = -1;
static volatile BOOL listening;
static CMVideoFormatDescriptionRef format;
static NSData *sps, *pps;
static volatile double lastFrameTime;

static void stopListening(void) {
    listening = NO;
    int l = listenFd, c = clientFd;
    listenFd = -1; clientFd = -1;
    if (l >= 0) { shutdown(l, SHUT_RDWR); close(l); }
    if (c >= 0) { shutdown(c, SHUT_RDWR); close(c); }
}

@interface ArxMirrorView : NSView
@end
@implementation ArxMirrorView
- (void)layout { [super layout]; displayLayer.frame = self.bounds; }
@end

@interface ArxMirrorDelegate : NSObject <NSWindowDelegate>
@end
@implementation ArxMirrorDelegate
- (void)windowWillClose:(NSNotification *)note { stopListening(); }
@end
static ArxMirrorDelegate *mirrorDelegate;

static BOOL readAll(int fd, void *buffer, size_t length) {
    uint8_t *at = buffer;
    while (length) {
        ssize_t got = recv(fd, at, length, 0);
        if (got <= 0) return NO;
        at += got; length -= (size_t)got;
    }
    return YES;
}

// Where the next start code begins at or after from, or length if there is none
static size_t nextStart(const uint8_t *data, size_t from, size_t length, size_t *codeLength) {
    for (size_t j = from; j + 3 <= length; j++) {
        if (data[j] == 0 && data[j + 1] == 0) {
            if (data[j + 2] == 1) { *codeLength = 3; return j; }
            if (data[j + 2] == 0 && j + 4 <= length && data[j + 3] == 1) { *codeLength = 4; return j; }
        }
    }
    *codeLength = 0;
    return length;
}

// Annex-B in, one sample out: parameter sets update the format, the rest goes in with
// four byte lengths, which is what the display layer's decoder takes
static void handleFrame(const uint8_t *data, size_t length) {
    NSMutableData *avcc = [NSMutableData data];
    BOOL newSets = NO;
    size_t code = 0, at = nextStart(data, 0, length, &code);
    while (at < length) {
        size_t start = at + code, nextCode = 0;
        size_t end = nextStart(data, start, length, &nextCode);
        if (end > start) {
            NSData *nal = [NSData dataWithBytes:data + start length:end - start];
            int type = ((const uint8_t *)nal.bytes)[0] & 0x1f;
            if (type == 7) { newSets |= ![nal isEqualToData:sps]; sps = nal; }
            else if (type == 8) { newSets |= ![nal isEqualToData:pps]; pps = nal; }
            else {
                uint32_t size = CFSwapInt32HostToBig((uint32_t)nal.length);
                [avcc appendBytes:&size length:4];
                [avcc appendData:nal];
            }
        }
        at = end; code = nextCode;
    }
    if (newSets && sps && pps) {
        const uint8_t *sets[2] = { sps.bytes, pps.bytes };
        size_t sizes[2] = { sps.length, pps.length };
        CMVideoFormatDescriptionRef made = NULL;
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &made) == noErr) {
            if (format) CFRelease(format);
            format = made;
        }
    }
    if (!format || avcc.length == 0) return;

    CMBlockBufferRef block = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, avcc.length, NULL, NULL, 0, avcc.length, 0, &block) != noErr) return;
    CMBlockBufferReplaceDataBytes(avcc.bytes, block, 0, avcc.length);
    CMSampleBufferRef sample = NULL;
    size_t sampleSize = avcc.length;
    CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &sampleSize, &sample);
    CFRelease(block);
    if (!sample) return;
    // Shown the moment it is decoded: this is a live view, there is no clock to keep to
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, YES);
    CFMutableDictionaryRef first = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFDictionarySetValue(first, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    lastFrameTime = NSDate.timeIntervalSinceReferenceDate;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (displayLayer.status == AVQueuedSampleBufferRenderingStatusFailed) [displayLayer flush];
        [displayLayer enqueueSampleBuffer:sample];
        CFRelease(sample);
    });
}

// One headset at a time. Anything that is not the paired headset is closed at once.
static void serve(int fd) {
    char line[256]; size_t at = 0;
    struct timeval timeout = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (at < sizeof(line) - 1 && recv(fd, line + at, 1, 0) == 1 && line[at] != '\n') at++;
    line[at] = 0;
    NSString *expected = [mirrorToken stringByAppendingString:@" mirror"];
    if (!mirrorToken.length || ![expected isEqualToString:[NSString stringWithUTF8String:line] ?: @""]) return;
    struct timeval idle = { .tv_sec = 10 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
    sps = nil; pps = nil;
    uint8_t *frame = malloc(MIRROR_MAX_FRAME);
    uint8_t header[12];
    while (listening && readAll(fd, header, sizeof(header))) {
        uint32_t size = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
        if (size == 0 || size > MIRROR_MAX_FRAME || !readAll(fd, frame, size)) break;
        @autoreleasepool { handleFrame(frame, size); }
    }
    free(frame);
}

static void *acceptLoop(void *unused) {
    while (listening) {
        int fd = accept(listenFd, NULL, NULL);
        if (fd < 0) { if (listening) usleep(100000); continue; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        clientFd = fd;
        serve(fd);
        if (clientFd == fd) { clientFd = -1; close(fd); }
    }
    return NULL;
}

static BOOL startListening(void) {
    if (listening) return YES;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NO;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(MIRROR_PORT); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 2) < 0) { close(fd); return NO; }
    listenFd = fd; listening = YES;
    pthread_t thread;
    pthread_create(&thread, NULL, acceptLoop, NULL);
    pthread_detach(thread);
    return YES;
}

void arxMirrorShow(NSString *token) {
    mirrorToken = token;
    if (!mirrorWindow) {
        mirrorWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        mirrorWindow.title = @"ArX VR Mirror";
        mirrorWindow.releasedWhenClosed = NO;
        mirrorWindow.contentAspectRatio = NSMakeSize(16, 9);
        mirrorWindow.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        mirrorWindow.backgroundColor = [NSColor colorWithSRGBRed:0.04 green:0.04 blue:0.047 alpha:1];
        mirrorDelegate = [ArxMirrorDelegate new];
        mirrorWindow.delegate = mirrorDelegate;
        ArxMirrorView *view = [[ArxMirrorView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
        view.wantsLayer = YES;
        displayLayer = [AVSampleBufferDisplayLayer new];
        displayLayer.videoGravity = AVLayerVideoGravityResizeAspect;
        displayLayer.backgroundColor = CGColorCreateSRGB(0.04, 0.04, 0.047, 1);
        displayLayer.frame = view.bounds;
        [view.layer addSublayer:displayLayer];
        hint = [NSTextField labelWithString:@"Waiting for the headset.\nPut it on and open ArX VR, and what you see shows up here."];
        hint.alignment = NSTextAlignmentCenter;
        hint.textColor = [NSColor colorWithSRGBRed:0.75 green:0.74 blue:0.73 alpha:1];
        hint.font = [NSFont systemFontOfSize:18];
        hint.translatesAutoresizingMaskIntoConstraints = NO;
        [view addSubview:hint];
        [NSLayoutConstraint activateConstraints:@[[hint.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],
                                                  [hint.centerYAnchor constraintEqualToAnchor:view.centerYAnchor]]];
        mirrorWindow.contentView = view;
        [mirrorWindow center];
        [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
            hint.hidden = NSDate.timeIntervalSinceReferenceDate - lastFrameTime < 2.0;
        }];
    }
    if (!startListening()) hint.stringValue = @"Could not open the mirror port. Is another ArX VR Bridge running?";
    [mirrorWindow makeKeyAndOrderFront:nil];
}

NSString *arxMirrorState(void) {
    if (!listening) return @"closed";
    return NSDate.timeIntervalSinceReferenceDate - lastFrameTime < 2.0 ? @"showing the headset" : @"open, waiting for the headset";
}
