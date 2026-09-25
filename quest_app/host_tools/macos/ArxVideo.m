// Screen capture, hardware H.264 and the frame server the headset connects to.
//
// Wire format, after the client sends its token, optionally a space and a slot, and a newline:
//   header  "ARXV" v1, width, height, fps           (16 bytes, big endian)
//   frame   length, flags (1 = keyframe), pts_us, payload   (Annex-B)
// The client may send one byte 'I' at any time to ask for a fresh keyframe.
// No slot means slot 0, the main display.
#import "ArxVideo.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <netinet/in.h>
#import <netinet/tcp.h>
#import <sys/socket.h>
#import <unistd.h>
#import <pthread.h>

#define ARX_MAX_CLIENTS 4
#define ARX_MAX_WIDTH 2560

static uint64_t nowNs(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

// One desktop's capture, encoder and viewers
@interface ArxVideoStream : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    int slot, width, height, fps, kbps;
    // The display this stream captures: the main one, or a VR desktop
    CGDirectDisplayID display;
    SCStream *capture;
    VTCompressionSessionRef encoder;
    dispatch_queue_t queue;
    CVPixelBufferRef lastPixels;
    dispatch_source_t heartbeat;
    uint64_t lastEncodeNs;
    // -1, never 0: a zeroed slot is a valid descriptor, and the encoder would write frames
    // into this process's own stdio and wreck the JSON protocol it talks to the bridge over
    int clients[ARX_MAX_CLIENTS];
    // A new viewer starts on a keyframe; anything before it references frames it never saw
    int needsKey[ARX_MAX_CLIENTS];
    volatile int wantKeyframe;
    volatile long encoded, sent, dropped;
    NSString *error;
}
@end

static ArxVideoStream *streams[ARX_VIDEO_MAX_SLOTS];
static void recoverStream(int slot);
static pthread_mutex_t clientLock = PTHREAD_MUTEX_INITIALIZER;
static int listenSocket = -1;
static volatile int serverRunning;
static NSString *videoToken;
static int videoFps = 60;

static void sendToClients(ArxVideoStream *s, const uint8_t *payload, size_t length, int keyframe, uint64_t ptsUs) {
    uint8_t head[13];
    uint32_t n = (uint32_t)length;
    head[0] = n >> 24; head[1] = n >> 16; head[2] = n >> 8; head[3] = n;
    head[4] = keyframe ? 1 : 0;
    for (int i = 0; i < 8; i++) head[5 + i] = (uint8_t)(ptsUs >> (56 - 8 * i));
    pthread_mutex_lock(&clientLock);
    for (int i = 0; i < ARX_MAX_CLIENTS; i++) {
        int fd = s->clients[i];
        if (fd < 0) continue;
        if (s->needsKey[i]) { if (!keyframe) continue; s->needsKey[i] = 0; }
        if (send(fd, head, sizeof(head), 0) != (ssize_t)sizeof(head)
                || send(fd, payload, length, 0) != (ssize_t)length) {
            close(fd); s->clients[i] = -1; s->dropped++;
        } else s->sent++;
    }
    pthread_mutex_unlock(&clientLock);
}

// AVCC (length prefixed) to Annex-B (start codes), with the parameter sets ahead of every keyframe
static void emitSample(ArxVideoStream *s, CMSampleBufferRef sample) {
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return;
    int keyframe = 0;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments)) {
        CFDictionaryRef first = CFArrayGetValueAtIndex(attachments, 0);
        keyframe = !CFDictionaryContainsKey(first, kCMSampleAttachmentKey_NotSync);
    }
    NSMutableData *out = [NSMutableData data];
    static const uint8_t startCode[4] = { 0, 0, 0, 1 };
    if (keyframe) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        size_t count = 0; int headerLength = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, &count, &headerLength) == noErr) {
            for (size_t i = 0; i < count; i++) {
                const uint8_t *set = NULL; size_t setLength = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &set, &setLength, NULL, NULL) == noErr) {
                    [out appendBytes:startCode length:4];
                    [out appendBytes:set length:setLength];
                }
            }
        }
    }
    size_t totalLength = 0; char *data = NULL;
    if (CMBlockBufferGetDataPointer(block, 0, NULL, &totalLength, &data) != noErr) return;
    size_t offset = 0;
    while (offset + 4 <= totalLength) {
        uint32_t nalLength = 0;
        memcpy(&nalLength, data + offset, 4);
        nalLength = CFSwapInt32BigToHost(nalLength);
        if (nalLength == 0 || offset + 4 + nalLength > totalLength) break;
        [out appendBytes:startCode length:4];
        [out appendBytes:data + offset + 4 length:nalLength];
        offset += 4 + nalLength;
    }
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    sendToClients(s, out.bytes, out.length, keyframe, (uint64_t)(CMTimeGetSeconds(pts) * 1e6));
}

static void encodedFrame(void *context, void *source, OSStatus status, VTEncodeInfoFlags flags,
                         CMSampleBufferRef sample) {
    if (status != noErr || !sample || !CMSampleBufferDataIsReady(sample)) return;
    ArxVideoStream *s = (__bridge ArxVideoStream *)context;
    s->encoded++;
    emitSample(s, sample);
}

static void encodePixels(ArxVideoStream *s, CVPixelBufferRef pixels, CMTime pts) {
    if (!s->encoder) return;
    NSDictionary *properties = nil;
    if (s->wantKeyframe) { s->wantKeyframe = 0; properties = @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES}; }
    s->lastEncodeNs = nowNs();
    VTCompressionSessionEncodeFrame(s->encoder, pixels, pts, kCMTimeInvalid,
                                    (__bridge CFDictionaryRef)properties, NULL, NULL);
}

@implementation ArxVideoStream
- (instancetype)init {
    if ((self = [super init])) for (int i = 0; i < ARX_MAX_CLIENTS; i++) clients[i] = -1;
    return self;
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    CVImageBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels) return;
    encodePixels(self, pixels, CMSampleBufferGetPresentationTimeStamp(sample));
    // Kept so a motionless desktop still produces a heartbeat rather than a socket
    // the headset reads as dead: ScreenCaptureKit only delivers on change.
    CVPixelBufferRetain(pixels);
    CVPixelBufferRef previous = lastPixels;
    lastPixels = pixels;
    if (previous) CVPixelBufferRelease(previous);
}
// ScreenCaptureKit stops a stream when the display sleeps, and the heartbeat would
// otherwise go on re-sending the last picture forever: a frozen screen that still
// moves the Mac's pointer. A stopped stream is rebuilt, retrying until it takes.
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)failure {
    error = failure.localizedDescription ?: @"capture stopped";
    NSLog(@"ArX video slot %d stopped: %@, rebuilding", slot, error);
    int which = slot;
    dispatch_async(dispatch_get_main_queue(), ^{ recoverStream(which); });
}
@end

static void stopStream(ArxVideoStream *s) {
    if (!s) return;
    if (s->heartbeat) { dispatch_source_cancel(s->heartbeat); s->heartbeat = nil; }
    if (s->capture) { [s->capture stopCaptureWithCompletionHandler:^(NSError *e) {}]; s->capture = nil; }
    // On the capture queue, so nothing is mid-encode with what is being freed
    void (^release)(void) = ^{
        if (s->encoder) { VTCompressionSessionInvalidate(s->encoder); CFRelease(s->encoder); s->encoder = NULL; }
        if (s->lastPixels) { CVPixelBufferRelease(s->lastPixels); s->lastPixels = NULL; }
    };
    if (s->queue) dispatch_sync(s->queue, release); else release();
    pthread_mutex_lock(&clientLock);
    for (int i = 0; i < ARX_MAX_CLIENTS; i++) if (s->clients[i] >= 0) { close(s->clients[i]); s->clients[i] = -1; }
    pthread_mutex_unlock(&clientLock);
}

// Real pixels per point on a display, so a Retina screen is not captured at half resolution
static double pixelScale(CGDirectDisplayID displayID) {
    double scale = 1.0;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
    if (mode) {
        if (CGDisplayModeGetWidth(mode) > 0) scale = (double)CGDisplayModeGetPixelWidth(mode) / CGDisplayModeGetWidth(mode);
        CGDisplayModeRelease(mode);
    }
    return scale;
}


// Capture and encode one display. A display created a moment ago may not be listed
// yet, so it is looked for again a few times rather than failed on the first miss.
static void startStream(ArxVideoStream *s, CGDirectDisplayID displayID, int attempts, void (^done)(NSString *failure)) {
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
            completionHandler:^(SCShareableContent *content, NSError *listError) {
        SCDisplay *display = nil;
        for (SCDisplay *d in content.displays) if (d.displayID == displayID) display = d;
        if (!display) {
            if (attempts > 1) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                    startStream(s, displayID, attempts - 1, done);
                });
                return;
            }
            done(listError.localizedDescription ?: @"display not found");
            return;
        }
        // Real pixels, not points: a Retina display captured at its point size
        // halves the resolution of every letter in the headset
        double scale = pixelScale(displayID);
        s->display = displayID;
        int w = (int)lround(display.width * scale), h = (int)lround(display.height * scale);
        if (w > ARX_MAX_WIDTH) { h = (int)lround((double)h * ARX_MAX_WIDTH / w); w = ARX_MAX_WIDTH; }
        s->width = w & ~1; s->height = h & ~1;

        OSStatus status = VTCompressionSessionCreate(NULL, s->width, s->height, kCMVideoCodecType_H264,
            (__bridge CFDictionaryRef)@{(__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES},
            NULL, NULL, encodedFrame, (__bridge void *)s, &s->encoder);
        if (status != noErr) { done([NSString stringWithFormat:@"encoder failed (%d)", (int)status]); return; }
        VTSessionSetProperty(s->encoder, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(s->encoder, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(s->encoder, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
        VTSessionSetProperty(s->encoder, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef)@(s->kbps * 1000));
        VTSessionSetProperty(s->encoder, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFNumberRef)@(s->fps));
        VTSessionSetProperty(s->encoder, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef)@(s->fps * 2));
        VTCompressionSessionPrepareToEncodeFrames(s->encoder);

        SCStreamConfiguration *config = [SCStreamConfiguration new];
        config.width = s->width; config.height = s->height;
        config.minimumFrameInterval = CMTimeMake(1, s->fps);
        config.queueDepth = 5; config.showsCursor = YES;
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        // The bridge's own windows never go to the headset: the mirror of the headset
        // showing a picture of itself would be an endless hall of mirrors
        NSMutableArray<SCRunningApplication *> *bridge = [NSMutableArray array];
        for (SCRunningApplication *app in content.applications) if (app.processID == getpid()) [bridge addObject:app];
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingApplications:bridge exceptingWindows:@[]];
        s->capture = [[SCStream alloc] initWithFilter:filter configuration:config delegate:s];
        s->queue = dispatch_queue_create("ai.arvolve.arxvr.video", DISPATCH_QUEUE_SERIAL);
        NSError *addError = nil;
        if (![s->capture addStreamOutput:s type:SCStreamOutputTypeScreen sampleHandlerQueue:s->queue error:&addError]) {
            done(addError.localizedDescription ?: @"capture output refused");
            return;
        }
        [s->capture startCaptureWithCompletionHandler:^(NSError *startError) {
            if (startError) { done(startError.localizedDescription); return; }
            s->wantKeyframe = 1;
            // Re-sends the last picture when the desktop has been still for a while
            s->heartbeat = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, s->queue);
            dispatch_source_set_timer(s->heartbeat, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                                      NSEC_PER_SEC / 2, NSEC_PER_SEC / 10);
            dispatch_source_set_event_handler(s->heartbeat, ^{
                if (!s->encoder || !s->lastPixels) return;
                if (nowNs() - s->lastEncodeNs < 800 * NSEC_PER_MSEC) return;
                encodePixels(s, s->lastPixels, CMClockGetTime(CMClockGetHostTimeClock()));
            });
            dispatch_resume(s->heartbeat);
            done(nil);
        }];
    }];
}

static void *acceptLoop(void *unused) {
    while (serverRunning) {
        struct sockaddr_in from; socklen_t length = sizeof(from);
        int fd = accept(listenSocket, (struct sockaddr *)&from, &length);
        if (fd < 0) { if (serverRunning) usleep(100000); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 400000 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        // The token the bridge already gave the headset over USB, so a LAN listener is not open house
        char line[256]; size_t at = 0; int authorised = 0, slot = 0;
        while (at < sizeof(line) - 1) {
            ssize_t got = recv(fd, line + at, 1, 0);
            if (got != 1) break;
            if (line[at] == '\n') {
                line[at] = 0;
                char *space = strchr(line, ' ');
                if (space) { *space = 0; slot = atoi(space + 1); }
                authorised = videoToken && strcmp(line, videoToken.UTF8String) == 0;
                break;
            }
            at++;
        }
        ArxVideoStream *s = authorised && slot >= 0 && slot < ARX_VIDEO_MAX_SLOTS ? streams[slot] : nil;
        if (!s || !s->width) { close(fd); continue; }
        uint8_t header[16] = { 'A','R','X','V', 0,0,0,1 };
        header[8] = s->width >> 8; header[9] = s->width;
        header[10] = s->height >> 8; header[11] = s->height;
        header[12] = s->fps >> 8; header[13] = s->fps;
        if (send(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header)) { close(fd); continue; }
        int added = 0;
        pthread_mutex_lock(&clientLock);
        for (int i = 0; i < ARX_MAX_CLIENTS; i++) if (s->clients[i] < 0) { s->clients[i] = fd; s->needsKey[i] = 1; added = 1; break; }
        pthread_mutex_unlock(&clientLock);
        if (!added) { close(fd); continue; }
        s->wantKeyframe = 1;
    }
    return NULL;
}

// One byte 'I' from any client asks its stream for a keyframe; anything else is ignored
static void *commandLoop(void *unused) {
    while (serverRunning) {
        int idle = 1;
        for (int k = 0; k < ARX_VIDEO_MAX_SLOTS; k++) {
            ArxVideoStream *s = streams[k];
            if (!s) continue;
            int fds[ARX_MAX_CLIENTS];
            pthread_mutex_lock(&clientLock);
            memcpy(fds, s->clients, sizeof(fds));
            pthread_mutex_unlock(&clientLock);
            for (int i = 0; i < ARX_MAX_CLIENTS; i++) {
                if (fds[i] < 0) continue;
                char c; ssize_t got = recv(fds[i], &c, 1, MSG_DONTWAIT);
                if (got == 1) { idle = 0; if (c == 'I') s->wantKeyframe = 1; }
            }
        }
        if (idle) usleep(50000);
    }
    return NULL;
}

NSString *arxVideoToken(void) { return videoToken; }

NSString *arxVideoStart(int port, int fps, int bitrateKbps, NSString *token) {
    if (serverRunning) return nil;
    if (!token.length) return @"missing token";
    videoToken = token; videoFps = fps > 0 ? fps : 60;

    listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) return @"could not create the video socket";
    int one = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listenSocket, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listenSocket, 8) < 0) {
        close(listenSocket); listenSocket = -1;
        return [NSString stringWithFormat:@"could not listen on port %d", port];
    }

    ArxVideoStream *s = [ArxVideoStream new];
    s->slot = 0; s->fps = videoFps; s->kbps = bitrateKbps;
    __block NSString *failure = nil;
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    startStream(s, CGMainDisplayID(), 1, ^(NSString *reason) { failure = reason; dispatch_semaphore_signal(ready); });
    if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) failure = @"capture did not start";
    if (failure) {
        stopStream(s);
        close(listenSocket); listenSocket = -1;
        return failure;
    }
    streams[0] = s;

    serverRunning = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, acceptLoop, NULL); pthread_detach(thread);
    pthread_create(&thread, NULL, commandLoop, NULL); pthread_detach(thread);
    return nil;
}

// The display each VR desktop shows and its bitrate, kept apart from the stream so a
// rebuild after the capture stopped knows what to capture again
static CGDirectDisplayID slotDisplays[ARX_VIDEO_MAX_SLOTS];
static int slotKbps[ARX_VIDEO_MAX_SLOTS];

void arxVideoAttachDisplay(int slot, CGDirectDisplayID display, int bitrateKbps, void (^done)(NSString *failure)) {
    if (slot < 1 || slot >= ARX_VIDEO_MAX_SLOTS) { done(@"no such desktop slot"); return; }
    if (!serverRunning) { done(@"desktop video is not running"); return; }
    arxVideoDetach(slot);
    slotDisplays[slot] = display; slotKbps[slot] = bitrateKbps;
    ArxVideoStream *s = [ArxVideoStream new];
    s->slot = slot; s->fps = videoFps; s->kbps = bitrateKbps;
    // Up to three seconds for a brand new display to be listed
    startStream(s, display, 20, ^(NSString *failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (failure) stopStream(s);
            else streams[slot] = s;
            done(failure);
        });
    });
}

// The main screen's stream, rebuilt after it stopped. The old one's viewers are closed,
// and the headset reconnects to the new one by itself.
static void recoverMain(int attempt) {
    if (!serverRunning) return;
    ArxVideoStream *old = streams[0];
    streams[0] = nil;
    stopStream(old);
    ArxVideoStream *s = [ArxVideoStream new];
    s->slot = 0; s->fps = videoFps; s->kbps = old ? old->kbps : 30000;
    startStream(s, CGMainDisplayID(), 1, ^(NSString *failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!failure) { streams[0] = s; NSLog(@"ArX video main screen back after %d attempts", attempt); return; }
            stopStream(s);
            // Asleep still: try again shortly, for as long as it takes
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ recoverMain(attempt + 1); });
        });
    });
}

// A VR desktop's stream, rebuilt for as long as its display is still there
static void recoverDesktop(int slot) {
    CGDirectDisplayID display = slotDisplays[slot];
    int kbps = slotKbps[slot];
    if (!display || !CGDisplayIsOnline(display)) return;
    arxVideoAttachDisplay(slot, display, kbps, ^(NSString *failure) {
        if (!failure || !CGDisplayIsOnline(display)) return;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (!streams[slot]) recoverDesktop(slot);
        });
    });
}

static void recoverStream(int slot) {
    if (slot == 0) recoverMain(1);
    else recoverDesktop(slot);
}

NSArray<NSNumber *> *arxVideoDesktopSlots(void) {
    NSMutableArray *live = [NSMutableArray array];
    for (int k = 1; k < ARX_VIDEO_MAX_SLOTS; k++) if (streams[k]) [live addObject:@(k)];
    return live;
}

int arxVideoViewers(int slot) {
    if (slot < 0 || slot >= ARX_VIDEO_MAX_SLOTS || !streams[slot]) return 0;
    int viewers = 0;
    pthread_mutex_lock(&clientLock);
    for (int i = 0; i < ARX_MAX_CLIENTS; i++) if (streams[slot]->clients[i] >= 0) viewers++;
    pthread_mutex_unlock(&clientLock);
    return viewers;
}

void arxVideoDetach(int slot) {
    if (slot < 0 || slot >= ARX_VIDEO_MAX_SLOTS) return;
    if (slot > 0) slotDisplays[slot] = 0;
    ArxVideoStream *s = streams[slot];
    streams[slot] = nil;
    stopStream(s);
}

void arxVideoStop(void) {
    serverRunning = 0;
    for (int k = 0; k < ARX_VIDEO_MAX_SLOTS; k++) arxVideoDetach(k);
    if (listenSocket >= 0) { close(listenSocket); listenSocket = -1; }
}

NSDictionary *arxVideoStatus(void) {
    ArxVideoStream *s = streams[0];
    int clients = 0, live = 0;
    pthread_mutex_lock(&clientLock);
    for (int k = 0; k < ARX_VIDEO_MAX_SLOTS; k++) {
        if (!streams[k]) continue;
        live++;
        if (k == 0) for (int i = 0; i < ARX_MAX_CLIENTS; i++) if (s->clients[i] >= 0) clients++;
    }
    pthread_mutex_unlock(&clientLock);
    NSMutableDictionary *status = [@{ @"running": serverRunning && s ? @YES : @NO, @"clients": @(clients),
        @"encoded": @(s ? s->encoded : 0), @"sent": @(s ? s->sent : 0), @"dropped": @(s ? s->dropped : 0),
        @"width": @(s ? s->width : 0), @"height": @(s ? s->height : 0), @"fps": @(videoFps),
        @"streams": @(live) } mutableCopy];
    // Not called "error": a generic caller treats that key as the whole request having failed
    if (s && s->error.length) status[@"last_error"] = s->error;
    return status;
}
