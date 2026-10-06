/* engine_compat_android.h - empty shadow for the host ASan test of
 * android/aaudio_output.c (see test_aaudio_dispose_race.c). The sink itself
 * uses no compat symbols; the NDK build's copy has macOS-hostile static
 * inline collisions, so this directory is placed first on the include path.
 * Do not ship. */
#ifndef HALO_TEST_ENGINE_COMPAT_ANDROID_SHIM_H
#define HALO_TEST_ENGINE_COMPAT_ANDROID_SHIM_H
#endif
