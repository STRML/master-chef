/*
 * android_main.c - NativeActivity entry point for libhaloquest.so.
 *
 * The APK manifest declares android.app.NativeActivity with
 * meta-data lib_name=haloquest, so the system dlopens libhaloquest.so
 * and calls android_main after loading the native_app_glue relay
 * (compiled from the NDK's sources/android/native_app_glue; the r27
 * glue runs android_main on the glue thread and marks
 * ALooper_pollAll unavailable, so the pump below uses
 * ALooper_pollOnce).
 *
 * Startup order:
 *   1. dlopen the OpenXR runtime loader. NDK shared links tolerate the
 *      undefined xr* symbols (no --no-undefined), so the loader is
 *      brought into the global namespace at runtime: the loader staged
 *      into the APK's jniLibs (libopenxr.so, extractNativeLibs=true
 *      makes it a real file), then the Meta/QVR system
 *      libopenxr_loader.so.
 *   2. Pump the activity command queue until APP_CMD_INIT_WINDOW; the
 *      XR_KHR_android_create_instance path needs the JavaVM + activity
 *      object the glue carries.
 *   3. Bring up the XR shell session (xr_shell.c): instance, Vulkan
 *      enable2 device, session, per-eye swapchains, head space.
 *   4. Attach the OpenXR controller action set (xr_input_attach) once
 *      the session exists — before the first xrSyncActions inside the
 *      per-frame input poll.
 *   5. Start the engine loop: host_run() on a large-stack thread (the
 *      x86 interpreter recurses guest->host->guest; headless main.c
 *      uses 1.5 GB and so do we). The D3D9 shim presents each frame
 *      through metalwin_present below.
 *   6. Pump the XR frame loop here (the render thread): each tick
 *      polls activity commands, feeds the newest engine frame into
 *      the per-eye swapchains via xr_shell_set_engine_frame, polls the
 *      controller snapshot, and runs one xr_shell_frame (which
 *      re-checks the view configuration and recreates a per-eye
 *      swapchain when the recommended size changes).
 *   7. Teardown on DESTROY_WINDOW / DESTROY / XR exit request: signal
 *      the engine loop to quit (metalwin_should_close +
 *      host_quit_requested), destroy the shell, and
 *      ANativeActivity_finish.
 */
#include <android_native_app_glue.h>
#include <android/native_activity.h>
#include <android/input.h>
#include <android/log.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gamecontroller.h"
#include "host.h"
#include "metalrenderer.h"
#include "metalwin.h"
#include "xr.h"

/* Android-side entry points of openxr_input.c (the pure translation
 * lives in openxr_input.h; these need a live instance+session). */
int xr_input_attach(XrInstance instance, XrSession session);
int xr_input_poll_snapshot(HostGCSnapshot *out);

/* Engine host globals (host.c / shims_misc.c / d3d9.c). */
extern int host_frame_limit;
extern int host_quit_requested;
extern const char *host_command_line;
int host_run(const char *exe, const char *root);

#define AM_LOG(...) ((void)__android_log_print(ANDROID_LOG_INFO, "haloquest", __VA_ARGS__))

/* ------------------------------------------------------------------ */
/* OpenXR runtime loader bring-up                                      */
/* ------------------------------------------------------------------ */

static void *g_loader;

static int xr_open_runtime_loader(void) {
    /* Prefer the APK-staged loader (a real file with
     * extractNativeLibs=true); then the Meta/QVR system loader. */
    static const char *const names[] = {
        "libopenxr.so",        /* staged into jniLibs by setup --stage quest */
        "libopenxr_loader.so", /* on-device QVR/Meta runtime                  */
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        g_loader = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_loader) return 0;
    }
    AM_LOG("dlopen OpenXR loader failed: %s", dlerror());
    return -1;
}

/* ------------------------------------------------------------------ */
/* engine <-> activity shared state                                    */
/* ------------------------------------------------------------------ */

static xr_shell *g_shell;                 /* XR thread; read by engine */
static _Atomic int g_engine_done;         /* host_run returned          */
static _Atomic int g_engine_shutdown;     /* stop the activity pump     */
static _Atomic int g_window_ready;        /* APP_CMD_INIT_WINDOW seen   */
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *g_frame_buf;        /* newest engine present      */
static size_t g_frame_cap;
static int g_frame_w, g_frame_h;
static uint64_t g_frame_seq;              /* locked under g_frame_lock  */
static _Atomic uint64_t g_consumed_seq;   /* XR-thread consume serial   */

/* metalwin_present: the CPU-BGRA present path of d3d9.c. Replaces the
 * headless no-op sink in the APK link (jniLibs.mk drops
 * metalwin_stub.o): stash the newest frame for the XR pump. Called on
 * the engine thread; copied under the lock so the XR thread sees a
 * complete frame. */
void metalwin_present(const void *bgra, int width, int height) {
    size_t need = (size_t)width * (size_t)height * 4;
    if (!bgra || need == 0) return;
    pthread_mutex_lock(&g_frame_lock);
    if (need > g_frame_cap) {
        unsigned char *grown = (unsigned char *)realloc(g_frame_buf, need);
        if (!grown) { pthread_mutex_unlock(&g_frame_lock); return; }
        g_frame_buf = grown;
        g_frame_cap = need;
    }
    memcpy(g_frame_buf, bgra, need);
    g_frame_w = width;
    g_frame_h = height;
    g_frame_seq++;
    pthread_mutex_unlock(&g_frame_lock);
}
int metalwin_should_close(void) {
    if (atomic_load(&g_engine_shutdown)) return 1;
    if (g_shell && xr_shell_should_exit(g_shell)) return 1;
    return 0;
}

/* metalwin_init: the window-side stub the APK link swaps in for
 * metalwin_stub.o's (which jniLibs.mk filters out; d3d9.c:487 calls it once
 * before the first present). The XR pump owns the real surfaces, so success
 * is unconditional; the frame path is metalwin_present above. */
int metalwin_init(int width, int height, const char *title) {
    (void)width; (void)height; (void)title;
    return 0;
}

/* ------------------------------------------------------------------ */
/* activity command pump                                               */
/* ------------------------------------------------------------------ */

static int32_t on_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) return 0;
    if (AKeyEvent_getKeyCode(event) != AKEYCODE_BACK) return 0;
    if (AKeyEvent_getAction(event) != AKEY_EVENT_ACTION_UP) return 1;
    if (g_shell) xr_shell_send_lc_event(g_shell, XR_LC_QUIT);
    return 1;
}

static void on_app_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        atomic_store(&g_window_ready, 1);
        break;
    case APP_CMD_RESUME:
        if (g_shell) xr_shell_send_lc_event(g_shell, XR_LC_FOCUS_GAINED);
        break;
    case APP_CMD_PAUSE:
        if (g_shell) xr_shell_send_lc_event(g_shell, XR_LC_FOCUS_LOST);
        break;
    case APP_CMD_DESTROY:
    case APP_CMD_TERM_WINDOW:
        atomic_store(&g_engine_shutdown, 1);
        host_quit_requested = 1;
        if (g_shell) xr_shell_request_exit(g_shell);
        break;
    default:
        break;
    }
}

/* Drain pending activity commands without blocking; returns 1 when the
 * activity is being destroyed. One event per pollOnce call: the ident
 * is the registered fd id (LOOPER_ID_MAIN / LOOPER_ID_INPUT) and the
 * data pointer is the poll source. */
static int pump_activity_commands(struct android_app *app) {
    int ident;
    int events;
    void *data;
    while ((ident = ALooper_pollOnce(0, NULL, &events, &data)) >= 0) {
        struct android_poll_source *source = (struct android_poll_source *)data;
        if (source && source->process) source->process(app, source);
        if (app->destroyRequested) return 1;
    }
    return 0;
}

/* Feed the newest stashed engine frame into each eye swapchain, poll
 * the controller snapshot, then run one XR frame. Returns
 * xr_shell_frame's code (0 ok, 1 skip/exit, <0 fatal). */
static int pump_xr_frame(void) {
    if (atomic_load(&g_engine_done)) return 1;
    uint64_t seq;
    pthread_mutex_lock(&g_frame_lock);
    seq = g_frame_seq;
    if (seq && seq != atomic_load(&g_consumed_seq)) {
        size_t need = (size_t)g_frame_w * (size_t)g_frame_h * 4;
        if (g_frame_buf && need <= g_frame_cap)
            xr_shell_set_engine_frame(g_shell, g_frame_buf, g_frame_w, g_frame_h);
        atomic_store(&g_consumed_seq, seq);
    }
    pthread_mutex_unlock(&g_frame_lock);
    HostGCSnapshot snap;
    xr_input_poll_snapshot(&snap);
    return xr_shell_frame(g_shell);
}

/* ------------------------------------------------------------------ */
/* engine loop thread                                                  */
/* ------------------------------------------------------------------ */

static char g_exe[1024], g_root[1024];

static void *engine_thread(void *arg) {
    (void)arg;
    int rc = host_run(g_exe, g_root);
    AM_LOG("engine loop finished rc=%d", rc);
    atomic_store(&g_engine_done, 1);
    atomic_store(&g_engine_shutdown, 1);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

void android_main(struct android_app *app) {
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;

    /* Game root: the EXTERNAL files dir + /game — the exact path the
     * Quest setup stage adb-pushes to (/sdcard/Android/data/<pkg>/files).
     * internalDataPath is the /data/user/0 dir, which adb cannot write
     * without root; it is only a fallback for devices that report no
     * external storage. HALO_ROOT overrides for device-side testing. */
    const char *root = getenv("HALO_ROOT");
    if (!root || !root[0]) {
        static char rootbuf[1024];
        const char *data = app->activity->externalDataPath;
        if (!data || !data[0]) data = app->activity->internalDataPath;
        snprintf(rootbuf, sizeof rootbuf, "%s/game", data);
        root = rootbuf;
    }
    snprintf(g_root, sizeof g_root, "%s", root);
    snprintf(g_exe, sizeof g_exe, "%s/halo.exe", g_root);
    /* Log the resolved root so the on-device validation can confirm the
     * adb-pushed payload location via `adb logcat -s haloquest`. */
    AM_LOG("game root: %s", g_root);
    const char *extra = getenv("HALO_CMDLINE_EXTRA");
    static char cmd[2048];
    snprintf(cmd, sizeof cmd, "%s%s%s", host_command_line,
             extra && extra[0] ? " " : "", extra ? extra : "");
    host_command_line = cmd;

    /* 1. XR runtime loader into the global namespace. */
    if (xr_open_runtime_loader() != 0) {
        AM_LOG("no OpenXR runtime loader; finishing activity");
        ANativeActivity_finish(app->activity);
        return;
    }

    /* 2. Wait for the activity window (JNI handles for
     *    XR_KHR_android_create_instance). The glue registered the
     *    command pipe at LOOPER_ID_MAIN with a poll source; pollOnce
     *    hands it back, and process_cmd runs on_app_cmd. */
    while (!atomic_load(&g_window_ready) && !atomic_load(&g_engine_shutdown) &&
           !app->destroyRequested) {
        int ident;
        int events;
        void *data;
        while ((ident = ALooper_pollOnce(1000, NULL, &events, &data)) >= 0) {
            struct android_poll_source *source = (struct android_poll_source *)data;
            if (source && source->process) source->process(app, source);
            if (atomic_load(&g_window_ready)) break;
        }
        if (ident == ALOOPER_POLL_ERROR) break;
    }
    if (!atomic_load(&g_window_ready) || app->destroyRequested) goto finish;

    /* 3. XR shell: instance + Vulkan enable2 + session + per-eye
     *    swapchains + head space, on this (render) thread. */
    xr_shell_config cfg = {0};
    cfg.app_name = "halo";
    cfg.engine_root = g_root;
    cfg.application_vm = app->activity->vm;
    cfg.application_activity = app->activity->clazz;
    cfg.max_frames = 0;   /* run until the FSM says exit */
    cfg.show_quads = 1;
    g_shell = xr_shell_create(&cfg);
    if (!g_shell) {
        AM_LOG("xr_shell_create failed: %s", xr_shell_last_error());
        goto finish;
    }

    /* 4. Attach the controller action set to the session (before the
     *    first xrSyncActions inside the per-frame poll). */
    if (xr_input_attach(xr_shell_instance(g_shell), xr_shell_session(g_shell)) != 0)
        AM_LOG("xr_input_attach failed (controllers unavailable)");

    /* 5. Engine loop on a large-stack thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)1536 * 1024 * 1024);
    pthread_t eng;
    int pcr = pthread_create(&eng, &attr, engine_thread, NULL);
    pthread_attr_destroy(&attr);
    if (pcr != 0) {
        AM_LOG("pthread_create(engine) failed: %s; running on this thread", strerror(pcr));
        engine_thread(NULL);
    }

    /* 6. XR frame pump on this thread until the shell exits (back
     *    button, XR session exit, or the engine finishing). */
    while (!atomic_load(&g_engine_shutdown) && !xr_shell_should_exit(g_shell)) {
        int f = pump_xr_frame();
        if (f < 0) { AM_LOG("xr frame failed: %s", xr_shell_last_error()); break; }
        if (pump_activity_commands(app)) break;
    }

    /* 7. Teardown. */
    atomic_store(&g_engine_shutdown, 1);
    host_quit_requested = 1;
    if (!atomic_load(&g_engine_done)) pthread_join(eng, NULL);

finish:
    xr_shell_destroy(g_shell);
    g_shell = NULL;
    pthread_mutex_destroy(&g_frame_lock);
    free(g_frame_buf);
    g_frame_buf = NULL;
    ANativeActivity_finish(app->activity);
}
