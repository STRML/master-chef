/* One controller capture serves the reads a frame makes; mocked hardware.
 * clang -O1 -g -fobjc-arc -fblocks -fsanitize=address,undefined \
 *   native/EngineHost/tests/test_controller_snapshot.m -framework Foundation \
 *   -framework CoreHaptics -framework GameController -o /tmp/controller-snapshot
 */
#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

/* The pad's live state; a capture reads it through the element mocks. */
static _Atomic int pad_a, pad_menu;
static _Atomic float pad_lx;
static _Atomic unsigned captures_taken, lookups;

@interface MockElement : NSObject
@property(copy) NSString *name;
@end
@implementation MockElement
- (MockElement *)child:(NSString *)n { MockElement *e = [MockElement new]; e.name = n; return e; }
- (MockElement *)xAxis { return [self child:[self.name stringByAppendingString:@".x"]]; }
- (MockElement *)yAxis { return [self child:[self.name stringByAppendingString:@".y"]]; }
- (MockElement *)up { return [self child:@"up"]; }
- (MockElement *)down { return [self child:@"down"]; }
- (MockElement *)left { return [self child:@"left"]; }
- (MockElement *)right { return [self child:@"right"]; }
- (float)value { return [self.name isEqualToString:@"leftThumbstick.x"] ? atomic_load(&pad_lx) : 0.f; }
- (BOOL)isPressed {
    if ([self.name isEqualToString:@"buttonA"]) return atomic_load(&pad_a) != 0;
    if ([self.name isEqualToString:@"buttonMenu"]) return atomic_load(&pad_menu) != 0;
    return NO;
}
@end
@interface MockPad : NSObject
@end
@implementation MockPad
- (MockElement *)element:(NSString *)n { MockElement *e = [MockElement new]; e.name = n; return e; }
- (MockElement *)leftThumbstick { return [self element:@"leftThumbstick"]; }
- (MockElement *)rightThumbstick { return [self element:@"rightThumbstick"]; }
- (MockElement *)leftTrigger { return [self element:@"leftTrigger"]; }
- (MockElement *)rightTrigger { return [self element:@"rightTrigger"]; }
- (MockElement *)dpad { return [self element:@"dpad"]; }
- (MockElement *)buttonA { return [self element:@"buttonA"]; }
- (MockElement *)buttonB { return [self element:@"buttonB"]; }
- (MockElement *)buttonX { return [self element:@"buttonX"]; }
- (MockElement *)buttonY { return [self element:@"buttonY"]; }
- (MockElement *)leftShoulder { return [self element:@"leftShoulder"]; }
- (MockElement *)rightShoulder { return [self element:@"rightShoulder"]; }
- (MockElement *)leftThumbstickButton { return [self element:@"leftThumbstickButton"]; }
- (MockElement *)rightThumbstickButton { return [self element:@"rightThumbstickButton"]; }
- (MockElement *)buttonMenu { return [self element:@"buttonMenu"]; }
- (MockElement *)buttonOptions { return [self element:@"buttonOptions"]; }
- (MockElement *)buttonHome { return [self element:@"buttonHome"]; }
@end
@interface MockController : NSObject
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
static MockPad *attached_pad;
@implementation MockController
+ (MockController *)current { atomic_fetch_add(&lookups, 1); return attached; }
+ (NSArray<MockController *> *)controllers { return attached ? @[attached] : @[]; }
+ (void)startWirelessControllerDiscoveryWithCompletionHandler:(void (^)(void))completion { (void)completion; assert(!"hardware discovery forbidden"); }
+ (void)stopWirelessControllerDiscovery {}
- (GCDeviceHaptics *)haptics { return nil; }
- (GCExtendedGamepad *)extendedGamepad { return (GCExtendedGamepad *)attached_pad; }
- (NSString *)vendorName { return @"mock pad"; }
- (MockController *)capture { atomic_fetch_add(&captures_taken, 1); return self; }
@end
#define GCController MockController
#include "../gamecontroller.m"
void host_log(const char *fmt, ...) { (void)fmt; }

static void window(const char *us) {
    if (us) setenv("HALO_PAD_REUSE_US", us, 1); else unsetenv("HALO_PAD_REUSE_US");
    atomic_store(&g_window_ns, -1);
    g_recent_ns = 0;   /* forget the last reading so each part starts cold */
}

int main(void) { @autoreleasepool {
    g_inited = true;   /* keep this executable off hardware discovery */
    attached = [MockController new]; attached_pad = [MockPad new];
    HostGCSnapshot a, b;
    uint64_t captures, reuses; uint32_t window_us;

    /* The default window is 4 ms. The timing checks use the 20 ms maximum so
     * a busy machine cannot stretch a burst of reads past it. Reads close
     * together share one capture and its sequence; a read after the window
     * takes a new one. */
    window(NULL);
    hostgc_poll_stats(NULL, NULL, &window_us); assert(window_us == 4000);
    window("20000");
    atomic_store(&pad_a, 1); atomic_store(&pad_lx, 0.5f);
    assert(hostgc_poll(&a) && a.connected && a.buttons[HOSTGC_BTN_A] && a.lx == 0.5f);
    atomic_store(&pad_a, 0); atomic_store(&pad_lx, -0.25f);
    unsigned lookups_before = atomic_load(&lookups);
    for (int i = 0; i < 5; i++) {
        assert(hostgc_poll(&b) && b.buttons[HOSTGC_BTN_A] && b.lx == 0.5f && b.sequence == a.sequence);
    }
    assert(atomic_load(&captures_taken) == 1 && atomic_load(&lookups) == lookups_before);
    hostgc_poll_stats(&captures, &reuses, &window_us);
    assert(captures == 1 && reuses == 5 && window_us == 20000);
    usleep(25000);
    assert(hostgc_poll(&b) && !b.buttons[HOSTGC_BTN_A] && b.lx == -0.25f && b.sequence == a.sequence + 1);
    assert(atomic_load(&captures_taken) == 2);

    /* A disconnect is seen once the window has passed, and a reading of no
     * pad is itself reused, so a frame of retries asks GameController once. */
    attached = nil; usleep(25000);
    lookups_before = atomic_load(&lookups);
    assert(!hostgc_poll(&b) && !b.connected && !b.buttons[HOSTGC_BTN_A]);
    for (int i = 0; i < 5; i++) assert(!hostgc_poll(&b));
    assert(atomic_load(&lookups) == lookups_before + 1 && atomic_load(&captures_taken) == 2);
    attached = [MockController new]; atomic_store(&pad_menu, 1); usleep(25000);
    assert(hostgc_poll(&b) && b.buttons[HOSTGC_BTN_MENU]);

    /* An injected test snapshot is returned at once, window or not, and the
     * hardware answers again as soon as it is cleared. */
    HostGCSnapshot injected = { .connected = true, .sequence = 77 };
    injected.buttons[HOSTGC_BTN_B] = true;
    hostgc_inject_test_snapshot(&injected);
    assert(hostgc_poll(&b) && b.buttons[HOSTGC_BTN_B] && b.sequence == 77);
    hostgc_clear_test_snapshot();
    assert(hostgc_poll(&b) && !b.buttons[HOSTGC_BTN_B] && b.buttons[HOSTGC_BTN_MENU]);

    /* HALO_PAD_REUSE_US=0 is the old behaviour: every poll captures. */
    window("0");
    unsigned before = atomic_load(&captures_taken);
    for (int i = 0; i < 4; i++) assert(hostgc_poll(&b));
    assert(atomic_load(&captures_taken) == before + 4);
    hostgc_poll_stats(NULL, NULL, &window_us); assert(window_us == 0);
    window("999999"); hostgc_poll_stats(NULL, NULL, &window_us); assert(window_us == 20000);
    window("-5"); hostgc_poll_stats(NULL, NULL, &window_us); assert(window_us == 0);

    /* A capture that started before the kept one never replaces it. */
    window("20000");
    HostGCSnapshot newer = { .connected = true, .lx = 1.f }, older = { .connected = true, .lx = -1.f };
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    remember_reading(&newer, now);
    remember_reading(&older, now - 1000);
    assert(hostgc_poll(&b) && b.lx == 1.f && b.sequence == newer.sequence && older.sequence == newer.sequence + 1);

    /* Many threads at once: every call is either a capture or a reuse, every
     * answer is a whole reading, and the counts add up. */
    window(NULL);
    hostgc_poll_stats(&captures, &reuses, NULL);
    uint64_t calls_before = captures + reuses;
    dispatch_apply(4000, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t n) {
        HostGCSnapshot s; (void)n;
        assert(hostgc_poll(&s) && s.connected && s.buttons[HOSTGC_BTN_MENU]);
    });
    uint64_t captures_before = captures;
    hostgc_poll_stats(&captures, &reuses, NULL);
    assert(captures + reuses == calls_before + 4000 && captures - captures_before < 2000);
    printf("PASS controller snapshot: one capture per window (%llu captures for 4000 concurrent polls), "
           "disconnect seen after the window, test injection immediate, HALO_PAD_REUSE_US=0 captures every call, "
           "older capture never replaces newer; mocked hardware\n", (unsigned long long)(captures - captures_before));
}}
