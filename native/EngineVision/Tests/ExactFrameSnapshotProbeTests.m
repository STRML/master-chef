// CPU-only fixture: no game, renderer, input injection, or GPU is started.
#define main unused_menu_probe_main
#include "MenuInputProbe.m"
#undef main
#include <assert.h>

uint8_t *engine_flat_base;
static uint64_t flat_sequence=1200, panorama_sequence=1200, panorama_flat=1200;
static unsigned frame_copies, panorama_copies;
bool enginevision_frame_info(EngineVisionFrameInfo *out) {
    *out=(EngineVisionFrameInfo){flat_sequence,2,2,16};return true;
}
bool enginevision_copy_latest_frame(void *dst,size_t capacity,EngineVisionFrameInfo *out) {
    assert(capacity>=16);frame_copies++;memset(dst,0x31,16);return enginevision_frame_info(out);
}
bool enginevision_panorama_info(EngineVisionPanoramaInfo *out) {
    *out=(EngineVisionPanoramaInfo){.sequence=panorama_sequence,.width=2,.height=2,.byte_count=64,
        .source_epoch=777,.flat_sequence=panorama_flat,.status=1};return true;
}
bool enginevision_copy_panorama(void *dst,size_t capacity,EngineVisionPanoramaInfo *out) {
    assert(capacity>=64);panorama_copies++;
    for(unsigned i=0;i<4;i++)memset((uint8_t *)dst+i*16,0x40+i,16);
    return enginevision_panorama_info(out);
}
void enginevision_copy_status(char *dst,size_t capacity) { snprintf(dst,capacity,"fixture"); }
static NSString *directory;
static void reset_case(const char *target) {
    [[NSFileManager defaultManager] removeItemAtPath:directory error:NULL];
    assert([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL]);
    memset(&probe_snapshot,0,sizeof probe_snapshot);frame_copies=panorama_copies=0;
    flat_sequence=panorama_sequence=panorama_flat=1200;
    if(target)setenv("HALO_PROBE_SNAPSHOT_AT",target,1);else unsetenv("HALO_PROBE_SNAPSHOT_AT");
}
static NSUInteger file_count(void) {
    return [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:NULL] count];
}
static NSDictionary *metadata(NSString *suffix) {
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[@"snapshot-at-1200" stringByAppendingString:suffix]]];assert(data);
    NSDictionary *json=[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];assert(json);return json;
}
int main(void) {
    @autoreleasepool {
        char temporary[]="/private/tmp/halo-exact-snapshot-XXXXXX";assert(mkdtemp(temporary));
        directory=[NSString stringWithUTF8String:temporary];setenv("HALO_FRAME_CAPTURE",temporary,1);
        unsetenv("HALO_PROBE_CAPTURE_PANORAMA");
        // A sentinel guest allocation cannot be written by the snapshot helper.
        engine_flat_base=malloc(4096);assert(engine_flat_base);memset(engine_flat_base,0xA7,4096);
        reset_case(NULL);probe_snapshot_present(1200);assert(!file_count()&&!frame_copies&&!panorama_copies);
        const char *invalid[]={"","0","-1200","+1200"," 1200","1200junk","2147483648","18446744073709551616"};
        for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
            reset_case(invalid[i]);probe_snapshot_present(1200);assert(!file_count()&&!frame_copies);
        }
        reset_case("1200");probe_snapshot_present(1199);assert(!file_count()&&!frame_copies);
        probe_snapshot_present(1200);assert(file_count()==10&&frame_copies==1&&panorama_copies==1);
        NSDictionary *flat=metadata(@".json");assert([flat[@"frame_sequence"] unsignedLongLongValue]==1200);
        for(unsigned i=0;i<4;i++){
            NSString *suffix=i==3?@".hud":[NSString stringWithFormat:@".panorama-%u",i];
            NSDictionary *info=metadata([suffix stringByAppendingString:@".json"]);
            assert([info[@"frame_sequence"] unsignedLongLongValue]==1200);
            assert([info[@"flat_sequence"] unsignedLongLongValue]==1200);
            assert([info[@"source_epoch"] unsignedLongLongValue]==777&&[info[@"status"] unsignedIntValue]==1);
            NSData *bytes=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:[@"snapshot-at-1200" stringByAppendingFormat:@"%@.bgra",suffix]]];
            assert(bytes.length==16);for(unsigned j=0;j<16;j++)assert(((const uint8_t *)bytes.bytes)[j]==0x40+i);
        }
        probe_snapshot_present(1200);probe_snapshot_present(1201);assert(frame_copies==1&&panorama_copies==1&&file_count()==10);
        reset_case("1200");probe_snapshot_present(1201);probe_snapshot_present(1200);assert(!file_count()&&!frame_copies);
        reset_case("1200");flat_sequence=1201;probe_snapshot_present(1200);assert(!file_count()&&frame_copies==1&&!panorama_copies);
        flat_sequence=1200;probe_snapshot_present(1200);assert(!file_count()&&frame_copies==1);
        reset_case("1200");panorama_sequence=1201;probe_snapshot_present(1200);assert(file_count()==2&&panorama_copies==1);
        reset_case("1200");panorama_flat=1199;probe_snapshot_present(1200);assert(file_count()==2&&panorama_copies==1);
        for(unsigned i=0;i<4096;i++)assert(engine_flat_base[i]==0xA7);
        free(engine_flat_base);engine_flat_base=NULL;
        [[NSFileManager defaultManager] removeItemAtPath:directory error:NULL];
        puts("PASS: exact snapshot is default-off, strict, single-shot, read-only; saves matching flat/HUD/three planes with epoch and rejects missed or mismatched frames.");
    }
    return 0;
}
