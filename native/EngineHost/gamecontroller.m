/* Apple GameController -> HostGCSnapshot bridge.  See gamecontroller.h.
 *
 * Works with any GCExtendedGamepad-capable device (PS5 DualSense, DualShock 4,
 * Xbox, MFi) on macOS and visionOS.  We poll the profile on demand from the
 * game thread rather than depending on value-changed handlers, so no run loop
 * is required for input to flow; connect/disconnect notifications are only used
 * to keep a cached "current controller" pointer warm and to log hot-plugs.
 */
#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#import <os/lock.h>
#include "gamecontroller.h"
#include "gamecontroller_guard.h"
#include <dispatch/dispatch.h>
#include <time.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

void host_log(const char *fmt, ...);

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static bool           g_inited = false;
static bool           g_have_test = false;      /* a test snapshot is active   */
static HostGCSnapshot g_test;                   /* the injected snapshot       */
static uint64_t       g_sequence = 0;           /* hardware poll counter       */
static HostGCSnapshot g_recent;                 /* the newest hardware reading */
static uint64_t       g_recent_ns = 0;          /* when it was taken; 0 = none */
static uint64_t       g_captures = 0, g_reuses = 0;
static id             g_connect_obs = nil;
static id             g_disconnect_obs = nil;

/* The link guard (gamecontroller_guard.h) and the queue pulses play on. The
 * presenter used to call Core Haptics itself, 90 times a second, and a Start
 * could stall its frame; pulses now go to one serial queue. HALO_HAPTICS_GUARD=0
 * turns the pacing and backoff off (pulses still leave the render thread);
 * HALO_HAPTIC_MIN_INTERVAL_MS (default 250) and HALO_HAPTIC_CONNECT_GRACE_MS
 * (default 3000) tune it. */
static HostGCGuard      g_guard;
static os_unfair_lock   g_guard_lock = OS_UNFAIR_LOCK_INIT;
static bool             g_guard_enabled = true;
static dispatch_queue_t haptic_queue(void) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("halo.controller.haptics",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    });
    return queue;
}
static _Atomic uint64_t g_last_played_ns;
static _Atomic uint64_t g_last_connect_ns, g_last_disconnect_ns;
static _Atomic uint64_t g_haptic_failures, g_haptic_resets, g_haptic_stops;
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static long env_long(const char *name, long fallback, long low, long high) {
    const char *value = getenv(name);
    long v = value && value[0] ? strtol(value, NULL, 10) : fallback;
    return v < low ? low : v > high ? high : v;
}
static void haptic_release_on_disconnect(void);

/* One hardware reading serves every caller for a moment.
 *
 * A frame asks for the pad from half a dozen places: the keyboard and mouse
 * synthesis behind DirectInput's GetDeviceState and GetDeviceData, the
 * joystick's Acquire, Poll and GetDeviceState, USER32's message pump and the
 * pad-to-keyboard bridge at Present. Each used to look the controller up and
 * take its own [controller capture], which copies every element of the
 * profile into new objects, and on the headset that was several captures a
 * frame of readings microseconds apart. The Mac probe injects a test snapshot
 * and never paid it. A reading younger than this window is handed out again
 * instead. The window is well under the shortest frame the engine makes, so
 * the reads a frame makes together share one reading and none handed out is
 * more than the window old. HALO_PAD_REUSE_US sets it in microseconds (at
 * most 20000); 0 captures on every call, as before. */
static _Atomic int64_t g_window_ns = -1;         /* -1 until HALO_PAD_REUSE_US is read */
static uint64_t reuse_window_ns(void) {
    int64_t window = atomic_load_explicit(&g_window_ns, memory_order_relaxed);
    if (window < 0) {
        const char *option = getenv("HALO_PAD_REUSE_US");
        long us = option && option[0] ? strtol(option, NULL, 10) : 4000;
        if (us < 0) us = 0;
        if (us > 20000) us = 20000;
        window = (int64_t)us * 1000;
        atomic_store_explicit(&g_window_ns, window, memory_order_relaxed);
    }
    return (uint64_t)window;
}

/* Pick the extended-gamepad controller most appropriate for the current user.
 * Prefer GCController.current (the one the user last interacted with); fall
 * back to the first attached controller that exposes an extended gamepad. */
static GCController *current_extended_controller(void) {
    GCController *cur = nil;
    if (@available(macOS 11.0, iOS 14.0, *)) cur = GCController.current;
    if (cur && cur.extendedGamepad) return cur;
    for (GCController *c in GCController.controllers)
        if (c.extendedGamepad) return c;
    return nil;
}

void hostgc_init(void) {
    @autoreleasepool {
    os_unfair_lock_lock(&g_lock);
    if (g_inited) { os_unfair_lock_unlock(&g_lock); return; }
    g_inited = true;
    os_unfair_lock_unlock(&g_lock);

    const char *guard = getenv("HALO_HAPTICS_GUARD");
    g_guard_enabled = !(guard && guard[0] == '0');
    hostgc_guard_init(&g_guard, (uint64_t)env_long("HALO_HAPTIC_MIN_INTERVAL_MS", 250, 0, 5000) * 1000000ull,
                      (uint64_t)env_long("HALO_HAPTIC_CONNECT_GRACE_MS", 3000, 0, 60000) * 1000000ull);

    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    g_connect_obs = [nc addObserverForName:GCControllerDidConnectNotification
                                    object:nil queue:nil
                                usingBlock:^(NSNotification *n) {
        GCController *c = n.object;
        host_log("GameController connected: %s (%s)",
                 c.vendorName.UTF8String ?: "?",
                 c.extendedGamepad ? "extended gamepad" : "no extended profile");
        /* Discovery scans for new pads, and a scan takes radio time from the
         * link to the pad already in hand. Stop it once one is here. */
        [GCController stopWirelessControllerDiscovery];
        os_unfair_lock_lock(&g_guard_lock);
        hostgc_guard_connected(&g_guard, now_ns());
        atomic_store_explicit(&g_last_connect_ns, now_ns(), memory_order_relaxed);
        os_unfair_lock_unlock(&g_guard_lock);
    }];
    g_disconnect_obs = [nc addObserverForName:GCControllerDidDisconnectNotification
                                       object:nil queue:nil
                                   usingBlock:^(NSNotification *n) {
        GCController *c = n.object;
        uint64_t now = now_ns(), played = atomic_load_explicit(&g_last_played_ns, memory_order_relaxed);
        bool another = current_extended_controller() != nil;
        os_unfair_lock_lock(&g_guard_lock);
        uint64_t suspensions = g_guard.suspensions;
        hostgc_guard_disconnected(&g_guard, now, played);
        atomic_store_explicit(&g_last_disconnect_ns, now, memory_order_relaxed);
        bool suspended = g_guard.suspensions != suspensions;
        uint64_t until = g_guard.suspended_until_ns;
        /* Another pad is still here: it starts its own grace period. */
        if (another) hostgc_guard_connected(&g_guard, now);
        os_unfair_lock_unlock(&g_guard_lock);
        host_log("GameController disconnected: %s (last pulse %.1f s before)%s", c.vendorName.UTF8String ?: "?",
                 played ? (double)(now - played) * 1e-9 : -1.0, suspended ? "; haptics suspended" : "");
        if (suspended) host_log("[haptics] two drops within two minutes of pulses: haptics off for %.0f s",
                                (double)(until - now) * 1e-9);
        /* The engine belongs to the pad that just went away. Kept running, it
         * was still there when the pad came back. */
        haptic_release_on_disconnect();
    }];
    /* Paired pads connect by themselves. Discovery is only for finding a new
     * one, so run it only while none is here; the connect handler stops it. */
    if (!current_extended_controller()) [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{}];
    else {
        os_unfair_lock_lock(&g_guard_lock);
        hostgc_guard_connected(&g_guard, now_ns());
        atomic_store_explicit(&g_last_connect_ns, now_ns(), memory_order_relaxed);
        os_unfair_lock_unlock(&g_guard_lock);
    }
    host_log("GameController subsystem initialised (%lu controller(s) present)",
             (unsigned long)GCController.controllers.count);
    }
}

/* Haptics on the pad the host already reads.
 *
 * The engine is a translated 32-bit binary with no notion of force feedback,
 * so impacts are synthesised from the mixed audio. All this has to do is put
 * them on the hardware. The engine is kept and started once; Core Haptics
 * wants its setup off the audio path, so the first pulse builds it and later
 * ones reuse it. */
/* Only pulse callers own the cached engine/pad. Framework callbacks touch the
 * small state lock, never start/stop an engine or wait for a pulse to finish. */
static pthread_mutex_t g_haptic_play_lock = PTHREAD_MUTEX_INITIALIZER;
static CHHapticEngine *g_haptic_engine = nil;
static GCController   *g_haptic_pad = nil;
static char            g_haptic_state[96] = "not started";
static uint64_t        g_haptics_played = 0, g_haptic_generation = 0;
static bool            g_haptic_needs_start = true;
static os_unfair_lock  g_haptic_lock = OS_UNFAIR_LOCK_INIT;

static void haptic_note(const char *text) {
    os_unfair_lock_lock(&g_haptic_lock);
    snprintf(g_haptic_state, sizeof g_haptic_state, "%s", text);
    os_unfair_lock_unlock(&g_haptic_lock);
}

static void haptic_needs_restart(uint64_t generation, const char *reason) {
    os_unfair_lock_lock(&g_haptic_lock);
    if (generation == g_haptic_generation) {
        g_haptic_needs_start = true;
        snprintf(g_haptic_state, sizeof g_haptic_state, "%s", reason);
    }
    os_unfair_lock_unlock(&g_haptic_lock);
}

/* Called with play_lock held. Invalidate callbacks before asynchronous Stop;
 * a late callback from the old pad must not stop the replacement's pulses. */
static void haptic_discard_engine(void) {
    os_unfair_lock_lock(&g_haptic_lock);
    g_haptic_generation++;
    g_haptic_needs_start = true;
    os_unfair_lock_unlock(&g_haptic_lock);
    [g_haptic_engine stopWithCompletionHandler:nil];
    g_haptic_engine = nil;
    g_haptic_pad = nil;
}

/* Returns an engine ready to play, or nil with the reason recorded.
 * Restart only when a pulse is requested. A reset during suspension should not
 * itself reactivate controller hardware, and players are recreated per pulse. */
static CHHapticEngine *haptic_engine_for(GCController *pad) {
    if (g_haptic_pad != pad) haptic_discard_engine();
    if (!pad) { haptic_note("no controller"); return nil; }
    if (!g_haptic_engine) {
        GCDeviceHaptics *caps = pad.haptics;
        if (!caps) {
            char text[96];
            snprintf(text, sizeof text, "%s has no haptics", pad.vendorName.UTF8String ?: "pad");
            haptic_note(text); return nil;
        }
        CHHapticEngine *engine = [caps createEngineWithLocality:GCHapticsLocalityDefault];
        if (!engine) { atomic_fetch_add_explicit(&g_haptic_failures, 1, memory_order_relaxed); haptic_note("no haptic engine for this pad"); return nil; }
        g_haptic_pad = pad;
        g_haptic_engine = engine;
        /* Let an idle engine stop: a running one keeps a haptics stream open
         * to the pad between pulses. The stopped handler marks it for a
         * restart on the next pulse. */
        engine.autoShutdownEnabled = YES;
        os_unfair_lock_lock(&g_haptic_lock);
        uint64_t generation = ++g_haptic_generation;
        g_haptic_needs_start = true;
        os_unfair_lock_unlock(&g_haptic_lock);
        engine.resetHandler = ^{ atomic_fetch_add_explicit(&g_haptic_resets, 1, memory_order_relaxed); haptic_needs_restart(generation, "engine reset; awaiting pulse"); };
        engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
            atomic_fetch_add_explicit(&g_haptic_stops, 1, memory_order_relaxed);
            char text[96];
            snprintf(text, sizeof text, "engine stopped (%ld); awaiting pulse", (long)reason);
            haptic_needs_restart(generation, text);
        };
    }
    os_unfair_lock_lock(&g_haptic_lock);
    bool needs_start = g_haptic_needs_start;
    /* Clear before Start so a stop/reset arriving during Start is preserved. */
    g_haptic_needs_start = false;
    os_unfair_lock_unlock(&g_haptic_lock);
    if (needs_start) {
        NSError *error = nil;
        if (![g_haptic_engine startAndReturnError:&error]) {
            atomic_fetch_add_explicit(&g_haptic_failures, 1, memory_order_relaxed);
            char text[96];
            snprintf(text, sizeof text, "engine start failed: %s",
                     error.localizedDescription.UTF8String ?: "?");
            haptic_discard_engine();
            haptic_note(text); return nil;
        }
    }
    return g_haptic_engine;
}

static void haptic_play_locked(float intensity, float sharpness) {
    GCController *pad = current_extended_controller();
    CHHapticEngine *engine = haptic_engine_for(pad);
    if (!engine) return;
    CHHapticEventParameter *i =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                      value:intensity];
    CHHapticEventParameter *s =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                      value:sharpness];
    CHHapticEvent *event =
        [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient
                                      parameters:@[i, s] relativeTime:0];
    NSError *error = nil;
    CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
                                                            parameters:@[] error:&error];
    if (!pattern) { atomic_fetch_add_explicit(&g_haptic_failures, 1, memory_order_relaxed); haptic_note("pattern build failed"); return; }
    id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
    if (!player || ![player startAtTime:CHHapticTimeImmediate error:&error]) {
        atomic_fetch_add_explicit(&g_haptic_failures, 1, memory_order_relaxed);
        /* A disconnected or reset engine may remain unusable even if Start
         * succeeds. Drop it so the next pulse can acquire a fresh engine. */
        haptic_discard_engine();
        haptic_note("play failed; retrying next pulse");
        return;
    }
    atomic_store_explicit(&g_last_played_ns, now_ns(), memory_order_relaxed);
    os_unfair_lock_lock(&g_haptic_lock);
    g_haptics_played++;
    if (!g_haptic_needs_start)
        snprintf(g_haptic_state, sizeof g_haptic_state, "ready (%s)", pad.vendorName.UTF8String ?: "controller");
    os_unfair_lock_unlock(&g_haptic_lock);
}

void hostgc_play_haptic(float intensity, float sharpness) {
    if (!(intensity > 0.f)) return;
    if (intensity > 1.f) intensity = 1.f;
    if (!(sharpness >= 0.f)) sharpness = 0.f;
    if (sharpness > 1.f) sharpness = 1.f;
    hostgc_init();
    if (g_guard_enabled) {
        os_unfair_lock_lock(&g_guard_lock);
        int verdict = hostgc_guard_admit(&g_guard, now_ns());
        os_unfair_lock_unlock(&g_guard_lock);
        if (verdict == HOSTGC_GUARD_SUSPENDED) haptic_note("suspended after controller drops");
        if (verdict != HOSTGC_GUARD_PLAY) return;
    }
    dispatch_async(haptic_queue(), ^{
        @autoreleasepool {
            pthread_mutex_lock(&g_haptic_play_lock);
            haptic_play_locked(intensity, sharpness);
            pthread_mutex_unlock(&g_haptic_play_lock);
        }
    });
}

static void haptic_release_on_disconnect(void) {
    dispatch_async(haptic_queue(), ^{
        @autoreleasepool {
            pthread_mutex_lock(&g_haptic_play_lock);
            if (g_haptic_engine) { haptic_discard_engine(); haptic_note("released: controller disconnected"); }
            pthread_mutex_unlock(&g_haptic_play_lock);
        }
    });
}

void hostgc_link_stats(HostGCLinkStats *out) {
    if (!out) return;
    os_unfair_lock_lock(&g_guard_lock);
    uint64_t now = now_ns();
    *out = (HostGCLinkStats){
        .connects = g_guard.connects, .disconnects = g_guard.disconnects,
        .linked_disconnects = g_guard.linked_disconnects, .suspensions = g_guard.suspensions,
        .pulses_admitted = g_guard.admitted, .pulses_dropped_rate = g_guard.dropped_rate,
        .pulses_dropped_grace = g_guard.dropped_grace, .pulses_dropped_suspended = g_guard.dropped_suspended,
        .pulses_dropped_disconnected = g_guard.dropped_disconnected,
        .suspended_ms_left = g_guard.suspended_until_ns > now ? (uint32_t)((g_guard.suspended_until_ns - now) / 1000000ull) : 0,
        .guard_enabled = g_guard_enabled,
        .haptic_failures = atomic_load_explicit(&g_haptic_failures, memory_order_relaxed),
        .haptic_resets = atomic_load_explicit(&g_haptic_resets, memory_order_relaxed),
        .haptic_stops = atomic_load_explicit(&g_haptic_stops, memory_order_relaxed),
        .last_connect_ns = atomic_load_explicit(&g_last_connect_ns, memory_order_relaxed),
        .last_disconnect_ns = atomic_load_explicit(&g_last_disconnect_ns, memory_order_relaxed),
        .last_played_ns = atomic_load_explicit(&g_last_played_ns, memory_order_relaxed),
    };
    os_unfair_lock_unlock(&g_guard_lock);
}

const char *hostgc_haptic_state(void) {
    /* Returning the shared buffer exposed writes to Swift's String(cString:).
     * Keep the existing ABI, but hand each reader its own stable snapshot. */
    static _Thread_local char snapshot[sizeof g_haptic_state];
    os_unfair_lock_lock(&g_haptic_lock);
    memcpy(snapshot, g_haptic_state, sizeof snapshot);
    os_unfair_lock_unlock(&g_haptic_lock);
    return snapshot;
}
uint64_t hostgc_haptics_played(void) {
    os_unfair_lock_lock(&g_haptic_lock);
    uint64_t n = g_haptics_played;
    os_unfair_lock_unlock(&g_haptic_lock);
    return n;
}

/* Keep a hardware reading as the one to hand out. A reading that started
 * before the kept one (two threads capturing at once) does not replace it. */
static void remember_reading(HostGCSnapshot *reading, uint64_t started_ns) {
    os_unfair_lock_lock(&g_lock);
    reading->sequence = ++g_sequence;
    g_captures++;
    if (started_ns >= g_recent_ns) { g_recent = *reading; g_recent_ns = started_ns; }
    os_unfair_lock_unlock(&g_lock);
}

static bool capture_reading(HostGCSnapshot *out);

bool hostgc_poll(HostGCSnapshot *out) {
    if (!out) return false;
    /* Timed from before the capture, so a reading's age is never understated. */
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), window = reuse_window_ns();

    os_unfair_lock_lock(&g_lock);
    if (g_have_test) { *out = g_test; os_unfair_lock_unlock(&g_lock); return out->connected; }
    if (window && g_recent_ns && (now < g_recent_ns || now - g_recent_ns < window)) {
        *out = g_recent; g_reuses++;
        os_unfair_lock_unlock(&g_lock);
        return out->connected;
    }
    os_unfair_lock_unlock(&g_lock);

    /* The capture runs unlocked: the main thread's diagnostics may read the
     * pad at the same moment and must not wait on GameController here. */
    bool connected = capture_reading(out);
    remember_reading(out, now);
    return connected;
}

void hostgc_poll_stats(uint64_t *captures, uint64_t *reuses, uint32_t *window_us) {
    uint64_t window = reuse_window_ns();
    os_unfair_lock_lock(&g_lock);
    if (captures) *captures = g_captures;
    if (reuses) *reuses = g_reuses;
    os_unfair_lock_unlock(&g_lock);
    if (window_us) *window_us = (uint32_t)(window / 1000u);
}

static bool capture_reading(HostGCSnapshot *out) {
    @autoreleasepool {
    memset(out, 0, sizeof *out);
    GCController *controller = current_extended_controller();
    /* capture returns an immutable, internally consistent reading.  Reading
     * each live element directly could otherwise mix two hardware reports. */
    GCController *captured = controller ? [controller capture] : nil;
    GCExtendedGamepad *g = captured.extendedGamepad;
    if (!g) return false;

    out->connected = true;
    out->lx = g.leftThumbstick.xAxis.value;
    out->ly = g.leftThumbstick.yAxis.value;
    out->rx = g.rightThumbstick.xAxis.value;
    out->ry = g.rightThumbstick.yAxis.value;
    out->lt = g.leftTrigger.value;
    out->rt = g.rightTrigger.value;
    out->dpad_up    = g.dpad.up.pressed;
    out->dpad_down  = g.dpad.down.pressed;
    out->dpad_left  = g.dpad.left.pressed;
    out->dpad_right = g.dpad.right.pressed;

    out->buttons[HOSTGC_BTN_A]         = g.buttonA.pressed;
    out->buttons[HOSTGC_BTN_B]         = g.buttonB.pressed;
    out->buttons[HOSTGC_BTN_X]         = g.buttonX.pressed;
    out->buttons[HOSTGC_BTN_Y]         = g.buttonY.pressed;
    out->buttons[HOSTGC_BTN_LSHOULDER] = g.leftShoulder.pressed;
    out->buttons[HOSTGC_BTN_RSHOULDER] = g.rightShoulder.pressed;
    out->buttons[HOSTGC_BTN_LTRIGGER]  = g.leftTrigger.pressed;
    out->buttons[HOSTGC_BTN_RTRIGGER]  = g.rightTrigger.pressed;
    out->buttons[HOSTGC_BTN_LTHUMB]    = g.leftThumbstickButton.pressed;
    out->buttons[HOSTGC_BTN_RTHUMB]    = g.rightThumbstickButton.pressed;
    if (@available(macOS 10.15, iOS 13.0, *)) {
        out->buttons[HOSTGC_BTN_MENU]    = g.buttonMenu.pressed;
        out->buttons[HOSTGC_BTN_OPTIONS] = g.buttonOptions.pressed;
    }
    if (@available(macOS 11.0, iOS 14.0, *))
        out->buttons[HOSTGC_BTN_HOME]    = g.buttonHome.pressed;
    out->buttons[HOSTGC_BTN_DPAD_UP]    = out->dpad_up;
    out->buttons[HOSTGC_BTN_DPAD_DOWN]  = out->dpad_down;
    out->buttons[HOSTGC_BTN_DPAD_LEFT]  = out->dpad_left;
    out->buttons[HOSTGC_BTN_DPAD_RIGHT] = out->dpad_right;
    return true;
    }
}

bool hostgc_connected(void) {
    @autoreleasepool {
    os_unfair_lock_lock(&g_lock);
    if (g_have_test) { bool c = g_test.connected; os_unfair_lock_unlock(&g_lock); return c; }
    os_unfair_lock_unlock(&g_lock);
    return current_extended_controller() != nil;
    }
}

void hostgc_inject_test_snapshot(const HostGCSnapshot *snap) {
    if (!snap) { hostgc_clear_test_snapshot(); return; }
    os_unfair_lock_lock(&g_lock);
    g_have_test = true;
    g_test = *snap;
    os_unfair_lock_unlock(&g_lock);
}

void hostgc_clear_test_snapshot(void) {
    os_unfair_lock_lock(&g_lock);
    g_have_test = false;
    os_unfair_lock_unlock(&g_lock);
}
