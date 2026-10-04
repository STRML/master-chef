/* Direct3D 9 host: COM objects live in guest memory with vtables of native shims.
   Pretransformed graphics and surface copies use Metal; programmable shaders remain pending.
   Every method is accounted for so stdcall stacks stay balanced; unimplemented ones log once and return D3D_OK. */
#ifdef __ANDROID__
#include "engine_compat_android.h" /* d3d9_render.inc: clock_gettime_nsec_np */
#endif
#include "host.h"
#include "metalwin.h"
#include "metalrenderer.h"
#include "texture_decode.h"
#include "gamecontroller.h"
#include "panorama.h"
#include "pointer.h"
#include "directsound.h"
#include "haptics.h"
#include <pthread.h>
pthread_t host_present_thread; int host_present_thread_set;
#include "stateblock.h"
#include "radial_fog.h"
#include "halo_settings.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define D3D_OK 0
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL  0x8876086Cu
#define FMT_X8R8G8B8 22

typedef struct { const char *name; uint8_t argc; } Method;   /* argc includes `this` */
typedef struct { const char *iface; const Method *methods; int count; uint32_t vtable; } ComClass;

enum { K_D3D = 1, K_DEVICE, K_TEXTURE, K_CUBETEX, K_VOLTEX, K_VB, K_IB, K_SURFACE, K_VOLUME, K_VDECL, K_VSHADER, K_PSHADER, K_STATEBLOCK, K_SWAPCHAIN, K_QUERY };
typedef struct {
    int kind; uint32_t guest; int refs;
    uint32_t generation, free_next, hash_next, resource_device;
    uint32_t width, height, depth, levels, format, usage, pool, size, fvf, type;
    uint32_t data;              /* guest buffer for VB/IB/surfaces */
    uint32_t data_capacity;     /* reusable guest allocation size for implicit surfaces */
    uint32_t level_data[16];    /* guest buffers per mip level (textures) */
    uint32_t level_surface[16]; /* guest IDirect3DSurface9 per level */
    uint32_t face_data[6][16];  /* cube faces */
    uint32_t face_surface[6][16]; /* distinct cached alias for every face and mip */
    int locked;
    mr_context *renderer;
    int gpu_dirty;
    /* Content tracking for the texture upload cache: generation advances on
     * every guest lock/unlock, surface copy, fill or GPU readback that can
     * change the bytes; the last hashed generation/key are reused otherwise. */
    uint32_t content_generation, hashed_generation;
    uint64_t content_key;
    uint32_t mod_generation, mod_entry;
    uint8_t mod_checked;
    uint32_t owner;             /* guest texture that owns a level surface */
    HostStateBlock *stateblock; /* Custom Begin/End payload; predefined blocks remain legacy. */
    uint32_t stateblock_device;
    /* Immutable GPU copy owned by this object instance, never a guest-address
     * cache entry. obj_destroy releases it and obj_new clears it on slot reuse.
     * Lock, Unlock and ProcessVertices advance content_generation. */
    void *resident;
    uint32_t resident_generation;
    uint16_t resident_copies;   /* copies taken; a buffer rewritten again and again goes back to per-draw copies */
    uint8_t resident_refused;
    /* Vertex shaders: fog terms radial_fog.h can measure from the eye, found
     * once at creation so a draw only tests the flag. */
    int radial_fog_terms;
    int radial_fog_view_plane;  /* c7 rewrite actually changes this shader */
} D3DObj;
static D3DObj objs[65536]; static int obj_count = 1;
static uint32_t obj_free_head, obj_generation;
#define OBJ_HASH_BUCKETS 16384u
static uint32_t obj_hash[OBJ_HASH_BUCKETS];
static uint32_t frames_presented, draw_calls; int host_frame_limit = 0; extern int host_quit_requested;

static const Method m_d3d[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"RegisterSoftwareDevice",2},{"GetAdapterCount",1},{"GetAdapterIdentifier",4},{"GetAdapterModeCount",3},{"EnumAdapterModes",5},{"GetAdapterDisplayMode",3},{"CheckDeviceType",6},{"CheckDeviceFormat",7},{"CheckDeviceMultiSampleType",7},{"CheckDepthStencilMatch",6},{"CheckDeviceFormatConversion",5},{"GetDeviceCaps",4},{"GetAdapterMonitor",2},{"CreateDevice",7} };
static const Method m_dev[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"TestCooperativeLevel",1},{"GetAvailableTextureMem",1},{"EvictManagedResources",1},{"GetDirect3D",2},{"GetDeviceCaps",2},{"GetDisplayMode",3},{"GetCreationParameters",2},{"SetCursorProperties",4},{"SetCursorPosition",4},{"ShowCursor",2},{"CreateAdditionalSwapChain",3},{"GetSwapChain",3},{"GetNumberOfSwapChains",1},{"Reset",2},{"Present",5},{"GetBackBuffer",5},{"GetRasterStatus",3},{"SetDialogBoxMode",2},{"SetGammaRamp",4},{"GetGammaRamp",3},{"CreateTexture",9},{"CreateVolumeTexture",10},{"CreateCubeTexture",8},{"CreateVertexBuffer",7},{"CreateIndexBuffer",7},{"CreateRenderTarget",9},{"CreateDepthStencilSurface",9},{"UpdateSurface",5},{"UpdateTexture",3},{"GetRenderTargetData",3},{"GetFrontBufferData",3},{"StretchRect",6},{"ColorFill",4},{"CreateOffscreenPlainSurface",7},{"SetRenderTarget",3},{"GetRenderTarget",3},{"SetDepthStencilSurface",2},{"GetDepthStencilSurface",2},{"BeginScene",1},{"EndScene",1},{"Clear",7},{"SetTransform",3},{"GetTransform",3},{"MultiplyTransform",3},{"SetViewport",2},{"GetViewport",2},{"SetMaterial",2},{"GetMaterial",2},{"SetLight",3},{"GetLight",3},{"LightEnable",3},{"GetLightEnable",3},{"SetClipPlane",3},{"GetClipPlane",3},{"SetRenderState",3},{"GetRenderState",3},{"CreateStateBlock",3},{"BeginStateBlock",1},{"EndStateBlock",2},{"SetClipStatus",2},{"GetClipStatus",2},{"GetTexture",3},{"SetTexture",3},{"GetTextureStageState",4},{"SetTextureStageState",4},{"GetSamplerState",4},{"SetSamplerState",4},{"ValidateDevice",2},{"SetPaletteEntries",3},{"GetPaletteEntries",3},{"SetCurrentTexturePalette",2},{"GetCurrentTexturePalette",2},{"SetScissorRect",2},{"GetScissorRect",2},{"SetSoftwareVertexProcessing",2},{"GetSoftwareVertexProcessing",1},{"SetNPatchMode",2},{"GetNPatchMode",1},{"DrawPrimitive",4},{"DrawIndexedPrimitive",7},{"DrawPrimitiveUP",5},{"DrawIndexedPrimitiveUP",9},{"ProcessVertices",7},{"CreateVertexDeclaration",3},{"SetVertexDeclaration",2},{"GetVertexDeclaration",2},{"SetFVF",2},{"GetFVF",2},{"CreateVertexShader",3},{"SetVertexShader",2},{"GetVertexShader",2},{"SetVertexShaderConstantF",4},{"GetVertexShaderConstantF",4},{"SetVertexShaderConstantI",4},{"GetVertexShaderConstantI",4},{"SetVertexShaderConstantB",4},{"GetVertexShaderConstantB",4},{"SetStreamSource",5},{"GetStreamSource",5},{"SetStreamSourceFreq",3},{"GetStreamSourceFreq",3},{"SetIndices",2},{"GetIndices",2},{"CreatePixelShader",3},{"SetPixelShader",2},{"GetPixelShader",2},{"SetPixelShaderConstantF",4},{"GetPixelShaderConstantF",4},{"SetPixelShaderConstantI",4},{"GetPixelShaderConstantI",4},{"SetPixelShaderConstantB",4},{"GetPixelShaderConstantB",4},{"DrawRectPatch",4},{"DrawTriPatch",4},{"DeletePatch",2},{"CreateQuery",3} };
#define RES {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"SetPrivateData",5},{"GetPrivateData",4},{"FreePrivateData",2},{"SetPriority",2},{"GetPriority",1},{"PreLoad",1},{"GetType",1}
#define BASETEX {"SetLOD",2},{"GetLOD",1},{"GetLevelCount",1},{"SetAutoGenFilterType",2},{"GetAutoGenFilterType",1},{"GenerateMipSubLevels",1}
static const Method m_tex[]  = { RES, BASETEX, {"GetLevelDesc",3},{"GetSurfaceLevel",3},{"LockRect",5},{"UnlockRect",2},{"AddDirtyRect",2} };
static const Method m_cube[] = { RES, BASETEX, {"GetLevelDesc",3},{"GetCubeMapSurface",4},{"LockRect",6},{"UnlockRect",3},{"AddDirtyRect",3} };
static const Method m_vol[]  = { RES, BASETEX, {"GetLevelDesc",3},{"GetVolumeLevel",3},{"LockBox",5},{"UnlockBox",2},{"AddDirtyBox",2} };
static const Method m_vb[]   = { RES, {"Lock",5},{"Unlock",1},{"GetDesc",2} };
static const Method m_surf[] = { RES, {"GetContainer",3},{"GetDesc",2},{"LockRect",4},{"UnlockRect",1},{"GetDC",2},{"ReleaseDC",2} };
static const Method m_volume[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"SetPrivateData",5},{"GetPrivateData",4},{"FreePrivateData",2},{"GetContainer",3},{"GetDesc",2},{"LockBox",4},{"UnlockBox",1} };
static const Method m_vdecl[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"GetDeclaration",3} };
static const Method m_shader[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"GetFunction",3} };
static const Method m_sb[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"Capture",1},{"Apply",1} };
static const Method m_swap[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"Present",6},{"GetFrontBufferData",2},{"GetBackBuffer",4},{"GetRasterStatus",2},{"GetDisplayMode",2},{"GetDevice",2},{"GetPresentParameters",2} };
static const Method m_query[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"GetType",1},{"GetDataSize",1},{"Issue",2},{"GetData",4} };
#define N(a) (int)(sizeof(a)/sizeof((a)[0]))
static ComClass classes[] = {
    [K_D3D]={"IDirect3D9",m_d3d,N(m_d3d)}, [K_DEVICE]={"IDirect3DDevice9",m_dev,N(m_dev)}, [K_TEXTURE]={"IDirect3DTexture9",m_tex,N(m_tex)},
    [K_CUBETEX]={"IDirect3DCubeTexture9",m_cube,N(m_cube)}, [K_VOLTEX]={"IDirect3DVolumeTexture9",m_vol,N(m_vol)}, [K_VB]={"IDirect3DVertexBuffer9",m_vb,N(m_vb)},
    [K_IB]={"IDirect3DIndexBuffer9",m_vb,N(m_vb)}, [K_SURFACE]={"IDirect3DSurface9",m_surf,N(m_surf)}, [K_VOLUME]={"IDirect3DVolume9",m_volume,N(m_volume)},
    [K_VDECL]={"IDirect3DVertexDeclaration9",m_vdecl,N(m_vdecl)}, [K_VSHADER]={"IDirect3DVertexShader9",m_shader,N(m_shader)}, [K_PSHADER]={"IDirect3DPixelShader9",m_shader,N(m_shader)},
    [K_STATEBLOCK]={"IDirect3DStateBlock9",m_sb,N(m_sb)}, [K_SWAPCHAIN]={"IDirect3DSwapChain9",m_swap,N(m_swap)}, [K_QUERY]={"IDirect3DQuery9",m_query,N(m_query)} };

static uint32_t vtable_for(int kind) {
    ComClass *c = &classes[kind];
    if (!c->vtable) { c->vtable = guest_alloc(4u * (uint32_t)c->count); char name[96];
        for (int i = 0; i < c->count; i++) { snprintf(name, sizeof name, "%s::%s", c->iface, c->methods[i].name); S32(c->vtable + 4u * (uint32_t)i, host_proc_address("d3d9.dll", name)); } }
    return c->vtable;
}
/* Device/swapchain/query shutdown remains separate. Texture subresources are
 * cached by their parent; an externally referenced child keeps that parent alive. */
static int obj_reclaimable(int kind) {
    return kind==K_VB||kind==K_IB||kind==K_VDECL||kind==K_VSHADER||kind==K_PSHADER||kind==K_STATEBLOCK||
        kind==K_TEXTURE||kind==K_CUBETEX||kind==K_VOLTEX||kind==K_SURFACE||kind==K_VOLUME;
}
static uint32_t obj_bucket(uint32_t guest){return ((guest>>4)*2654435761u)&(OBJ_HASH_BUCKETS-1);}
static void obj_register(D3DObj *o){uint32_t bucket=obj_bucket(o->guest);o->hash_next=obj_hash[bucket];obj_hash[bucket]=(uint32_t)(o-objs);}
static void obj_unregister(D3DObj *o){
    uint32_t *link=&obj_hash[obj_bucket(o->guest)],slot=(uint32_t)(o-objs);
    while(*link&&*link!=slot)link=&objs[*link].hash_next;
    if(*link)*link=o->hash_next;
}
static D3DObj *obj_new(int kind) {
    uint32_t slot=obj_free_head;
    if(slot)obj_free_head=objs[slot].free_next;
    else {if(obj_count>=65536){host_log("d3d object table full");host_exit(3);}slot=(uint32_t)obj_count++;}
    D3DObj *o=&objs[slot];memset(o,0,sizeof *o);o->kind=kind;o->refs=1;
    o->generation=++obj_generation;if(!o->generation)o->generation=++obj_generation;
    o->guest=guest_alloc(16);S32(o->guest,vtable_for(kind));S32(o->guest+4,slot);S32(o->guest+8,o->generation);obj_register(o);
    return o;
}
static D3DObj *obj_from_guest(uint32_t g) {
    if(!g||g>UINT32_MAX-15)return NULL;
    uint32_t i=G32(g+4);if(!i||i>=(uint32_t)obj_count)return NULL;
    D3DObj *o=&objs[i];
    return o->kind&&o->guest==g&&G32(g+8)==o->generation?o:NULL;
}
/* Binding validation never dereferences an arbitrary caller-supplied pointer. */
static D3DObj *obj_find(uint32_t guest) {
    for(uint32_t i=guest?obj_hash[obj_bucket(guest)]:0;i;i=objs[i].hash_next)if(objs[i].guest==guest)return &objs[i];
    return NULL;
}
static void surface_destroy_renderer(D3DObj *surface);
/* Object-owned static geometry only. In-flight Metal commands retain their
 * own references until completion. Bound this cache independently of textures
 * so a large mission cannot turn the copy optimization into memory pressure. */
#ifndef VB_RESIDENT_BUDGET_BYTES
#define VB_RESIDENT_BUDGET_BYTES (128ull * 1024 * 1024)
#endif
static uint64_t vb_resident_live_bytes, vb_resident_peak_bytes, vb_resident_budget_fallbacks;
static void vb_resident_drop(D3DObj *o) {
    if(!o->resident)return;
    mr_buffer_release(o->resident);o->resident=NULL;
    vb_resident_live_bytes-=o->size;
}
static uint32_t obj_release(D3DObj *o);
static uint32_t obj_retain(D3DObj *o){
    if(!o)return 0;
    if(!o->refs){
        /* A cached zero-reference level remains valid internally. GetLevel
         * reacquires its one active-child owner before returning it again. */
        D3DObj *parent=obj_find(o->owner);if(!parent||parent->refs<=0)return 0;
        obj_retain(parent);
    }
    return (uint32_t)++o->refs;
}
static void obj_destroy(D3DObj *o){
    uint32_t slot=(uint32_t)(o-objs),guest=o->guest,device=o->resource_device,block_device=o->stateblock_device;
    if(o->stateblock)host_sb_free(o->stateblock);
    if(o->kind==K_TEXTURE||o->kind==K_CUBETEX||o->kind==K_VOLTEX){
        /* All cached children have zero external refs here; active children
         * own the parent, so final parent release cannot precede them. Destroy
         * targets before freeing shared pages so pending GPU work is finished. */
        for(unsigned level=0;level<16;level++){
            D3DObj *child=obj_find(o->level_surface[level]);if(child)obj_destroy(child);
            if(o->level_data[level])guest_page_free(o->level_data[level]);
            for(unsigned face=0;face<6;face++){
                child=obj_find(o->face_surface[face][level]);if(child)obj_destroy(child);
                if(o->face_data[face][level])guest_page_free(o->face_data[face][level]);
            }
        }
    }
    if(o->kind==K_SURFACE||o->kind==K_VOLUME)surface_destroy_renderer(o);
    /* Queued draws retain the copy they were encoded with; this is the
     * object's reference. A reused slot starts with none. */
    vb_resident_drop(o);
    if(o->data&&!o->owner){
        if(o->kind==K_VB||o->kind==K_IB||o->kind==K_SURFACE||o->kind==K_VOLUME)guest_page_free(o->data);
        else guest_free(o->data);
    }
    obj_unregister(o);memset(GPTR(guest),0,16);memset(o,0,sizeof *o);o->free_next=obj_free_head;obj_free_head=slot;
    guest_free(guest);
    if(device)obj_release(obj_find(device));
    if(block_device)obj_release(obj_find(block_device));
}
static uint32_t obj_release(D3DObj *o) {
    if(!o||o->refs<=0)return 0;
    if(!obj_reclaimable(o->kind)){if(o->refs>1)--o->refs;return (uint32_t)o->refs;}
    uint32_t refs=(uint32_t)--o->refs;if(refs)return refs;
    if(o->owner){
        /* Keep the cached level and its GPU-authoritative contents until its
         * parent dies. Releasing this last owner may destroy both objects. */
        obj_release(obj_find(o->owner));return 0;
    }
    obj_destroy(o);return 0;
}
static void obj_attach_device(D3DObj *o,D3DObj *device){if(device){o->resource_device=device->guest;obj_retain(device);}}
static uint32_t bytes_per_pixel_x8(uint32_t fmt) {  /* returns bits per pixel; block formats handled by caller */
    switch (fmt) { case 21: case 22: case 32: case 33: case 35: case 36: case 60+3: case 75: case 77: case 80-9: return 32; case 23: case 24: case 25: case 26: case 51: case 60: case 80: case 70: return 16; case 28: case 50: case 41: return 8; default: return 32; }
}
static uint32_t level_size(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch) {
    if (fmt == 0x31545844u || fmt == 0x32545844u || fmt == 0x33545844u || fmt == 0x34545844u || fmt == 0x35545844u) { /* DXTn */
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 0x31545844u ? 8 : 16; *pitch = bw * bs; return bw * bh * bs; }
    uint32_t bpp = bytes_per_pixel_x8(fmt); *pitch = (w * bpp + 7) / 8; return *pitch * h;
}
static void fill_caps(uint32_t c) {
    memset(GPTR(c), 0, 304);
    S32(c + 0, 1); S32(c + 8, 0x20000); S32(c + 12, 0xE0020000u); S32(c + 16, 0x20); S32(c + 20, 0x8000000Fu); S32(c + 24, 3);
    S32(c + 28, 0x1BF7A0F0u); S32(c + 32, 0x002FCEF2u); S32(c + 36, 0x0F736191u); S32(c + 40, 0xFF); S32(c + 44, 0x3FFF); S32(c + 48, 0x3FFF); S32(c + 52, 0xFF);
    S32(c + 56, 0x00084208u); S32(c + 60, 0x0001EC05u); S32(c + 64, 0x03030700u); S32(c + 68, 0x03030300u); S32(c + 72, 0x03030300u); S32(c + 76, 0x3F); S32(c + 80, 0x3F); S32(c + 84, 0x1F);
    S32(c + 88, 4096); S32(c + 92, 4096); S32(c + 96, 512); S32(c + 100, 8192); S32(c + 104, 4096); S32(c + 108, 16);
    float one = 1.0e10f; memcpy(GPTR(c + 112), &one, 4); float gb = -1.0e8f; memcpy(GPTR(c + 116), &gb, 4); memcpy(GPTR(c + 120), &gb, 4); gb = 1.0e8f; memcpy(GPTR(c + 124), &gb, 4); memcpy(GPTR(c + 128), &gb, 4);
    S32(c + 136, 0x1FF); S32(c + 140, 0x00080008u); S32(c + 144, 0x03FFFFFFu); S32(c + 148, 8); S32(c + 152, 8); S32(c + 156, 0x3B); S32(c + 160, 8); S32(c + 164, 6); S32(c + 168, 4); S32(c + 172, 255);
    float ps = 256.0f; memcpy(GPTR(c + 176), &ps, 4); S32(c + 180, 0xFFFFF); S32(c + 184, 0xFFFFFF); S32(c + 188, 16); S32(c + 192, 255);
    S32(c + 196, 0xFFFE0200u); S32(c + 200, 256); S32(c + 204, 0xFFFF0200u); float px = 65504.0f; memcpy(GPTR(c + 208), &px, 4);
    S32(c + 212, 0x51); float np = 1.0f; memcpy(GPTR(c + 216), &np, 4); S32(c + 224, 0); S32(c + 228, 0); S32(c + 232, 1); S32(c + 236, 0x3FF); S32(c + 240, 4); S32(c + 244, 0x03000300u);
    S32(c + 248, 0x1F); S32(c + 252, 24); S32(c + 256, 32); S32(c + 260, 4); S32(c + 264, 0x1F); S32(c + 268, 24); S32(c + 272, 32); S32(c + 276, 4); S32(c + 280, 512);
    S32(c + 284, 0x03000300u); S32(c + 288, 65535); S32(c + 292, 65535); S32(c + 296, 32768); S32(c + 300, 32768);
}
static void fill_mode(uint32_t m, uint32_t w, uint32_t h) { S32(m, w); S32(m + 4, h); S32(m + 8, 60); S32(m + 12, FMT_X8R8G8B8); }
static const uint32_t modes[][2] = { {640,480},{800,600},{1024,768},{1280,720},{1280,960},{1600,900},{1600,1200},{1920,1080},{1920,1440},{2560,1920} };
static uint32_t backbuffer_w = 640, backbuffer_h = 480;

static void unimplemented(EngineCPU *cpu, ComClass *c, int i) {
    static uint32_t seen[64]; uint32_t key = (uint32_t)(c - classes) * 256u + (uint32_t)i; int found = 0;
    for (int k = 0; k < 64; k++) if (seen[k] == key) { found = 1; break; }
    if (!found) { for (int k = 0; k < 64; k++) if (!seen[k]) { seen[k] = key; break; } host_log("d3d9 %s::%s not implemented (returning D3D_OK)", c->iface, c->methods[i].name); }
    RET_STDCALL(D3D_OK, c->methods[i].argc);
}
static uint32_t make_surface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage) {
    D3DObj *s = obj_new(K_SURFACE); s->width = w; s->height = h; s->format = fmt; s->usage = usage; s->type = 1;
    uint32_t pitch; s->size = level_size(fmt, w, h, &pitch); s->data_capacity = s->size ? s->size : 16;
    s->data = guest_page_alloc(s->data_capacity); return s->guest;
}
static D3DObj *the_device;
static uint32_t current_depth_guest; /* independently owned depth binding */
static uint32_t current_rt_guest;   /* guest ptr of the current color render-target surface */
int host_window_shown; static int window_ready;
static D3DObj *rt_surface(void) { D3DObj *s = obj_from_guest(current_rt_guest); if (!s && the_device) s = obj_from_guest(the_device->data); return s; }

typedef struct { float diffuse[4],ambient[4],specular[4],emissive[4],power; } HostD3DMaterial;
typedef struct {
    uint32_t type; float diffuse[4],specular[4],ambient[4],position[3],direction[3];
    float range,falloff,attenuation[3],theta,phi;
} HostD3DLight;
typedef struct { HostD3DLight light; uint8_t defined,enabled; } HostD3DLightSlot;
enum { HOST_D3D_MAX_LIGHTS=256, HOST_D3D_MAX_ACTIVE_LIGHTS=8 };
static HostD3DMaterial current_material;
static HostD3DLightSlot light_slots[HOST_D3D_MAX_LIGHTS];
typedef struct {
    HostD3DLight light;
    float direction[3];
    float spot_direction[3];
    float spot_outer,spot_inner;
    uint8_t direction_valid,spot_direction_valid;
} HostD3DPreparedLight;
typedef struct {
    HostD3DPreparedLight lights[HOST_D3D_MAX_ACTIVE_LIGHTS];
    uint32_t count;
    float world[16];
    float normal_c0[3],normal_c1[3],normal_c2[3],normal_det;
    uint8_t normal_singular;
} HostD3DPreparedLighting;
static uint32_t d3d9_light_vertex(const float world[16],const float position[3],const float normal[3],uint32_t diffuse_color,uint32_t specular_color);
static uint32_t d3d9_light_vertex_prepared(const HostD3DPreparedLighting *prepared,const float position[3],const float normal[3],uint32_t diffuse_color,uint32_t specular_color);
static float vec_normalize(float v[3]);

static int finite_floats(const float *v,size_t count){for(size_t i=0;i<count;i++)if(!isfinite(v[i]))return 0;return 1;}
static void default_light(HostD3DLight *l){memset(l,0,sizeof *l);l->type=3;l->diffuse[0]=l->diffuse[1]=l->diffuse[2]=1.f;l->direction[2]=1.f;}
static void lighting_defaults(void){memset(&current_material,0,sizeof current_material);memset(light_slots,0,sizeof light_slots);}
static int valid_light(const HostD3DLight *l){
    if(!l||(l->type<1||l->type>3)||!finite_floats(&l->diffuse[0],25))return 0;
    if(l->type==3)return 1;
    if(l->range<0.f||l->range>sqrtf(FLT_MAX)||l->attenuation[0]<0.f||l->attenuation[1]<0.f||l->attenuation[2]<0.f)return 0;
    if(l->type==2&&(l->theta<0.f||l->phi<=0.f||l->theta>l->phi||l->phi>(float)M_PI))return 0;
    return 1;
}
static uint32_t lighting_set_material(const HostD3DMaterial *m){if(!m||!finite_floats(&m->diffuse[0],17)||m->power<0.f)return D3DERR_INVALIDCALL;current_material=*m;return D3D_OK;}
static uint32_t lighting_set_light(uint32_t index,const HostD3DLight *l){if(index>=HOST_D3D_MAX_LIGHTS||!valid_light(l))return D3DERR_INVALIDCALL;light_slots[index].light=*l;light_slots[index].defined=1;return D3D_OK;}
static uint32_t lighting_enable(uint32_t index,int enable){if(index>=HOST_D3D_MAX_LIGHTS)return D3DERR_INVALIDCALL;if(!light_slots[index].defined){default_light(&light_slots[index].light);light_slots[index].defined=1;}if(enable&&!light_slots[index].enabled){int active=0;for(int k=0;k<HOST_D3D_MAX_LIGHTS;k++)active+=light_slots[k].enabled!=0;if(active>=HOST_D3D_MAX_ACTIVE_LIGHTS)return D3DERR_INVALIDCALL;}light_slots[index].enabled=(uint8_t)(enable!=0);return D3D_OK;}

static void d3d9_prepare_lighting(const float world[16],HostD3DPreparedLighting *prepared){
    memset(prepared,0,sizeof *prepared);
    memcpy(prepared->world,world,sizeof prepared->world);
    float x[3]={world[0],world[1],world[2]},y[3]={world[4],world[5],world[6]},z[3]={world[8],world[9],world[10]};
    float c0[3]={y[1]*z[2]-y[2]*z[1],y[2]*z[0]-y[0]*z[2],y[0]*z[1]-y[1]*z[0]};
    float c1[3]={z[1]*x[2]-z[2]*x[1],z[2]*x[0]-z[0]*x[2],z[0]*x[1]-z[1]*x[0]};
    float c2[3]={x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    float det=x[0]*c0[0]+x[1]*c0[1]+x[2]*c0[2];
    prepared->normal_det=det;prepared->normal_singular=(uint8_t)(fabsf(det)<1.e-12f);
    if(!prepared->normal_singular){memcpy(prepared->normal_c0,c0,sizeof c0);memcpy(prepared->normal_c1,c1,sizeof c1);memcpy(prepared->normal_c2,c2,sizeof c2);}
    for(int i=0;i<HOST_D3D_MAX_LIGHTS&&prepared->count<HOST_D3D_MAX_ACTIVE_LIGHTS;i++){
        HostD3DLightSlot *slot=&light_slots[i];if(!slot->defined||!slot->enabled)continue;
        HostD3DPreparedLight *out=&prepared->lights[prepared->count++];out->light=slot->light;
        if(out->light.type==3){
            out->direction[0]=-out->light.direction[0];out->direction[1]=-out->light.direction[1];out->direction[2]=-out->light.direction[2];
            out->direction_valid=(uint8_t)(vec_normalize(out->direction)!=0.f);
        }else if(out->light.type==2){
            out->spot_direction[0]=out->light.direction[0];out->spot_direction[1]=out->light.direction[1];out->spot_direction[2]=out->light.direction[2];
            out->spot_direction_valid=(uint8_t)(vec_normalize(out->spot_direction)!=0.f);
            if(out->spot_direction_valid){out->spot_outer=cosf(out->light.phi*.5f);out->spot_inner=cosf(out->light.theta*.5f);}
        }
    }
}

#include "d3d9_render.inc"
#include "shader_mod_runtime.inc"

/* Observational only: prove whether the currently stubbed state blocks are
 * used by the original engine. No guest state or state-block semantics change. */
enum { SB_TRACE_CREATE, SB_TRACE_BEGIN, SB_TRACE_END, SB_TRACE_CAPTURE, SB_TRACE_APPLY, SB_TRACE_ACTIONS };
static struct {
    int initialized, enabled, summary_started;
    uint64_t last_summary_ns, counts[SB_TRACE_ACTIONS], omitted;
    unsigned details;
} stateblock_trace;
static int stateblock_trace_enabled(void) {
    if (!stateblock_trace.initialized) {
        const char *flag=getenv("HALO_STATEBLOCK_TRACE");
        stateblock_trace.initialized=1;
        stateblock_trace.enabled=flag&&strcmp(flag,"1")==0;
        if(stateblock_trace.enabled)host_log("[stateblock] trace-enabled version=1 detail-cap=32 summary-seconds=1 semantics=unchanged-stubs");
    }
    return stateblock_trace.enabled;
}
static uint64_t stateblock_trace_hash(uint64_t hash,const void *bytes,size_t size) {
    const uint8_t *p=bytes;
    for(size_t i=0;i<size;i++){hash^=p[i];hash*=UINT64_C(1099511628211);}
    return hash;
}
static void stateblock_trace_poll(void) {
    if(!stateblock_trace_enabled())return;
    uint64_t now=host_monotonic_ns();
    if(stateblock_trace.summary_started&&now-stateblock_trace.last_summary_ns<UINT64_C(1000000000))return;
    stateblock_trace.summary_started=1;stateblock_trace.last_summary_ns=now;
    host_log("[stateblock] summary frame=%u create=%llu begin=%llu end=%llu capture=%llu apply=%llu detail-omitted=%llu",
        frames_presented,(unsigned long long)stateblock_trace.counts[SB_TRACE_CREATE],
        (unsigned long long)stateblock_trace.counts[SB_TRACE_BEGIN],(unsigned long long)stateblock_trace.counts[SB_TRACE_END],
        (unsigned long long)stateblock_trace.counts[SB_TRACE_CAPTURE],(unsigned long long)stateblock_trace.counts[SB_TRACE_APPLY],
        (unsigned long long)stateblock_trace.omitted);
}
static void stateblock_trace_event(EngineCPU *cpu,unsigned action,uint32_t block,uint32_t type) {
    if(!stateblock_trace_enabled()||action>=SB_TRACE_ACTIONS)return;
    stateblock_trace.counts[action]++;
    if(stateblock_trace.details>=32){stateblock_trace.omitted++;return;}
    stateblock_trace.details++;
    static const char *names[SB_TRACE_ACTIONS]={"create","begin","end","capture","apply"};
    uint64_t bindings=UINT64_C(14695981039346656037),matrices=bindings,constants=bindings;
    bindings=stateblock_trace_hash(bindings,draw_state.stream,sizeof draw_state.stream);
    bindings=stateblock_trace_hash(bindings,draw_state.offset,sizeof draw_state.offset);
    bindings=stateblock_trace_hash(bindings,draw_state.stride,sizeof draw_state.stride);
    bindings=stateblock_trace_hash(bindings,&draw_state.indices,sizeof draw_state.indices);
    matrices=stateblock_trace_hash(matrices,draw_state.world,sizeof draw_state.world);
    matrices=stateblock_trace_hash(matrices,draw_state.view,sizeof draw_state.view);
    matrices=stateblock_trace_hash(matrices,draw_state.proj,sizeof draw_state.proj);
    constants=stateblock_trace_hash(constants,draw_state.vs_float,sizeof draw_state.vs_float);
    constants=stateblock_trace_hash(constants,draw_state.ps_float,sizeof draw_state.ps_float);
    D3DObj *trace_block=NULL;
    for(int k=1;block&&k<obj_count;k++)if(objs[k].guest==block){trace_block=&objs[k];break;}
    const char *semantics=(action==SB_TRACE_BEGIN?stateblock_recording!=NULL:trace_block&&trace_block->stateblock)?"custom-masked":"unchanged-stub";
    host_log("[stateblock] %s frame=%u caller=%08x block=%08x type=%u vs=%08x ps=%08x decl=%08x fvf=%08x bindings=%016llx matrices=%016llx float-constants=%016llx semantics=%s",
        names[action],frames_presented+1,G32(cpu->gpr[4]),block,type,draw_state.vs,draw_state.ps,draw_state.decl,draw_state.fvf,
        (unsigned long long)bindings,(unsigned long long)matrices,(unsigned long long)constants,semantics);
}

static void reset_implicit_surface(D3DObj *surface,uint32_t width,uint32_t height,uint32_t format,uint32_t usage){
    uint32_t pitch,size=level_size(format,width,height,&pitch);uint32_t capacity=size?size:16;
    if(!surface->data||surface->data_capacity<capacity){
        uint32_t old_data=surface->data;
        surface->data=guest_page_alloc(capacity);surface->data_capacity=capacity;
        if(old_data)guest_page_free(old_data);
    }
    memset(GPTR(surface->data),0,size);surface->width=width;surface->height=height;surface->format=format;
    surface->usage=usage;surface->size=size;surface->gpu_dirty=0;
}
static uint32_t reset_device(D3DObj *device,uint32_t pp){
    if(!device||device->kind!=K_DEVICE||!pp||stateblock_recording)return D3DERR_INVALIDCALL;
    uint32_t requested_w=G32(pp),requested_h=G32(pp+4);
    uint32_t width=requested_w?requested_w:backbuffer_w,height=requested_h?requested_h:backbuffer_h;
    if(width<64||height<64||width>8192||height>8192)return D3DERR_INVALIDCALL;
    D3DObj *backbuffer=obj_from_guest(device->data),*depth=obj_from_guest(device->level_surface[0]);
    if(!backbuffer||backbuffer->kind!=K_SURFACE||!depth||depth->kind!=K_SURFACE)return D3DERR_INVALIDCALL;
    /* Preserve the renderer and its immutable pipeline/texture caches, but replace
     * its size-dependent color and depth targets before publishing new dimensions. */
    if(backbuffer->renderer&&mr_resize(backbuffer->renderer,(int)width,(int)height))return D3DERR_NOTAVAILABLE;
    uint32_t backbuffer_format=G32(pp+8);if(!backbuffer_format){backbuffer_format=FMT_X8R8G8B8;S32(pp+8,backbuffer_format);}
    uint32_t depth_format=G32(pp+40);if(!depth_format)depth_format=75;
    reset_implicit_surface(backbuffer,width,height,backbuffer_format,1);
    reset_implicit_surface(depth,width,height,depth_format,2);
    backbuffer_w=width;backbuffer_h=height;S32(pp,width);S32(pp+4,height);
    binding_replace(&current_rt_guest,device->data);binding_replace(&current_depth_guest,device->level_surface[0]);render_defaults(width,height);lighting_defaults();host_panorama_invalidate();
    host_log("d3d9 Reset: %ux%u bbfmt=%u dsfmt=%u",width,height,backbuffer_format,depth_format);
    return D3D_OK;
}

static uint32_t create_texture(D3DObj *device,int kind,uint32_t w,uint32_t h,uint32_t d,uint32_t levels,uint32_t usage,uint32_t format,uint32_t pool,uint32_t out){
    if(!out||!w||!h||!d||w>8192||h>8192||d>512||pool>3)return D3DERR_INVALIDCALL;
    uint32_t maxdim=w>h?w:h;if(d>maxdim)maxdim=d;
    uint32_t maxlevels=1;for(uint32_t n=maxdim;n>1;n>>=1)maxlevels++;
    if(!levels)levels=maxlevels;if(levels>maxlevels||levels>16)return D3DERR_INVALIDCALL;
    uint32_t pitch,size=level_size(format,w,h,&pitch);
    if((uint64_t)size*d>UINT32_MAX)return D3DERR_INVALIDCALL;
    D3DObj *t=obj_new(kind);obj_attach_device(t,device);t->width=w;t->height=h;t->depth=d;t->levels=levels;
    t->format=format;t->usage=usage;t->pool=pool;t->type=kind==K_TEXTURE?3:kind==K_VOLTEX?4:5;
    S32(out,t->guest);return D3D_OK;
}
static uint32_t create_surface(D3DObj *device,uint32_t w,uint32_t h,uint32_t format,uint32_t usage,uint32_t pool,uint32_t out){
    if(!out||!w||!h||w>8192||h>8192||pool>3)return D3DERR_INVALIDCALL;
    D3DObj *s=obj_find(make_surface(w,h,format,usage));s->pool=pool;obj_attach_device(s,device);S32(out,s->guest);return D3D_OK;
}

static uint32_t create_buffer(D3DObj *device,int kind,uint32_t len,uint32_t usage,uint32_t format,uint32_t pool,uint32_t out){
    if(!out||!len||pool>3||(kind==K_IB&&format!=101&&format!=102))return D3DERR_INVALIDCALL;
    D3DObj *b=obj_new(kind);obj_attach_device(b,device);b->size=len;b->usage=usage;b->pool=pool;b->type=kind==K_VB?6:7;
    if(kind==K_VB)b->fvf=format;else b->format=format;
    b->data=guest_page_alloc(len);S32(out,b->guest);return D3D_OK;
}

static float light_clamp(float v){return v<0.f?0.f:v>1.f?1.f:v;}
static void unpack_color(uint32_t c,float out[4]){out[0]=((c>>16)&255)/255.f;out[1]=((c>>8)&255)/255.f;out[2]=(c&255)/255.f;out[3]=((c>>24)&255)/255.f;}
static const float *material_source(uint32_t state,const float *material,const float c1[4],const float c2[4]){
    if(!draw_state.rs[141])return material;uint32_t source=draw_state.rs[state];return source==1?c1:source==2?c2:material;
}
static float vec_normalize(float v[3]){float n=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(!(n>1.e-12f)||!isfinite(n))return 0.f;v[0]/=n;v[1]/=n;v[2]/=n;return n;}
static void transform_position(const float m[16],const float p[3],float out[3]){out[0]=p[0]*m[0]+p[1]*m[4]+p[2]*m[8]+m[12];out[1]=p[0]*m[1]+p[1]*m[5]+p[2]*m[9]+m[13];out[2]=p[0]*m[2]+p[1]*m[6]+p[2]*m[10]+m[14];}
static void transform_normal(const float m[16],const float n[3],float out[3]){
    float x[3]={m[0],m[1],m[2]},y[3]={m[4],m[5],m[6]},z[3]={m[8],m[9],m[10]};
    float c0[3]={y[1]*z[2]-y[2]*z[1],y[2]*z[0]-y[0]*z[2],y[0]*z[1]-y[1]*z[0]};
    float c1[3]={z[1]*x[2]-z[2]*x[1],z[2]*x[0]-z[0]*x[2],z[0]*x[1]-z[1]*x[0]};
    float c2[3]={x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    float det=x[0]*c0[0]+x[1]*c0[1]+x[2]*c0[2];if(fabsf(det)<1.e-12f){memcpy(out,n,12);return;}
    out[0]=(n[0]*c0[0]+n[1]*c1[0]+n[2]*c2[0])/det;out[1]=(n[0]*c0[1]+n[1]*c1[1]+n[2]*c2[1])/det;out[2]=(n[0]*c0[2]+n[1]*c1[2]+n[2]*c2[2])/det;
}
static uint32_t d3d9_light_vertex_prepared(const HostD3DPreparedLighting *prepared,const float position[3],const float normal[3],uint32_t diffuse_color,uint32_t specular_color){
    if(!draw_state.rs[137])return diffuse_color;
    float c1[4],c2[4];unpack_color(diffuse_color,c1);unpack_color(specular_color,c2);
    const float *md=material_source(145,current_material.diffuse,c1,c2),*ma=material_source(147,current_material.ambient,c1,c2),*me=material_source(148,current_material.emissive,c1,c2);
    float ambient[4];unpack_color(draw_state.rs[139],ambient);float color[3]={me[0]+ma[0]*ambient[0],me[1]+ma[1]*ambient[1],me[2]+ma[2]*ambient[2]};
    float wp[3],wn[3];transform_position(prepared->world,position,wp);if(prepared->normal_singular)memcpy(wn,normal,12);else{
        wn[0]=(normal[0]*prepared->normal_c0[0]+normal[1]*prepared->normal_c1[0]+normal[2]*prepared->normal_c2[0])/prepared->normal_det;
        wn[1]=(normal[0]*prepared->normal_c0[1]+normal[1]*prepared->normal_c1[1]+normal[2]*prepared->normal_c2[1])/prepared->normal_det;
        wn[2]=(normal[0]*prepared->normal_c0[2]+normal[1]*prepared->normal_c1[2]+normal[2]*prepared->normal_c2[2])/prepared->normal_det;
    }if(draw_state.rs[143])vec_normalize(wn);
    for(uint32_t i=0;i<prepared->count;i++){const HostD3DPreparedLight*ps=&prepared->lights[i];const HostD3DLight*l=&ps->light;float toward[3],atten=1.f,spot=1.f;
        if(l->type==3){if(!ps->direction_valid)continue;toward[0]=ps->direction[0];toward[1]=ps->direction[1];toward[2]=ps->direction[2];}
        else {toward[0]=l->position[0]-wp[0];toward[1]=l->position[1]-wp[1];toward[2]=l->position[2]-wp[2];float d=vec_normalize(toward);if(!d||d>l->range)continue;float den=l->attenuation[0]+l->attenuation[1]*d+l->attenuation[2]*d*d;if(!(den>0.f))continue;atten=1.f/den;
            if(l->type==2){if(!ps->spot_direction_valid)continue;float rho=-(toward[0]*ps->spot_direction[0]+toward[1]*ps->spot_direction[1]+toward[2]*ps->spot_direction[2]);if(rho<=ps->spot_outer)spot=0.f;else if(rho<ps->spot_inner&&ps->spot_inner>ps->spot_outer)spot=powf((rho-ps->spot_outer)/(ps->spot_inner-ps->spot_outer),l->falloff);}
        }
        float ndotl=wn[0]*toward[0]+wn[1]*toward[1]+wn[2]*toward[2];if(ndotl<0.f)ndotl=0.f;if(ndotl>1.f)ndotl=1.f;
        for(int k=0;k<3;k++)color[k]+=atten*(ma[k]*l->ambient[k]+md[k]*l->diffuse[k]*ndotl*spot);
    }
    uint32_t a=(uint32_t)(light_clamp(md[3])*255.f+.5f),r=(uint32_t)(light_clamp(color[0])*255.f+.5f),g=(uint32_t)(light_clamp(color[1])*255.f+.5f),b=(uint32_t)(light_clamp(color[2])*255.f+.5f);
    return (a<<24)|(r<<16)|(g<<8)|b;
}
static uint32_t d3d9_light_vertex(const float world[16],const float position[3],const float normal[3],uint32_t diffuse_color,uint32_t specular_color){
    if(!draw_state.rs[137])return diffuse_color;HostD3DPreparedLighting prepared;d3d9_prepare_lighting(world,&prepared);
    return d3d9_light_vertex_prepared(&prepared,position,normal,diffuse_color,specular_color);
}

static void method_d3d(EngineCPU *cpu, D3DObj *o, int i) {
    (void)o;
    switch (i) {
    case 0: S32(ARG(2), ARG(0)); RET_STDCALL(D3D_OK, 3);
    case 1: RET_STDCALL(2, 1); case 2: RET_STDCALL(1, 1);
    case 4: RET_STDCALL(1, 1);
    case 5: { uint32_t id = ARG(3); memset(GPTR(id), 0, 1100); strcpy((char *)GPTR(id), "halo-vision-metal"); strcpy((char *)GPTR(id + 512), "Halo Vision Metal Renderer"); strcpy((char *)GPTR(id + 1024), "\\\\.\\DISPLAY1"); S64(id + 1056, UINT64_C(0x0009000F00000000)); S32(id + 1064, 0x10DE); S32(id + 1068, 0x0001); RET_STDCALL(D3D_OK, 4); }
    case 6: RET_STDCALL(N(modes), 3);
    case 7: { uint32_t m = ARG(3); if (m >= N(modes)) RET_STDCALL(D3DERR_INVALIDCALL, 5); fill_mode(ARG(4), modes[m][0], modes[m][1]); RET_STDCALL(D3D_OK, 5); }
    case 8: fill_mode(ARG(2), 1920, 1080); RET_STDCALL(D3D_OK, 3);
    case 9: RET_STDCALL(D3D_OK, 6); case 10: RET_STDCALL(D3D_OK, 7);
    case 11: { host_log("d3d9 CheckDeviceFormat(adapter=%u devtype=%u adfmt=%u usage=%X rtype=%u checkfmt=%u)", ARG(1),ARG(2),ARG(3),ARG(4),ARG(5),ARG(6)); RET_STDCALL(D3D_OK, 7); }
    case 12: RET_STDCALL(D3D_OK, 6); case 13: RET_STDCALL(D3D_OK, 5);
    case 14: fill_caps(ARG(3)); RET_STDCALL(D3D_OK, 4);
    case 15: RET_STDCALL(0x50001, 2);
    case 16: { uint32_t pp = ARG(5), out = ARG(6); D3DObj *d = obj_new(K_DEVICE); the_device = d;
        backbuffer_w = G32(pp) >= 64 ? G32(pp) : 640; backbuffer_h = G32(pp + 4) >= 64 ? G32(pp + 4) : 480; S32(pp, backbuffer_w); S32(pp + 4, backbuffer_h); if (!G32(pp + 8)) S32(pp + 8, FMT_X8R8G8B8);
        host_log("d3d9 CreateDevice pp: w=%u h=%u bbfmt=%u bbcount=%u swap=%u hwnd=%08X windowed=%u autods=%u dsfmt=%u flags=%08X refresh=%u present=%u",
                 G32(pp),G32(pp+4),G32(pp+8),G32(pp+12),G32(pp+24),G32(pp+28),G32(pp+32),G32(pp+36),G32(pp+40),G32(pp+44),G32(pp+48),G32(pp+52));
        d->data = make_surface(backbuffer_w, backbuffer_h, FMT_X8R8G8B8, 1); d->level_surface[0] = make_surface(backbuffer_w, backbuffer_h, G32(pp + 40) ? G32(pp + 40) : 75, 2);
        binding_replace(&current_rt_guest,d->data);binding_replace(&current_depth_guest,d->level_surface[0]);
        render_defaults(backbuffer_w,backbuffer_h); lighting_defaults();
        if (!window_ready) { window_ready = (metalwin_init((int)backbuffer_w, (int)backbuffer_h, "Halo Vision Pro") == 0) ? 1 : -1; host_log("metalwin_init %s", window_ready==1?"ok":"unavailable (no main runloop?)"); if(window_ready==1) host_window_shown=1; }
        S32(out, d->guest); RET_STDCALL(D3D_OK, 7); }
    default: unimplemented(cpu, &classes[K_D3D], i);
    }
}
/* Shared custom-block device handler, independently exercisable offline. */
static int stateblock_device_method(EngineCPU *cpu,D3DObj *o,int i,uint32_t *hr){
    switch(i){
    case 59:{D3DObj *s=obj_new(K_STATEBLOCK);S32(ARG(2),s->guest);stateblock_trace_event(cpu,SB_TRACE_CREATE,s->guest,ARG(1));*hr=D3D_OK;return 1;}
    case 60:
        if(stateblock_recording){*hr=D3DERR_INVALIDCALL;return 1;}
        stateblock_recording=stateblock_begin_payload();if(!stateblock_recording){*hr=0x8007000Eu;return 1;}
        stateblock_trace_event(cpu,SB_TRACE_BEGIN,0,0);*hr=D3D_OK;return 1;
    case 61:{
        if(!stateblock_recording||!ARG(1)){*hr=D3DERR_INVALIDCALL;return 1;}
        D3DObj *s=obj_new(K_STATEBLOCK);s->stateblock=stateblock_recording;stateblock_recording=NULL;
        s->stateblock_device=o->guest;stateblock_retain(NULL,o->guest);S32(ARG(1),s->guest);
        stateblock_trace_event(cpu,SB_TRACE_END,s->guest,0);*hr=D3D_OK;return 1;}
    default:return 0;
    }
}
static void method_device(EngineCPU *cpu, D3DObj *o, int i) {
    uint32_t render_hr;
    render_surface_probe(i, cpu);
    if(stateblock_device_method(cpu,o,i,&render_hr))RET_STDCALL(render_hr,classes[K_DEVICE].methods[i].argc);
    if(render_method(cpu,o,i,&render_hr)) RET_STDCALL(render_hr,classes[K_DEVICE].methods[i].argc);
    switch (i) {
    case 0: S32(ARG(2), ARG(0)); RET_STDCALL(D3D_OK, 3);
    case 1: RET_STDCALL(++o->refs, 1); case 2: RET_STDCALL(o->refs > 1 ? --o->refs : 0, 1);
    case 3: RET_STDCALL(D3D_OK, 1);
    case 4: RET_STDCALL(256u << 20, 1);
    case 5: RET_STDCALL(D3D_OK, 1);
    case 7: fill_caps(ARG(1)); RET_STDCALL(D3D_OK, 2);
    case 8: fill_mode(ARG(2), backbuffer_w, backbuffer_h); RET_STDCALL(D3D_OK, 3);
    case 9: { uint32_t p = ARG(1); S32(p, 0); S32(p + 4, 1); S32(p + 8, host_main_hwnd); S32(p + 12, 0x40); RET_STDCALL(D3D_OK, 2); }
    case 12: RET_STDCALL(0, 2);
    case 14: { D3DObj *s = obj_new(K_SWAPCHAIN); S32(ARG(2), s->guest); RET_STDCALL(D3D_OK, 3); }
    case 15: RET_STDCALL(1, 1);
    case 16: RET_STDCALL(reset_device(o,ARG(1)),2);
    case 17: { frames_presented++; stateblock_trace_poll();
        /* The presenting thread is the engine's; the frame limiter's spin is
         * only its own yields (shims_kernel32.c). */
        host_present_thread = pthread_self(); host_present_thread_set = 1;
        host_fp_environment_check();
        { /* Host-side scriptable keyboard injection: HALO_KEYSEQ="startF:endF:hexScan,..." holds each key in [startF,endF]. */
          extern uint8_t host_keyboard_state[256]; const char *seq=getenv("HALO_KEYSEQ");
          if(seq){ static uint8_t held[256]; /* clear previously-held nav keys each frame, then re-apply active ones */
            for(int i=0;i<256;i++) if(held[i]){ host_keyboard_state[i]=0; held[i]=0; }
            const char *p=seq; while(*p){ int a=0,b=0,sc=0; if(sscanf(p,"%d:%d:%x",&a,&b,&sc)==3 && sc>=0 && sc<256){
                if((int)frames_presented>=a && (int)frames_presented<=b){ host_keyboard_state[sc]=0x80; held[sc]=1; } }
              const char *c=strchr(p,','); if(!c) break; p=c+1; } } }
        { /* Host-side mouse-look injection: HALO_LOOKSEQ="startF:endF:dx:dy,..." sets relative mouse
             delta each frame in [startF,endF] so the probe can TURN the view (test full traversal). */
          extern int32_t host_mouse_dx, host_mouse_dy; const char *seq=getenv("HALO_LOOKSEQ");
          if(seq){ const char *p=seq; while(*p){ int a=0,b=0,dx=0,dy=0;
              if(sscanf(p,"%d:%d:%d:%d",&a,&b,&dx,&dy)==4){
                  if((int)frames_presented>=a && (int)frames_presented<=b){ host_mouse_dx+=dx; host_mouse_dy+=dy; } }
              const char *c=strchr(p,','); if(!c) break; p=c+1; } } }
        /* The gaze pointer: while a menu is up, steer the engine's own cursor
         * to where the viewer is looking (pointer.c); in play it does nothing. */
        host_pointer_servo();
        /* The audio queue's stall watchdog (directsound.c), unless the app
         * runs it on its own timer. */
        if(!host_dsound_watchdog_is_external()) host_dsound_watchdog();
        { /* Pad -> keyboard/mouse bridge: drive the WORKING keyboard-move + mouse-look input paths
             from the DualSense snapshot (hostgc_poll returns real hardware OR the probe's injected
             stick), so the controller moves+aims the player both in-probe and on-device. This
             sidesteps Halo's flaky PC analog-joystick binding. On by default (HALO_PAD2KEY=0 off). */
          const char *p2k=getenv("HALO_PAD2KEY");
          if(!p2k || strcmp(p2k,"0")!=0){
              extern uint8_t host_keyboard_state[256]; extern int32_t host_mouse_dx, host_mouse_dy;
              static uint8_t pk[256]; for(int i=0;i<256;i++) if(pk[i]){ host_keyboard_state[i]=0; pk[i]=0; }
              extern int host_dinput_menu_active(void);
              HostGCSnapshot pad; if(hostgc_poll(&pad) && pad.connected && !host_dinput_menu_active()){
                  const float dz=0.35f;
                  /* The trigger is the haptics' witness for the player's own shots. */
                  if(pad.rt>0.5f || pad.buttons[HOSTGC_BTN_RTRIGGER]) halo_haptics_note_fire();
                  if(pad.ly> dz){ host_keyboard_state[0x11]=0x80; pk[0x11]=1; } /* W forward */
                  if(pad.ly<-dz){ host_keyboard_state[0x1F]=0x80; pk[0x1F]=1; } /* S back    */
                  if(pad.lx<-dz){ host_keyboard_state[0x1E]=0x80; pk[0x1E]=1; } /* A strafe L */
                  if(pad.lx> dz){ host_keyboard_state[0x20]=0x80; pk[0x20]=1; } /* D strafe R */
                  const char *lse=getenv("HALO_LOOKSCALE"); float ls=lse?(float)atof(lse):25.0f;
                  if(pad.rx>dz||pad.rx<-dz) host_mouse_dx += (int32_t)(pad.rx*ls);       /* look yaw   */
                  if(pad.ry>dz||pad.ry<-dz) host_mouse_dy += (int32_t)(-pad.ry*ls);      /* look pitch */
              }
          }
        }
        if(getenv("HALO_CTLLOG") && frames_presented%60==0){
            /* trace the whole input->player chain: pad snapshot -> device packet (0x007124AC) ->
               control record (*(0x006B145C)+0x10). Shows exactly where input dies. */
            uint32_t pg=G32(0x0087a478u); unsigned idis = pg?*(uint8_t*)GPTR(pg+0x11u):255u;
            float mvx=0,mvy=0,aimx=0,aimy=0; memcpy(&mvx,GPTR(0x007124ACu),4); memcpy(&mvy,GPTR(0x007124B0u),4);
            memcpy(&aimx,GPTR(0x007124B4u),4); memcpy(&aimy,GPTR(0x007124B8u),4);
            uint32_t cr=G32(0x006B145Cu); float fwd=0,strafe=0,yaw=0; unsigned hdr=255u;
            if(cr){ hdr=*(uint8_t*)GPTR(cr+0x0Cu); memcpy(&fwd,GPTR(cr+0x24u),4); memcpy(&strafe,GPTR(cr+0x28u),4); memcpy(&yaw,GPTR(cr+0x1Cu),4); }
            extern uint8_t host_keyboard_state[256];
            HostGCSnapshot pad={0}; int pc=hostgc_poll(&pad);
            host_log("[ctl] frame=%u idis=%u hdr=%u kbdW=%u dev(mvx=%.2f mvy=%.2f aimx=%.2f aimy=%.2f) rec(fwd=%.2f str=%.2f yaw=%.3f) pad(conn=%d ly=%.2f lx=%.2f rx=%.2f)",
                frames_presented, idis, hdr, host_keyboard_state[0x11], mvx,mvy,aimx,aimy, fwd,strafe,yaw, pc?pad.connected:-1, pad.ly,pad.lx,pad.rx);
        }
        { const char *fm=getenv("HALO_SETMODE1_AT"); static int done=0; if(fm && !done && (int)frames_presented>=atoi(fm)){ done=1; *(int16_t*)GPTR(0x00719720u)=1; host_log("[force] set DAT_00719720=1 (SP mode) at frame %u", frames_presented); } }
        { const char *ms=getenv("HALO_MKSESSION_AT"); static int mdone=0; if(ms && !mdone && (int)frames_presented>=atoi(ms)){ mdone=1;
            extern uint32_t host_call_guest(EngineCPU*,uint32_t,int,const uint32_t*,int);
            uint32_t fn=(uint32_t)strtoul(getenv("HALO_MKSESSION_FN")?getenv("HALO_MKSESSION_FN"):"0049d210",0,16);
            host_log("[mksession] calling FUN_%08x at frame %u (c2d8=%08x)", fn, frames_presented, G32(0x0071c2d8u));
            host_call_guest(cpu, fn, 0, 0, 0);
            host_log("[mksession] returned; c2d8=%08x c2d4=%08x mode=%d", G32(0x0071c2d8u), G32(0x0071c2d4u), (int)*(int16_t*)GPTR(0x00719720u)); } }
        { static int sw=-1; if(sw<0) sw=getenv("HALO_SAVEWATCH")?1:0;
          if(sw && frames_presented%60==0) host_log("[save] e3000_busy=%u e3001=%u reverted=%u", (unsigned)*(uint8_t*)GPTR(0x006e3000u), (unsigned)*(uint8_t*)GPTR(0x006e3001u), (unsigned)*(uint8_t*)GPTR(0x006e3009u));
          if(getenv("HALO_SAVEFORCE")) *(uint8_t*)GPTR(0x006e3000u)=0; }
        /* generic runtime-configurable memory pokes/peeks: address supplied by env so
           new candidate globals can be tested WITHOUT rebuilding (run-only cycle). */
        { const char *p=getenv("HALO_POKE8"); if(p){ while(*p){ unsigned a=0,v=0;
              if(sscanf(p,"%x=%u",&a,&v)==2 && a>=0x400000u && a<0x00c00000u) *(uint8_t*)GPTR((uint32_t)a)=(uint8_t)v;
              const char *c=strchr(p,','); if(!c) break; p=c+1; } } }
        { const char *p=getenv("HALO_POKE32"); if(p){ while(*p){ unsigned a=0,v=0;
              if(sscanf(p,"%x=%u",&a,&v)==2 && a>=0x400000u && a<0x00c00000u) *(uint32_t*)GPTR((uint32_t)a)=(uint32_t)v;
              const char *c=strchr(p,','); if(!c) break; p=c+1; } } }
        { const char *p=getenv("HALO_PEEK"); if(p && frames_presented%60==0){ char buf[512]; int n=0; const char *q=p;
              while(*q && n<400){ unsigned a=0; if(sscanf(q,"%x",&a)==1 && a>=0x400000u && a<0x00c00000u)
                  n+=snprintf(buf+n,sizeof(buf)-n,"%06x=%u(0x%x) ",a,(unsigned)G32((uint32_t)a),(unsigned)G32((uint32_t)a));
                const char *c=strchr(q,','); if(!c) break; q=c+1; }
              host_log("[peek] %s", buf); } }
        /* force campaign difficulty (0=easy 1=normal 2=heroic 3=legendary): write BOTH the
           options global (0x00696564) and the live game_globals field (*(0x006b0b80)+0x0E).
           On normal-solo, a10 mission script takes the tutorial branch; heroic makes it wake
           fast_setup (tick-timed cryo-exit) which sets mark_fast_setup and unblocks mission_a10. */
        { const char *fd=getenv("HALO_FORCEDIFF"); if(fd){ int16_t d=(int16_t)atoi(fd);
              *(int16_t*)GPTR(0x00696564u)=d;
              uint32_t gg=G32(0x006b0b80u); if(gg) *(int16_t*)GPTR(gg+0x0Eu)=d; } }
        /* directly set HSC scenario boolean global(s) by descriptor index: "idx=val,idx=val".
           slot = (idx + 0x1eb)*8, value byte at +4. base = *(*(0x0087a46c)+0x34).
           e.g. HALO_SETGLOBAL=40=1 sets mark_fast_setup true (guaranteed mission_a10 unblock). */
        { const char *sg=getenv("HALO_SETGLOBAL"); if(sg){ uint32_t hdr=G32(0x0087a46cu);
              if(hdr){ uint32_t gb=G32(hdr+0x34u); if(gb){ const char *p=sg; while(*p){ unsigned idx=0,v=0;
                  if(sscanf(p,"%u=%u",&idx,&v)==2){ uint32_t slot=(idx+0x1ebu)*8u+4u; *(uint8_t*)GPTR(gb+slot)=(uint8_t)v; }
                  const char *c=strchr(p,','); if(!c) break; p=c+1; } } } } }
        /* Diagnostic shortcut only, explicitly enabled with HALO_A10_AUTOPLAY=1.
           This changes difficulty, script progress, save state and input gates.
           It must not run by default or establish campaign-playability evidence.
           The original Normal branch wakes tutorial_setup; fast_setup calls
           cinematic_stop, whose core 00449EB0 re-enables player input. */
        { const char *ap=getenv("HALO_A10_AUTOPLAY"); if(ap && strcmp(ap,"1")==0){
              *(int16_t*)GPTR(0x00696564u)=2;                                 /* options difficulty = Heroic */
              uint32_t gg=G32(0x006b0b80u); if(gg) *(int16_t*)GPTR(gg+0x0Eu)=2;/* live difficulty field */
              *(uint8_t*)GPTR(0x006e3000u)=0;                                 /* clear save-busy */
              uint32_t hdr=G32(0x0087a46cu); if(hdr){ uint32_t gb=G32(hdr+0x34u);
                  if(gb) *(uint8_t*)GPTR(gb+0x109Cu)=1; }                     /* mark_fast_setup = true */
              /* Bypass gates only for this explicit diagnostic mode. Normal
                 cinematic/tutorial execution owns these fields otherwise. */
              uint32_t pg=G32(0x0087a478u); if(pg) *(uint8_t*)GPTR(pg+0x11u)=0;
              uint32_t cr=G32(0x006B145Cu); if(cr){ uint8_t*h=(uint8_t*)GPTR(cr+0x0Cu); *h=(uint8_t)(*h & ~1u); } }
        }
        /* wake a dormant HSC thread by script index: scan thread array, if a thread for
           that script exists and is sleeping, set wake<=now so the scheduler runs it. */
        { const char *ws=getenv("HALO_WAKE_SCRIPT"); if(ws){ int want=atoi(ws);
              uint32_t ta=G32(0x0087A470u); uint32_t tm=G32(0x006f1d6cu); int now=tm?(int)G32(tm+0xcu):0;
              if(ta){ uint32_t tbase=G32(ta+0x34u); int last=(int)(*(int16_t*)GPTR(ta+0x2eu));
                  if(tbase && last>0 && last<=2048) for(int t=0;t<last;t++){ uint32_t e=tbase+(uint32_t)t*0x218u;
                      if(!*(uint16_t*)GPTR(e)) continue; if((int)G32(e+4u)==want){ S32(e+8u,(uint32_t)now); } } } } }
        if(getenv("HALO_SCAN_THREADS") && frames_presented%90==0){
            uint32_t ta=G32(0x0087A470u); uint32_t tm=G32(0x006f1d6cu); int now=tm?(int)G32(tm+0xcu):-1;
            if(ta){ uint32_t tbase=G32(ta+0x34u); int last=(int)(*(int16_t*)GPTR(ta+0x2eu));
                if(tbase && last>0 && last<=2048){
                    host_log("[hsc] scheduler_enabled=%u now=%d threads(last=%d):", (unsigned)*(uint8_t*)GPTR(0x006b15e8u), now, last);
                    for(int t=0;t<last;t++){ uint32_t e=tbase+(uint32_t)t*0x218u;
                        if(!*(uint16_t*)GPTR(e)) continue;
                        int sidx=(int)G32(e+4u); int wake=(int)G32(e+8u); uint8_t typ=*(uint8_t*)GPTR(e+2u);
                        host_log("  thr%d script=%d type=%u wake=%d %s", t, sidx, typ, wake, (wake>now)?"SLEEPING":"runnable"); }
                } } }
        if(getenv("HALO_SCAN_DEVICES") && frames_presented%120==0){ extern double sqrt(double);
            uint32_t oa=G32(0x008603B0u);
            if(oa){ uint32_t esize=(uint32_t)(*(uint16_t*)GPTR(oa+0x22u)); uint32_t base=G32(oa+0x34u); int last=(int)(*(int16_t*)GPTR(oa+0x2eu));
                if(esize>=8 && esize<=64 && last>0 && last<=4096){
                    /* player position */
                    uint32_t pg=G32(0x0087A478u), player=pg?G32(pg+4):0xffffffffu;
                    uint32_t pa=G32(0x0087A480u), pesz=pa?(uint32_t)(*(uint16_t*)GPTR(pa+0x22u)):0, pbase=pa?G32(pa+0x34u):0;
                    uint32_t pr=(pbase&&player!=0xffffffffu)?pbase+(player&0xffffu)*pesz:0;
                    uint32_t punit=pr?G32(pr+0x34u):0xffffffffu;
                    uint32_t pbody=(base&&punit!=0xffffffffu)?G32(base+(punit&0xffffu)*esize+8u):0;
                    float pp[3]={0,0,0}; if(pbody) memcpy(pp,GPTR(pbody+0x5cu),12);
                    int found=0;
                    for(int idx=0; idx<last && found<10; idx++){ uint32_t elem=base+(uint32_t)idx*esize;
                        if(!*(uint16_t*)GPTR(elem)) continue; uint32_t body=G32(elem+8u);
                        if(!body || body<0x10000u) continue;
                        if(*(uint8_t*)GPTR(body+0xB4u)!=7) continue; /* machine */
                        float mp[3]; memcpy(mp,GPTR(body+0x5cu),12);
                        float dx=mp[0]-pp[0],dy=mp[1]-pp[1],dz=mp[2]-pp[2]; float d2=dx*dx+dy*dy+dz*dz;
                        if(d2>225.0f) continue; found++;
                        if(getenv("HALO_OPEN_DEVICES")){ float one=1.0f; memcpy(GPTR(body+0x1FCu),&one,4); memcpy(GPTR(body+0x200u),&one,4); }
                        float f[10]; for(int k=0;k<10;k++) memcpy(&f[k],GPTR(body+0x1F0u+4u*k),4);
                        host_log("[dev] idx=%d body=%08x pos=%.2f,%.2f,%.2f dist=%.2f f[1F0..214]=%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
                            idx,body,mp[0],mp[1],mp[2],(float)sqrt(d2),f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9]);
                    }
                    if(pbody) host_log("[dev] player body=%08x pos=%.2f,%.2f,%.2f found=%d machines nearby", pbody,pp[0],pp[1],pp[2],found);
                } } }
        if(getenv("HALO_TRACE")){ uint32_t cx=G32(0x0071c2d8u); uint32_t cd4=G32(0x0071c2d4u); uint32_t simc=G32(0x0069c648u); uint32_t gc2=G32(0x006f1d6cu); unsigned gb2=gc2?(unsigned)*(uint8_t*)GPTR(gc2+2):9;
            int mode=(int)*(int16_t*)GPTR(0x00719720u); unsigned req=(unsigned)*(uint8_t*)GPTR(0x00719739u); unsigned shell=(unsigned)*(uint8_t*)GPTR(0x00718FC9u);
            char scr[32]={0}; uint32_t shobj=G32(0x00718f94u); if(shobj){ uint32_t np=G32(shobj+4); if(np){ char*s=(char*)GPTR(np); for(int i=0;i<31&&s[i];i++) scr[i]=s[i]; } }
            static int lm=-9,lreq=-9,lsh=-9; static uint32_t lcx=0xffffffffu; static char lscr[32]={0};
            int changed = mode!=lm||((int)req)!=lreq||((int)shell)!=lsh||cx!=lcx||strcmp(scr,lscr);
            if(changed || frames_presented%60==0){ host_log("[trace] f=%u mode=%d req739=%u c2d8=%08x shell=%u c2d4=%08x sim=%u f1d6c2=%u camp778=%u map=%.20s screen='%s'%s", frames_presented, mode, req, cx, shell, cd4, simc, gb2, (unsigned)*(uint8_t*)GPTR(0x00719778u), (char*)GPTR(0x00719779u), scr, changed?" *":"");
                lm=mode; lreq=req; lsh=shell; lcx=cx; snprintf(lscr,sizeof lscr,"%s",scr); } }
        { const char *cf=getenv("HALO_CONSOLE_FRAME"), *cc=getenv("HALO_CONSOLE_CMD"); static uint32_t did=0;
          if(cf && cc && frames_presented==(uint32_t)atoi(cf) && !(did & 1u)){ did|=1u;
              extern uint32_t guest_strdup(const char*); extern uint32_t host_call_guest(EngineCPU*,uint32_t,int,const uint32_t*,int);
              uint32_t p=guest_strdup(cc); cpu->gpr[7]=p; uint32_t flag=0; host_log("[console] exec: %s", cc);
              host_call_guest(cpu, 0x004c6a80u, 1, &flag, 0); host_log("[console] exec done"); } }
        { const char *lm=getenv("HALO_LOADMAP"), *lf=getenv("HALO_LOADMAP_FRAME"); static uint32_t ldid=0;
          if(lm && lf){ uint32_t f0=(uint32_t)atoi(lf);
            if(frames_presented==f0 && !ldid){ ldid=1;
              char *dst=(char*)GPTR(0x00719779u); size_t i=0; for(;lm[i] && i<254;i++) dst[i]=lm[i]; dst[i]=0;
              if(getenv("HALO_CAMPAIGN")){ /* call the real campaign-start FUN_0049cfd0(param1, diffstruct) */
                  extern uint32_t guest_alloc(uint32_t); extern uint32_t host_call_guest(EngineCPU*,uint32_t,int,const uint32_t*,int);
                  int diff=getenv("HALO_DIFF")?atoi(getenv("HALO_DIFF")):1;
                  S32(0x006894b8u,1); /* single-player => FUN_0049cfd0 takes its solo branch */
                  uint32_t ds=guest_alloc(16); for(int b=0;b<16;b++) S8(ds+b,0); *(uint16_t*)GPTR(ds+2)=(uint16_t)diff;
                  uint32_t args2[2]={0u, ds};
                  host_log("[campaign] calling FUN_0049cfd0 (start solo campaign a10, diff=%d)", diff);
                  host_call_guest(cpu, 0x0049cfd0u, 2, args2, 0);
                  host_log("[campaign] returned"); }
              else S8(0x00719739u,1); /* request map load; the game's own main loop pumps FUN_004c9770 */
              host_log("[loadmap] requested map=%s", dst); }
            { const char *gm=getenv("HALO_GAMEMODE"), *gmf=getenv("HALO_GAMEMODE_FRAME"); static uint32_t gmdone=0;
              if(gm && frames_presented==(uint32_t)(gmf?atoi(gmf):(int)f0) && !gmdone){ gmdone=1;
                uint32_t addr = !strcmp(gm,"e8")?0x007196E8u : !strcmp(gm,"f4")?0x007196F4u : 0x007196D8u; S32(addr,1);
                host_log("[loadmap] gamemode set %08x=1 at frame %u (tick the loaded mission)", addr, frames_presented); } }
            if(ldid && frames_presented%30==0){ uint32_t gc=G32(0x006f1d6cu); uint32_t cx=G32(0x0071c2d8u); uint32_t cd4=G32(0x0071c2d4u); uint32_t simc=G32(0x0069c648u); uint32_t gc2=G32(0x006f1d6cu); unsigned gb2=gc2?(unsigned)*(uint8_t*)GPTR(gc2+2):9;
                unsigned b0=gc?(unsigned)*(uint8_t*)GPTR(gc):0xff, b1=gc?(unsigned)*(uint8_t*)GPTR(gc+1):0xff, b2=gc?(unsigned)*(uint8_t*)GPTR(gc+2):0xff;
                unsigned st=cx?(unsigned)*(uint16_t*)GPTR(cx+0xeda):0xffff;
                host_log("[loadmap] f=%u req739=%u mode719720=%d 006f1d6c=%08x[%u %u %u] 0071c2d8=%08x eda=%u shell718FC9=%u", frames_presented, (unsigned)*(uint8_t*)GPTR(0x00719739u), (int)*(int16_t*)GPTR(0x00719720u), gc,b0,b1,b2, cx,st, (unsigned)*(uint8_t*)GPTR(0x00718FC9u)); } } }
        D3DObj *bb = obj_from_guest(the_device->data);
        host_panorama_gpu_carry_missing();
        int gpu_slot = host_panorama_gpu_slot();
        if (gpu_slot >= 0 && panorama_info.valid && window_ready == 1 && bb) {
            /* Zero-copy frame: the views and HUD were blitted into the platform pool; the
             * bridge publishes them when the GPU finishes. No readback, no CPU copies. */
            /* Layers the rotating schedule left standing this frame live in
             * the previously published slot, not this one. */
            metalwin_present_gpu(gpu_slot, (int)bb->width, (int)bb->height); host_panorama_gpu_handoff();
            fp_capture_finish(panorama_info.status,panorama_info.source_epoch);model_capture_finish(panorama_info.status,panorama_info.source_epoch);host_panorama_present_complete(); if (metalwin_should_close()) host_exit(0);
        } else if (host_panorama_gpu_sink_active() && panorama_info.status == HALO_PANORAMA_WORLD_INCOMPLETE && window_ready == 1 && bb) {
            /* Dropped world frame (pool busy or a pass failed): with a zero-copy sink the old
             * fallback, a GPU readback plus a 20 MB flat copy, only stalled the engine thread. */
            metalwin_present_dropped((int)bb->width, (int)bb->height);
            fp_capture_finish(panorama_info.status,panorama_info.source_epoch);model_capture_finish(panorama_info.status,panorama_info.source_epoch);host_panorama_present_complete(); if (metalwin_should_close()) host_exit(0);
        } else {
        if(surface_flush(bb)) { fp_capture_finish(HALO_PANORAMA_WORLD_INCOMPLETE,panorama_info.source_epoch);model_capture_finish(HALO_PANORAMA_WORLD_INCOMPLETE,panorama_info.source_epoch);host_panorama_present_complete(); RET_STDCALL(D3DERR_NOTAVAILABLE,5); }
        if (window_ready == 1 && bb && bb->data) { metalwin_present(GPTR(bb->data), (int)bb->width, (int)bb->height); fp_capture_finish(panorama_info.status,panorama_info.source_epoch);model_capture_finish(panorama_info.status,panorama_info.source_epoch);host_panorama_present_complete(); if (metalwin_should_close()) host_exit(0); }
        }
        const char *capture=getenv("HALO_FRAME_CAPTURE");
        const char *capture_every=getenv("HALO_FRAME_CAPTURE_EVERY");
        uint32_t capture_interval=250;
        if(capture_every){uint32_t parsed;if(render_diag_number(capture_every,&parsed))capture_interval=parsed;}
        if(capture && bb && (frames_presented==1||frames_presented==(uint32_t)host_frame_limit||(capture_interval&&frames_presented%capture_interval==0)||(getenv("HALO_CAPTURE_DENSE")&&frames_presented%20==0))){
            char path[2048];snprintf(path,sizeof path,"%s/frame-%04u.bgra",capture,frames_presented);FILE *f=fopen(path,"wb");if(f){fwrite(GPTR(bb->data),1,bb->size,f);fclose(f);}
            snprintf(path,sizeof path,"%s/frame-%04u.json",capture,frames_presented);f=fopen(path,"w");if(f){fprintf(f,"{\"width\":%u,\"height\":%u,\"frames\":%u,\"successfulDraws\":%u}\n",bb->width,bb->height,frames_presented,draw_calls);fclose(f);}
        }
        if (frames_presented <= 3 || frames_presented % 60 == 0) host_log("d3d9 Present: frame %u (%u draw calls)", frames_presented, draw_calls);
        draw_traffic_report();
        if (host_frame_limit && frames_presented >= (uint32_t)host_frame_limit) { uint64_t real_d = 0, fb = 0; mr_program_compile_stats(&real_d, &fb); host_log("[shader] real pipeline: %llu draws, %llu fallbacks", (unsigned long long)real_d, (unsigned long long)fb); render_surface_report(); host_log("frame limit reached"); host_exit(0); } RET_STDCALL(D3D_OK, 5); }

    case 19: { uint32_t r = ARG(2); S32(r, 1); S32(r + 4, 0); RET_STDCALL(D3D_OK, 3); }
    case 22: { uint32_t r = ARG(2); for (uint32_t k = 0; k < 256; k++) { S16(r + 2 * k, (uint16_t)(k * 257)); S16(r + 512 + 2 * k, (uint16_t)(k * 257)); S16(r + 1024 + 2 * k, (uint16_t)(k * 257)); } RET_STDCALL(D3D_OK, 3); }
    case 23: RET_STDCALL(create_texture(o,K_TEXTURE,ARG(1),ARG(2),1,ARG(3),ARG(4),ARG(5),ARG(6),ARG(7)),9);
    case 24: RET_STDCALL(create_texture(o,K_VOLTEX,ARG(1),ARG(2),ARG(3),ARG(4),ARG(5),ARG(6),ARG(7),ARG(8)),10);
    case 25: RET_STDCALL(create_texture(o,K_CUBETEX,ARG(1),ARG(1),1,ARG(2),ARG(3),ARG(4),ARG(5),ARG(6)),8);
    case 26: case 27: RET_STDCALL(create_buffer(o,i==26?K_VB:K_IB,ARG(1),ARG(2),ARG(3),ARG(4),ARG(5)),7);
    case 28: case 29: RET_STDCALL(create_surface(o,ARG(1),ARG(2),ARG(3),i==28?1:2,0,ARG(7)),9);
    case 36: RET_STDCALL(create_surface(o,ARG(1),ARG(2),ARG(3),0,ARG(4),ARG(5)),7);
    case 41: case 42: RET_STDCALL(D3D_OK, 1);
    /* A full colour clear replaces the entire target. Keep its GPU copy
     * authoritative: CPU readers already call surface_flush, and a following
     * draw must not upload the stale guest image over this clear. Depth-only
     * clears still prepare the current colour contents normally. */
    case 43: {
        if(ARG(1)) RET_STDCALL(D3DERR_NOTAVAILABLE,7);
        D3DObj *rt = rt_surface();
        if(rt && (ARG(3)&7)){
            if(ARG(3)&1)rt->gpu_dirty=1;
            if(surface_prepare(rt)) RET_STDCALL(D3DERR_NOTAVAILABLE,7);
            if(ARG(3)&1){mr_clear(rt->renderer,ARG(4));content_changed(rt);}
            if(ARG(3)&2){float depth;uint32_t bits=ARG(5);memcpy(&depth,&bits,4);mr_clear_depth(rt->renderer,depth);}
            if(ARG(3)&4)mr_clear_stencil(rt->renderer,ARG(6));
        }
        RET_STDCALL(D3D_OK, 7); }
    case 45: { uint32_t m = ARG(2); memset(GPTR(m), 0, 64); float one = 1.0f; memcpy(GPTR(m), &one, 4); memcpy(GPTR(m + 20), &one, 4); memcpy(GPTR(m + 40), &one, 4); memcpy(GPTR(m + 60), &one, 4); RET_STDCALL(D3D_OK, 3); }
    case 48: { uint32_t v = ARG(1); S32(v, 0); S32(v + 4, 0); S32(v + 8, backbuffer_w); S32(v + 12, backbuffer_h); float z = 0.f; memcpy(GPTR(v + 16), &z, 4); z = 1.f; memcpy(GPTR(v + 20), &z, 4); RET_STDCALL(D3D_OK, 2); }
    case 49: { uint32_t p=ARG(1);if(!p)RET_STDCALL(D3DERR_INVALIDCALL,2);HostD3DMaterial m;memcpy(&m,GPTR(p),sizeof m);uint32_t hr=lighting_set_material(&m);if(hr)RET_STDCALL(hr,2);if(getenv("HALO_FF3D_LOG")){static int reports;if(reports++<12)host_log("[lighting] material diffuse=%.3f,%.3f,%.3f ambient=%.3f,%.3f,%.3f emissive=%.3f,%.3f,%.3f power=%.2f",m.diffuse[0],m.diffuse[1],m.diffuse[2],m.ambient[0],m.ambient[1],m.ambient[2],m.emissive[0],m.emissive[1],m.emissive[2],m.power);}RET_STDCALL(D3D_OK,2); }
    case 50: { uint32_t p=ARG(1);if(!p)RET_STDCALL(D3DERR_INVALIDCALL,2);memcpy(GPTR(p),&current_material,sizeof current_material);RET_STDCALL(D3D_OK,2); }
    case 51: { uint32_t index=ARG(1),p=ARG(2);if(!p)RET_STDCALL(D3DERR_INVALIDCALL,3);HostD3DLight l;memcpy(&l,GPTR(p),sizeof l);uint32_t hr=lighting_set_light(index,&l);if(hr)RET_STDCALL(hr,3);if(getenv("HALO_FF3D_LOG")){static int reports;if(reports++<12)host_log("[lighting] light %u type=%u diffuse=%.3f,%.3f,%.3f direction=%.3f,%.3f,%.3f range=%.2f",index,l.type,l.diffuse[0],l.diffuse[1],l.diffuse[2],l.direction[0],l.direction[1],l.direction[2],l.range);}RET_STDCALL(D3D_OK,3); }
    case 52: { uint32_t index=ARG(1),p=ARG(2);if(index>=HOST_D3D_MAX_LIGHTS||!p||!light_slots[index].defined)RET_STDCALL(D3DERR_INVALIDCALL,3);memcpy(GPTR(p),&light_slots[index].light,sizeof light_slots[index].light);RET_STDCALL(D3D_OK,3); }
    case 53: { uint32_t index=ARG(1),enable=ARG(2)!=0;uint32_t hr=lighting_enable(index,enable);if(hr)RET_STDCALL(hr,3);if(getenv("HALO_FF3D_LOG")){static int reports;if(reports++<12)host_log("[lighting] light %u %s",index,enable?"enabled":"disabled");}RET_STDCALL(D3D_OK,3); }
    case 54: { uint32_t index=ARG(1),p=ARG(2);if(index>=HOST_D3D_MAX_LIGHTS||!p||!light_slots[index].defined)RET_STDCALL(D3DERR_INVALIDCALL,3);S32(p,light_slots[index].enabled?1u:0u);RET_STDCALL(D3D_OK,3); }
    case 44: case 46: case 47: case 55: case 57: case 65: case 67: case 69: case 71: case 73: case 75: case 77: case 79: case 87: case 89: case 92: case 94: case 96: case 98: case 100: case 102: case 104: case 107: case 109: case 111: case 113:
        RET_STDCALL(D3D_OK, classes[K_DEVICE].methods[i].argc);
    case 58: S32(ARG(2), 0); RET_STDCALL(D3D_OK, 3);
    case 70: S32(ARG(1), 1); RET_STDCALL(D3D_OK, 2);
    case 78: RET_STDCALL(0, 1);
    case 80: engine_fp_push(cpu, 0.0); RET_STDCALL(0, 1);
    case 81: case 82: case 83: case 84: draw_calls++; RET_STDCALL(D3D_OK, classes[K_DEVICE].methods[i].argc);
    case 86: { D3DObj *d = obj_new(K_VDECL); obj_attach_device(d,o); uint32_t el = ARG(1); uint32_t n = 0; while (n < 64 && G16(el + 8 * n) != 0xFF) n++; d->size = 8 * (n + 1); d->data = guest_alloc(d->size); memcpy(GPTR(d->data), GPTR(el), d->size); S32(ARG(2), d->guest); RET_STDCALL(D3D_OK, 3); }
    case 91: case 106: { D3DObj *s = obj_new(i == 91 ? K_VSHADER : K_PSHADER); obj_attach_device(s,o); uint32_t fn = ARG(1); uint32_t n = 0; while (n < 65536 && G32(fn + 4 * n) != 0x0000FFFFu) n++; s->size = 4 * (n + 1);
        const void *shader_bytes=GPTR(fn);
        const HaloShaderMod *replacement=i==106?shader_mod_lookup(shader_bytes,s->size):NULL;
        if(replacement){shader_bytes=replacement->replacement;s->size=replacement->replacement_size;}
        s->data = guest_alloc(s->size); memcpy(GPTR(s->data), shader_bytes, s->size);
        { /* Token hash, computed once, so pipeline lookups do not rehash the shader per draw. */
          uint64_t h = UINT64_C(1469598103934665603); const uint8_t *b = GPTR(s->data);
          for (uint32_t k = 0; k < s->size; k++) { h ^= b[k]; h *= UINT64_C(1099511628211); }
          s->content_key = h ? h : 1; }
        /* Translate and compile it now, off the engine thread, not at its first draw. */
        mr_shader_created(i == 106, (const uint32_t *)GPTR(s->data), s->size, s->content_key);
        if (i == 91) {
            s->radial_fog_terms = halo_radial_fog_terms(GPTR(s->data), s->size, HALO_RADIAL_FOG_VIEW_PLANE);
            s->radial_fog_view_plane = s->radial_fog_terms > halo_radial_fog_terms(GPTR(s->data), s->size, HALO_RADIAL_FOG_DEPTH);
            static unsigned reported;
            if (s->radial_fog_terms > 0 && reported++ < 64)
                host_log("[radial-fog] vertex shader %08X: %d eligible fog term%s; enabled=%d (panorama only)",
                         s->guest, s->radial_fog_terms, s->radial_fog_terms == 1 ? "" : "s", halo_settings_radial_fog());
        }
        capture_shader(s); S32(ARG(2), s->guest); RET_STDCALL(D3D_OK, 3); }
    case 118: { D3DObj *q = obj_new(K_QUERY); q->type = ARG(1); S32(ARG(2), q->guest); RET_STDCALL(D3D_OK, 3); }
    default: unimplemented(cpu, &classes[K_DEVICE], i);
    }
}
static void write_surface_desc(uint32_t d, D3DObj *o, uint32_t w, uint32_t h) { S32(d, o->format); S32(d + 4, 1); S32(d + 8, o->usage); S32(d + 12, o->pool); S32(d + 16, 0); S32(d + 20, 0); S32(d + 24, w); S32(d + 28, h); }
static uint32_t resource_get_device(D3DObj *o,uint32_t out){
    if(!out)return D3DERR_INVALIDCALL;
    D3DObj *resource=o->owner?obj_find(o->owner):o;
    D3DObj *device=resource?obj_find(resource->resource_device):NULL;
    if(!device)device=the_device; /* implicit device-owned back/depth buffers */
    obj_retain(device);S32(out,device?device->guest:0);return device?D3D_OK:D3DERR_INVALIDCALL;
}
static uint32_t resource_get_container(D3DObj *o,uint32_t iid,uint32_t out){
    if(!out||!iid)return D3DERR_INVALIDCALL;S32(out,0);
    D3DObj *parent=obj_find(o->owner);
    if(!parent)parent=obj_find(o->resource_device); /* stand-alone surface */
    if(!parent)return 0x80004002u;
    /* Standard COM interface identifiers, stored as their little-endian words. */
    static const uint32_t unknown[4]={0,0,0x000000c0,0x46000000};
    static const uint32_t resource[4]={0x05eec05d,0x43628f7d,0xbad199b9,0x04c757f3};
    static const uint32_t base[4]={0x580ca87e,0x4d541d3c,0xd3b71d99,0xce98c2e3};
    static const uint32_t texture[4]={0x85c31227,0x4f003de5,0x1af13a9b,0xb5188cc3};
    static const uint32_t cube[4]={0xfff32f81,0x473ad953,0xd6932392,0x3fa9ab52};
    static const uint32_t volume[4]={0x2518526c,0x4111e789,0xef47b9a7,0xe6138d32};
    static const uint32_t device[4]={0xd0223b96,0x43fdbf7a,0x3ba4bd92,0xebb9820d};
    if(parent->kind==K_DEVICE){
        if(memcmp(GPTR(iid),unknown,16)&&memcmp(GPTR(iid),device,16))return 0x80004002u;
        obj_retain(parent);S32(out,parent->guest);return D3D_OK;
    }
    const uint32_t *specific=parent->kind==K_TEXTURE?texture:parent->kind==K_CUBETEX?cube:volume;
    if(memcmp(GPTR(iid),unknown,16)&&memcmp(GPTR(iid),resource,16)&&memcmp(GPTR(iid),base,16)&&memcmp(GPTR(iid),specific,16))return 0x80004002u;
    obj_retain(parent);S32(out,parent->guest);return D3D_OK;
}
static void write_volume_desc(uint32_t d,D3DObj *o,uint32_t w,uint32_t h,uint32_t depth){
    S32(d,o->format);S32(d+4,2);S32(d+8,o->usage);S32(d+12,o->pool);S32(d+16,w);S32(d+20,h);S32(d+24,depth);
}
static void method_texture(EngineCPU *cpu,D3DObj *o,int i){
    uint32_t argc=classes[o->kind].methods[i].argc;
    switch(i){
    case 0: if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,3);obj_retain(o);S32(ARG(2),o->guest);RET_STDCALL(D3D_OK,3);
    case 1: RET_STDCALL(obj_retain(o),1);case 2: RET_STDCALL(obj_release(o),1);
    case 3: RET_STDCALL(resource_get_device(o,ARG(1)),2);
    case 8: RET_STDCALL(0,1);case 10: RET_STDCALL(o->type,1);case 12: RET_STDCALL(0,1);case 13: RET_STDCALL(o->levels,1);case 15: RET_STDCALL(2,1);
    case 4: case 6: case 7: case 9: case 11: case 14: case 16: RET_STDCALL(D3D_OK,argc);
    default: break;
    }
    if(i==21){if(o->kind==K_CUBETEX&&ARG(1)>=6)RET_STDCALL(D3DERR_INVALIDCALL,argc);o->content_generation++;RET_STDCALL(D3D_OK,argc);}
    uint32_t face=0,level=ARG(1);
    if(o->kind==K_CUBETEX&&i>=18){face=ARG(1);level=ARG(2);}
    if(face>=6||level>=o->levels||level>=16)RET_STDCALL(D3DERR_INVALIDCALL,argc);
    uint32_t w=o->width>>level,h=o->height>>level,d=o->depth>>level;if(!w)w=1;if(!h)h=1;if(!d)d=1;
    uint32_t pitch,size=level_size(o->format,w,h,&pitch);if(o->kind==K_VOLTEX)size*=d;
    uint32_t *slot=o->kind==K_CUBETEX?&o->face_data[face][level]:&o->level_data[level];
    uint32_t *alias=o->kind==K_CUBETEX?&o->face_surface[face][level]:&o->level_surface[level];
    if(i==17){
        if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,argc);
        if(o->kind==K_VOLTEX)write_volume_desc(ARG(2),o,w,h,d);else write_surface_desc(ARG(2),o,w,h);
        RET_STDCALL(D3D_OK,argc);
    }
    if(i==18){
        uint32_t out=o->kind==K_CUBETEX?ARG(3):ARG(2);if(!out)RET_STDCALL(D3DERR_INVALIDCALL,argc);
        if(!*slot)*slot=guest_page_alloc(size?size:16);
        D3DObj *child=obj_find(*alias);
        if(!child){
            child=obj_new(o->kind==K_VOLTEX?K_VOLUME:K_SURFACE);child->refs=0;child->owner=o->guest;
            child->width=w;child->height=h;child->depth=d;child->format=o->format;child->usage=o->usage;child->pool=o->pool;
            child->size=size;child->data=*slot;child->type=o->kind==K_VOLTEX?2:1;*alias=child->guest;
        }
        obj_retain(child);S32(out,child->guest);RET_STDCALL(D3D_OK,argc);
    }
    if(i==19){
        uint32_t out=o->kind==K_CUBETEX?ARG(3):ARG(2);if(!out)RET_STDCALL(D3DERR_INVALIDCALL,argc);
        D3DObj *child=obj_find(*alias);if(child&&surface_flush(child))RET_STDCALL(D3DERR_NOTAVAILABLE,argc);
        if(!*slot)*slot=guest_page_alloc(size?size:16);o->content_generation++;
        if(o->kind==K_VOLTEX){S32(out,pitch);S32(out+4,pitch*h);S32(out+8,*slot);}else{S32(out,pitch);S32(out+4,*slot);}
        RET_STDCALL(D3D_OK,argc);
    }
    if(i==20){o->content_generation++;RET_STDCALL(D3D_OK,argc);}
    unimplemented(cpu,&classes[o->kind],i);
}
static void method_buffer(EngineCPU *cpu, D3DObj *o, int i) {
    switch (i) {
    case 0: if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,3);obj_retain(o);S32(ARG(2),o->guest);RET_STDCALL(D3D_OK,3);
    case 1: RET_STDCALL(obj_retain(o),1);case 2: RET_STDCALL(obj_release(o),1);
    case 3: if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,2);{D3DObj *d=obj_find(o->resource_device);obj_retain(d);S32(ARG(1),d?d->guest:0);}RET_STDCALL(D3D_OK,2);
    case 8: RET_STDCALL(0, 1); case 10: RET_STDCALL(o->type, 1);
    case 4: case 6: case 7: case 9: RET_STDCALL(D3D_OK, classes[o->kind].methods[i].argc);
    case 11: {uint32_t off=ARG(1),size=ARG(2);if(!ARG(3)||off>o->size||size>o->size-off)RET_STDCALL(D3DERR_INVALIDCALL,5);
        /* NOOVERWRITE/DISCARD identify streaming use even if the caller omitted
         * D3DUSAGE_DYNAMIC. Keep those buffers on the existing per-draw copy. */
        if(ARG(4)&(0x1000u|0x2000u))vb_resident_retire(o);
        ++o->locked;o->content_generation++;S32(ARG(3),o->data+off);RET_STDCALL(D3D_OK,5); }
    case 12: if(!o->locked)RET_STDCALL(D3DERR_INVALIDCALL,1);--o->locked;o->content_generation++;RET_STDCALL(D3D_OK,1);
    case 13: { uint32_t d = ARG(1); S32(d, o->kind == K_VB ? 0 : o->format); S32(d + 4, o->type); S32(d + 8, o->usage); S32(d + 12, o->pool); S32(d + 16, o->size); if (o->kind == K_VB) S32(d + 20, o->fvf); RET_STDCALL(D3D_OK, 2); }
    default: unimplemented(cpu, &classes[o->kind], i);
    }
}
static void method_surface(EngineCPU *cpu, D3DObj *o, int i) {
    switch (i) {
    case 0: if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,3);obj_retain(o);S32(ARG(2),o->guest);RET_STDCALL(D3D_OK,3);
    case 1: RET_STDCALL(obj_retain(o),1);case 2: RET_STDCALL(obj_release(o),1);
    case 3: RET_STDCALL(resource_get_device(o,ARG(1)),2);
    case 8: RET_STDCALL(0, 1); case 10: RET_STDCALL(o->type, 1);
    case 4: case 6: case 7: case 9: RET_STDCALL(D3D_OK, classes[K_SURFACE].methods[i].argc);
    case 11: RET_STDCALL(resource_get_container(o,ARG(1),ARG(2)),3);
    case 12: if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,2);write_surface_desc(ARG(1), o, o->width, o->height); RET_STDCALL(D3D_OK, 2);
    case 13: { if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,4);if(surface_flush(o)) RET_STDCALL(D3DERR_NOTAVAILABLE,4); content_changed(o); uint32_t pitch; level_size(o->format, o->width, o->height, &pitch); S32(ARG(1), pitch); S32(ARG(1) + 4, o->data); RET_STDCALL(D3D_OK, 4); }
    case 14: content_changed(o); RET_STDCALL(D3D_OK, 1);
    case 15: S32(ARG(1), 0x20003); RET_STDCALL(D3D_OK, 2); case 16: RET_STDCALL(D3D_OK, 2);
    default: unimplemented(cpu, &classes[K_SURFACE], i);
    }
}
static void method_volume(EngineCPU *cpu, D3DObj *o, int i) {
    switch (i) {
    case 0: if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,3);obj_retain(o);S32(ARG(2),o->guest);RET_STDCALL(D3D_OK,3);
    case 1: RET_STDCALL(obj_retain(o),1);case 2: RET_STDCALL(obj_release(o),1);
    case 3: RET_STDCALL(resource_get_device(o,ARG(1)),2);
    case 4: case 6: RET_STDCALL(D3D_OK, classes[K_VOLUME].methods[i].argc);
    case 7: RET_STDCALL(resource_get_container(o,ARG(1),ARG(2)),3);
    case 8: { if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,2);write_volume_desc(ARG(1),o,o->width,o->height,o->depth?o->depth:1); RET_STDCALL(D3D_OK, 2); }
    case 9: { if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,4);content_changed(o);uint32_t pitch; level_size(o->format, o->width, o->height, &pitch); S32(ARG(1), pitch); S32(ARG(1) + 4, pitch * o->height); S32(ARG(1) + 8, o->data); RET_STDCALL(D3D_OK, 4); }
    case 10: content_changed(o);RET_STDCALL(D3D_OK,1);
    default: unimplemented(cpu, &classes[K_VOLUME], i);
    }
}
static void method_simple(EngineCPU *cpu, D3DObj *o, int i) {   /* vdecl, shaders, state blocks, swap chains, queries */
    ComClass *c = &classes[o->kind];
    if(o->kind==K_STATEBLOCK&&o->stateblock_device){
        if(o->refs<=0||!o->stateblock)RET_STDCALL(D3DERR_INVALIDCALL,c->methods[i].argc);
        if(i==0){if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,3);++o->refs;S32(ARG(2),o->guest);RET_STDCALL(D3D_OK,3);}
        if(i==1)RET_STDCALL(++o->refs,1);
        if(i==2)RET_STDCALL(obj_release(o),1);
        if(i==3){if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,2);stateblock_retain(NULL,o->stateblock_device);S32(ARG(1),o->stateblock_device);RET_STDCALL(D3D_OK,2);}
        if(i==4||i==5){
            if(stateblock_recording)RET_STDCALL(D3DERR_INVALIDCALL,1);
            stateblock_trace_event(cpu,i==4?SB_TRACE_CAPTURE:SB_TRACE_APPLY,o->guest,0);
            if(i==4)stateblock_capture_payload(o->stateblock);else stateblock_apply_payload(o->stateblock);
            RET_STDCALL(D3D_OK,1);
        }
    }
    switch (i) {
    case 0: if(!ARG(2))RET_STDCALL(D3DERR_INVALIDCALL,3);obj_retain(o);S32(ARG(2),o->guest);RET_STDCALL(D3D_OK,3);
    case 1: RET_STDCALL(obj_retain(o),1);case 2: RET_STDCALL(obj_release(o),1);
    default: break;
    }
    if (o->kind == K_VDECL || o->kind == K_VSHADER || o->kind == K_PSHADER) {
        if (i == 3) {if(!ARG(1))RET_STDCALL(D3DERR_INVALIDCALL,2);D3DObj *d=obj_find(o->resource_device);obj_retain(d);S32(ARG(1),d?d->guest:0);RET_STDCALL(D3D_OK,2);}
        if (i == 4) { uint32_t out = ARG(1), sz = ARG(2); if (o->kind == K_VDECL) { if (out) memcpy(GPTR(out), GPTR(o->data), o->size); S32(sz, o->size / 8); } else { if (out) memcpy(GPTR(out), GPTR(o->data), o->size); S32(sz, o->size); } RET_STDCALL(D3D_OK, 3); }
    } else if (o->kind == K_STATEBLOCK) { if (i == 3) { S32(ARG(1), the_device->guest); RET_STDCALL(D3D_OK, 2); } if (i == 4 || i == 5) { stateblock_trace_event(cpu,i==4?SB_TRACE_CAPTURE:SB_TRACE_APPLY,o->guest,0); RET_STDCALL(D3D_OK, 1); } }
    else if (o->kind == K_SWAPCHAIN) {
        if (i == 3) { frames_presented++; stateblock_trace_poll(); RET_STDCALL(D3D_OK, 6); } if (i == 5) { if(ARG(1)||ARG(2)||!ARG(3)||!the_device)RET_STDCALL(D3DERR_INVALIDCALL,4);obj_retain(obj_find(the_device->data));S32(ARG(3),the_device->data);RET_STDCALL(D3D_OK,4); }
        if (i == 6) { S32(ARG(1), 1); S32(ARG(1) + 4, 0); RET_STDCALL(D3D_OK, 2); } if (i == 7) { fill_mode(ARG(1), backbuffer_w, backbuffer_h); RET_STDCALL(D3D_OK, 2); }
        if (i == 8) { S32(ARG(1), the_device->guest); RET_STDCALL(D3D_OK, 2); } if (i == 9) { memset(GPTR(ARG(1)), 0, 56); S32(ARG(1), backbuffer_w); S32(ARG(1) + 4, backbuffer_h); S32(ARG(1) + 8, FMT_X8R8G8B8); S32(ARG(1) + 12, 1); S32(ARG(1) + 32, 1); RET_STDCALL(D3D_OK, 2); }
    } else if (o->kind == K_QUERY) {
        if (i == 3) { S32(ARG(1), the_device->guest); RET_STDCALL(D3D_OK, 2); } if (i == 4) RET_STDCALL(o->type, 1); if (i == 5) RET_STDCALL(4, 1); if (i == 6) RET_STDCALL(D3D_OK, 2);
        if (i == 7) { uint32_t out = ARG(1), sz = ARG(2); if (out && sz >= 4) S32(out, 1); RET_STDCALL(0, 4); }
    }
    unimplemented(cpu, c, i);
}
static void dispatch_method(EngineCPU *cpu) {
    /* Called for any "d3d9.dll!Iface::Method" magic proc: recover object and method index from the return site. */
    (void)cpu; host_log("internal: d3d9 dispatcher misuse"); engine_fail(cpu, "d3d9 dispatch");
}
/* One shim per (class, vtable slot); generated explicitly so stdcall stacks and `this` are recovered per slot. */
#define METHOD_SHIM(cls, idx, handler) static void shim_##cls##_##idx(EngineCPU *cpu) { D3DObj *o = obj_from_guest(ARG(0)); if (!o) { host_log("d3d9 method on bad object %08X", ARG(0)); engine_fail(cpu, "d3d9 bad this"); } handler(cpu, o, idx); }
METHOD_SHIM(d3d, 0, method_d3d) METHOD_SHIM(d3d, 1, method_d3d) METHOD_SHIM(d3d, 2, method_d3d) METHOD_SHIM(d3d, 3, method_d3d) METHOD_SHIM(d3d, 4, method_d3d) METHOD_SHIM(d3d, 5, method_d3d) METHOD_SHIM(d3d, 6, method_d3d) METHOD_SHIM(d3d, 7, method_d3d) METHOD_SHIM(d3d, 8, method_d3d) METHOD_SHIM(d3d, 9, method_d3d) METHOD_SHIM(d3d, 10, method_d3d) METHOD_SHIM(d3d, 11, method_d3d) METHOD_SHIM(d3d, 12, method_d3d) METHOD_SHIM(d3d, 13, method_d3d) METHOD_SHIM(d3d, 14, method_d3d) METHOD_SHIM(d3d, 15, method_d3d) METHOD_SHIM(d3d, 16, method_d3d)
METHOD_SHIM(dev, 0, method_device) METHOD_SHIM(dev, 1, method_device) METHOD_SHIM(dev, 2, method_device) METHOD_SHIM(dev, 3, method_device) METHOD_SHIM(dev, 4, method_device) METHOD_SHIM(dev, 5, method_device) METHOD_SHIM(dev, 6, method_device) METHOD_SHIM(dev, 7, method_device) METHOD_SHIM(dev, 8, method_device) METHOD_SHIM(dev, 9, method_device) METHOD_SHIM(dev, 10, method_device) METHOD_SHIM(dev, 11, method_device) METHOD_SHIM(dev, 12, method_device) METHOD_SHIM(dev, 13, method_device) METHOD_SHIM(dev, 14, method_device) METHOD_SHIM(dev, 15, method_device) METHOD_SHIM(dev, 16, method_device) METHOD_SHIM(dev, 17, method_device) METHOD_SHIM(dev, 18, method_device) METHOD_SHIM(dev, 19, method_device) METHOD_SHIM(dev, 20, method_device) METHOD_SHIM(dev, 21, method_device) METHOD_SHIM(dev, 22, method_device) METHOD_SHIM(dev, 23, method_device) METHOD_SHIM(dev, 24, method_device) METHOD_SHIM(dev, 25, method_device) METHOD_SHIM(dev, 26, method_device) METHOD_SHIM(dev, 27, method_device) METHOD_SHIM(dev, 28, method_device) METHOD_SHIM(dev, 29, method_device) METHOD_SHIM(dev, 30, method_device) METHOD_SHIM(dev, 31, method_device) METHOD_SHIM(dev, 32, method_device) METHOD_SHIM(dev, 33, method_device) METHOD_SHIM(dev, 34, method_device) METHOD_SHIM(dev, 35, method_device) METHOD_SHIM(dev, 36, method_device) METHOD_SHIM(dev, 37, method_device) METHOD_SHIM(dev, 38, method_device) METHOD_SHIM(dev, 39, method_device) METHOD_SHIM(dev, 40, method_device) METHOD_SHIM(dev, 41, method_device) METHOD_SHIM(dev, 42, method_device) METHOD_SHIM(dev, 43, method_device) METHOD_SHIM(dev, 44, method_device) METHOD_SHIM(dev, 45, method_device) METHOD_SHIM(dev, 46, method_device) METHOD_SHIM(dev, 47, method_device) METHOD_SHIM(dev, 48, method_device) METHOD_SHIM(dev, 49, method_device) METHOD_SHIM(dev, 50, method_device) METHOD_SHIM(dev, 51, method_device) METHOD_SHIM(dev, 52, method_device) METHOD_SHIM(dev, 53, method_device) METHOD_SHIM(dev, 54, method_device) METHOD_SHIM(dev, 55, method_device) METHOD_SHIM(dev, 56, method_device) METHOD_SHIM(dev, 57, method_device) METHOD_SHIM(dev, 58, method_device) METHOD_SHIM(dev, 59, method_device) METHOD_SHIM(dev, 60, method_device) METHOD_SHIM(dev, 61, method_device) METHOD_SHIM(dev, 62, method_device) METHOD_SHIM(dev, 63, method_device) METHOD_SHIM(dev, 64, method_device) METHOD_SHIM(dev, 65, method_device) METHOD_SHIM(dev, 66, method_device) METHOD_SHIM(dev, 67, method_device) METHOD_SHIM(dev, 68, method_device) METHOD_SHIM(dev, 69, method_device) METHOD_SHIM(dev, 70, method_device) METHOD_SHIM(dev, 71, method_device) METHOD_SHIM(dev, 72, method_device) METHOD_SHIM(dev, 73, method_device) METHOD_SHIM(dev, 74, method_device) METHOD_SHIM(dev, 75, method_device) METHOD_SHIM(dev, 76, method_device) METHOD_SHIM(dev, 77, method_device) METHOD_SHIM(dev, 78, method_device) METHOD_SHIM(dev, 79, method_device) METHOD_SHIM(dev, 80, method_device) METHOD_SHIM(dev, 81, method_device) METHOD_SHIM(dev, 82, method_device) METHOD_SHIM(dev, 83, method_device) METHOD_SHIM(dev, 84, method_device) METHOD_SHIM(dev, 85, method_device) METHOD_SHIM(dev, 86, method_device) METHOD_SHIM(dev, 87, method_device) METHOD_SHIM(dev, 88, method_device) METHOD_SHIM(dev, 89, method_device) METHOD_SHIM(dev, 90, method_device) METHOD_SHIM(dev, 91, method_device) METHOD_SHIM(dev, 92, method_device) METHOD_SHIM(dev, 93, method_device) METHOD_SHIM(dev, 94, method_device) METHOD_SHIM(dev, 95, method_device) METHOD_SHIM(dev, 96, method_device) METHOD_SHIM(dev, 97, method_device) METHOD_SHIM(dev, 98, method_device) METHOD_SHIM(dev, 99, method_device) METHOD_SHIM(dev, 100, method_device) METHOD_SHIM(dev, 101, method_device) METHOD_SHIM(dev, 102, method_device) METHOD_SHIM(dev, 103, method_device) METHOD_SHIM(dev, 104, method_device) METHOD_SHIM(dev, 105, method_device) METHOD_SHIM(dev, 106, method_device) METHOD_SHIM(dev, 107, method_device) METHOD_SHIM(dev, 108, method_device) METHOD_SHIM(dev, 109, method_device) METHOD_SHIM(dev, 110, method_device) METHOD_SHIM(dev, 111, method_device) METHOD_SHIM(dev, 112, method_device) METHOD_SHIM(dev, 113, method_device) METHOD_SHIM(dev, 114, method_device) METHOD_SHIM(dev, 115, method_device) METHOD_SHIM(dev, 116, method_device) METHOD_SHIM(dev, 117, method_device) METHOD_SHIM(dev, 118, method_device)
METHOD_SHIM(tex, 0, method_texture) METHOD_SHIM(tex, 1, method_texture) METHOD_SHIM(tex, 2, method_texture) METHOD_SHIM(tex, 3, method_texture) METHOD_SHIM(tex, 4, method_texture) METHOD_SHIM(tex, 5, method_texture) METHOD_SHIM(tex, 6, method_texture) METHOD_SHIM(tex, 7, method_texture) METHOD_SHIM(tex, 8, method_texture) METHOD_SHIM(tex, 9, method_texture) METHOD_SHIM(tex, 10, method_texture) METHOD_SHIM(tex, 11, method_texture) METHOD_SHIM(tex, 12, method_texture) METHOD_SHIM(tex, 13, method_texture) METHOD_SHIM(tex, 14, method_texture) METHOD_SHIM(tex, 15, method_texture) METHOD_SHIM(tex, 16, method_texture) METHOD_SHIM(tex, 17, method_texture) METHOD_SHIM(tex, 18, method_texture) METHOD_SHIM(tex, 19, method_texture) METHOD_SHIM(tex, 20, method_texture) METHOD_SHIM(tex, 21, method_texture)
METHOD_SHIM(buf, 0, method_buffer) METHOD_SHIM(buf, 1, method_buffer) METHOD_SHIM(buf, 2, method_buffer) METHOD_SHIM(buf, 3, method_buffer) METHOD_SHIM(buf, 4, method_buffer) METHOD_SHIM(buf, 5, method_buffer) METHOD_SHIM(buf, 6, method_buffer) METHOD_SHIM(buf, 7, method_buffer) METHOD_SHIM(buf, 8, method_buffer) METHOD_SHIM(buf, 9, method_buffer) METHOD_SHIM(buf, 10, method_buffer) METHOD_SHIM(buf, 11, method_buffer) METHOD_SHIM(buf, 12, method_buffer) METHOD_SHIM(buf, 13, method_buffer)
METHOD_SHIM(surf, 0, method_surface) METHOD_SHIM(surf, 1, method_surface) METHOD_SHIM(surf, 2, method_surface) METHOD_SHIM(surf, 3, method_surface) METHOD_SHIM(surf, 4, method_surface) METHOD_SHIM(surf, 5, method_surface) METHOD_SHIM(surf, 6, method_surface) METHOD_SHIM(surf, 7, method_surface) METHOD_SHIM(surf, 8, method_surface) METHOD_SHIM(surf, 9, method_surface) METHOD_SHIM(surf, 10, method_surface) METHOD_SHIM(surf, 11, method_surface) METHOD_SHIM(surf, 12, method_surface) METHOD_SHIM(surf, 13, method_surface) METHOD_SHIM(surf, 14, method_surface) METHOD_SHIM(surf, 15, method_surface) METHOD_SHIM(surf, 16, method_surface)
METHOD_SHIM(volume, 0, method_volume) METHOD_SHIM(volume, 1, method_volume) METHOD_SHIM(volume, 2, method_volume) METHOD_SHIM(volume, 3, method_volume) METHOD_SHIM(volume, 4, method_volume) METHOD_SHIM(volume, 5, method_volume) METHOD_SHIM(volume, 6, method_volume) METHOD_SHIM(volume, 7, method_volume) METHOD_SHIM(volume, 8, method_volume) METHOD_SHIM(volume, 9, method_volume) METHOD_SHIM(volume, 10, method_volume)
METHOD_SHIM(simple, 0, method_simple) METHOD_SHIM(simple, 1, method_simple) METHOD_SHIM(simple, 2, method_simple) METHOD_SHIM(simple, 3, method_simple) METHOD_SHIM(simple, 4, method_simple) METHOD_SHIM(simple, 5, method_simple) METHOD_SHIM(simple, 6, method_simple) METHOD_SHIM(simple, 7, method_simple) METHOD_SHIM(simple, 8, method_simple) METHOD_SHIM(simple, 9, method_simple)
static HostShim d3d_fns[] = { shim_d3d_0, shim_d3d_1, shim_d3d_2, shim_d3d_3, shim_d3d_4, shim_d3d_5, shim_d3d_6, shim_d3d_7, shim_d3d_8, shim_d3d_9, shim_d3d_10, shim_d3d_11, shim_d3d_12, shim_d3d_13, shim_d3d_14, shim_d3d_15, shim_d3d_16 };
static HostShim dev_fns[] = { shim_dev_0, shim_dev_1, shim_dev_2, shim_dev_3, shim_dev_4, shim_dev_5, shim_dev_6, shim_dev_7, shim_dev_8, shim_dev_9, shim_dev_10, shim_dev_11, shim_dev_12, shim_dev_13, shim_dev_14, shim_dev_15, shim_dev_16, shim_dev_17, shim_dev_18, shim_dev_19, shim_dev_20, shim_dev_21, shim_dev_22, shim_dev_23, shim_dev_24, shim_dev_25, shim_dev_26, shim_dev_27, shim_dev_28, shim_dev_29, shim_dev_30, shim_dev_31, shim_dev_32, shim_dev_33, shim_dev_34, shim_dev_35, shim_dev_36, shim_dev_37, shim_dev_38, shim_dev_39, shim_dev_40, shim_dev_41, shim_dev_42, shim_dev_43, shim_dev_44, shim_dev_45, shim_dev_46, shim_dev_47, shim_dev_48, shim_dev_49, shim_dev_50, shim_dev_51, shim_dev_52, shim_dev_53, shim_dev_54, shim_dev_55, shim_dev_56, shim_dev_57, shim_dev_58, shim_dev_59, shim_dev_60, shim_dev_61, shim_dev_62, shim_dev_63, shim_dev_64, shim_dev_65, shim_dev_66, shim_dev_67, shim_dev_68, shim_dev_69, shim_dev_70, shim_dev_71, shim_dev_72, shim_dev_73, shim_dev_74, shim_dev_75, shim_dev_76, shim_dev_77, shim_dev_78, shim_dev_79, shim_dev_80, shim_dev_81, shim_dev_82, shim_dev_83, shim_dev_84, shim_dev_85, shim_dev_86, shim_dev_87, shim_dev_88, shim_dev_89, shim_dev_90, shim_dev_91, shim_dev_92, shim_dev_93, shim_dev_94, shim_dev_95, shim_dev_96, shim_dev_97, shim_dev_98, shim_dev_99, shim_dev_100, shim_dev_101, shim_dev_102, shim_dev_103, shim_dev_104, shim_dev_105, shim_dev_106, shim_dev_107, shim_dev_108, shim_dev_109, shim_dev_110, shim_dev_111, shim_dev_112, shim_dev_113, shim_dev_114, shim_dev_115, shim_dev_116, shim_dev_117, shim_dev_118 };
static HostShim tex_fns[] = { shim_tex_0, shim_tex_1, shim_tex_2, shim_tex_3, shim_tex_4, shim_tex_5, shim_tex_6, shim_tex_7, shim_tex_8, shim_tex_9, shim_tex_10, shim_tex_11, shim_tex_12, shim_tex_13, shim_tex_14, shim_tex_15, shim_tex_16, shim_tex_17, shim_tex_18, shim_tex_19, shim_tex_20, shim_tex_21 };
static HostShim buf_fns[] = { shim_buf_0, shim_buf_1, shim_buf_2, shim_buf_3, shim_buf_4, shim_buf_5, shim_buf_6, shim_buf_7, shim_buf_8, shim_buf_9, shim_buf_10, shim_buf_11, shim_buf_12, shim_buf_13 };
static HostShim surf_fns[] = { shim_surf_0, shim_surf_1, shim_surf_2, shim_surf_3, shim_surf_4, shim_surf_5, shim_surf_6, shim_surf_7, shim_surf_8, shim_surf_9, shim_surf_10, shim_surf_11, shim_surf_12, shim_surf_13, shim_surf_14, shim_surf_15, shim_surf_16 };
static HostShim volume_fns[] = { shim_volume_0, shim_volume_1, shim_volume_2, shim_volume_3, shim_volume_4, shim_volume_5, shim_volume_6, shim_volume_7, shim_volume_8, shim_volume_9, shim_volume_10 };
static HostShim simple_fns[] = { shim_simple_0, shim_simple_1, shim_simple_2, shim_simple_3, shim_simple_4, shim_simple_5, shim_simple_6, shim_simple_7, shim_simple_8, shim_simple_9 };

SHIM(Direct3DCreate9) { D3DObj *d = obj_new(K_D3D); host_log("Direct3DCreate9(sdk %u) -> Metal bridge object %08X", ARG(0), d->guest); RET_STDCALL(d->guest, 1); }

/* Build the shim table at startup: one entry per (class, method). */
static HostShimEntry d3d9_table[512]; static char d3d9_names[512][96];
const HostShimEntry *host_shims_d3d9_build(void) {
    int n = 0;
    d3d9_table[n++] = (HostShimEntry){ "d3d9.dll", "Direct3DCreate9", shim_Direct3DCreate9 };
    struct { int kind; HostShim *fns; int count; } sets[] = {
        { K_D3D, d3d_fns, N(m_d3d) }, { K_DEVICE, dev_fns, N(m_dev) }, { K_TEXTURE, tex_fns, N(m_tex) }, { K_CUBETEX, tex_fns, N(m_cube) }, { K_VOLTEX, tex_fns, N(m_vol) },
        { K_VB, buf_fns, N(m_vb) }, { K_IB, buf_fns, N(m_vb) }, { K_SURFACE, surf_fns, N(m_surf) }, { K_VOLUME, volume_fns, N(m_volume) },
        { K_VDECL, simple_fns, N(m_vdecl) }, { K_VSHADER, simple_fns, N(m_shader) }, { K_PSHADER, simple_fns, N(m_shader) }, { K_STATEBLOCK, simple_fns, N(m_sb) }, { K_SWAPCHAIN, simple_fns, N(m_swap) }, { K_QUERY, simple_fns, N(m_query) } };
    for (size_t s = 0; s < sizeof sets / sizeof sets[0]; s++) {
        ComClass *c = &classes[sets[s].kind];
        for (int i = 0; i < c->count && n < 511; i++) { snprintf(d3d9_names[n], 96, "%s::%s", c->iface, c->methods[i].name); d3d9_table[n] = (HostShimEntry){ "d3d9.dll", d3d9_names[n], sets[s].fns[i] }; n++; }
    }
    d3d9_table[n] = (HostShimEntry){ NULL, NULL, NULL };
    return d3d9_table;
}
uint32_t host_d3d9_frames(void) { return frames_presented; }
