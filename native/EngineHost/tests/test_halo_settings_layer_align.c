/* Layer alignment's setting: off unless HALO_LAYER_ALIGN=1, and the live
 * toggle turns it on and off. It stays off by default until a headset A/B
 * supports it, so a launch with no environment must read 0. The settings are
 * read from the environment once per process, so each environment value is
 * checked in its own child. */
#include "../halo_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* The setting as a fresh process with this HALO_LAYER_ALIGN (NULL: unset)
 * sees it. */
static int seen_with(const char *value) {
    fflush(stdout);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        if (value) setenv("HALO_LAYER_ALIGN", value, 1); else unsetenv("HALO_LAYER_ALIGN");
        HaloSettings settings;
        halo_settings_get(&settings);
        int seen = halo_settings_layer_align();
        _exit(seen == settings.layer_align && (seen == 0 || seen == 1) ? seen : 7);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return WEXITSTATUS(status);
}

int main(void) {
    assert(seen_with(NULL) == 0);
    assert(seen_with("") == 0);
    assert(seen_with("0") == 0);
    assert(seen_with("yes") == 0);
    assert(seen_with("1") == 1);

    /* The settings toggle, live: on, then off again, nothing else moved. */
    unsetenv("HALO_LAYER_ALIGN");
    HaloSettings before, after;
    halo_settings_get(&before);
    assert(before.layer_align == 0 && halo_settings_layer_align() == 0);
    HaloSettings on = before;
    on.layer_align = 1;
    halo_settings_set(&on);
    assert(halo_settings_layer_align() == 1);
    halo_settings_get(&after);
    assert(after.layer_align == 1 && after.gaze_pointer == before.gaze_pointer && after.spatial_shell == before.spatial_shell);
    on.layer_align = 0;
    halo_settings_set(&on);
    assert(halo_settings_layer_align() == 0);
    puts("PASS layer alignment setting: off by default, HALO_LAYER_ALIGN=1 or the toggle turns it on");
    return 0;
}
