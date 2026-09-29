/* Exercise the production event mailbox, independently of DSP timing.
 * clang -O2 -pthread -I native/EngineHost \
 *   native/EngineHost/tests/test_haptics_mailbox.c -lm -o /tmp/haptics-mailbox
 */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../haptics.c"

float halo_settings_haptics_strength(void) { return 1.f; }
static _Atomic int finished;
static void *produce(void *unused) {
    (void)unused;
    for (int i = 0; i < 500000; ++i) {
        publish_event(.2f, .8f);
        if (!(i & 31)) sched_yield();
        publish_event(.8f, .2f);
    }
    atomic_store_explicit(&finished, 1, memory_order_release);
    return NULL;
}
int main(void) {
    float intensity = -1, sharpness = -1;
    assert(!halo_haptics_take(&intensity, &sharpness));
    publish_event(0.f, 1.f);
    assert(halo_haptics_take(&intensity, &sharpness));
    assert(intensity == 0.f && sharpness == 1.f);
    publish_event(.2f, .8f);
    publish_event(.8f, .2f); /* Deliberate latest-event-wins semantics. */
    assert(halo_haptics_take(&intensity, &sharpness));
    assert(intensity == .8f && sharpness == .2f);
    assert(!halo_haptics_take(NULL, NULL));
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, produce, NULL));
    unsigned consumed = 0;
    do {
        if (halo_haptics_take(&intensity, &sharpness)) {
            assert((intensity == .2f && sharpness == .8f) ||
                   (intensity == .8f && sharpness == .2f));
            ++consumed;
        }
    } while (!atomic_load_explicit(&finished, memory_order_acquire));
    assert(!pthread_join(producer, NULL));
    if (halo_haptics_take(&intensity, &sharpness)) {
        assert((intensity == .2f && sharpness == .8f) ||
               (intensity == .8f && sharpness == .2f));
        ++consumed;
    }
    assert(consumed);
    printf("PASS coherent haptics mailbox: %u concurrent events consumed\n", consumed);
}
