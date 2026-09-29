/* Force cache pressure on the real Metal path without allocating 512 MiB. */
#define MR_MAX_CACHED_TEXTURE_BYTES 1024u
#define main renderer_fixture_main
#include "test_metalrenderer_fastpaths.m"
#undef main
int main(void) {
 @autoreleasepool {
  mr_context *c=mr_create(16,16);if(!c){puts("SKIP no Metal");return 77;}
  uint8_t pixels[8*8*4];memset(pixels,0x55,sizeof pixels);
  uint32_t a=mr_texture_create_cached(c,101,8,8,pixels,32);assert(a); /* 340 bytes */
  mr_texture_bindings_begin();assert(mr_texture_find_cached(c,101)==a);
  uint32_t b=mr_texture_create_cached(c,102,8,8,pixels,32),d=mr_texture_create_cached(c,103,8,8,pixels,32);assert(b&&d);
  assert(!mr_texture_create_cached(c,104,8,8,pixels,32)); /* fail instead of rebinding a */
  assert(c->s->texture_keys[a]==101 && c->s->texture_keys[b]==102 && c->s->texture_keys[d]==103);
  assert(mr_cached_texture_bytes(c)==1020);
  mr_texture_bindings_end();
  assert(mr_texture_create_cached(c,104,8,8,pixels,32));assert(!mr_texture_find_cached(c,101));
  assert(mr_cached_texture_bytes(c)==1020);
  uint64_t hits,uploads,evictions;mr_texture_cache_counters(&hits,&uploads,&evictions);assert(uploads==4&&evictions==1);
  mr_destroy(c);puts("PASS Metal texture bindings survive pressure; budget stays bounded; eviction resumes after draw");
 }
}
