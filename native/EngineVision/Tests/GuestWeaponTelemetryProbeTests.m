// CPU-only diagnostic fixture. No engine, renderer, game input or GPU started.
#define main unused_menu_probe_main
#include "MenuInputProbe.m"
#undef main
#include <assert.h>
uint8_t *engine_flat_base;
bool hostgc_poll(HostGCSnapshot *p) { *p=(HostGCSnapshot){.connected=true,.rt=1,.sequence=77};p->buttons[HOSTGC_BTN_RTRIGGER]=true;return true; }
void host_dinput_mouse_buttons_state(uint8_t p[8]) { memset(p,0,8);p[0]=0x80; }
static void put16(uint32_t p,uint16_t v) { memcpy(engine_flat_base+p,&v,2); }
static void put32(uint32_t p,uint32_t v) { memcpy(engine_flat_base+p,&v,4); }
static void putfloat(uint32_t p,float v) { memcpy(engine_flat_base+p,&v,4); }
int main(void) { @autoreleasepool {
    const size_t size=0x900000;
    engine_flat_base=calloc(1,size);assert(engine_flat_base);
    const uint32_t pg=0x10000,pa=0x11000,pr=0x12000,ot=0x13000,entries=0x14000,
        unit=0x15000,weapon=0x16000,controls=0x17000,time=0x18000;
    const uint32_t playerDatum=0x12340000,unitDatum=0x23450000,weaponDatum=0x34560001;
    put32(0x87a478,pg);put32(0x87a480,pa);put32(pg+4,playerDatum);
    put16(pa+0x20,16);put16(pa+0x22,0x200);put32(pa+0x34,pr);put16(pr,playerDatum>>16);put32(pr+0x34,unitDatum);
    put32(0x8603b0,ot);put16(ot+0x20,2);put16(ot+0x22,12);put32(ot+0x34,entries);
    put16(entries,unitDatum>>16);put16(entries+6,0x550);put32(entries+8,unit);put16(unit+0xb4,0);
    put16(entries+12,weaponDatum>>16);put16(entries+18,0x340);put32(entries+20,weapon);put16(weapon+0xb4,2);
    put16(unit+0x9c,37);put16(weapon+0x9c,0xffff); // Spatial cluster is not object kind.
    put16(unit+0x2f2,0);put16(unit+0x2f4,1);put32(unit+0x2f8,weaponDatum);put32(unit+0x208,0x800);
    put32(weapon,0xe9ca0047);put32(weapon+0x1f4,2);put32(weapon+0x22c,4);putfloat(weapon+0x234,.25);putfloat(weapon+0x240,.5);
    engine_flat_base[weapon+0x260]=0xff;engine_flat_base[weapon+0x261]=3;put16(weapon+0x262,7);put32(weapon+0x264,9);putfloat(weapon+0x270,.75);
    put16(weapon+0x2b0,0);put16(weapon+0x2b2,5);put16(weapon+0x2b4,10);put16(weapon+0x2b6,48);put16(weapon+0x2b8,12);
    put32(0x6b145c,controls);put32(controls,0x10);put32(controls+4,0x20);put32(controls+8,0x40);put32(controls+0x10,unitDatum);
    put32(0x6f1d6c,time);put32(time+12,12345);
    atomic_store(&probe_live_control,true);
    uint8_t *before=malloc(size);assert(before);memcpy(before,engine_flat_base,size);
    capture_guest_pose(1200);NSDictionary *d=live_guest_pose();assert([d[@"unit_valid"] boolValue]&&[d[@"weapon_valid"] boolValue]);
    assert([d[@"input"][@"actions_valid"] boolValue]&&[d[@"input"][@"primary_trigger_observed"] boolValue]);
    assert([d[@"input"][@"host_merged_mouse_left"] unsignedIntValue]==0x80);
    assert([d[@"weapon"][@"magazine0"][@"loaded_rounds"] intValue]==12&&[d[@"weapon"][@"magazine0"][@"reserved_rounds"] intValue]==48);
    assert([d[@"weapon"][@"trigger0"][@"elapsed_ticks"] intValue]==-1);
    assert([NSJSONSerialization isValidJSONObject:d]&&!memcmp(before,engine_flat_base,size));
    put16(weapon+0x2b8,7);put16(weapon+0x2b6,42);put32(controls,0);putfloat(weapon+0x270,NAN);
    capture_guest_pose(1201);d=live_guest_pose();assert([d[@"weapon"][@"magazine0"][@"loaded_rounds"] intValue]==7);
    assert(![d[@"input"][@"primary_trigger_observed"] boolValue]&&!d[@"weapon"][@"trigger_rate_0270"]);
    assert([NSJSONSerialization isValidJSONObject:d]);
    put16(entries+12,0x9999);capture_guest_pose(1201);d=live_guest_pose();assert(![d[@"weapon_valid"] boolValue]&&!d[@"weapon"]);
    put16(entries+12,weaponDatum>>16);
    put16(unit+0x2f2,0xffff);capture_guest_pose(1202);assert(!guest_pose.hasWeapon&&guest_pose.weapon==UINT32_MAX);
    put16(unit+0x2f2,4);capture_guest_pose(1203);assert(!guest_pose.hasWeapon);
    put16(unit+0x2f2,0);put16(entries+18,0x100);capture_guest_pose(1204);assert(!guest_pose.hasWeapon);
    put16(entries+18,0x340);put16(weapon+0xb4,1);capture_guest_pose(1205);assert(!guest_pose.hasWeapon);put16(weapon+0xb4,2);
    put16(ot+0x20,1);capture_guest_pose(1206);assert(!guest_pose.hasWeapon);put16(ot+0x20,2);
    put32(entries+20,0xffffff80);capture_guest_pose(1207);assert(!guest_pose.hasWeapon);put32(entries+20,weapon);
    put32(ot+0x34,0);capture_guest_pose(1208);assert(!guest_pose.hasUnit);put32(ot+0x34,entries);
    put16(pr,0x9999);capture_guest_pose(1209);assert(!guest_pose.hasUnit);put16(pr,playerDatum>>16);
    put32(0x6b145c,0);put32(0x87a478,0);capture_guest_pose(1210);d=live_guest_pose();assert(![d[@"input"][@"actions_valid"] boolValue]&&!d[@"input"][@"actions_observed"]);
    assert(d[@"input"][@"player_input_disabled"]==[NSNull null]);
    free(before);free(engine_flat_base);engine_flat_base=NULL;
    puts("PASS: read-only guest weapon telemetry; salts/capacity/size/type/range/slots; unavailable states explicit; JSON valid.");
} }
