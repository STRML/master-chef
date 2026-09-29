/* Execute the production session coordinator with deterministic framework and
 * queue failures. No audio hardware or actual session changes are involved. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#undef TARGET_OS_VISION
#define TARGET_OS_VISION 1
#define AVAudioSession HaloTestAudioSession
#define AVAudioSessionPortDescription HaloTestAudioPortDescription
enum { ENGINEVISION_STARTING=1, ENGINEVISION_RUNNING=2 };
enum { AVAudioSessionInterruptionTypeBegan=1, AVAudioSessionInterruptionTypeEnded=0,
       AVAudioSessionInterruptionOptionShouldResume=1 };
static NSString *const AVAudioSessionCategoryPlayback=@"playback";
static NSString *const AVAudioSessionModeDefault=@"default";
static NSString *const AVAudioSessionInterruptionNotification=@"interrupt";
static NSString *const AVAudioSessionInterruptionTypeKey=@"type";
static NSString *const AVAudioSessionInterruptionOptionKey=@"option";
static NSString *const AVAudioSessionMediaServicesWereResetNotification=@"reset";
static NSString *const UIApplicationDidBecomeActiveNotification=@"active";
static NSString *const UIApplicationDidEnterBackgroundNotification=@"background";
static NSString *const AVAudioSessionRouteChangeNotification=@"route";
static NSString *const AVAudioSessionRouteChangeReasonKey=@"reason";
@interface AVAudioSessionPortDescription : NSObject
@property NSString *portType;
@end
@implementation AVAudioSessionPortDescription
@end
@interface MockRoute : NSObject
@property NSArray *outputs;
@end
@implementation MockRoute
@end
static unsigned activation_attempts, rebuilds, resumes, pauses, watchdogs;
static int fail_activation, fail_rebuild, fail_resume, queue_suspended=1;
static int runtime_state=ENGINEVISION_RUNNING;
static uint64_t now_ns=1000000000ull;
@interface AVAudioSession : NSObject
+ (instancetype)sharedInstance;
- (BOOL)setCategory:(NSString *)category mode:(NSString *)mode options:(NSUInteger)options error:(NSError **)error;
- (BOOL)setActive:(BOOL)active error:(NSError **)error;
@property(readonly) MockRoute *currentRoute;
@property(readonly) float outputVolume;
@property(readonly) double sampleRate;
@property(readonly) NSInteger outputNumberOfChannels;
@end
@implementation AVAudioSession
+ (instancetype)sharedInstance { static id value; static dispatch_once_t once; dispatch_once(&once,^{value=[self new];}); return value; }
- (BOOL)setCategory:(NSString *)c mode:(NSString *)m options:(NSUInteger)o error:(NSError **)e { return YES; }
- (BOOL)setActive:(BOOL)a error:(NSError **)e { activation_attempts++; return !fail_activation; }
- (MockRoute *)currentRoute { return nil; }
- (float)outputVolume { return .5f; }
- (double)sampleRate { return 48000; }
- (NSInteger)outputNumberOfChannels { return 2; }
@end
static int enginevision_runtime_state(void) { return runtime_state; }
static void host_dsound_watchdog_set_external(void) {}
static void host_dsound_watchdog(void) { watchdogs++; }
static int host_dsound_rebuild_output(void) { rebuilds++; return !fail_rebuild; }
static int host_dsound_resume_output(void) { resumes++; if(fail_resume)return 0; queue_suspended=0; return 1; }
static void host_dsound_pause_output(void) { pauses++; queue_suspended=1; }
static uint64_t fake_clock(clockid_t clock) { return now_ns; }
#define clock_gettime_nsec_np fake_clock
#include "../Sources/audio_session.inc"

static void event(NSString *name, NSDictionary *info) {
    [[NSNotificationCenter defaultCenter] postNotificationName:name object:[AVAudioSession sharedInstance] userInfo:info];
    dispatch_sync(audio_control_queue,^{});
}
static void tick(uint64_t delta) {
    dispatch_sync(audio_control_queue,^{now_ns+=delta;audio_session_tick();});
}
int main(void) { @autoreleasepool {
    fail_activation=1; audio_session_prepare();
    dispatch_suspend(audio_watchdog_timer); /* deterministic manual ticks below */
    assert(activation_attempts==1&&queue_suspended);
    fail_activation=0;
    for(int i=0;i<7;i++)tick(250000000ull);
    assert(activation_attempts==1); /* retry rate is bounded */
    tick(250000000ull);
    if(activation_attempts!=2||queue_suspended){fputs("FAIL activation failure never retried; audio remains suspended\n",stderr);return 1;}
    event(UIApplicationDidEnterBackgroundNotification,nil);
    unsigned attempts=activation_attempts;
    event(AVAudioSessionMediaServicesWereResetNotification,nil);
    tick(5000000000ull);assert(activation_attempts==attempts&&queue_suspended);
    fail_rebuild=1;event(UIApplicationDidBecomeActiveNotification,nil);
    assert(rebuilds==1&&queue_suspended);
    fail_rebuild=0;tick(2000000000ull);assert(rebuilds==2&&!queue_suspended);
    event(AVAudioSessionInterruptionNotification,@{AVAudioSessionInterruptionTypeKey:@1});
    attempts=activation_attempts;tick(5000000000ull);assert(activation_attempts==attempts&&queue_suspended);
    event(AVAudioSessionInterruptionNotification,@{AVAudioSessionInterruptionTypeKey:@0,AVAudioSessionInterruptionOptionKey:@0});
    tick(5000000000ull);assert(activation_attempts==attempts&&queue_suspended);
    fail_resume=1;event(UIApplicationDidBecomeActiveNotification,nil);
    assert(queue_suspended);attempts=activation_attempts;
    tick(1999000000ull);assert(activation_attempts==attempts);
    fail_resume=0;tick(1000000ull);assert(!queue_suspended);
    fail_activation=1;event(UIApplicationDidBecomeActiveNotification,nil);
    attempts=activation_attempts;runtime_state=0;tick(5000000000ull);
    assert(activation_attempts==attempts&&queue_suspended);
    dispatch_source_cancel(audio_watchdog_timer);
    dispatch_resume(audio_watchdog_timer);
    dispatch_sync(audio_control_queue,^{});
    for(id observer in audio_observers)[[NSNotificationCenter defaultCenter] removeObserver:observer];
    puts("PASS audio session retry: activation, rebuild, queue-start failures; bounded retries; background, interruption and stopped-runtime gates");
 } return 0; }
