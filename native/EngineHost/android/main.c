/* main.c - headless entry for the Android (arm64-v8a) build.
 *
 * The macOS main.c drives the game on a worker pthread and pumps the Cocoa /
 * CoreFoundation run loop on the main thread so the Metal window stays alive.
 * The headless build has no window, so there is nothing to pump: run the
 * engine on a large-stack thread and block until host_run() returns.
 *
 * host_run() runs the full CPU translation loop (import shims, DirectSound
 * mixer, D3D9->stub renderer) and returns once host_frame_limit frames have
 * been presented, or longjmp-exits via host_exit(). Frame limit is the only
 * exit path in a headless run: the metalwin stub never reports a close.
 */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Defined here (the macOS main.c that owns it is not part of this build).
 * -window -novideo matches the working macOS invocation; the D3D9/DDraw
 * shims satisfy the device query so the game never opens a real window. */
const char *host_command_line = "\"C:\\Halo\\halo.exe\" -window -novideo";

int host_run(const char *exe, const char *root);
extern int host_frame_limit;

static char exe_s[1024], root_s[1024];
static int rc;

static void *run_thread(void *arg) {
    (void)arg;
    rc = host_run(exe_s, root_s);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: halo-headless <game dir> [--quiet] [--frames N]\n");
        return 2;
    }
    const char *root = argv[1];
    static char cmd[1024];
    const char *extra = getenv("HALO_CMDLINE_EXTRA");
    snprintf(cmd, sizeof cmd, "%s%s%s",
             host_command_line, extra && extra[0] ? " " : "", extra ? extra : "");
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--quiet")) host_trace_imports = 0;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) host_frame_limit = atoi(argv[++i]);
    }
    host_command_line = cmd;

    snprintf(exe_s, sizeof exe_s, "%s/halo.exe", root);
    snprintf(root_s, sizeof root_s, "%s", root);

    /* The engine interpreter recurses deeply (guest -> host -> guest); it
     * needs the same 1.5 GB stack the macOS runner gives its worker. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)1536 * 1024 * 1024);
    pthread_t th;
    if (pthread_create(&th, &attr, run_thread, NULL) != 0) {
        /* Fall back to running on the main thread (small stack); likely to
         * overflow on the engine, so report it rather than crash silently. */
        fprintf(stderr, "[host] pthread_create failed; running on main thread (stack may be too small)\n");
        return host_run(exe_s, root_s);
    }
    pthread_join(th, NULL);
    fprintf(stderr, "[host] headless run finished rc=%d\n", rc);
    return rc;
}
