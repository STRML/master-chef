/* Optional a10 diagnostics at original engine call boundaries. No asset edits.
 * 004C6E80 is called at the beginning of each main-loop iteration, outside Present.
 * Campaign mode is 0; 1/2 and 0071C2D8 are network client/server state.
 */
static float a10_float(uint32_t p) { float v; memcpy(&v, GPTR(p), 4); return v; }
static void host_a10_boundary(EngineCPU *cpu, uint32_t address) {
    static int enabled = -1;
    static uint32_t loops;
    if (enabled < 0) enabled = getenv("HALO_A10_TRACE") != NULL;
    if (!enabled) return;
    if (address == 0x004C8800u || address == 0x00477EA0u)
        host_log("[a10-call] fn=%08x profileCount=%u shell=%u", address, G16(0x006894B8u), G8(0x00718FC9u));
    if ((address == 0x005527F0u || address == 0x005528F0u) && loops % 60 == 0)
        host_log("[a10-bsp] loop=%u fn=%08x bsp=%d tag=%08x cluster=%d visibleClusters=%u surfaces=%u geometry=%u indexBuffer=%08x observerLeaf=%08x observerCluster=%d",
            loops,address,(int16_t)G16(0x0069E8D8u),G32(0x00746F9Cu),(int32_t)G32(0x007C3348u),G16(0x007D0390u),G16(0x00850394u),G8(0x006E3AD8u),G32(0x006E3ADCu),G32(0x006AC6DCu),(int16_t)G16(0x006AC6E0u));
    if ((address == 0x00552DE0u || address == 0x00511F30u) && loops % 60 == 0) {
        static uint32_t last_loop, count;
        if (last_loop != loops) { last_loop = loops; count = 0; }
        if (count++ < 12) host_log("[a10-material] loop=%u fn=%08x eax=%08x ecx=%08x arg=%08x,%08x,%08x,%08x target=%08x renderMode=%u",
            loops,address,cpu->gpr[0],cpu->gpr[1],ARG(0),ARG(1),ARG(2),ARG(3),G32(0x007C048Cu),G16(0x0069C67Cu));
    }
    if (address == 0x005528F0u && loops % 60 == 0)
        host_log("[a10-effects] loop=%u enabled=%u callback=%08x effects=%08x,%08x,%08x,%08x variant=%u",
            loops,G8(0x006893F1u),G32(0x007C048Cu),G32(0x0069D410u),G32(0x0069D430u),G32(0x0069D450u),G32(0x0069D470u),G8(0x006E0A04u));
    if (address != 0x004C6E80u) return;
    loops++;
    if (loops > 3 && loops % 60) return;
    uint32_t pg = G32(0x0087A478u), pa = G32(0x0087A480u);
    uint32_t player = pg ? G32(pg + 4) : UINT32_MAX;
    uint32_t pr = pa && player != UINT32_MAX ? G32(pa + 0x34) + (player & 0xffffu) * 0x200u : 0;
    uint32_t unit = pr ? G32(pr + 0x34) : UINT32_MAX;
    uint32_t oa = G32(0x008603B0u);
    uint32_t body = oa && unit != UINT32_MAX ? G32(G32(oa + 0x34) + (unit & 0xffffu) * 12u + 8) : 0;
    uint32_t controls = G32(0x006B145Cu), time = G32(0x006F1D6Cu), cine = G32(0x006F187Cu);
    host_log("[a10] loop=%u tick=%u step=%u mode=%u profiles=%u player=%08x unit=%08x controlled=%08x shell=%u time=%u,%u,%u director=%u camera=%08x cinematic=%u,%u inputBlock=%u,%u queue=%u",
        loops, time ? G32(time+12) : 0, time ? G16(time+16) : 0, G16(0x00719720u), G16(0x006894B8u), player, unit,
        controls ? G32(controls+16) : UINT32_MAX, G8(0x00718FC9u), time?G8(time):0,time?G8(time+1):0,time?G8(time+2):0,
        G16(0x006AC55Cu),G32(0x006AC568u),cine?G8(cine+9):0,cine?G8(cine+10):0,G8(0x006AC5B1u),G8(0x006AC5B2u),G32(0x006F7ECCu));
    host_log("[a10-cinematic] loop=%u bsp=%d scriptCamera=%u point=%u remaining=%.5f skipBusy=%u save=%u commit=%u revert=%u cameraControl=%u difficulty=%u",
        loops,(int16_t)G16(0x0069E8D8u),G16(0x006869D2u),G16(0x006869D4u),a10_float(0x006869D8u),G8(0x006E3000u),G8(0x0071973Cu),G8(0x0071973Fu),G8(0x0071973Bu),
        G32(0x0087BC0Cu)?G8(G32(0x0087BC0Cu)):0,G16(0x00714DDEu));
    host_log("[a10-view] loop=%u unitPos=%.5f,%.5f,%.5f observer=%.5f,%.5f,%.5f forward=%.5f,%.5f,%.5f render=%.5f,%.5f,%.5f",
        loops,body?a10_float(body+0x5c):0,body?a10_float(body+0x60):0,body?a10_float(body+0x64):0,
        a10_float(0x006AC6D0u),a10_float(0x006AC6D4u),a10_float(0x006AC6D8u),a10_float(0x006AC6F0u),a10_float(0x006AC6F4u),a10_float(0x006AC6F8u),
        a10_float(0x00719BC8u),a10_float(0x00719BCCu),a10_float(0x00719BD0u));
}
