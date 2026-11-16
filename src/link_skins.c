/* SPDX-License-Identifier: GPL-3.0-or-later
   Link Skins: a skin picker for the OoT Remake co-op mod loader (oot-remake-multiplayer, docs/mods.md).

   Layout, inside the game's mods folder:
     link-skins/mod.json          this mod; its "skin" choice is rewritten from the skins found
     link-skins/link_skins.dll    this file
     link-skins/skins/<id>/       one skin: skin.json (optional) and romfs/ (Model, Tex, ...)
     link-skins/romfs/            every skin's files (see sync_romfs); the game lists them at its start
     link-skins/romfs.txt         what is in romfs/ now

   A skin never ships a resource size table of its own (one found in a skin is only read for its sizes). The table
   in romfs/ is built here: the game's own, raised by every other mod's table found in the mods folder, plus the
   entries the chosen skin's files need. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_mod_api.h"
#include "zstd.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define VERSION "0.4.4"
#ifndef DLL_NAME
#define DLL_NAME "link_skins.dll"
#endif
#define MAX_SKINS 63
#define MAX_PATH_U8 1024
#define TABLE_DIR "System/Resource"
#define TABLE_TAIL ".rsizetable.zs"

static const CoopModApi *api;

static void say(const char *fmt, ...) {
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (api != NULL) api->log(api, line);
    else fprintf(stderr, "link-skins: %s\n", line);
}

/* ---------------------------------------------------------------- files (UTF-8 paths, '/' between parts) */

#ifdef _WIN32
static wchar_t *wide(const char *u8, wchar_t *out, int cap) {
    if (MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cap) <= 0) out[0] = 0;
    for (wchar_t *p = out; *p; ++p)
        if (*p == L'/') *p = L'\\';
    return out;
}
static FILE *open_file(const char *path, const char *mode) {
    wchar_t w[MAX_PATH_U8], m[8];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, m, 8);
    return _wfopen(wide(path, w, MAX_PATH_U8), m);
}
static int make_dir(const char *path) {
    wchar_t w[MAX_PATH_U8];
    return CreateDirectoryW(wide(path, w, MAX_PATH_U8), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
static int remove_file(const char *path) {
    wchar_t w[MAX_PATH_U8];
    wide(path, w, MAX_PATH_U8);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(w) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}
static void remove_dir(const char *path) {
    wchar_t w[MAX_PATH_U8];
    RemoveDirectoryW(wide(path, w, MAX_PATH_U8)); /* only when empty */
}
typedef void (*list_fn)(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime);
static int list_dir(const char *path, list_fn fn, void *user) {
    wchar_t w[MAX_PATH_U8 + 4];
    WIN32_FIND_DATAW d;
    HANDLE h;
    wide(path, w, MAX_PATH_U8);
    wcscat(w, L"\\*");
    h = FindFirstFileW(w, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[MAX_PATH_U8];
        if (wcscmp(d.cFileName, L".") == 0 || wcscmp(d.cFileName, L"..") == 0) continue;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; /* links are left out */
        if (WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, name, sizeof name, NULL, NULL) <= 0) continue;
        fn(user, name, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
           ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow,
           ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return 1;
}
#else
static FILE *open_file(const char *path, const char *mode) { return fopen(path, mode); }
static int make_dir(const char *path) {
    struct stat st;
    return mkdir(path, 0777) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}
static int remove_file(const char *path) {
    struct stat st;
    return unlink(path) == 0 || stat(path, &st) != 0;
}
static void remove_dir(const char *path) { rmdir(path); }
typedef void (*list_fn)(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime);
static int list_dir(const char *path, list_fn fn, void *user) {
    DIR *d = opendir(path);
    struct dirent *e;
    if (d == NULL) return 0;
    while ((e = readdir(d)) != NULL) {
        char full[MAX_PATH_U8];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;
        fn(user, e->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, (uint64_t)st.st_mtime);
    }
    closedir(d);
    return 1;
}
#endif

static int join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}

/* Makes every folder of `path` below `root` (path names a file: its last part is not made). */
static int make_parents(const char *root, const char *path) {
    char buf[MAX_PATH_U8];
    size_t r = strlen(root);
    if (strlen(path) >= sizeof buf) return 0;
    strcpy(buf, path);
    for (char *p = buf + r + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (!make_dir(buf)) return 0;
        *p = '/';
    }
    return 1;
}

static unsigned char *read_all(const char *path, size_t *size, size_t most) {
    FILE *f = open_file(path, "rb");
    unsigned char *data = NULL;
    long n;
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && (size_t)n <= most && fseek(f, 0, SEEK_SET) == 0) {
        data = (unsigned char *)malloc((size_t)n + 1);
        if (data != NULL && fread(data, 1, (size_t)n, f) != (size_t)n) {
            free(data);
            data = NULL;
        }
        if (data != NULL) {
            data[n] = 0;
            *size = (size_t)n;
        }
    }
    fclose(f);
    return data;
}

/* Writes through a .tmp beside the file, so a half-written file is never left under the real name. */
static int write_all(const char *path, const void *data, size_t size) {
    char tmp[MAX_PATH_U8];
    FILE *f;
    int ok;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    f = open_file(tmp, "wb");
    if (f == NULL) return 0;
    ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove_file(tmp);
        return 0;
    }
#ifdef _WIN32
    {
        wchar_t a[MAX_PATH_U8], b[MAX_PATH_U8];
        ok = MoveFileExW(wide(tmp, a, MAX_PATH_U8), wide(path, b, MAX_PATH_U8), MOVEFILE_REPLACE_EXISTING) != 0;
    }
#else
    ok = rename(tmp, path) == 0;
#endif
    if (!ok) remove_file(tmp);
    return ok;
}

static int copy_file(const char *from, const char *to) {
    FILE *in = open_file(from, "rb"), *out;
    static char buf[1 << 16];
    size_t n;
    int ok = 1;
    if (in == NULL) return 0;
    out = open_file(to, "wb");
    if (out == NULL) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) remove_file(to);
    return ok;
}

/* ---------------------------------------------------------------- every file under a folder */

typedef struct {
    char *rel; /* path below the root, '/' between parts */
    uint64_t size, mtime;
} Entry;
typedef struct {
    Entry *v;
    size_t n, cap;
    int failed;
} Entries;
typedef struct {
    Entries *out;
    const char *root;
    const char *rel;
    int depth;
} WalkAt;

static void walk(Entries *out, const char *root, const char *rel, int depth);

static void walk_one(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime) {
    WalkAt *at = (WalkAt *)user;
    char rel[MAX_PATH_U8];
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return;
    if (snprintf(rel, sizeof rel, "%s%s%s", at->rel, at->rel[0] ? "/" : "", name) >= (int)sizeof rel) {
        at->out->failed = 1;
        return;
    }
    if (is_dir) {
        if (at->depth < 12) walk(at->out, at->root, rel, at->depth + 1);
        return;
    }
    if (at->out->n == at->out->cap) {
        size_t cap = at->out->cap ? at->out->cap * 2 : 64;
        Entry *v = (Entry *)realloc(at->out->v, cap * sizeof *v);
        if (v == NULL) {
            at->out->failed = 1;
            return;
        }
        at->out->v = v;
        at->out->cap = cap;
    }
    at->out->v[at->out->n].rel = strdup(rel);
    at->out->v[at->out->n].size = size;
    at->out->v[at->out->n].mtime = mtime;
    if (at->out->v[at->out->n].rel == NULL) at->out->failed = 1;
    else at->out->n++;
}

static void walk(Entries *out, const char *root, const char *rel, int depth) {
    char dir[MAX_PATH_U8];
    WalkAt at = {out, root, rel, depth};
    if (snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel) >= (int)sizeof dir) {
        out->failed = 1;
        return;
    }
    list_dir(dir, walk_one, &at);
}

static int by_rel(const void *a, const void *b) { return strcmp(((const Entry *)a)->rel, ((const Entry *)b)->rel); }

static void entries_free(Entries *e) {
    for (size_t i = 0; i < e->n; ++i) free(e->v[i].rel);
    free(e->v);
    memset(e, 0, sizeof *e);
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s + a - b, tail) == 0;
}

static int is_table(const char *rel) {
    return strncmp(rel, TABLE_DIR "/", sizeof TABLE_DIR) == 0 && ends_with(rel, TABLE_TAIL);
}

/* ---------------------------------------------------------------- the resource size table (RESTBL, version 1)
   magic "RESTBL", u32 version, u32 name length, u32 hash rows, u32 name rows; (crc32 of the path, size) rows sorted
   by crc; then (name, size) rows for paths whose crc collides. A path is the romfs path without ".zs". */

typedef struct {
    uint32_t crc, size;
} Row;
typedef struct {
    uint32_t version, name_len, names;
    Row *rows;
    size_t n, cap;
    unsigned char *name_rows; /* names * (name_len + 4) bytes, kept as read */
} Table;

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t crc32_of(const char *s) {
    static uint32_t t[256];
    uint32_t c = 0xFFFFFFFFu;
    if (t[1] == 0)
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i;
            for (int k = 0; k < 8; ++k) r = (r >> 1) ^ (0xEDB88320u & (0u - (r & 1)));
            t[i] = r;
        }
    for (; *s; ++s) c = t[(c ^ (unsigned char)*s) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void table_free(Table *t) {
    free(t->rows);
    free(t->name_rows);
    memset(t, 0, sizeof *t);
}

static int table_read(const char *path, Table *t) {
    size_t zn = 0, n;
    unsigned char *z = read_all(path, &zn, 64u << 20), *d = NULL;
    unsigned long long full;
    int ok = 0;
    memset(t, 0, sizeof *t);
    if (z == NULL) return 0;
    full = ZSTD_getFrameContentSize(z, zn);
    if (full == ZSTD_CONTENTSIZE_ERROR || full == ZSTD_CONTENTSIZE_UNKNOWN || full > (256u << 20)) goto done;
    d = (unsigned char *)malloc((size_t)full + 1);
    if (d == NULL) goto done;
    n = ZSTD_decompress(d, (size_t)full, z, zn);
    if (ZSTD_isError(n) || n < 0x16 || memcmp(d, "RESTBL", 6) != 0) goto done;
    t->version = rd32(d + 6);
    t->name_len = rd32(d + 10);
    t->n = rd32(d + 14);
    t->names = rd32(d + 18);
    if (t->name_len == 0 || t->name_len > 4096 || t->n > (n - 0x16) / 8) goto done;
    if ((uint64_t)t->names * (t->name_len + 4) != n - 0x16 - t->n * 8) goto done;
    t->cap = t->n + 256;
    t->rows = (Row *)malloc(t->cap * sizeof(Row));
    t->name_rows = (unsigned char *)malloc((size_t)t->names * (t->name_len + 4) + 1);
    if (t->rows == NULL || t->name_rows == NULL) goto done;
    for (size_t i = 0; i < t->n; ++i) {
        t->rows[i].crc = rd32(d + 0x16 + 8 * i);
        t->rows[i].size = rd32(d + 0x16 + 8 * i + 4);
        if (i > 0 && t->rows[i].crc <= t->rows[i - 1].crc) goto done; /* not sorted: not a table as the game's */
    }
    memcpy(t->name_rows, d + 0x16 + t->n * 8, (size_t)t->names * (t->name_len + 4));
    ok = 1;
done:
    free(z);
    free(d);
    if (!ok) table_free(t);
    return ok;
}

static int table_write(const Table *t, const char *path) {
    size_t n = 0x16 + t->n * 8 + (size_t)t->names * (t->name_len + 4), cap = ZSTD_compressBound(n), zn;
    unsigned char *d = (unsigned char *)malloc(n), *z = (unsigned char *)malloc(cap);
    int ok = 0;
    if (d == NULL || z == NULL) goto done;
    memcpy(d, "RESTBL", 6);
    wr32(d + 6, t->version);
    wr32(d + 10, t->name_len);
    wr32(d + 14, (uint32_t)t->n);
    wr32(d + 18, t->names);
    for (size_t i = 0; i < t->n; ++i) {
        wr32(d + 0x16 + 8 * i, t->rows[i].crc);
        wr32(d + 0x16 + 8 * i + 4, t->rows[i].size);
    }
    memcpy(d + 0x16 + t->n * 8, t->name_rows, (size_t)t->names * (t->name_len + 4));
    zn = ZSTD_compress(z, cap, d, n, 9);
    if (ZSTD_isError(zn)) goto done;
    ok = write_all(path, z, zn);
done:
    free(d);
    free(z);
    return ok;
}

static unsigned char *table_name_row(const Table *t, const char *path) {
    size_t len = strlen(path);
    if (len >= t->name_len) return NULL;
    for (uint32_t i = 0; i < t->names; ++i) {
        unsigned char *row = t->name_rows + (size_t)i * (t->name_len + 4);
        if (memcmp(row, path, len + 1) == 0) return row;
    }
    return NULL;
}

static size_t table_find(const Table *t, uint32_t crc, int *found) {
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t->rows[mid].crc < crc) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < t->n && t->rows[lo].crc == crc;
    return lo;
}

/* 0 when the table has no entry */
static uint32_t table_get(const Table *t, const char *path) {
    unsigned char *row = table_name_row(t, path);
    int found;
    size_t at;
    if (row != NULL) return rd32(row + t->name_len);
    at = table_find(t, crc32_of(path), &found);
    return found ? t->rows[at].size : 0;
}

static int table_raise_crc(Table *t, uint32_t crc, uint32_t size) {
    int found;
    size_t at = table_find(t, crc, &found);
    if (found) {
        if (t->rows[at].size < size) t->rows[at].size = size;
        return 1;
    }
    if (t->n == t->cap) {
        size_t cap = t->cap * 2 + 256;
        Row *rows = (Row *)realloc(t->rows, cap * sizeof(Row));
        if (rows == NULL) return 0;
        t->rows = rows;
        t->cap = cap;
    }
    memmove(t->rows + at + 1, t->rows + at, (t->n - at) * sizeof(Row));
    t->rows[at].crc = crc;
    t->rows[at].size = size;
    t->n++;
    return 1;
}

/* The entry for `path` becomes at least `size` (added when the table has none). */
static int table_raise(Table *t, const char *path, uint32_t size) {
    unsigned char *row = table_name_row(t, path);
    if (row != NULL) {
        if (rd32(row + t->name_len) < size) wr32(row + t->name_len, size);
        return 1;
    }
    return table_raise_crc(t, crc32_of(path), size);
}

/* Every entry of `other` that is larger, or that `t` lacks, goes into `t`. */
static void table_merge(Table *t, const Table *other) {
    for (size_t i = 0; i < other->n; ++i) table_raise_crc(t, other->rows[i].crc, other->rows[i].size);
    if (other->name_len == t->name_len)
        for (uint32_t i = 0; i < other->names; ++i) {
            const unsigned char *row = other->name_rows + (size_t)i * (other->name_len + 4);
            unsigned char *mine = memchr(row, 0, other->name_len) ? table_name_row(t, (const char *)row) : NULL;
            if (mine != NULL && rd32(mine + t->name_len) < rd32(row + other->name_len))
                wr32(mine + t->name_len, rd32(row + other->name_len));
        }
}

/* ---------------------------------------------------------------- skin.json (a small JSON reader: what we need) */

typedef struct {
    const char *p, *end;
    int bad;
} Json;

static void js_space(Json *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

/* Reads a string into out (cut at cap - 1); escapes \" \\ \/ \n \t and \uXXXX (as UTF-8). */
static int js_string(Json *j, char *out, size_t cap) {
    size_t n = 0;
    js_space(j);
    if (j->p >= j->end || *j->p != '"') return j->bad = 1, 0;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        char utf[4];
        int len = 1;
        utf[0] = (char)c;
        if (c == '\\' && j->p < j->end) {
            c = (unsigned char)*j->p++;
            if (c == 'n') utf[0] = '\n';
            else if (c == 't') utf[0] = '\t';
            else if (c == 'r') utf[0] = '\r';
            else if (c == 'b' || c == 'f') utf[0] = ' ';
            else if (c == 'u' && j->end - j->p >= 4) {
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    unsigned h = (unsigned char)j->p[k];
                    v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : 0);
                }
                j->p += 4;
                if (v < 0x80) utf[0] = (char)v;
                else if (v < 0x800) {
                    utf[0] = (char)(0xC0 | v >> 6);
                    utf[1] = (char)(0x80 | (v & 0x3F));
                    len = 2;
                } else {
                    utf[0] = (char)(0xE0 | v >> 12);
                    utf[1] = (char)(0x80 | ((v >> 6) & 0x3F));
                    utf[2] = (char)(0x80 | (v & 0x3F));
                    len = 3;
                }
            } else utf[0] = (char)c;
        }
        for (int k = 0; k < len; ++k)
            if (out != NULL && n + 1 < cap) out[n++] = utf[k];
    }
    if (j->p >= j->end) return j->bad = 1, 0;
    j->p++;
    if (out != NULL && cap > 0) out[n] = 0;
    return 1;
}

static void js_skip(Json *j, int depth);

static void js_skip_group(Json *j, char close, int depth) {
    j->p++;
    for (;;) {
        js_space(j);
        if (j->p >= j->end) {
            j->bad = 1;
            return;
        }
        if (*j->p == close) {
            j->p++;
            return;
        }
        if (close == '}') {
            if (!js_string(j, NULL, 0)) return;
            js_space(j);
            if (j->p >= j->end || *j->p != ':') {
                j->bad = 1;
                return;
            }
            j->p++;
        }
        js_skip(j, depth + 1);
        if (j->bad) return;
        js_space(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

static void js_skip(Json *j, int depth) {
    js_space(j);
    if (j->p >= j->end || depth > 32) {
        j->bad = 1;
        return;
    }
    if (*j->p == '"') js_string(j, NULL, 0);
    else if (*j->p == '{') js_skip_group(j, '}', depth);
    else if (*j->p == '[') js_skip_group(j, ']', depth);
    else
        while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\n' &&
               *j->p != '\r' && *j->p != '\t')
            j->p++;
}

typedef struct {
    char path[200];
    uint32_t size;
} SizeNote;

typedef struct {
    char id[64];
    char name[80], author[80], version[32], description[200];
    char dev[20];      /* skin.json "dev": the check number of the key that unlocks this skin in the picker ("": anyone) */
    int locked;        /* a dev skin, and this PC has no dev.key that fits */
    char colour[16];   /* skin.json "colour": the co-op tunic colour that stands for this skin ("": one is given) */
    int colour_at;     /* its number in kColours, -1: none (more skins than colours) */
    SizeNote *sizes;
    size_t n_sizes;
} Skin;

/* Reads skin.json's top level: name, author, version, description, and sizes {"<romfs path>": bytes}. */
static int skin_json(const char *text, size_t len, Skin *s) {
    Json j = {text, text + len, 0};
    if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) j.p += 3;
    js_space(&j);
    if (j.p >= j.end || *j.p != '{') return 0;
    j.p++;
    for (;;) {
        char key[64];
        js_space(&j);
        if (j.p < j.end && *j.p == '}') return 1;
        if (!js_string(&j, key, sizeof key)) return 0;
        js_space(&j);
        if (j.p >= j.end || *j.p != ':') return 0;
        j.p++;
        js_space(&j);
        if (j.p < j.end && *j.p == '"' &&
            (!strcmp(key, "colour") || !strcmp(key, "color"))) {
            if (!js_string(&j, s->colour, sizeof s->colour)) return 0;
        } else if (j.p < j.end && *j.p == '"' && !strcmp(key, "dev")) {
            if (!js_string(&j, s->dev, sizeof s->dev)) return 0;
        } else if (j.p < j.end && *j.p == '"' &&
            (!strcmp(key, "name") || !strcmp(key, "author") || !strcmp(key, "version") || !strcmp(key, "description"))) {
            char *to = !strcmp(key, "name") ? s->name : !strcmp(key, "author") ? s->author
                       : !strcmp(key, "version") ? s->version : s->description;
            size_t cap = !strcmp(key, "name") ? sizeof s->name : !strcmp(key, "author") ? sizeof s->author
                         : !strcmp(key, "version") ? sizeof s->version : sizeof s->description;
            if (!js_string(&j, to, cap)) return 0;
        } else if (!strcmp(key, "sizes") && j.p < j.end && *j.p == '{') {
            j.p++;
            for (;;) {
                SizeNote note, *grown;
                char num[24];
                size_t k = 0;
                js_space(&j);
                if (j.p < j.end && *j.p == '}') {
                    j.p++;
                    break;
                }
                if (!js_string(&j, note.path, sizeof note.path)) return 0;
                js_space(&j);
                if (j.p >= j.end || *j.p != ':') return 0;
                j.p++;
                js_space(&j);
                while (j.p < j.end && *j.p >= '0' && *j.p <= '9' && k + 1 < sizeof num) num[k++] = *j.p++;
                num[k] = 0;
                if (k == 0 || k > 10 || strtoull(num, NULL, 10) > 0xFFFFFFFFull) return 0;
                note.size = (uint32_t)strtoull(num, NULL, 10);
                grown = (SizeNote *)realloc(s->sizes, (s->n_sizes + 1) * sizeof *grown);
                if (grown == NULL) return 0;
                s->sizes = grown;
                s->sizes[s->n_sizes++] = note;
                js_space(&j);
                if (j.p < j.end && *j.p == ',') j.p++;
            }
        } else {
            js_skip(&j, 0);
            if (j.bad) return 0;
        }
        js_space(&j);
        if (j.p < j.end && *j.p == ',') j.p++;
    }
}

/* ---------------------------------------------------------------- the skins on disk */

static char g_mod[MAX_PATH_U8];   /* this mod's folder */
static char g_table_name[200];    /* the game's table file name (below System/Resource) */
static char g_game_romfs[MAX_PATH_U8];
static Skin g_skins[MAX_SKINS];
/* what the Mods tab's settings say */
static char g_choice[64] = "none";          /* "skin": our own Link's */
static int g_by_colour = 1;                  /* "others": a tunic colour stands for a skin on the other players (off: colours stay colours) */
static int g_table_on = 1;                   /* "table": build a size table of our own */
#define N_COLOURS 10
static const char *const kColours[N_COLOURS] = {"Black", "Blue", "Cyan", "Green", "Orange", "Pink", "Purple", "Red", "White", "Yellow"};
/* The order colours are given to skins without one of their own. Green is never given (a player with no skin), and
   the colours co-op gives by player number (blue, red, yellow) come last. */
static const int kGiveOrder[] = {4, 5, 2, 8, 6, 0, 9, 7, 1}; /* Orange Pink Cyan White Purple Black Yellow Red Blue */
static char g_colour_skin[N_COLOURS][64];    /* "players_<colour>": the skin the puppets of that tunic colour wear; "own": none */
static char g_pack_romfs[MAX_PATH_U8];       /* the co-op puppet pack's romfs folder ("": none found) */
#define PLAYER_MODEL "Model/Player_Adult.Player_Adult.bfres.zs"
static int g_n_skins;
static volatile long g_busy;

static int id_ok(const char *id) {
    size_t n = strlen(id);
    if (n == 0 || n > 48 || !strcmp(id, "none")) return 0;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' ||
              *p == '_' || *p == '-'))
            return 0;
    return 1;
}

static void printable(char *s) { /* what goes into mod.json and the menu: no control characters */
    for (; *s; ++s)
        if ((unsigned char)*s < 0x20) *s = ' ';
}

static void find_skin(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime) {
    char path[MAX_PATH_U8], romfs[MAX_PATH_U8];
    Entries files = {0};
    Skin *s;
    size_t n = 0;
    unsigned char *text;
    (void)user;
    (void)size;
    (void)mtime;
    if (!is_dir || name[0] == '.' || name[0] == '_') return; /* a folder starting with . or _ is switched off */
    if (!id_ok(name)) {
        say("skins/%s is left out: a skin's folder name is 1 to 48 letters, digits, '.', '_' or '-'", name);
        return;
    }
    if (g_n_skins == MAX_SKINS) {
        say("skins/%s is left out: at most %d skins", name, MAX_SKINS);
        return;
    }
    if (snprintf(romfs, sizeof romfs, "%s/skins/%s/romfs", g_mod, name) >= (int)sizeof romfs) return;
    walk(&files, romfs, "", 0);
    n = files.n;
    entries_free(&files);
    if (n == 0) {
        say("skins/%s is left out: no files in its romfs folder", name);
        return;
    }
    s = &g_skins[g_n_skins];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", name);
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(path, sizeof path, "%s/skins/%s/skin.json", g_mod, name);
    text = read_all(path, &n, 1u << 20);
    if (text != NULL) {
        if (!skin_json((const char *)text, n, s)) {
            say("skins/%s: its skin.json could not be read as JSON: the folder's name is shown", name);
            free(s->sizes);
            s->sizes = NULL;
            s->n_sizes = 0;
            snprintf(s->name, sizeof s->name, "%s", name);
        }
        free(text);
    }
    if (s->name[0] == 0) snprintf(s->name, sizeof s->name, "%s", name);
    printable(s->name);
    printable(s->author);
    printable(s->version);
    printable(s->description);
    g_n_skins++;
}

static int by_id(const void *a, const void *b) { return strcmp(((const Skin *)a)->id, ((const Skin *)b)->id); }

/* A dev skin (skin.json "dev": 16 hex digits) is in the picker only on a PC whose link-skins/dev.key holds the key
   those digits were made from (FNV-1a, 64 bits, of the key's first line). Everyone still has the skin's files, so
   everyone sees it on the player who wears it. A courtesy lock, not a secret: the files are all on the disk. */
static void check_dev_keys(void) {
    char path[MAX_PATH_U8], hex[20] = "";
    size_t n = 0;
    unsigned char *key;
    snprintf(path, sizeof path, "%s/dev.key", g_mod);
    key = read_all(path, &n, 4096);
    if (key != NULL) {
        uint64_t h = 1469598103934665603ull;
        size_t len = 0;
        while (len < n && key[len] != '\r' && key[len] != '\n') len++;
        while (len > 0 && (key[len - 1] == ' ' || key[len - 1] == '\t')) len--;
        for (size_t i = 0; i < len; ++i) h = (h ^ key[i]) * 1099511628211ull;
        if (len > 0) snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
        free(key);
    }
    for (int i = 0; i < g_n_skins; ++i) {
        const char *a = g_skins[i].dev, *b = hex;
        int same = *a != 0;
        for (; same && (*a || *b); ++a, ++b)
            if ((*a | 32) != (*b | 32)) same = 0;
        g_skins[i].locked = g_skins[i].dev[0] != 0 && !same;
    }
}

static int same_word(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if ((*a | 32) != (*b | 32)) return 0;
    return *a == *b;
}

/* Each skin gets one co-op tunic colour: the one its skin.json names when it is free, else the next of kGiveOrder.
   Every player with the same skins installed gets the same list. */
static void give_colours(void) {
    for (int c = 0; c < N_COLOURS; ++c) strcpy(g_colour_skin[c], "own");
    for (int i = 0; i < g_n_skins; ++i) g_skins[i].colour_at = -1;
    if (!g_by_colour) return;
    for (int i = 0; i < g_n_skins; ++i) {
        for (int c = 0; c < N_COLOURS; ++c)
            if (c != 3 && same_word(g_skins[i].colour, kColours[c]) && !strcmp(g_colour_skin[c], "own")) {
                g_skins[i].colour_at = c;
                strcpy(g_colour_skin[c], g_skins[i].id);
            }
    }
    for (int i = 0; i < g_n_skins; ++i) {
        if (g_skins[i].colour_at >= 0) continue;
        for (size_t k = 0; k < sizeof kGiveOrder / sizeof kGiveOrder[0]; ++k)
            if (!strcmp(g_colour_skin[kGiveOrder[k]], "own")) {
                g_skins[i].colour_at = kGiveOrder[k];
                strcpy(g_colour_skin[kGiveOrder[k]], g_skins[i].id);
                break;
            }
    }
}

static void scan_skins(void) {
    char dir[MAX_PATH_U8];
    for (int i = 0; i < g_n_skins; ++i) free(g_skins[i].sizes);
    g_n_skins = 0;
    snprintf(dir, sizeof dir, "%s/skins", g_mod);
    make_dir(dir);
    list_dir(dir, find_skin, NULL);
    qsort(g_skins, (size_t)g_n_skins, sizeof g_skins[0], by_id);
    check_dev_keys();
    give_colours();
}

static const Skin *skin_by_id(const char *id) {
    for (int i = 0; i < g_n_skins; ++i)
        if (!strcmp(g_skins[i].id, id)) return &g_skins[i];
    return NULL;
}

/* ---------------------------------------------------------------- mod.json, with one choice per skin */

typedef struct {
    char *p;
    size_t n, cap;
} Text;

static void put(Text *t, const char *s) {
    size_t len = strlen(s);
    if (t->n + len + 1 > t->cap) {
        size_t cap = (t->cap + len + 1) * 2;
        char *p = (char *)realloc(t->p, cap);
        if (p == NULL) return;
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, len + 1);
    t->n += len;
}

static void put_quoted(Text *t, const char *s) {
    char one[8];
    put(t, "\"");
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') snprintf(one, sizeof one, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) snprintf(one, sizeof one, " ");
        else snprintf(one, sizeof one, "%c", *s);
        put(t, one);
    }
    put(t, "\"");
}

static void write_mod_json(void) {
    Text t = {0};
    char path[MAX_PATH_U8];
    unsigned char *old;
    size_t old_n = 0;
    put(&t, "{\n  \"id\": \"link-skins\",\n  \"name\": \"Link Skins\",\n  \"version\": \"" VERSION "\",\n"
            "  \"author\": \"Jack\",\n"
            "  \"description\": \"Pick a skin for Link. Skins are folders in mods\\\\link-skins\\\\skins.\",\n"
            "  \"coop\": \"own\",\n  \"api\": 1,\n  \"code\": {\"windows\": \"" DLL_NAME "\"},\n"
            "  \"settings\": [\n    {\"key\": \"skin\", \"name\": \"Link's skin\", \"type\": \"choice\",\n"
            "     \"help\": \"No restart: save and quit to the title screen, then load your save (or change age). Tunics change when put on again.\",\n"
            "     \"default\": \"none\",\n     \"choices\": [\n       {\"value\": \"none\", \"name\": \"The game's own Link\"}");
    for (int i = 0; i < g_n_skins; ++i) {
        char shown[200];
        if (g_skins[i].locked) continue; /* a dev skin without its key: worn by its makers only */
        if (g_pack_romfs[0] && g_skins[i].colour_at >= 0) snprintf(shown, sizeof shown, "%s (co-op: %s tunic)", g_skins[i].name, kColours[g_skins[i].colour_at]);
        else snprintf(shown, sizeof shown, "%s", g_skins[i].name);
        if (strlen(shown) > 64) { /* the loader takes a choice's name of at most 64 bytes: the colour note goes first */
            size_t cut = 64;
            snprintf(shown, sizeof shown, "%s", g_skins[i].name);
            if (strlen(shown) > 64) {
                while (cut > 0 && ((unsigned char)shown[cut] & 0xC0) == 0x80) --cut;
                shown[cut] = 0;
            }
        }
        put(&t, ",\n       {\"value\": ");
        put_quoted(&t, g_skins[i].id);
        put(&t, ", \"name\": ");
        put_quoted(&t, shown);
        put(&t, "}");
    }
    put(&t, "\n     ]},\n    {\"key\": \"table\", \"name\": \"Size table\", \"type\": \"choice\", \"restart\": true,\n"
            "     \"help\": \"Co-op hides the other players while a mod brings its own size table. 'None' is a test: skins without one.\",\n"
            "     \"default\": \"own\",\n     \"choices\": [\n       {\"value\": \"own\", \"name\": \"Built by Link Skins\"},\n"
            "       {\"value\": \"none\", \"name\": \"None (co-op test)\"}\n     ]}");
    if (g_pack_romfs[0])
        put(&t, ",\n    {\"key\": \"others\", \"name\": \"Skins on other players by tunic colour\", \"type\": \"toggle\", \"default\": true,\n"
                "     \"help\": \"On: a player whose tunic colour is the one a skin names shows in that skin on your screen. Off: tunic colours are just colours.\"}");
    put(&t, "\n  ]\n}\n");
    if (t.p == NULL) return;
    snprintf(path, sizeof path, "%s/mod.json", g_mod);
    old = read_all(path, &old_n, 1u << 20);
    if (old == NULL || old_n != t.n || memcmp(old, t.p, t.n) != 0) {
        if (write_all(path, t.p, t.n)) say("the skin list changed (%d skin%s): the Mods tab shows it after a restart", g_n_skins, g_n_skins == 1 ? "" : "s");
        else say("mod.json could not be written: the Mods tab keeps the old skin list");
    }
    free(old);
    free(t.p);
}

/* ---------------------------------------------------------------- putting a skin into romfs/ */

/* The bytes a .zs file holds when unpacked (0 when it does not say); a file that is not .zs: its own size. */
static uint64_t content_size(const char *path, uint64_t file_size) {
    unsigned char head[18];
    FILE *f;
    size_t n;
    unsigned long long full;
    if (!ends_with(path, ".zs")) return file_size;
    f = open_file(path, "rb");
    if (f == NULL) return 0;
    n = fread(head, 1, sizeof head, f);
    fclose(f);
    full = ZSTD_getFrameContentSize(head, n);
    return (full == ZSTD_CONTENTSIZE_ERROR || full == ZSTD_CONTENTSIZE_UNKNOWN) ? 0 : full;
}

static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (uint32_t)((v + 7) & ~7ull); }

/* The table entry a skin's file needs. `key`: its romfs path without ".zs"; `mine`: the bytes it unpacks to;
   `base`: the game's entry (0: the game has no such file); `orig`: the bytes the game's own file unpacks to.
   A texture's entry is its size + 280 (true of every texture of the game). A file no larger than the game's keeps
   the game's entry. Otherwise the entry grows with the file, with room to spare: a guess, which a skin's
   "sizes" can overrule. */
static uint32_t needed_size(const char *key, uint64_t mine, uint32_t base, uint64_t orig) {
    if (ends_with(key, ".bntx")) return clamp32(mine + 280);
    if (base != 0 && orig != 0 && mine <= orig) return base;
    if (base != 0 && orig != 0) return clamp32((uint64_t)base + (mine - orig) + (mine - orig) / 4 + 0x2000);
    return clamp32(mine + mine / 4 + 0x20000);
}

typedef struct {
    Table *into;
    const char *mods;
    int depth;
    char rel[MAX_PATH_U8];
    int found;
} TableHunt;

static void hunt_tables(TableHunt *h);

static void hunt_one(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime) {
    TableHunt *h = (TableHunt *)user;
    char keep[MAX_PATH_U8], full[MAX_PATH_U8];
    (void)size;
    (void)mtime;
    if (!is_dir || name[0] == '.') return;
    if (h->depth == 0 && !strcmp(name, "link-skins")) return; /* not our own */
    strcpy(keep, h->rel);
    if (snprintf(h->rel, sizeof h->rel, "%s%s%s", keep, keep[0] ? "/" : "", name) < (int)sizeof h->rel) {
        if (!strcmp(name, "romfs")) {
            Entries files = {0};
            char dir[MAX_PATH_U8];
            if (snprintf(dir, sizeof dir, "%s/%s/" TABLE_DIR, h->mods, h->rel) < (int)sizeof dir) walk(&files, dir, "", 12);
            for (size_t i = 0; i < files.n; ++i) {
                Table other;
                if (!ends_with(files.v[i].rel, TABLE_TAIL) || !join(full, sizeof full, dir, files.v[i].rel)) continue;
                if (table_read(full, &other)) {
                    table_merge(h->into, &other);
                    table_free(&other);
                    h->found++;
                    say("size table: the larger entries of mods/%s are kept", h->rel);
                }
            }
            entries_free(&files);
        } else if (h->depth < 5) {
            h->depth++;
            hunt_tables(h);
            h->depth--;
        }
    }
    strcpy(h->rel, keep);
}

static void hunt_tables(TableHunt *h) {
    char dir[MAX_PATH_U8];
    if (snprintf(dir, sizeof dir, "%s%s%s", h->mods, h->rel[0] ? "/" : "", h->rel) < (int)sizeof dir)
        list_dir(dir, hunt_one, h);
}

/* romfs.txt: what romfs/ holds. Line 1 "link-skins <version>", line 2 the chosen skin's id, line 3 the table's
   check number, then one line per file: "<size> <mtime> <from> <path>", where <from> is a skin's id, or "game" for
   the game's own copy of a file. */

typedef struct {
    char *rel;          /* romfs path */
    char *src;          /* the file it is copied from */
    char from[64];      /* a skin's id, "game" or "pack" */
    uint64_t size, mtime;
} Want;
typedef struct {
    Want *v;
    size_t n, cap;
} Wants;

static int by_want(const void *a, const void *b) { return strcmp(((const Want *)a)->rel, ((const Want *)b)->rel); }

static Want *want_find(Wants *w, const char *rel) {
    for (size_t i = 0; i < w->n; ++i)
        if (!strcmp(w->v[i].rel, rel)) return &w->v[i];
    return NULL;
}

static Want *want_add(Wants *w, const char *rel) {
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 64;
        Want *v = (Want *)realloc(w->v, cap * sizeof *v);
        if (v == NULL) return NULL;
        w->v = v;
        w->cap = cap;
    }
    memset(&w->v[w->n], 0, sizeof w->v[0]);
    w->v[w->n].rel = strdup(rel);
    return w->v[w->n].rel != NULL ? &w->v[w->n++] : NULL;
}

static void wants_free(Wants *w) {
    for (size_t i = 0; i < w->n; ++i) {
        free(w->v[i].rel);
        free(w->v[i].src);
    }
    free(w->v);
    memset(w, 0, sizeof *w);
}

typedef struct {
    uint64_t size, mtime;
    int found;
    const char *name;
} OneFile;

static void one_file(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime) {
    OneFile *o = (OneFile *)user;
    if (!is_dir && !strcmp(name, o->name)) {
        o->size = size;
        o->mtime = mtime;
        o->found = 1;
    }
}

/* The size and time of one file (0: there is none). */
static int file_info(const char *path, uint64_t *size, uint64_t *mtime) {
    char dir[MAX_PATH_U8];
    const char *slash = strrchr(path, '/');
    OneFile o = {0, 0, 0, NULL};
    if (slash == NULL || strlen(path) >= sizeof dir) return 0;
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = 0;
    o.name = slash + 1;
    list_dir(dir, one_file, &o);
    *size = o.size;
    *mtime = o.mtime;
    return o.found;
}

static uint32_t table_check(const Table *t) { /* a check number over the rows */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < t->n; ++i) {
        h = (h ^ t->rows[i].crc) * 16777619u;
        h = (h ^ t->rows[i].size) * 16777619u;
    }
    for (size_t i = 0; i < (size_t)t->names * (t->name_len + 4); ++i) h = (h ^ t->name_rows[i]) * 16777619u;
    return h;
}

/* Brings romfs/ in line with the skins on disk and the chosen skin `id` ("none": the game's own Link).
   Every skin's files are in romfs/ all the time, so the game lists them at its start:
     - a file only one skin has, and the game has not: that skin's (its own textures);
     - a file the game has too, or several skins have: the chosen skin's, else the game's own copy.
   The size table covers every skin at once, so choosing another skin changes file contents only.
   *listed: the set of files or the table changed (the game sees that at its next start only).
   *swapped: the contents of files the game already lists changed. */
static int sync_romfs(int *listed, int *swapped) {
    const char *id = g_choice;
    char path[MAX_PATH_U8], from[MAX_PATH_U8], romfs[MAX_PATH_U8], mods[MAX_PATH_U8];
    const Skin *chosen = strcmp(id, "none") ? skin_by_id(id) : NULL;
    Wants wants = {0}, had = {0};
    Entries now = {0};
    Table table;
    Text out = {0};
    unsigned char *old;
    size_t old_n = 0;
    uint32_t had_check = 0, check = 0;
    char had_id[64] = "";
    int ok = 0, have_table = 0;
    *listed = 0;
    *swapped = 0;
    if (strcmp(id, "none") && chosen == NULL) {
        say("no skin \"%s\" in the skins folder: the game's own Link", id);
        id = "none";
    } else if (chosen != NULL && chosen->locked) {
        say("skin \"%s\" is a dev skin and this PC has no key for it: the game's own Link", id);
        chosen = NULL;
        id = "none";
    }
    snprintf(romfs, sizeof romfs, "%s/romfs", g_mod);
    make_dir(romfs);
    if (g_table_name[0] == 0) {
        say("the game's resource size table was not found beside the game: no skin is put on");
        return 0;
    }

    /* what romfs.txt says is there */
    snprintf(path, sizeof path, "%s/romfs.txt", g_mod);
    old = read_all(path, &old_n, 8u << 20);
    if (old != NULL) {
        char *line = (char *)old, *end;
        int row = 0;
        for (; line != NULL && *line; line = end ? end + 1 : NULL, ++row) {
            end = strchr(line, '\n');
            if (end) *end = 0;
            if (row == 0) {
                if (strcmp(line, "link-skins " VERSION)) break; /* another version's: everything is put again */
            } else if (row == 1) snprintf(had_id, sizeof had_id, "%s", line);
            else if (row == 2) had_check = (uint32_t)strtoul(line, NULL, 16);
            else {
                unsigned long long size, mtime;
                char src[64];
                int at = 0;
                Want *w;
                if (sscanf(line, "%llu %llu %63s %n", &size, &mtime, src, &at) < 3 || at == 0) continue;
                w = want_add(&had, line + at);
                if (w == NULL) goto done;
                w->size = size;
                w->mtime = mtime;
                snprintf(w->from, sizeof w->from, "%s", src);
            }
        }
        free(old);
    }

    /* the table over every skin, and what each file of romfs/ should be */
    snprintf(path, sizeof path, "%s/" TABLE_DIR "/%s", g_game_romfs, g_table_name);
    if (!table_read(path, &table)) {
        say("the game's resource size table could not be read: no skin is put on");
        goto done;
    }
    have_table = 1;
    snprintf(mods, sizeof mods, "%s/..", g_mod);
    {
        TableHunt hunt = {&table, mods, 0, "", 0};
        hunt_tables(&hunt);
    }
    for (int s = 0; s < g_n_skins; ++s) {
        Entries files = {0};
        Table own;
        int has_own = 0;
        char src[MAX_PATH_U8];
        snprintf(src, sizeof src, "%s/skins/%s/romfs", g_mod, g_skins[s].id);
        walk(&files, src, "", 0);
        qsort(files.v, files.n, sizeof files.v[0], by_rel);
        for (size_t i = 0; i < files.n && !has_own; ++i)
            if (is_table(files.v[i].rel) && join(path, sizeof path, src, files.v[i].rel)) has_own = table_read(path, &own);
        for (size_t i = 0; i < files.n; ++i) {
            char key[MAX_PATH_U8];
            uint64_t mine, orig = 0, gs = 0, gm = 0;
            uint32_t base, want, noted;
            int in_game;
            Want *w;
            if (is_table(files.v[i].rel)) continue;
            if (!join(from, sizeof from, src, files.v[i].rel) || !join(path, sizeof path, g_game_romfs, files.v[i].rel)) continue;
            mine = content_size(from, files.v[i].size);
            if (mine == 0) {
                say("skin %s: %s does not say its unpacked size: left out", g_skins[s].id, files.v[i].rel);
                continue;
            }
            snprintf(key, sizeof key, "%s", files.v[i].rel);
            if (ends_with(key, ".zs")) key[strlen(key) - 3] = 0;
            in_game = file_info(path, &gs, &gm);
            if (in_game && ends_with(path, ".zs")) orig = content_size(path, gs);
            base = table_get(&table, key);
            want = needed_size(key, mine, base, orig);
            noted = has_own ? table_get(&own, key) : 0;
            if (noted > want) want = noted;
            for (size_t k = 0; k < g_skins[s].n_sizes; ++k)
                if (!strcmp(g_skins[s].sizes[k].path, key) && g_skins[s].sizes[k].size > want) want = g_skins[s].sizes[k].size;
            if (!table_raise(&table, key, want)) goto done;

            w = want_find(&wants, files.v[i].rel);
            if (w == NULL) {
                w = want_add(&wants, files.v[i].rel);
                if (w == NULL) goto done;
                if (in_game) { /* until the chosen skin claims it below: the game's own copy */
                    strcpy(w->from, "game");
                    w->src = strdup(path);
                    w->size = gs;
                    w->mtime = gm;
                }
            }
            if (&g_skins[s] == chosen || (w->from[0] == 0)) { /* the chosen skin's; or the first skin that has it */
                snprintf(w->from, sizeof w->from, "%s", g_skins[s].id);
                free(w->src);
                w->src = strdup(from);
                w->size = files.v[i].size;
                w->mtime = files.v[i].mtime;
            }
            if (!g_table_on && in_game && orig != 0 && mine > orig)
                say("skin %s: %s is larger than the game's: without a size table of ours the game may stop when it loads it",
                    g_skins[s].id, files.v[i].rel);
        }
        if (has_own) table_free(&own);
        entries_free(&files);
    }
    /* the co-op puppets' models, one per age and tunic colour: the pack's own copy, or a skin's model of that age
       for the colour that stands for the skin. (Ours are read before the pack's: a listed mod is above the pack.)
       Its size-table entry must hold the largest skin: by now the entry of Link's own model of that age does. */
    if (g_pack_romfs[0])
        for (int age = 0; age < 2; ++age) {
            const char *who = age == 0 ? "Adult" : "Child";
            char body_key[80], body_rel[96];
            uint32_t body;
            snprintf(body_key, sizeof body_key, "Model/Player_%s.Player_%s.bfres", who, who);
            snprintf(body_rel, sizeof body_rel, "%s.zs", body_key);
            body = table_get(&table, body_key);
            for (int c = 0; c < N_COLOURS; ++c) {
                char rel[200], key[200];
                uint64_t ps = 0, pm = 0, ss = 0, sm = 0;
                const Skin *worn = strcmp(g_colour_skin[c], "own") ? skin_by_id(g_colour_skin[c]) : NULL;
                Want *w;
                snprintf(key, sizeof key, "Model/CoopTunic_%s_%s.Player_%s.bfres", who, kColours[c], who);
                snprintf(rel, sizeof rel, "%s.zs", key);
                if (!join(path, sizeof path, g_pack_romfs, rel) || !file_info(path, &ps, &pm)) continue; /* a pack without it */
                /* (every colour's entry is raised, worn or not: switching "others" then changes contents only) */
                if (body != 0 && table_get(&table, key) < body && !table_raise(&table, key, body)) goto done;
                w = want_add(&wants, rel);
                if (w == NULL) goto done;
                if (worn != NULL && snprintf(from, sizeof from, "%s/skins/%s/romfs/%s", g_mod, worn->id, body_rel) < (int)sizeof from &&
                    file_info(from, &ss, &sm)) {
                    snprintf(w->from, sizeof w->from, "%s", worn->id);
                    w->src = strdup(from);
                    w->size = ss;
                    w->mtime = sm;
                    continue;
                }
                strcpy(w->from, "pack"); /* no skin for this colour, or the skin leaves this age alone */
                w->src = strdup(path);
                w->size = ps;
                w->mtime = pm;
            }
        }
    qsort(wants.v, wants.n, sizeof wants.v[0], by_want);
    check = (g_n_skins > 0 && g_table_on) ? table_check(&table) : 0;

    /* files that should go */
    walk(&now, romfs, "", 0);
    for (size_t i = 0; i < now.n; ++i) {
        if (!strcmp(now.v[i].rel, "link-skins.txt") || want_find(&wants, now.v[i].rel) != NULL) continue;
        if (is_table(now.v[i].rel) && g_n_skins > 0 && g_table_on) continue;
        if (join(path, sizeof path, romfs, now.v[i].rel) && remove_file(path)) *listed = 1;
        else say("could not remove romfs/%s", now.v[i].rel);
    }
    /* files that should be there */
    ok = 1;
    for (size_t i = 0; i < wants.n; ++i) {
        Want *w = &wants.v[i], *h = want_find(&had, w->rel);
        uint64_t ds = 0, dm = 0;
        int there;
        if (!join(path, sizeof path, romfs, w->rel)) continue;
        there = file_info(path, &ds, &dm);
        if (there && h != NULL && !strcmp(h->from, w->from) && h->size == w->size && h->mtime == w->mtime && ds == w->size)
            continue; /* as it should be */
        if (w->src == NULL || !make_parents(romfs, path) || !copy_file(w->src, path)) {
            say("%s could not be put into romfs (from %s): is another program using it?", w->rel, w->from);
            ok = 0;
            w->mtime = 0; /* tried again at the next start */
            continue;
        }
        if (there) *swapped = 1;
        else *listed = 1;
    }
    /* the table */
    snprintf(path, sizeof path, "%s/" TABLE_DIR "/%s", romfs, g_table_name);
    if (g_n_skins > 0 && g_table_on) {
        uint64_t ts = 0, tm = 0;
        if (check != had_check || !file_info(path, &ts, &tm)) {
            if (make_parents(romfs, path) && table_write(&table, path)) *listed = 1;
            else {
                say("the size table could not be written");
                ok = 0;
                check = 0;
            }
        }
    }
    {
        static const char note[] = "Link Skins keeps every skin's files here. Do not edit: change the skins in skins/ instead.\n";
        uint64_t ns = 0, nm = 0;
        snprintf(path, sizeof path, "%s/link-skins.txt", romfs);
        if (!file_info(path, &ns, &nm)) write_all(path, note, sizeof note - 1);
    }

    put(&out, "link-skins " VERSION "\n");
    put(&out, id);
    snprintf(path, sizeof path, "\n%08x\n", check);
    put(&out, path);
    for (size_t i = 0; i < wants.n; ++i) {
        char line[MAX_PATH_U8 + 160];
        snprintf(line, sizeof line, "%llu %llu %s %s\n", (unsigned long long)wants.v[i].size,
                 (unsigned long long)wants.v[i].mtime, wants.v[i].from, wants.v[i].rel);
        put(&out, line);
    }
    snprintf(path, sizeof path, "%s/romfs.txt", g_mod);
    if (out.p == NULL || !write_all(path, out.p, out.n)) say("romfs.txt could not be written: the files are checked again at the next start");
    (void)had_id;
done:
    if (have_table) table_free(&table);
    wants_free(&wants);
    wants_free(&had);
    entries_free(&now);
    free(out.p);
    return ok;
}

/* ---------------------------------------------------------------- finding the game's files */

typedef struct {
    char name[200];
} TableName;

static void first_table(void *user, const char *name, int is_dir, uint64_t size, uint64_t mtime) {
    TableName *t = (TableName *)user;
    (void)size;
    (void)mtime;
    if (!is_dir && t->name[0] == 0 && strncmp(name, "ResourceSizeTable.", 18) == 0 && ends_with(name, TABLE_TAIL) &&
        strlen(name) < sizeof t->name)
        strcpy(t->name, name);
}

/* The game's romfs is a folder named romfs above the mods folder (bin/mods -> ../../romfs, or mods beside it). */
static void find_game(void) {
    char up[MAX_PATH_U8], dir[MAX_PATH_U8];
    g_table_name[0] = 0;
    g_game_romfs[0] = 0;
    snprintf(up, sizeof up, "%s/..", g_mod);
    for (int level = 0; level < 4; ++level) {
        TableName t = {{0}};
        if (snprintf(dir, sizeof dir, "%s/../romfs/" TABLE_DIR, up) >= (int)sizeof dir) return;
        list_dir(dir, first_table, &t);
        if (t.name[0]) {
            snprintf(g_game_romfs, sizeof g_game_romfs, "%s/../romfs", up);
            strcpy(g_table_name, t.name);
            return;
        }
        if (strlen(up) + 4 >= sizeof up) return;
        strcat(up, "/..");
    }
}

/* ---------------------------------------------------------------- the mod */

static int lock(void) {
#ifdef _WIN32
    return InterlockedCompareExchange(&g_busy, 1, 0) == 0;
#else
    return __sync_bool_compare_and_swap(&g_busy, 0, 1);
#endif
}
static void unlock(void) { g_busy = 0; }

static void take_setting(const char *key, const char *value) {
    if (!strcmp(key, "skin")) snprintf(g_choice, sizeof g_choice, "%s", value);
    else if (!strcmp(key, "table")) g_table_on = strcmp(value, "none") != 0;
    else if (!strcmp(key, "others")) g_by_colour = !strcmp(value, "1");
}

static void choose(int at_start) {
    int listed = 0, swapped = 0, ok;
    char why[200];
    const Skin *skin;
    if (!lock()) return;
    ok = sync_romfs(&listed, &swapped);
    skin = skin_by_id(g_choice);
    if (skin != NULL && skin->locked) skin = NULL;
    if (!ok) {
        say("the skins could not be put in place whole: see the lines above");
    } else if (listed) {
        snprintf(why, sizeof why, "the skins' files changed: restart the game once");
        say("%s", why);
        if (api != NULL) api->restart_needed(api, why);
    } else if (swapped && !at_start) {
        /* seen in the game: Link's body model is read again when the save is loaded again or Link changes age, the
           tunic and gauntlet models each time they are put on. No restart. */
        say("%s: Link's body changes when you load your save again (save and quit to the title screen) or change age; the tunics when put on again",
            skin != NULL ? skin->name : "the game's own Link");
    } else {
        say("skin: %s%s", skin != NULL ? skin->name : "the game's own Link", g_table_on ? "" : " (no size table of ours)");
    }
    unlock();
}

static void on_setting(void *user, const char *key, const char *value) {
    (void)user;
    take_setting(key, value);
    if (!strcmp(key, "others")) { /* the colours the skins stand for, and the names the picker shows */
        give_colours();
        write_mod_json();
    }
    choose(0);
}

static void find_pack(void) {
    uint64_t s = 0, m = 0;
    g_pack_romfs[0] = 0;
    {
        char probe[MAX_PATH_U8];
        snprintf(probe, sizeof probe, "%s/../contents/0400486024DEC000/coop-puppet/romfs/Model/CoopTunic_Adult_Blue.Player_Adult.bfres.zs", g_mod);
        if (file_info(probe, &s, &m)) snprintf(g_pack_romfs, sizeof g_pack_romfs, "%s/../contents/0400486024DEC000/coop-puppet/romfs", g_mod);
    }
}

COOP_MOD_EXPORT int coop_mod_start(const CoopModApi *a) {
    char value[64];
    if (a == NULL || a->version < 1 || a->size < sizeof(CoopModApi) || a->mod_folder == NULL) return 0;
    api = a;
    if (strlen(a->mod_folder) >= sizeof g_mod - 160) {
        say("this mod's folder path is too long");
        return 0;
    }
    strcpy(g_mod, a->mod_folder);
    for (char *p = g_mod; *p; ++p)
        if (*p == '\\') *p = '/';
    while (g_mod[0] && g_mod[strlen(g_mod) - 1] == '/') g_mod[strlen(g_mod) - 1] = 0;
    find_game();
    find_pack();
    if (g_table_name[0] == 0) say("the game's romfs folder was not found above the mods folder: skins cannot be put on");
    if (api->setting(api, "skin", value, sizeof value) >= 0) take_setting("skin", value);
    if (api->setting(api, "table", value, sizeof value) >= 0) take_setting("table", value);
    if (api->setting(api, "others", value, sizeof value) >= 0) take_setting("others", value);
    scan_skins();
    write_mod_json();
    if (g_pack_romfs[0] && g_by_colour)
        for (int i = 0; i < g_n_skins; ++i) {
            if (g_skins[i].colour_at >= 0) say("co-op: players whose tunic colour is %s show as %s", kColours[g_skins[i].colour_at], g_skins[i].name);
            else say("co-op: %s has no tunic colour left (9 skins at most): the others cannot see it", g_skins[i].name);
        }
    say("link-skins " VERSION ": %d skin%s found%s", g_n_skins, g_n_skins == 1 ? "" : "s", g_pack_romfs[0] ? ", the co-op puppet pack too" : "");
    choose(1);
    api->on_setting(api, on_setting, NULL);
    return 1;
}

COOP_MOD_EXPORT void coop_mod_stop(void) {}
