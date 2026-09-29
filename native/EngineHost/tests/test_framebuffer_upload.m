/* GPU ordering and staging lifetime regression; link with metalrenderer.o,
 * metalshader.o and the four mojoshader host objects, Metal and Foundation. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../metalrenderer.h"
#include <assert.h>
#include <string.h>
int main(void) { @autoreleasepool {
    enum { W=67, H=19, N=W*H };
    mr_context *c=mr_create(W,H); assert(c);
    id<MTLDevice> dev=(__bridge id<MTLDevice>)mr_shared_device();
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    d.storageMode=MTLStorageModeShared;
    id<MTLTexture> saved=[dev newTextureWithDescriptor:d]; assert(saved);
    uint32_t pixels[N], result[N];
    for(int i=0;i<N;i++)pixels[i]=0xff123456u;
    assert(mr_write_framebuffer(c,pixels,sizeof pixels)==MR_OK);
    assert(mr_blit_target_to(c,(__bridge void *)saved)==MR_OK);
    for(int i=0;i<N;i++)pixels[i]=0xffabcdefu;
    assert(mr_write_framebuffer(c,pixels,sizeof pixels)==MR_OK);
    memset(pixels,0,sizeof pixels); /* caller may reuse guest storage immediately */
    assert(mr_read_framebuffer(c,result,sizeof result)==0);
    for(int i=0;i<N;i++)assert(result[i]==0xffabcdefu);
    [saved getBytes:result bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0];
    for(int i=0;i<N;i++)assert(result[i]==0xff123456u);
    mr_clear(c,0);
    assert(mr_read_framebuffer(c,result,sizeof result)==0);
    for(int i=0;i<N;i++)assert(result[i]==0);
    mr_destroy(c);
    puts("framebuffer upload ordering, odd row pitch, lifetime and clear: PASS");
} return 0; }
