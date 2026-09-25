#import <Foundation/Foundation.h>
// Finder entry point. The bridge owns the native worker and closes it on exit.
int main(void) {
    @autoreleasepool {
        NSBundle *bundle=NSBundle.mainBundle;
        NSTask *task=[NSTask new];
        task.executableURL=[NSURL fileURLWithPath:[bundle objectForInfoDictionaryKey:@"ArxPython"]];
        task.arguments=@[[bundle objectForInfoDictionaryKey:@"ArxBridgeEntry"]];
        NSMutableDictionary *environment=[NSProcessInfo.processInfo.environment mutableCopy];
        environment[@"ARXVR_GUI_LAUNCH"]=@"1"; task.environment=environment;
        NSError *error=nil;
        if(![task launchAndReturnError:&error]) { NSLog(@"ArX VR Bridge could not launch: %@",error.localizedDescription); return 1; }
        [task waitUntilExit]; return task.terminationStatus;
    }
}
