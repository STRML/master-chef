/* Shared ABI for the same translated C compiled against two x87 layouts. */
#ifndef HALO_X87_TRANSLATED_TEST_H
#define HALO_X87_TRANSLATED_TEST_H
#include <stdint.h>

enum { X87T_RECORDS = 64, X87T_FUNCTIONS = 4 };
#define X87T_OBJECT(k) (0x10000000u + (k) * 0x400u)
#define X87T_BOX(k) (0x10100000u + (k) * 0x40u)
#define X87T_MATRIX(k) (0x10200000u + (k) * 0x40u)
#define X87T_OUTPUT(k) (0x10300000u + (k) * 0x40u)
#define X87T_PLANES(k) (0x10400000u + (k) * 0x100u)
#define X87T_BYTES(k) (0x10500000u + (k) * 0x10u)
#define X87T_STACK 0x00200000u
#define X87T_RETURN 0x00401234u

typedef struct {
    /* Include empty logical register contents: FLDENV may make them valid. */
    uint64_t st[8], instruction_count, instruction_limit;
    uint32_t gpr[8], flags, pc, fs_base;
    uint16_t control, status, status_word, tag_word;
    uint8_t valid, top;
    int host_round, host_exceptions;
    const char *failure;
} X87TranslatedSnapshot;

#define X87T_DECLARE(prefix) \
    void prefix##_call(unsigned which, unsigned record, unsigned top, unsigned depth, \
                       uint16_t control, unsigned alias, X87TranslatedSnapshot *out); \
    double prefix##_time(unsigned which, unsigned iterations)
X87T_DECLARE(x87t_shifting);
X87T_DECLARE(x87t_rotating);
#endif
