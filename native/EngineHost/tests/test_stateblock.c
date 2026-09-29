#include <stdlib.h>
#include <assert.h>
static int fail_allocation=-1,allocation_calls,outstanding;
static void *test_malloc(size_t size){if(allocation_calls++==fail_allocation)return NULL;void *p=malloc(size);if(p)++outstanding;return p;}
static void *test_calloc(size_t count,size_t size){if(allocation_calls++==fail_allocation)return NULL;void *p=calloc(count,size);if(p)++outstanding;return p;}
static void test_free(void *p){if(p)--outstanding;free(p);}
#define HOST_SB_MALLOC test_malloc
#define HOST_SB_CALLOC test_calloc
#define HOST_SB_FREE test_free
#include "../stateblock.h"
#include <assert.h>
#include <stdio.h>
typedef struct {uint32_t bindings[2];float registers[8][4];uint32_t untouched;} State;
static int refs[8];
static void retain(void *context,uint32_t value){(void)context;assert(value<8);++refs[value];}
static void release(void *context,uint32_t value){(void)context;assert(value<8&&refs[value]>0);--refs[value];}
int main(void){
    State live={.bindings={1,2},.untouched=99},original=live;
    size_t resources[]={offsetof(State,bindings[0]),offsetof(State,bindings[1])};
    for(int fail=0;fail<2;fail++){allocation_calls=0;fail_allocation=fail;assert(!host_sb_new(&live,sizeof live,resources,2,retain,release,NULL)&&!outstanding);}
    fail_allocation=-1;
    HostStateBlock *b=host_sb_new(&live,sizeof live,resources,2,retain,release,NULL);assert(b);
    uint32_t binding=3;float reg[4]={1,2,3,4};
    assert(host_sb_write(b,offsetof(State,bindings[0]),&binding,4));
    assert(host_sb_write(b,offsetof(State,registers[2]),reg,sizeof reg));
    assert(!memcmp(&live,&original,sizeof live)&&refs[3]==1);
    assert(!host_sb_write(b,0,&binding,1)&&!host_sb_write(b,sizeof live,&binding,4));
    binding=4;assert(host_sb_write(b,0,&binding,4));assert(!refs[3]&&refs[4]==1);
    assert(host_sb_write(b,4,&binding,4));assert(refs[4]==2); /* duplicate slots */
    unsigned char mask[sizeof live];memcpy(mask,b->mask,sizeof mask);
    live.bindings[0]=5;live.bindings[1]=0;live.registers[2][3]=42;
    host_sb_capture(b,&live);assert(!memcmp(mask,b->mask,sizeof mask)&&refs[5]==1&&!refs[4]);
    live.bindings[0]=6;live.registers[2][3]=0;live.registers[3][0]=88;live.untouched=77;
    host_sb_apply(b,&live);assert(live.bindings[0]==5&&live.bindings[1]==0&&live.registers[2][3]==42);
    assert(live.registers[3][0]==88&&live.untouched==77);
    unsigned char *allocation=b->values;
    for(int i=0;i<1000;i++){host_sb_capture(b,&live);host_sb_apply(b,&live);assert(b->values==allocation&&refs[5]==1&&!memcmp(mask,b->mask,sizeof mask));}
    host_sb_free(b);for(int i=0;i<8;i++)assert(!refs[i]);
    b=host_sb_new(&live,sizeof live,resources,2,retain,release,NULL);State before=live;
    host_sb_capture(b,&live);host_sb_apply(b,&live);assert(!memcmp(&before,&live,sizeof live));host_sb_free(b);
    resources[0]=sizeof live;assert(!host_sb_new(&live,sizeof live,resources,2,retain,release,NULL));
    assert(!outstanding);
    puts("PASS: custom mask recording, sparse registers, replacement/null/duplicate resource ownership, stable Capture masks and allocations, Apply isolation, empty blocks.");
}
