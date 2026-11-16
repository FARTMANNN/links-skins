/* A stand-in for the co-op loader, to run the mod's logic on Linux. argv: mod folder, then skin ids chosen in turn. */
#include <stdio.h>
#include <string.h>
#include "coop_mod_api.h"
int coop_mod_start(const CoopModApi *a);
static char keys[16][40], vals[16][64]; static int nset;
static void set(const char *kv) { const char *e = strchr(kv, '='); int i; char k[40]; snprintf(k, sizeof k, "%.*s", (int)(e - kv), kv); for (i = 0; i < nset; ++i) if (!strcmp(keys[i], k)) break; if (i == nset) nset++; strcpy(keys[i], k); snprintf(vals[i], 64, "%s", e + 1); }
static coop_mod_setting_fn set_fn;
static void t_log(const CoopModApi *s, const char *l) { (void)s; printf("log: %s\n", l); }
static uint64_t t_base(void) { return 0; }
static int t_rw(uint64_t a, void *o, size_t n) { (void)a; (void)o; (void)n; return 0; }
static int t_w(uint64_t a, const void *o, size_t n) { (void)a; (void)o; (void)n; return 0; }
static int t_setting(const CoopModApi *s, const char *k, char *out, size_t cap) { (void)s; for (int i = 0; i < nset; ++i) if (!strcmp(keys[i], k)) { snprintf(out, cap, "%s", vals[i]); return (int)strlen(vals[i]); } if (!strcmp(k, "skin")) { snprintf(out, cap, "none"); return 4; } return -1; }
static int t_frame(const CoopModApi *s, coop_mod_frame_fn f, void *u) { (void)s; (void)f; (void)u; return 1; }
static int t_overlay(const CoopModApi *s, coop_mod_overlay_fn f, void *u) { (void)s; (void)f; (void)u; return 1; }
static int t_key(const CoopModApi *s, coop_mod_key_fn f, void *u) { (void)s; (void)f; (void)u; return 1; }
static int t_on_setting(const CoopModApi *s, coop_mod_setting_fn f, void *u) { (void)s; (void)u; set_fn = f; return 1; }
static int t_hook(const CoopModApi *s, uint32_t h, coop_mod_hook_fn f, void *u) { (void)s; (void)h; (void)f; (void)u; return 0; }
static int t_has(uint32_t h) { (void)h; return 0; }
static void t_restart(const CoopModApi *s, const char *why) { (void)s; printf("restart needed: %s\n", why); }
int main(int argc, char **argv) {
    CoopModApi a = {sizeof(CoopModApi), 1, "link-skins", argv[1], "test", t_log, t_base, t_rw, t_w, t_setting,
                    t_frame, t_overlay, t_key, t_on_setting, t_hook, t_has, t_restart};
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--"); ++i) set(argv[i]);
    printf("start: %d\n", coop_mod_start(&a));
    for (++i; i < argc; ++i) { printf("-- set %s\n", argv[i]); set(argv[i]); { char k[40]; snprintf(k, sizeof k, "%.*s", (int)(strchr(argv[i], '=') - argv[i]), argv[i]); set_fn(0, k, strchr(argv[i], '=') + 1); } }
    return 0;
}
