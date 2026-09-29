/* Explicitly enabled, test-only capture of resident guest memory for original
 * instruction differential tests. Never loaded by the production game. */
#include <errno.h>
static void host_capture_for_test(EngineCPU *cpu) {
    static int captured;
    const char *pc_text=getenv("HALO_CAPTURE_PC"), *directory=getenv("HALO_CAPTURE_DIR");
    if(captured || !pc_text || !directory || cpu->pc!=(uint32_t)strtoul(pc_text,NULL,16))return;
    captured=1;
    size_t page=(size_t)getpagesize(), pages=UINT64_C(0x100000000)/page;
    char *resident=malloc(pages);
    if(!resident || mincore(engine_flat_base,UINT64_C(0x100000000),resident)) {
        host_log("capture mincore failed: %s",strerror(errno));free(resident);return;
    }
    char path[2048];snprintf(path,sizeof path,"%s/memory.bin",directory);
    FILE *data=fopen(path,"wb");snprintf(path,sizeof path,"%s/state.json",directory);
    FILE *state=fopen(path,"w");
    if(!data || !state){host_log("capture output unavailable");if(data)fclose(data);if(state)fclose(state);free(resident);return;}
    fprintf(state,"{\"pc\":%u,\"flags\":%u,\"gpr\":[",cpu->pc,cpu->flags);
    for(int i=0;i<8;i++)fprintf(state,"%s%u",i?",":"",cpu->gpr[i]);
    fprintf(state,"],\"fpBits\":[");
    /* ST(0)..ST(7) in stack order, as fpValid's bits are. */
    for(unsigned i=0;i<8;i++){uint64_t bits;memcpy(&bits,&cpu->fp_reg[engine_fp_physical(cpu,i)],8);fprintf(state,"%s%llu",i?",":"",(unsigned long long)bits);}
    fprintf(state,"],\"fpControl\":%u,\"fpStatus\":%u,\"fpTop\":%u,\"fpValid\":%u,\"regions\":[",cpu->fp_control,cpu->fp_status,cpu->fp_top,cpu->fp_valid);
    size_t offset=0;int first=1,complete=1;
    for(size_t i=0x10000/page;i<pages;) {
        if(!(resident[i]&MINCORE_INCORE)){i++;continue;}
        size_t begin=i;while(i<pages && (resident[i]&MINCORE_INCORE))i++;
        size_t count=(i-begin)*page;
        if(count>536870912u-offset || fwrite(engine_flat_base+begin*page,1,count,data)!=count){complete=0;break;}
        fprintf(state,"%s{\"address\":%llu,\"count\":%zu,\"offset\":%zu}",first?"":",",(unsigned long long)(begin*page),count,offset);
        first=0;offset+=count;
    }
    fprintf(state,"],\"complete\":%s,\"testOnly\":true}\n",complete?"true":"false");
    fclose(data);fclose(state);free(resident);
    host_log("test capture %s: %zu bytes at %08X",complete?"complete":"FAILED",offset,cpu->pc);
}
