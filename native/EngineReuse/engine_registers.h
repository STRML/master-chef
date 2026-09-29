/* Include after engine_cpu.h and helper declarations, before generated code. */
#define eax (cpu->gpr[0])
#define ecx (cpu->gpr[1])
#define edx (cpu->gpr[2])
#define ebx (cpu->gpr[3])
#define esp (cpu->gpr[4])
#define ebp (cpu->gpr[5])
#define esi (cpu->gpr[6])
#define edi (cpu->gpr[7])
#define eflags (cpu->flags)
#define LO8(r) ((uint8_t)(r))
#define HI8(r) ((uint8_t)((r)>>8))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r,v) ((r)=((r)&0xffffff00u)|(uint8_t)(v))
#define SET_HI8(r,v) ((r)=((r)&0xffff00ffu)|((uint32_t)(uint8_t)(v)<<8))
#define SET_LO16(r,v) ((r)=((r)&0xffff0000u)|(uint16_t)(v))
#define BSWAP32(r) __builtin_bswap32((uint32_t)(r))
/* Segment-selector pseudo-registers. Real memory access never uses these in the
   flat model (FS/GS bases are folded in at the address-formatting stage); they
   exist only so that rare or spuriously-decoded segment ops (push cs, mov ax,ds,
   ...) compile. Values are the standard Win32 user-mode selectors; unused. */
#define _seg_cs 0x001Bu
#define _seg_ds 0x0023u
#define _seg_es 0x0023u
#define _seg_ss 0x0023u
#define _seg_fs 0x003Bu
#define _seg_gs 0x0000u
