/* Default-off original sound ownership observations. No guest writes or CPU
 * restoration: each wrapped source call executes exactly once with its own ABI.
 * Transition observations are sampled at scheduler/call boundaries, not stores. */
static int audio_owner_enabled = -1;
static unsigned audio_owner_reports, audio_owner_nested;
static uint32_t audio_owner_last_table, audio_owner_last_header;
static int audio_owner_last_valid = -1;
static int audio_owner_ok(uint32_t address, uint32_t length) {
    return engine_flat_base && address >= 0x10000u && length &&
        (uint64_t)address + length <= 0x100000000ull;
}
static uint32_t audio_owner_read(uint32_t address) {
    return audio_owner_ok(address,4) ? G32(address) : UINT32_MAX;
}
static void audio_owner_name(uint32_t address,char out[161]) {
    unsigned i=0;
    if(audio_owner_ok(address,160))for(;i<160;i++) {
        unsigned char c=G8(address+i);
        if(!c)break;
        out[i]=(c>=32 && c<127)?(char)c:'?';
    }
    out[i]=0;
}
static uint32_t audio_owner_snapshot(EngineCPU *cpu,uint32_t fn,uint32_t caller,
                                 const char *stage,uint32_t tag,uint32_t tracked) {
    if(audio_owner_reports>=512)return UINT32_MAX;
    uint32_t table=G32(0x0087BC14u),header=G32(0x006A8954u);
    uint32_t count=audio_owner_ok(header,16)?G32(header+12):0;
    uint32_t definition=UINT32_MAX,record=UINT32_MAX,flags=UINT32_MAX,salt=UINT32_MAX;
    char name[161]={0},map[161]={0};
    if(count<=65536 && audio_owner_ok(table,count*32u) && tag!=UINT32_MAX && (tag&65535)<count) {
        uint32_t entry=table+(tag&65535)*32u;
        /* Reject stale salt before interpreting an unrelated definition. */
        if(G32(entry+12)==tag) {
            definition=G32(entry+20);audio_owner_name(G32(entry+16),name);
            if(audio_owner_ok(definition,32))record=G32(definition+28);
        }
    }
    /* Name of the actual current scenario definition; no guessed map globals. */
    uint32_t scenario=G32(0x00746F8Cu);
    if(scenario>=0x10000 && count<=65536 && audio_owner_ok(table,count*32u))
        for(uint32_t i=0;i<count;i++)if(G32(table+i*32u+20)==scenario) {
            audio_owner_name(G32(table+i*32u+16),map);break;
        }
    uint32_t definition_record=record;
    if(tracked!=UINT32_MAX)record=tracked;
    uint32_t pool=G32(0x007461A0u);
    if(record!=UINT32_MAX && audio_owner_ok(pool,56)) {
        uint32_t capacity=G16(pool+32),base=G32(pool+52);
        if((record&65535)<capacity && audio_owner_ok(base,(record&65535)*52u+52u)) {
            uint32_t at=base+(record&65535)*52u;salt=G16(at);
            if(salt==(record>>16))flags=G32(at+4);
        }
    }
    ++audio_owner_reports;
    host_log("[audio-ownership] seq=%u stage=%s fn=%08x caller=%08x valid=%u owner=%u table=%08x header=%08x count=%u map=%s tag=%08x name=%s definition=%08x definitionRecord=%08x record=%08x salt=%08x flags=%08x eax=%08x",
        audio_owner_reports,stage,fn,caller,G8(0x006A8150u),G8(0x00718FC6u),
        table,header,count,map,tag,name,definition,definition_record,record,salt,flags,cpu->gpr[0]);
    return record;
}
static int host_audio_ownership_dispatch(EngineCPU *cpu,uint32_t address) {
    if(audio_owner_enabled<0) {
        const char *value=getenv("HALO_AUDIO_OWNERSHIP_TRACE");
        audio_owner_enabled=value && !strcmp(value,"1");
    }
    if(!audio_owner_enabled || audio_owner_reports>=512)return 0;
    if(address!=0x00442550u && address!=0x00544090u && address!=0x00544120u && address!=0x0048A1A0u)return 0;
    uint32_t table=G32(0x0087BC14u),header=G32(0x006A8954u);
    int valid=G8(0x006A8150u);
    uint32_t caller=audio_owner_read(cpu->gpr[4]);
    if(table!=audio_owner_last_table || header!=audio_owner_last_header || valid!=audio_owner_last_valid) {
        audio_owner_snapshot(cpu,address,caller,"table-observed",UINT32_MAX,UINT32_MAX);
        audio_owner_last_table=table;audio_owner_last_header=header;audio_owner_last_valid=valid;
    }
    if(address==0x0048A1A0u)return 0;
    unsigned bit=address==0x00442550u?1:address==0x00544090u?2:4;
    if(audio_owner_nested&bit)return 0;
    if(bit==1 && audio_owner_read(cpu->gpr[4]+4)!=0x0066A044u)return 0;
    uint32_t tag=bit==1?UINT32_MAX:cpu->gpr[0];
    uint32_t tracked=audio_owner_snapshot(cpu,address,caller,"before",tag,UINT32_MAX);
    audio_owner_nested|=bit;
    engine_dispatch(cpu,address);
    audio_owner_nested&=~bit;
    if(bit==1)tag=cpu->gpr[0];
    audio_owner_snapshot(cpu,address,caller,"after",tag,bit==4?tracked:UINT32_MAX);
    return 1;
}
