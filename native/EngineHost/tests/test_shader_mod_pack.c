#include "shader_mod_pack.h"
#include <assert.h>
#include <stdio.h>
static void put(uint8_t *p,uint32_t n){for(int i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
int main(void) {
    uint8_t b[80]={0};memcpy(b,"HVSHD001",8);put(b+8,1);put(b+16,8);put(b+20,8);
    put(b+24,0xffff0200);put(b+28,65535);put(b+32,0xffff0200);put(b+36,65535);
    HaloShaderPack p;assert(hsm_open(&p,b,40));assert(hsm_find(&p,b+24,8));assert(!hsm_find(&p,b+24,4));
    for(size_t n=0;n<40;n++){assert(!hsm_open(&p,b,n));assert(p.count==0);}
    assert(!hsm_open(&p,b,41));put(b+16,0xffffffff);assert(!hsm_open(&p,b,40));put(b+16,8);
    put(b+36,0x05000051);assert(!hsm_open(&p,b,40));put(b+36,65535);
    put(b+8,HSM_MAX_ENTRIES+1);assert(!hsm_open(&p,b,40));put(b+8,2);
    memcpy(b+40,b+16,24);assert(!hsm_open(&p,b,64)); /* ambiguous duplicate */
    put(b+8,1);put(b+12,1);assert(!hsm_open(&p,b,40));
    puts("shader pack bounds, exact match, truncation and token validation PASS");
}
