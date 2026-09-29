/* The experimental switch is strict opt-in and immutable across bearings.
 * Each child starts before initialization; concurrent first reads exercise
 * the same getter used by the renderer and device diagnostics. */
#include "halo_settings.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic int readers_ready;
static _Atomic int readers_go;
static int expected_enabled;

static void *read_setting(void *unused) {
    (void)unused;
    atomic_fetch_add(&readers_ready, 1);
    while (!atomic_load(&readers_go)) {}
    for (int i = 0; i < 10000; ++i)
        assert(halo_settings_radial_fog() == expected_enabled);
    return NULL;
}

static void check_value(const char *value, int expected) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        if (value) assert(setenv("HALO_RADIAL_FOG", value, 1) == 0);
        else assert(unsetenv("HALO_RADIAL_FOG") == 0);
        expected_enabled = expected;
        pthread_t readers[16];
        for (int i = 0; i < 16; ++i)
            assert(pthread_create(&readers[i], NULL, read_setting, NULL) == 0);
        while (atomic_load(&readers_ready) != 16) {}
        atomic_store(&readers_go, 1);
        for (int i = 0; i < 16; ++i) assert(pthread_join(readers[i], NULL) == 0);
        assert(setenv("HALO_RADIAL_FOG", expected ? "0" : "1", 1) == 0);
        assert(halo_settings_radial_fog() == expected);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    const char *disabled[] = {NULL, "", "0", "true", "false", "01", "10", "1 ", " 1", "+1", "1\n"};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i)
        check_value(disabled[i], 0);
    check_value("1", 1);
    puts("PASS radial fog settings: 12 environments, 16 concurrent readers, 1920000 reads, cached after environment changes");
    return 0;
}
