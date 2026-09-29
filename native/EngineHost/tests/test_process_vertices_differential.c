/* Real host include chain, including FENV_ACCESS ON / FP_CONTRACT OFF. */
#include "../host.h"
#include "../process_vertices_differential.h"
#include <assert.h>
static void *load(const char *dir,const char *name,size_t *n) {
    char path[2048];snprintf(path,sizeof path,"%s/%s",dir,name);FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*n=ftell(f);rewind(f);void *p=malloc(*n);assert(p&&fread(p,1,*n,f)==*n);fclose(f);return p;
}
static unsigned corpus(const char *dir) {
    size_t wb,db,cb;uint32_t *words=load(dir,"vs-0272CE20.bin",&wb);uint8_t *decl=load(dir,"decl-027201D0.bin",&db);
    assert(pv_skin_shader_matches(words,wb));uint32_t *mutant=malloc(wb);memcpy(mutant,words,wb);
    for(size_t i=0;i<wb/4;i++){mutant[i]^=1;assert(!pv_skin_shader_matches(mutant,wb));mutant[i]^=1;}
    assert(!pv_skin_shader_matches(words,wb-4)&&!pv_skin_shader_matches(NULL,wb));
    for(int fast=0;fast<2;fast++)for(int diff=0;diff<2;diff++) {
        assert(pv_skin_dispatch_executor(words,wb,fast,diff)==(fast&&!diff?pv_skin_execute:pv_execute));
        mutant[25]^=1;assert(pv_skin_dispatch_executor(mutant,wb,fast,diff)==pv_execute);mutant[25]^=1;
    }
    unsigned vertices=0;
    for(unsigned draw=64;draw<70;draw++) {
        char name[128];size_t rb,pb,ob;snprintf(name,sizeof name,"draw-%03u.vertices.bin",draw);uint8_t *raw=load(dir,name,&rb);unsigned n=rb/68;assert(n*68==rb);vertices+=n;
        snprintf(name,sizeof name,"draw-%03u.pv-before.bin",draw);uint8_t *pre=load(dir,name,&pb);
        snprintf(name,sizeof name,"draw-%03u.pv-output.bin",draw);uint8_t *recorded=load(dir,name,&ob);assert(pb==n*32&&ob==pb);
        snprintf(name,sizeof name,"draw-%03u.vs-constants.bin",draw);float (*constants)[4]=load(dir,name,&cb);assert(cb==4096);
        PVProgram p;assert(pv_compile(&p,words,wb,constants));PVStream streams[16]={{raw,rb,0,68}};
        uint8_t *a=malloc(pb+64),*b=malloc(pb+64);const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
        for(unsigned mode=0;mode<4;mode++) {
            assert(!fesetround(modes[mode]));feclearexcept(FE_ALL_EXCEPT);feraiseexcept(FE_DIVBYZERO);fenv_t before;assert(!fegetenv(&before));
            memset(a,0xa5,pb+64);memcpy(a+32,pre,pb);memcpy(b,a,pb+64);
            assert(pv_process(&p,decl,db,streams,0,n,a,pb+64,1,0x112,1));int flags=fetestexcept(FE_ALL_EXCEPT);
            assert(!fesetenv(&before));PVDifferentialResult result;
            assert(pv_process_differential(&p,decl,db,streams,0,n,b,pb+64,1,0x112,1,&result));
            assert(result.matched&&!result.skipped&&!result.environment_restore_failed);assert(fegetround()==modes[mode]&&fetestexcept(FE_ALL_EXCEPT)==flags);assert(!memcmp(a,b,pb+64));
            for(int fast=0;fast<2;fast++)for(int diff=0;diff<2;diff++) {
                assert(!fesetenv(&before));memset(b,0xa5,pb+64);memcpy(b+32,pre,pb);
                assert(pv_process_skin_dispatch(&p,words,wb,decl,db,streams,0,n,b,pb+64,1,0x112,1,fast,diff));
                assert(!memcmp(a,b,pb+64)&&fetestexcept(FE_ALL_EXCEPT)==flags&&fegetround()==modes[mode]);
            }
            if(!mode)assert(!memcmp(b+32,recorded,pb));
        }
        assert(!fesetround(FE_TONEAREST));
        /* Deliberately mismatched executor semantics: generic is authoritative. */
        PVProgram other=p;other.constants[8][3]=0;other.instructions[0].src[1]=(other.instructions[0].src[1]&~2047u)|8u;
        memcpy(a,pre,pb);memcpy(b,pre,pb);assert(pv_process(&other,decl,db,streams,0,n,a,pb,0,0x112,1));PVDifferentialResult mismatch;
        assert(pv_process_differential(&other,decl,db,streams,0,n,b,pb,0,0x112,1,&mismatch));
        assert(!memcmp(a,b,pb));if(draw!=67)assert(!mismatch.matched&&mismatch.first_byte<pb);
        /* Valid mutated bytecode is routed through generic when fast is enabled. */
        memcpy(mutant,words,wb);mutant[25]=(mutant[25]&~2047u)|8u;
        memcpy(b,pre,pb);assert(pv_process_skin_dispatch(&other,mutant,wb,decl,db,streams,0,n,b,pb,0,0x112,1,1,0));assert(!memcmp(a,b,pb));
        memcpy(mutant,words,wb);
        /* Invalid second vertex must leave the original destination untouched. */
        int16_t ids[2]={32767,32767};memcpy(raw+68+56,ids,4);memcpy(a,pre,pb);PVDifferentialResult r;
        assert(!pv_process_differential(&p,decl,db,streams,0,n,a,pb,0,0x112,1,&r));assert(r.matched&&!r.skipped&&!memcmp(a,pre,pb));
        for(int fast=0;fast<2;fast++) {
            memcpy(a,pre,pb);assert(!pv_process_skin_dispatch(&p,words,wb,decl,db,streams,0,n,a,pb,0,0x112,1,fast,0));assert(!memcmp(a,pre,pb));
            assert(!pv_process_skin_dispatch(&p,words,wb,decl,db,streams,0,n,a,pb-1,0,0x112,1,fast,0));assert(!memcmp(a,pre,pb));
            assert(!pv_process_skin_dispatch(&p,words,wb,decl,db,streams,n,1,a,pb,0,0x112,1,fast,0));assert(!memcmp(a,pre,pb));
            assert(!pv_process_skin_dispatch(&p,words,wb,decl,db,streams,0,n,a,pb,0,0x112,0,fast,0));assert(!memcmp(a,pre,pb));
        }
        assert(!pv_process_differential(&p,decl,db,streams,0,n,a,pb-1,0,0x112,1,&r));assert(r.skipped==1&&!memcmp(a,pre,pb));
        /* Alias check preserves generic output and avoids a misleading second run. */
        uint8_t *alias=malloc(rb);memcpy(alias,raw,rb);PVStream as[16]={{alias,rb,0,68}};
        (void)pv_process_differential(&p,decl,db,as,0,n,alias,rb,0,0x112,1,&r);assert(r.skipped==5);free(alias);
        /* Byte cap skips comparison without introducing a new failure code. */
        PVStream empty[16]={{0}};size_t big=PV_DIFFERENTIAL_BYTE_CAP+32;uint8_t *buffer=malloc(big);memset(buffer,0xa5,big);
        assert(!pv_process_differential(&p,decl,db,empty,0,big/32,buffer,big,0,0x112,1,&r));assert(r.skipped==2);free(buffer);
        free(a);free(b);free(raw);free(pre);free(recorded);free(constants);
    }
    free(mutant);free(words);free(decl);return vertices;
}
int main(int argc,char **argv) {
    uint32_t parsed=73;
    const char *bad[]={NULL,"","-1","+1"," 1","1 ","0x10","12x","4294967296","999999999999999999999"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){assert(!pv_differential_parse_frame(bad[i],&parsed));assert(parsed==73);}
    assert(pv_differential_parse_frame("0",&parsed)&&parsed==0);assert(pv_differential_parse_frame("000500",&parsed)&&parsed==500);
    assert(pv_differential_parse_frame("4294967295",&parsed)&&parsed==UINT32_MAX);
    assert(!pv_differential_frame_allowed(499,500,1)&&pv_differential_frame_allowed(500,500,1));
    assert(!pv_differential_frame_allowed(UINT32_MAX,500,0));unsigned budget=0;
    for(unsigned frame=1;frame<=650;frame++) {
        if(pv_differential_frame_allowed(frame,500,1)&&budget<128)budget++;
        if(frame<500)assert(!budget); /* Menu calls consume no comparison budget. */
        assert(pv_skin_dispatch_executor(NULL,0,1,1)==pv_execute); /* Priority before/from/after cap. */
    }
    assert(budget==128);
    assert(argc>=2);fenv_t original;assert(!fegetenv(&original));unsigned n=0;for(int i=1;i<argc;i++)n+=corpus(argv[i]);assert(!fesetenv(&original));
    printf("PASS: %u actual captured vertices byte-exact; 4 rounding modes, complete-token mutations, preserved FP flags, fast off/on, differential priority, mutated fallback, authority/atomicity/range/alias/byte-cap/strict-frame-gate checks.\n",n);return 0;
}
