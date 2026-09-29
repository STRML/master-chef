/* Windows names are case-insensitive; visionOS app storage is case-sensitive.
 * Resolve each existing component to its real spelling. Preserve new leaves
 * and wildcard patterns so create and directory enumeration still work. */
const char *host_path(const char *wp, char *buf, size_t size) {
    if (!size) return buf;
    const char *p = wp;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    int prefix = snprintf(buf, size, "%s", host_game_root);
    if (prefix < 0 || (size_t)prefix >= size) { buf[0] = 0; return buf; }
    size_t used = (size_t)prefix;
    while (*p) {
        size_t count = strcspn(p, "\\/");
        char component[1024], matched[1024];
        if (count >= sizeof component) { buf[0] = 0; return buf; }
        memcpy(component, p, count); component[count] = 0;
        matched[0] = 0;
        if (!strpbrk(component, "*?")) {
            DIR *directory = opendir(buf);
            if (directory) {
                struct dirent *entry;
                while ((entry = readdir(directory))) {
                    if (!strcasecmp(entry->d_name, component)) {
                        snprintf(matched, sizeof matched, "%s", entry->d_name);
                        if (!strcmp(entry->d_name, component)) break;
                    }
                }
                closedir(directory);
            }
        }
        const char *leaf = matched[0] ? matched : component;
        size_t length = strlen(leaf);
        int slash = used && buf[used - 1] != '/';
        if (used + (size_t)slash + length >= size) { buf[0] = 0; return buf; }
        if (slash) buf[used++] = '/';
        memcpy(buf + used, leaf, length + 1); used += length;
        p += count;
        while (*p == '\\' || *p == '/') p++;
    }
    return buf;
}
