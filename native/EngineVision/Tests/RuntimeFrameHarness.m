#import <Foundation/Foundation.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "EngineVisionBridge.h"

int host_trace_imports;
int host_quit_requested;
void hostgc_init(void) {}
int host_run(const char *exe, const char *root) {
    assert(strstr(exe, "/halo.exe") != NULL);
    assert(root[0] == '/');
    return 0;
}

int main(void) {
    assert(enginevision_start(NULL) == EINVAL);
    int start = enginevision_start("/tmp/owned-halo");
    if (start == 0) {
        for (int n = 0; n < 200 && enginevision_runtime_state() < ENGINEVISION_STOPPED; n++) usleep(1000);
        assert(enginevision_runtime_state() == ENGINEVISION_STOPPED);
        assert(enginevision_exit_code() == 0);
    } else {
        char status[512];
        enginevision_copy_status(status, sizeof status);
        assert(enginevision_runtime_state() == ENGINEVISION_FAILED);
        assert(strstr(status, "No unsafe fallback was started") != NULL);
    }
    assert(enginevision_start("/tmp/owned-halo") == EALREADY);

    const uint8_t first[16] = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16 };
    uint8_t copied[16] = {0};
    EngineVisionFrameInfo info = {0};
    assert(metalwin_init(2, 2, "test") == 0);
    assert(!enginevision_frame_info(&info));
    metalwin_present(first, 2, 2);
    assert(enginevision_frame_info(&info));
    assert(info.sequence == 1 && info.width == 2 && info.height == 2 && info.byte_count == 16);
    assert(!enginevision_copy_latest_frame(copied, 15, &info));
    assert(enginevision_copy_latest_frame(copied, sizeof copied, &info));
    assert(memcmp(first, copied, sizeof first) == 0);
    EngineFrameDigest digest;enginevision_frame_digest(&digest);
    assert(digest.sequence==1 && digest.sample_count==4 && digest.nonblack_count==4 && digest.differing_count==3);
    const uint8_t black[16]={0};metalwin_present(black,2,2);enginevision_frame_digest(&digest);
    assert(digest.sequence==2 && digest.nonblack_count==0 && digest.differing_count==0);
    metalwin_present(first, 9000, 2);
    assert(enginevision_frame_info(&info) && info.sequence == 2);
    puts("RUNTIME_FRAME_HARNESS_OK frame-copy bounds explicit-stack-result singleton");
    return 0;
}
