/* Read RT_STRING resources out of a PE file on disk (used for Strings.dll and halo.exe's own strings). */
#include "host.h"
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct { uint8_t *data; size_t size; uint32_t image_base, rsrc_rva, rsrc_size; int loaded; } PEImage;
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static const uint8_t *rva_ptr(PEImage *pe, uint32_t rva) {
    uint32_t e_lfanew = rd32(pe->data + 0x3c); uint16_t nsec = rd16(pe->data + e_lfanew + 6); uint16_t optsize = rd16(pe->data + e_lfanew + 20);
    const uint8_t *sec = pe->data + e_lfanew + 24 + optsize;
    for (uint16_t i = 0; i < nsec; i++, sec += 40) { uint32_t va = rd32(sec + 12), vsz = rd32(sec + 8), raw = rd32(sec + 20), rsz = rd32(sec + 16); uint32_t span = vsz > rsz ? vsz : rsz;
        if (rva >= va && rva < va + span) { uint32_t off = raw + (rva - va); return off < pe->size ? pe->data + off : NULL; } }
    return NULL;
}
static int pe_load(PEImage *pe, const char *path) {
    if (pe->loaded) return pe->data != NULL; pe->loaded = 1;
    int fd = open(path, O_RDONLY); if (fd < 0) return 0; struct stat st; fstat(fd, &st); pe->data = malloc((size_t)st.st_size); pe->size = (size_t)st.st_size; read(fd, pe->data, pe->size); close(fd);
    uint32_t e_lfanew = rd32(pe->data + 0x3c); const uint8_t *opt = pe->data + e_lfanew + 24; pe->image_base = rd32(opt + 28);
    pe->rsrc_rva = rd32(opt + 96 + 2 * 8); pe->rsrc_size = rd32(opt + 96 + 2 * 8 + 4); return 1;
}
/* Walk resource dir: type -> name(id) -> language(first). Returns data RVA + size. */
static int find_resource(PEImage *pe, uint32_t type, uint32_t id, uint32_t *rva, uint32_t *size) {
    const uint8_t *root = rva_ptr(pe, pe->rsrc_rva); if (!root) return 0;
    const uint8_t *dir = root;
    for (int level = 0; level < 3; level++) {
        uint16_t nnamed = rd16(dir + 12), nid = rd16(dir + 14); const uint8_t *entries = dir + 16; const uint8_t *hit = NULL;
        uint32_t want = level == 0 ? type : level == 1 ? id : 0x409;   /* prefer US English at the language level */
        for (uint32_t i = 0; i < (uint32_t)nnamed + nid; i++) { const uint8_t *e = entries + 8 * i; uint32_t name = rd32(e);
            if (!(name & 0x80000000u) && name == want) { hit = e; break; } }
        if (!hit && level == 2 && nnamed + nid > 0) hit = entries;   /* fall back to the first language */
        if (!hit) return 0;
        uint32_t off = rd32(hit + 4);
        if (off & 0x80000000u) dir = root + (off & 0x7FFFFFFFu);
        else { const uint8_t *entry = root + off; *rva = rd32(entry); *size = rd32(entry + 4); return 1; }
    }
    return 0;
}
static PEImage strings_dll, main_exe;
static PEImage *image_for(uint32_t hinst) {
    const char *name = host_module_name(hinst); char path[1024];
    if (hinst == engine_pe_image_base || hinst == 0) { snprintf(path, sizeof path, "%s/halo.exe", host_game_root); return pe_load(&main_exe, path) ? &main_exe : NULL; }
    if (name && !strncasecmp(name, "strings", 7)) { snprintf(path, sizeof path, "%s/strings.dll", host_game_root); return pe_load(&strings_dll, path) ? &strings_dll : NULL; }
    return NULL;
}
/* LoadStringA semantics: string table block (id/16)+1 holds 16 counted UTF-16 strings. */
uint32_t host_load_string(uint32_t hinst, uint32_t id, char *out, uint32_t cap) {
    PEImage *pe = image_for(hinst); if (!pe) return 0;
    uint32_t rva, size; if (!find_resource(pe, 6, id / 16 + 1, &rva, &size)) return 0;
    const uint8_t *p = rva_ptr(pe, rva); if (!p) return 0; const uint8_t *end = p + size;
    for (uint32_t k = 0; k < 16 && p + 2 <= end; k++) { uint16_t len = rd16(p); p += 2;
        if (k == id % 16) { uint32_t n = 0; for (uint16_t i = 0; i < len && n + 1 < cap; i++) { uint16_t c = rd16(p + 2 * i); out[n++] = c < 128 ? (char)c : '?'; } out[n] = 0; return n; }
        p += 2u * len; }
    return 0;
}
int host_find_resource(uint32_t hinst, uint32_t type, uint32_t id, uint32_t *rva, uint32_t *size, const uint8_t **bytes) {
    PEImage *pe = image_for(hinst); if (!pe) return 0;
    if (!find_resource(pe, type, id, rva, size)) return 0;
    *bytes = rva_ptr(pe, *rva); if (!*bytes) return 0;
    /* Clamp the reported size to what is actually present in the file image. */
    size_t avail = pe->size - (size_t)(*bytes - pe->data);
    if (*size > avail) *size = (uint32_t)avail;
    return 1;
}

/* Dialog templates (DLGTEMPLATEEX): log every text and pick a sensible button to "press". */
static const uint8_t *skip_sz_or_ord(const uint8_t *p, char *text, size_t cap) {
    uint16_t first = rd16(p);
    if (first == 0xFFFF) { snprintf(text, cap, "#%u", rd16(p + 2)); return p + 4; }
    size_t n = 0; while (rd16(p)) { uint16_t c = rd16(p); if (n + 1 < cap) text[n++] = c < 128 ? (char)c : '?'; p += 2; } text[n] = 0; return p + 2;
}
uint32_t host_dialog_choose(uint32_t hinst, uint32_t id, char *summary, size_t cap) {
    uint32_t rva, size; const uint8_t *b; summary[0] = 0;
    if (!host_find_resource(hinst, 5, id, &rva, &size, &b)) return 1;
    if (rd16(b) != 1 || rd16(b + 2) != 0xFFFF) return 1;   /* only DLGTEMPLATEEX handled */
    uint32_t style = rd32(b + 12); uint16_t items = rd16(b + 16); const uint8_t *p = b + 26; char text[256];
    p = skip_sz_or_ord(p, text, sizeof text); p = skip_sz_or_ord(p, text, sizeof text);
    p = skip_sz_or_ord(p, text, sizeof text); size_t used = (size_t)snprintf(summary, cap, "[%s]", text);
    if (style & 0x40) { p += 6; p = skip_sz_or_ord(p, text, sizeof text); }   /* DS_SETFONT: size, weight, italic, charset, face */
    uint32_t choice = 1, first_button = 0, normal_video_choice = 0;
    for (uint16_t i = 0; i < items && p < b + size; i++) {
        p = b + (((p - b) + 3) & ~3u);
        uint32_t cstyle = rd32(p + 8); uint32_t cid = rd32(p + 20); p += 24;
        char cls[64]; p = skip_sz_or_ord(p, cls, sizeof cls); p = skip_sz_or_ord(p, text, sizeof text); uint16_t extra = rd16(p); p += 2 + extra;
        if (text[0] && used + strlen(text) + 4 < cap) used += (size_t)snprintf(summary + used, cap - used, " | %s", text);
        int is_button = !strcmp(cls, "#128") || !strcasecmp(cls, "Button");
        /* Halo's adapter warning also contains "Continue in 'Safe Mode'".
         * Selecting the last Continue button silently changes video settings.
         * Prefer the normal launch choice for this specific warning only. */
        if (id == 102 && is_button && !strcmp(text, "Continue Anyway")) normal_video_choice = cid;
        if (is_button && (cstyle & 0xF) <= 1) { if (!first_button) first_button = cid; if (!strncasecmp(text, "Continue", 8) || !strncasecmp(text, "OK", 2) || !strncasecmp(text, "&Continue", 9)) choice = cid; }
    }
    if (normal_video_choice) return normal_video_choice;
    if (choice == 1 && first_button) choice = first_button;
    return choice;
}
