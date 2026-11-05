#include "games_ini.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#if defined(_WIN32)
#include <direct.h>
#define safir_sys_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define safir_sys_mkdir(p) mkdir(p, 0777)
#endif

static void ensure_parent_dirs(const char *filepath) {
    if (!filepath) return;
    char tmp[512];
    size_t len = strlen(filepath);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, filepath, len + 1);

    size_t start = 0;
    if (strncmp(tmp, "sdmc:/", 6) == 0) {
        start = 6;
    } else if (len >= 3 && tmp[1] == ':' && (tmp[2] == '/' || tmp[2] == '\\')) {
        start = 3;
    }

    for (size_t i = start; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char orig = tmp[i];
            tmp[i] = '\0';
            safir_sys_mkdir(tmp);
            tmp[i] = orig;
        }
    }
}

static void trim_line(char *str) {
    if (!str) return;
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' || str[len - 1] == ' ' || str[len - 1] == '\t')) {
        str[--len] = '\0';
    }
}

bool safir_games_ini_load(const char *ini_path, uint64_t title_id, SafirRect *out_rect) {
    if (!ini_path || !out_rect) return false;

    FILE *f = fopen(ini_path, "r");
    if (!f) return false;

    char section_hdr1[64];
    char section_hdr2[64];
    snprintf(section_hdr1, sizeof(section_hdr1), "[%016" PRIX64 "]", title_id);
    snprintf(section_hdr2, sizeof(section_hdr2), "[%016" PRIx64 "]", title_id);

    char line[256];
    bool in_section = false;
    int found_mask = 0; /* bit 0: x, bit 1: y, bit 2: w, bit 3: h */
    SafirRect r = { 0, 0, 0, 0 };

    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            if (in_section) {
                /* Exited target section */
                break;
            }
            if (strcmp(line, section_hdr1) == 0 || strcmp(line, section_hdr2) == 0) {
                in_section = true;
            }
            continue;
        }

        if (in_section) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            trim_line(key);
            while (*val == ' ' || *val == '\t') val++;

            if (strcmp(key, "dialogue_x") == 0) {
                r.x = (uint16_t)atoi(val);
                found_mask |= 1;
            } else if (strcmp(key, "dialogue_y") == 0) {
                r.y = (uint16_t)atoi(val);
                found_mask |= 2;
            } else if (strcmp(key, "dialogue_w") == 0) {
                r.w = (uint16_t)atoi(val);
                found_mask |= 4;
            } else if (strcmp(key, "dialogue_h") == 0) {
                r.h = (uint16_t)atoi(val);
                found_mask |= 8;
            }
        }
    }

    fclose(f);

    if (found_mask == 0x0F) {
        *out_rect = r;
        return true;
    }
    return false;
}

bool safir_games_ini_save(const char *ini_path, uint64_t title_id, SafirRect rect) {
    if (!ini_path) return false;
    ensure_parent_dirs(ini_path);

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", ini_path);

    FILE *in = fopen(ini_path, "r");
    FILE *out = fopen(temp_path, "w");
    if (!out) {
        if (in) fclose(in);
        return false;
    }

    char target_hdr[64];
    char target_hdr_lower[64];
    snprintf(target_hdr, sizeof(target_hdr), "[%016" PRIX64 "]", title_id);
    snprintf(target_hdr_lower, sizeof(target_hdr_lower), "[%016" PRIx64 "]", title_id);

    bool replaced = false;

    if (in) {
        char line[256];
        bool skipping_existing = false;

        while (fgets(line, sizeof(line), in)) {
            char check[256];
            strncpy(check, line, sizeof(check) - 1);
            check[sizeof(check) - 1] = '\0';
            trim_line(check);

            if (check[0] == '[') {
                if (skipping_existing) {
                    skipping_existing = false;
                }
                if (strcmp(check, target_hdr) == 0 || strcmp(check, target_hdr_lower) == 0) {
                    /* Write the new section here */
                    fprintf(out, "[%016" PRIX64 "]\n", title_id);
                    fprintf(out, "dialogue_x=%u\n", (unsigned)rect.x);
                    fprintf(out, "dialogue_y=%u\n", (unsigned)rect.y);
                    fprintf(out, "dialogue_w=%u\n", (unsigned)rect.w);
                    fprintf(out, "dialogue_h=%u\n\n", (unsigned)rect.h);
                    skipping_existing = true;
                    replaced = true;
                    continue;
                }
            }

            if (!skipping_existing) {
                fputs(line, out);
            }
        }
        fclose(in);
    }

    if (!replaced) {
        /* Section was not present in the original file, append it */
        fprintf(out, "\n[%016" PRIX64 "]\n", title_id);
        fprintf(out, "dialogue_x=%u\n", (unsigned)rect.x);
        fprintf(out, "dialogue_y=%u\n", (unsigned)rect.y);
        fprintf(out, "dialogue_w=%u\n", (unsigned)rect.w);
        fprintf(out, "dialogue_h=%u\n", (unsigned)rect.h);
    }

    fclose(out);

    remove(ini_path);
    if (rename(temp_path, ini_path) != 0) {
        return false;
    }

    return true;
}
