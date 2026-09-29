// Diagnostic telemetry must preserve the distinction between a unit's origin
// and animated bounding center (the latter is used by original trigger tests).
#define main unused_menu_probe_main
#include "MenuInputProbe.m"
#undef main
#include <assert.h>

uint8_t *engine_flat_base;
static void put32(uint32_t address,uint32_t value) { memcpy(engine_flat_base+address,&value,4); }

int main(void) {
    @autoreleasepool {
        size_t bytes=0x01000000;
        engine_flat_base=calloc(1,bytes);assert(engine_flat_base);
        atomic_store(&probe_live_control,true);
        put32(0x006F1D6C,0x30000);put32(0x3000C,420);
        put32(0x0087A478,0x30100);put32(0x30104,0xCAFE0000);
        put32(0x0087A480,0x31000);put32(0x31034,0x32000);
        put32(0x32034,0xBEEF0002);
        put32(0x008603B0,0x34000);put32(0x34034,0x35000);
        put32(0x35018,0xBEEF);put32(0x35020,0x36000);
        float origin[]={1,2,3},center[]={7,8,9},observer[]={10,11,12},forward[]={0,1,0};
        memcpy(engine_flat_base+0x3605C,origin,12);memcpy(engine_flat_base+0x360A0,center,12);
        memcpy(engine_flat_base+0x006AC6D0,observer,12);memcpy(engine_flat_base+0x006AC6F0,forward,12);
        void *before=malloc(bytes);assert(before);memcpy(before,engine_flat_base,bytes);
        capture_guest_pose(123);
        assert(!memcmp(before,engine_flat_base,bytes)); // Capture is read-only.
        NSDictionary *pose=live_guest_pose();
        assert([pose[@"frame_sequence"] unsignedLongLongValue]==123);
        assert([pose[@"tick"] unsignedIntValue]==420);
        assert([pose[@"unit_datum"] unsignedIntValue]==0xBEEF0002);
        assert(([pose[@"observer"] isEqual:@[@10,@11,@12]]));
        assert(([pose[@"forward"] isEqual:@[@0,@1,@0]]));
        assert(([pose[@"unit_origin"] isEqual:@[@1,@2,@3]]));
        assert(([pose[@"unit_bounding_center"] isEqual:@[@7,@8,@9]]));
        assert([NSJSONSerialization dataWithJSONObject:pose options:0 error:NULL]);
        // Stale object datums cannot expose another object's position.
        put32(0x35018,0xABCD);capture_guest_pose(124);pose=live_guest_pose();
        assert(!pose[@"unit_origin"] && !pose[@"unit_bounding_center"]);
        engine_flat_base[0x00718FC9]=1;capture_guest_pose(125);pose=live_guest_pose();
        assert([pose[@"ui_shell"] boolValue] && !pose[@"unit_origin"]);
        assert(!guest_pose_range(0,4) && !guest_pose_range(UINT32_MAX,2));
        assert(guest_pose_range(UINT32_MAX,1));
        float invalid[]={NAN,0,0};assert(!pose_vector(invalid));
        free(before);free(engine_flat_base);engine_flat_base=NULL;
        puts("PASS: pose snapshots retain frame/tick provenance, distinguish origin/center, reject stale datums, serialize finite vectors, and never write guest memory.");
    }
    return 0;
}
