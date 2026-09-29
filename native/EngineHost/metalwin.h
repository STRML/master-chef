/*
 * metalwin.h - Minimal macOS Metal window for blitting a software framebuffer.
 *
 * Plain C API (no Objective-C leaks into this header) so the D3D9/DDraw shim
 * host can drive a real on-screen surface. The implementation lives in
 * metalwin.m and owns a Cocoa NSWindow + CAMetalLayer + a tiny render pipeline
 * that samples the uploaded framebuffer onto a fullscreen triangle.
 *
 * Pixel format: BGRA8, i.e. Direct3D's D3DFMT_X8R8G8B8 byte order in memory:
 * byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=X (ignored). This maps 1:1 onto
 * MTLPixelFormatBGRA8Unorm, so no per-pixel conversion is performed.
 *
 * Threading: the host runs the game on a pthread with no NSApp runloop, so this
 * module bootstraps NSApplication itself and pumps events during present/poll.
 * See metalwin.m for the full threading contract; in short: call all four
 * functions from the same thread (whichever thread first calls into the module).
 *
 * Build (macOS arm64):
 *   clang -ObjC -fobjc-arc metalwin.m testmain.c \
 *       -framework Cocoa -framework Metal -framework QuartzCore -o metalwin_test
 */
#ifndef METALWIN_H
#define METALWIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the window + Metal layer. Returns 0 on success, nonzero on failure
 * (e.g. no Metal device / no window server). Safe to call more than once;
 * subsequent calls are no-ops that return the first result. */
int metalwin_init(int width, int height, const char *title);

/* Upload a width*height BGRA8 framebuffer to a texture and draw it fullscreen,
 * then pump one round of Cocoa events so the window stays responsive.
 * `bgra` must point to at least width*height*4 bytes. If the module has not
 * been initialized yet, this lazily creates the window at the given size. */
void metalwin_present(const void *bgra, int width, int height);
/* Zero-copy present: the frame's views were blitted into platform pool slot
 * `slot`; the platform publishes it when the GPU finishes. Weak no-op on macOS. */
void metalwin_present_gpu(int slot, int width, int height);
/* A world frame that could not be handed off (pool busy, pass failed): no
 * readback and no flat copy; the platform keeps its last complete panorama. */
void metalwin_present_dropped(int width, int height);

/* Returns nonzero once the user has closed the window. */
int metalwin_should_close(void);

/* Pump pending Cocoa events without presenting a frame. */
void metalwin_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* METALWIN_H */
