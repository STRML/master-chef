/*
 * metalwin.m - macOS Metal implementation of the metalwin.h C API.
 *
 * Owns a single NSWindow whose content view is layer-hosting a CAMetalLayer.
 * Each present() uploads a BGRA8 framebuffer into an MTLTexture (via
 * replaceRegion) and draws it onto a fullscreen triangle with a linear sampler,
 * then presents the drawable and pumps one round of NSApp events.
 *
 * Threading contract (important - read this)
 * ------------------------------------------
 * AppKit requires that NSWindow / NSView be created and touched on the process
 * main thread. Modern macOS (26.x) enforces this with a hard exception, not a
 * warning, so we must respect it. Metal itself is thread-safe: texture uploads,
 * acquiring a CAMetalLayer drawable, encoding, and presenting may all run on the
 * caller's thread. We use that split:
 *
 *   - Window + layer creation always happens ON THE MAIN THREAD:
 *       * called from the main thread  -> we create directly, bootstrap NSApp
 *         ([NSApplication sharedApplication], activation policy Regular,
 *         finishLaunching) and pump events manually - this is the host's
 *         "no runloop" case.
 *       * called from a secondary thread (the real host runs the game on a
 *         pthread) -> we marshal creation onto the main queue. We do NOT block
 *         forever: we wait on a semaphore with a timeout, so if the main thread
 *         is not servicing its queue we fail gracefully instead of deadlocking.
 *   - Framebuffer upload + draw + present run on whichever thread calls
 *     present(), so the game thread can render without bouncing to main.
 *   - Event pumping runs on the main thread: directly when the caller is main,
 *     otherwise dispatched to the main queue.
 *
 * Consequence for the current host: main.c spawns the game on a pthread and then
 * blocks in pthread_join, so its main thread never services AppKit. In that
 * exact topology no window can appear (macOS forbids off-main window creation),
 * and metalwin_init() will time out and return nonzero rather than crash/hang.
 * For the window to appear when driven from the game thread, the host's MAIN
 * thread must service AppKit (run the runloop / drain the main dispatch queue)
 * while the game runs on the worker thread. See testmain.c "bg" mode for a
 * correctly structured example.
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metalwin.h"

/* ---- Window delegate: flips a flag when the user closes the window -------- */

static volatile int g_should_close = 0;

@interface MetalWinDelegate : NSObject <NSWindowDelegate>
@end

@implementation MetalWinDelegate
- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    g_should_close = 1;
}
@end

/* ---- Module state (single window; ARC manages these strong globals) ------- */

static int                    g_init_attempted = 0;  /* init tried at least once */
static int                    g_init_result = 1;      /* cached metalwin_init() return */
static int                    g_inited = 0;           /* 1 once init succeeded */
static NSWindow              *g_window;
static NSView                *g_view;
static CAMetalLayer          *g_layer;
static id<MTLDevice>          g_device;
static id<MTLCommandQueue>    g_queue;
static id<MTLRenderPipelineState> g_pipeline;
static id<MTLSamplerState>    g_sampler;
static id<MTLTexture>         g_texture;         /* recreated when size changes */
static int                    g_tex_w = 0, g_tex_h = 0;
static MetalWinDelegate      *g_delegate;
static NSUInteger             g_draw_w = 0, g_draw_h = 0; /* fixed drawable size, px */

/* Embedded Metal shader: fullscreen triangle + textured fragment.
 * The vertex shader emits an oversized triangle covering the whole viewport and
 * flips V so texture row 0 (the top of the D3D framebuffer) shows at the top of
 * the window. */
static NSString *const kShaderSource = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
"vertex VOut v_main(uint vid [[vertex_id]]) {\n"
"    float2 p[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
"    float2 t[3] = { float2(0.0,1.0),  float2(2.0,1.0),  float2(0.0,-1.0) };\n"
"    VOut o;\n"
"    o.pos = float4(p[vid], 0.0, 1.0);\n"
"    o.uv  = t[vid];\n"
"    return o;\n"
"}\n"
"fragment float4 f_main(VOut in [[stage_in]],\n"
"                       texture2d<float> tex [[texture(0)]],\n"
"                       sampler samp [[sampler(0)]]) {\n"
"    return tex.sample(samp, in.uv);\n"
"}\n";

/* ---- Event pump: drain everything queued, without blocking ---------------- */

static void pump_events_now(void) {
    @autoreleasepool {
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES]) != nil) {
            [NSApp sendEvent:e];
        }
    }
}

/* Pump events on the main thread regardless of the caller's thread. */
static void pump_events(void) {
    if (!g_inited) return;
    if ([NSThread isMainThread]) {
        pump_events_now();
    } else {
        /* Non-blocking: if main services its queue the events get pumped there;
         * if it does not, we simply skip (never block the game thread). */
        dispatch_async(dispatch_get_main_queue(), ^{ pump_events_now(); });
    }
}

/* ---- One-time creation of NSApp + window + Metal objects (MAIN THREAD) ---- */

static int create_all(int width, int height, const char *title) {
    @autoreleasepool {
        if (width  <= 0) width  = 640;
        if (height <= 0) height = 480;

        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device) {
            NSLog(@"metalwin: no Metal device available");
            return 1;
        }
        g_queue = [g_device newCommandQueue];
        if (!g_queue) {
            NSLog(@"metalwin: failed to create command queue");
            return 2;
        }

        /* Compile the embedded shader and build the render pipeline. */
        NSError *err = nil;
        id<MTLLibrary> lib = [g_device newLibraryWithSource:kShaderSource
                                                    options:nil
                                                      error:&err];
        if (!lib) {
            NSLog(@"metalwin: shader compile failed: %@", err);
            return 3;
        }
        id<MTLFunction> vfn = [lib newFunctionWithName:@"v_main"];
        id<MTLFunction> ffn = [lib newFunctionWithName:@"f_main"];
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction   = vfn;
        pd.fragmentFunction = ffn;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_pipeline = [g_device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_pipeline) {
            NSLog(@"metalwin: pipeline creation failed: %@", err);
            return 4;
        }

        /* Linear sampler, clamp to edge, so scaling the framebuffer to the
         * window looks smooth without wrapping artifacts. */
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_sampler = [g_device newSamplerStateWithDescriptor:sd];

        /* Bootstrap NSApp. Safe here because create_all only ever runs on the
         * main thread (directly, or marshalled via the main queue). */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, width, height);
        /* Fixed-size window: keeps the drawable size constant so present() never
         * has to touch AppKit from a background thread. */
        NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable;
        g_window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        if (!g_window) {
            NSLog(@"metalwin: failed to create window");
            return 5;
        }
        [g_window setTitle:[NSString stringWithUTF8String:(title ? title : "metalwin")]];
        [g_window center];

        /* Layer-hosting content view backed by a CAMetalLayer. Setting the layer
         * BEFORE wantsLayer makes the view layer-*hosting*, so AppKit will not
         * replace our CAMetalLayer with one of its own. */
        g_layer = [CAMetalLayer layer];
        g_layer.device = g_device;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = YES;
        g_layer.opaque = YES;

        g_view = [[NSView alloc] initWithFrame:frame];
        g_view.layer = g_layer;
        g_view.wantsLayer = YES;
        g_layer.frame = g_view.bounds;

        [g_window setContentView:g_view];

        CGFloat scale = g_window.backingScaleFactor > 0 ? g_window.backingScaleFactor : 1.0;
        g_layer.contentsScale = scale;
        g_draw_w = (NSUInteger)(width  * scale);
        g_draw_h = (NSUInteger)(height * scale);
        g_layer.drawableSize = CGSizeMake(g_draw_w, g_draw_h);

        g_delegate = [[MetalWinDelegate alloc] init];
        g_window.delegate = g_delegate;

        [NSApp finishLaunching];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        return 0;
    }
}

/* Run create_all on the main thread, whatever thread we were called on. */
static int create_on_main(int width, int height, const char *title) {
    if ([NSThread isMainThread]) {
        return create_all(width, height, title);
    }
    /* Marshal to the main queue. Wait with a timeout so a host whose main thread
     * is blocked (e.g. stuck in pthread_join with no runloop) fails cleanly
     * instead of deadlocking the game thread forever. */
    __block int result = -1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        result = create_all(width, height, title);
        dispatch_semaphore_signal(sem);
    });
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC));
    if (dispatch_semaphore_wait(sem, deadline) != 0) {
        NSLog(@"metalwin: main thread is not servicing its dispatch queue; "
              @"cannot create the window off-main (host must run its main "
              @"runloop / drain the main queue while the game runs on a worker "
              @"thread).");
        return 6;
    }
    return result;
}

/* Recreate the upload texture if the framebuffer dimensions changed. */
static int ensure_texture(int width, int height) {
    if (g_texture && g_tex_w == width && g_tex_h == height) return 1;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    /* Shared storage on Apple Silicon lets replaceRegion write directly. */
    td.storageMode = MTLStorageModeShared;
    g_texture = [g_device newTextureWithDescriptor:td];
    if (!g_texture) { g_tex_w = g_tex_h = 0; return 0; }
    g_tex_w = width;
    g_tex_h = height;
    return 1;
}

/* ---- Public C API -------------------------------------------------------- */

int metalwin_init(int width, int height, const char *title) {
    if (g_init_attempted) return g_init_result;
    g_init_attempted = 1;
    g_init_result = create_on_main(width, height, title);
    g_inited = (g_init_result == 0);
    return g_init_result;
}

void metalwin_present(const void *bgra, int width, int height) {
    /* Lazy init so a host that only ever calls present() still gets a window.
     * (init is attempt-once, so a failed/timed-out init won't stall here again.) */
    if (!g_inited) {
        if (metalwin_init(width, height, "metalwin") != 0) return;
    }
    if (!bgra || width <= 0 || height <= 0) { pump_events(); return; }

    @autoreleasepool {
        if (!ensure_texture(width, height)) { pump_events(); return; }

        /* Upload the BGRA8 framebuffer straight into the texture (thread-safe). */
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [g_texture replaceRegion:region
                     mipmapLevel:0
                       withBytes:bgra
                     bytesPerRow:(NSUInteger)width * 4];

        /* Acquiring a drawable and encoding may run on the caller's thread. */
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (drawable) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLRenderCommandEncoder> enc =
                [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:g_pipeline];
            [enc setFragmentTexture:g_texture atIndex:0];
            [enc setFragmentSamplerState:g_sampler atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cb presentDrawable:drawable];
            [cb commit];
        }
    }

    pump_events();
}

int metalwin_should_close(void) {
    return g_should_close;
}

void metalwin_poll(void) {
    pump_events();
}
