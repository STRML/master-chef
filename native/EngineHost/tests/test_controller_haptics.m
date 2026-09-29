/* Mock controller and CoreHaptics engine; no hardware/discovery or playback.
 * clang -O1 -g -fobjc-arc -fblocks -fsanitize=address,undefined \
 *   native/EngineHost/tests/test_controller_haptics.m -framework Foundation \
 *   -framework CoreHaptics -framework GameController -o /tmp/controller-haptics
 */
#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <assert.h>
#include <string.h>

@interface MockEngine : NSObject
@property(copy) CHHapticEngineResetHandler resetHandler;
@property(copy) CHHapticEngineStoppedHandler stoppedHandler;
@property BOOL autoShutdownEnabled, failStart, failPlayer, failPlay, resetDuringStart;
@property NSUInteger starts, stops, plays;
- (BOOL)startAndReturnError:(NSError **)error;
- (void)stopWithCompletionHandler:(CHHapticCompletionHandler)completion;
- (id<CHHapticPatternPlayer>)createPlayerWithPattern:(CHHapticPattern *)pattern error:(NSError **)error;
@end
@interface MockPlayer : NSObject
@property(weak) MockEngine *engine;
- (BOOL)startAtTime:(NSTimeInterval)time error:(NSError **)error;
@end
@implementation MockEngine
- (BOOL)startAndReturnError:(NSError **)error {
    self.starts++;
    if (self.resetDuringStart) { self.resetDuringStart=NO; self.resetHandler(); }
    if (self.failStart && error) *error=[NSError errorWithDomain:@"fixture" code:1 userInfo:nil];
    return !self.failStart;
}
- (void)stopWithCompletionHandler:(CHHapticCompletionHandler)completion {
    self.stops++;
    if (self.stoppedHandler) self.stoppedHandler(CHHapticEngineStoppedReasonNotifyWhenFinished);
    if (completion) completion(nil);
}
- (id<CHHapticPatternPlayer>)createPlayerWithPattern:(CHHapticPattern *)pattern error:(NSError **)error {
    (void)error; assert(pattern);
    if (self.failPlayer) return nil;
    MockPlayer *player=[MockPlayer new]; player.engine=self;
    return (id<CHHapticPatternPlayer>)player;
}
@end
@implementation MockPlayer
- (BOOL)startAtTime:(NSTimeInterval)time error:(NSError **)error {
    (void)time;(void)error;
    if (self.engine.failPlay) return NO;
    self.engine.plays++; return YES;
}
@end
@interface MockHaptics : NSObject
@property(strong) NSMutableArray<MockEngine *> *engines;
@property BOOL failNextStart;
- (CHHapticEngine *)createEngineWithLocality:(GCHapticsLocality)locality;
@end
@implementation MockHaptics
- (instancetype)init { if ((self=[super init])) _engines=[NSMutableArray array]; return self; }
- (CHHapticEngine *)createEngineWithLocality:(GCHapticsLocality)locality {
    (void)locality;MockEngine *engine=[MockEngine new]; engine.failStart=self.failNextStart; self.failNextStart=NO;
    [self.engines addObject:engine];return (CHHapticEngine *)engine;
}
@end
@interface MockController : NSObject
@property(strong) MockHaptics *mockHaptics;
@property(readonly) GCDeviceHaptics *haptics;
@property(readonly) GCExtendedGamepad *extendedGamepad;
@property(readonly) NSString *vendorName;
+ (MockController *)current;
+ (NSArray<MockController *> *)controllers;
+ (void)startWirelessControllerDiscoveryWithCompletionHandler:(void (^)(void))completion;
+ (void)stopWirelessControllerDiscovery;
- (MockController *)capture;
@end
static MockController *attached;
@implementation MockController
+ (MockController *)current {return attached;}
+ (NSArray<MockController *> *)controllers {return attached?@[attached]:@[];}
+ (void)startWirelessControllerDiscoveryWithCompletionHandler:(void (^)(void))completion {(void)completion;assert(!"hardware discovery forbidden");}
+ (void)stopWirelessControllerDiscovery {}
- (GCDeviceHaptics *)haptics {return (GCDeviceHaptics *)self.mockHaptics;}
- (GCExtendedGamepad *)extendedGamepad {return (GCExtendedGamepad *)self;}
- (NSString *)vendorName {return @"mock pad";}
- (MockController *)capture {return self;}
@end
#define GCController MockController
#include "../gamecontroller.m"
void host_log(const char *fmt,...) {(void)fmt;}
/* Pulses play on the haptics queue; wait for each one so the checks below see
 * its effect. The pacing guard is tested on its own (test_controller_link_guard.c)
 * and at the end of this test. */
static void play_now(float intensity, float sharpness) {
    hostgc_play_haptic(intensity, sharpness); dispatch_sync(haptic_queue(), ^{});
}
#define hostgc_play_haptic play_now

static MockController *pad(void) {
    MockController *p=[MockController new];p.mockHaptics=[MockHaptics new];return p;
}
int main(void) { @autoreleasepool {
    g_inited=true; /* Keep this executable entirely off hardware discovery. */
    g_guard_enabled=false;
    attached=pad();
    hostgc_play_haptic(0.8f,0.3f);
    MockController *first=attached;
    MockEngine *engine=first.mockHaptics.engines.lastObject;
    assert(engine.starts==1&&engine.plays==1&&hostgc_haptics_played()==1);
    hostgc_play_haptic(0.8f,0.3f);
    assert(engine.starts==1&&engine.plays==2&&first.mockHaptics.engines.count==1);
    engine.stoppedHandler(CHHapticEngineStoppedReasonApplicationSuspended);
    assert(engine.starts==1&&strstr(hostgc_haptic_state(),"stopped"));
    hostgc_play_haptic(0.8f,0.3f);
    assert(engine.starts==2&&engine.plays==3&&strstr(hostgc_haptic_state(),"ready"));
    engine.resetHandler();assert(engine.starts==2);
    hostgc_play_haptic(0.8f,0.3f);assert(engine.starts==3&&engine.plays==4);
    engine.resetDuringStart=YES;engine.resetHandler();
    hostgc_play_haptic(0.8f,0.3f);assert(engine.starts==4&&g_haptic_needs_start);
    hostgc_play_haptic(0.8f,0.3f);assert(engine.starts==5&&!g_haptic_needs_start);

    /* A failed restart must be observable and drop the poisoned engine. */
    engine.failStart=YES;engine.resetHandler();
    uint64_t played=hostgc_haptics_played();
    hostgc_play_haptic(0.8f,0.3f);
    assert(!g_haptic_engine&&hostgc_haptics_played()==played&&strstr(hostgc_haptic_state(),"start failed"));
    hostgc_play_haptic(0.8f,0.3f);
    assert(first.mockHaptics.engines.count==2&&hostgc_haptics_played()==played+1);
    engine=first.mockHaptics.engines.lastObject;
    CHHapticEngineResetHandler oldReset=engine.resetHandler;
    CHHapticEngineStoppedHandler oldStop=engine.stoppedHandler;
    attached=pad();hostgc_play_haptic(0.8f,0.3f);
    MockEngine *replacement=attached.mockHaptics.engines.lastObject;
    oldReset();oldStop(CHHapticEngineStoppedReasonSystemError);
    hostgc_play_haptic(0.8f,0.3f);
    assert(replacement.starts==1&&replacement.plays==2&&!g_haptic_needs_start);
    assert(engine.stops==1&&strstr(hostgc_haptic_state(),"ready"));

    /* Observed disconnect releases the cached engine, even if the same object reconnects. */
    MockController *second=attached;attached=nil;hostgc_play_haptic(1,0.5f);
    assert(!g_haptic_engine&&replacement.stops==1&&strstr(hostgc_haptic_state(),"no controller"));
    attached=second;hostgc_play_haptic(1,0.5f);
    assert(second.mockHaptics.engines.count==2);
    engine=second.mockHaptics.engines.lastObject;engine.failPlayer=YES;
    played=hostgc_haptics_played();hostgc_play_haptic(1,0.5f);
    assert(!g_haptic_engine&&hostgc_haptics_played()==played);
    hostgc_play_haptic(1,0.5f);assert(hostgc_haptics_played()==played+1);
    engine=second.mockHaptics.engines.lastObject;engine.failPlay=YES;
    hostgc_play_haptic(1,0.5f);assert(!g_haptic_engine);
    hostgc_play_haptic(1,0.5f);assert(hostgc_haptics_played()==played+2);

    attached=pad();attached.mockHaptics.failNextStart=YES;
    hostgc_play_haptic(1,0.5f);assert(!g_haptic_engine);
    hostgc_play_haptic(1,0.5f);assert(attached.mockHaptics.engines.count==2);
    engine=attached.mockHaptics.engines.lastObject;
    played=hostgc_haptics_played();
    dispatch_apply(200,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^(size_t n){
        hostgc_play_haptic(0.5f,0.5f);
        assert(strstr(hostgc_haptic_state(),"ready"));
        (void)hostgc_haptics_played();(void)n;
    });
    assert(hostgc_haptics_played()==played+200&&engine.starts==1&&attached.mockHaptics.engines.count==2);
    const char *snapshot=hostgc_haptic_state();haptic_note("changed after snapshot");
    assert(strstr(snapshot,"ready"));
    assert(!strcmp(hostgc_haptic_state(),"changed after snapshot"));
    /* A disconnect releases the engine at once instead of keeping it running
     * for the pad's return. */
    assert(g_haptic_engine);
    haptic_release_on_disconnect(); dispatch_sync(haptic_queue(), ^{});
    assert(!g_haptic_engine&&strstr(hostgc_haptic_state(),"released"));
    hostgc_play_haptic(1,0.5f);assert(g_haptic_engine&&hostgc_haptics_played()==played+201);
    /* With the guard on, a connected pad past its grace period gets at most one
     * pulse per interval, and none after a disconnect. */
    g_guard_enabled=true; hostgc_guard_init(&g_guard,250000000ull,0);
    hostgc_guard_connected(&g_guard,now_ns());
    uint64_t before=hostgc_haptics_played();
    for(int n=0;n<5;n++)hostgc_play_haptic(1,0.5f);
    assert(hostgc_haptics_played()==before+1&&g_guard.dropped_rate==4);
    hostgc_guard_disconnected(&g_guard,now_ns(),0);
    hostgc_play_haptic(1,0.5f);assert(hostgc_haptics_played()==before+1&&g_guard.dropped_disconnected==1);
    pthread_mutex_lock(&g_haptic_play_lock);haptic_discard_engine();pthread_mutex_unlock(&g_haptic_play_lock);
    puts("PASS controller haptics: pulses off the caller's thread, engine released on disconnect, pacing guard, stop/reset resume, callback during restart, failed creation/play recovery, stale callbacks, reconnect, concurrent callers and stable diagnostics; mocked hardware");
}}
