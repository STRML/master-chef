#include "host.h"
#include <stdlib.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>
#include "metalwin.h"
const char *host_command_line = "\"C:\\Halo\\halo.exe\" -window -novideo";
int host_run(const char *exe, const char *root);
int host_proc_has_shim(uint32_t magic);
extern int host_frame_limit;
static char exe_s[1024], root_s[1024]; static int rc; static volatile int game_done;
static void *run_thread(void *arg) { (void)arg; rc = host_run(exe_s, root_s); game_done = 1; CFRunLoopStop(CFRunLoopGetMain()); return NULL; }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: halo-host <game dir> [--quiet] [--frames N]\n"); return 2; }
    const char *root = argv[1];
    static char cmd[1024]; const char *extra = getenv("HALO_CMDLINE_EXTRA");
    snprintf(cmd, sizeof cmd, "%s%s%s", host_command_line, extra && extra[0] ? " " : "", extra ? extra : "");
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--quiet")) host_trace_imports = 0;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) host_frame_limit = atoi(argv[++i]);
    }
    host_command_line = cmd;
    snprintf(exe_s, sizeof exe_s, "%s/halo.exe", root); snprintf(root_s, sizeof root_s, "%s", root);
    char exe[1024]; snprintf(exe, sizeof exe, "%s/halo.exe", root);
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, (size_t)1536 * 1024 * 1024);
    pthread_t th;
    if (pthread_create(&th, &attr, run_thread, NULL) == 0) {
        /* Main thread services AppKit / the main dispatch queue so the Metal window
           (created on the main thread from the game worker) works, until the game ends. */
        while (!game_done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        pthread_join(th, NULL);
        extern int host_window_shown;
        if (host_window_shown) {
            /* Keep the last presented frame on screen so the result is visible/screenshottable. */
            fprintf(stderr, "[host] game ended; window stays open ~20s (or close it)\n");
            for (int i = 0; i < 400 && !metalwin_should_close(); i++) { metalwin_poll(); usleep(50000); }
        }
        return rc;
    }
    return host_run(exe, root);
}
