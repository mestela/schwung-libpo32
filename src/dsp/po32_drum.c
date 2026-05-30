/*
 * po32_drum — Schwung module: PO-32 Tonic drum synthesizer
 *
 * 16 instruments, MIDI notes 36-51 → instruments 1-16.
 * Patches stored as .mtdrum files (built-in kits) or .json (saved kits).
 * Both live in the same unified kit list under the `kit` param.
 */

#include "plugin_api_v1.h"
#include "po32.h"
#include "po32_synth.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── constants ───────────────────────────────────────────────────── */

#define NUM_INSTRUMENTS  16
#define MAX_VOICES       8
#define MAX_KITS         64
#define MAX_KIT_NAME     64
#define MAX_PATH         512
#define MIDI_NOTE_BASE   36
#define VOICE_DURATION_S 0.6f
#define SAMPLE_RATE      44100

#define KIT_TYPE_DIR  0   /* kits/ subdirectory of .mtdrum files */
#define KIT_TYPE_JSON 1   /* presets/ JSON file */

static const host_api_v1_t *g_host = NULL;

/* ── voice ───────────────────────────────────────────────────────── */

typedef struct {
    float  *buf;
    size_t  len;
    size_t  pos;
    int     active;
} voice_t;

/* ── module state ────────────────────────────────────────────────── */

typedef struct {
    char module_dir[MAX_PATH];

    po32_synth_t        synth;
    po32_patch_params_t patches[NUM_INSTRUMENTS];

    voice_t voices[MAX_VOICES];
    float  *voice_bufs;
    size_t  voice_buf_frames;

    /* unified kit list (built-in dirs + saved JSON) */
    char kit_names[MAX_KITS][MAX_KIT_NAME]; /* display name */
    char kit_paths[MAX_KITS][MAX_KIT_NAME]; /* dir name or .json filename */
    int  kit_types[MAX_KITS];               /* KIT_TYPE_DIR or KIT_TYPE_JSON */
    int  kit_count;
    int  kit_index;
    int  kit_dir_count;    /* leading entries that are KIT_TYPE_DIR */
    int  kit_query_idx;    /* for name queries without loading patches */

    float level;
    float decay_scale;

    int selected_inst;

    char error[128];
} po32_drum_t;

/* ── logging ─────────────────────────────────────────────────────── */

static void plog(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

static void plogf(po32_drum_t *m, const char *fmt, ...) {
    (void)m;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    plog(buf);
}

/* ── JSON helpers ────────────────────────────────────────────────── */

static int json_get_str(const char *json, const char *key, char *out, int len) {
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < len - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

static int json_get_float(const char *json, const char *key, float *out) {
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    *out = (float)atof(p);
    return 1;
}

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    buf[fread(buf, 1, sz, f)] = '\0';
    fclose(f);
    return buf;
}

/* ── kit scanning ────────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void scan_unified_kits(po32_drum_t *m) {
    m->kit_count = 0;
    m->kit_dir_count = 0;

    /* 1. Scan kits/ subdirectories */
    char kits_dir[MAX_PATH];
    snprintf(kits_dir, sizeof(kits_dir), "%s/kits", m->module_dir);
    DIR *d = opendir(kits_dir);
    if (d) {
        char *names[MAX_KITS];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < MAX_KITS) {
            if (e->d_name[0] == '.') continue;
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", kits_dir, e->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                names[n++] = strdup(e->d_name);
        }
        closedir(d);
        qsort(names, n, sizeof(char *), cmp_str);
        for (int i = 0; i < n && m->kit_count < MAX_KITS; i++) {
            int k = m->kit_count;
            strncpy(m->kit_paths[k], names[i], MAX_KIT_NAME - 1);
            /* strip leading NN_ prefix for display */
            const char *disp = strchr(names[i], '_');
            strncpy(m->kit_names[k], disp ? disp + 1 : names[i], MAX_KIT_NAME - 1);
            m->kit_types[k] = KIT_TYPE_DIR;
            m->kit_count++;
            free(names[i]);
        }
        m->kit_dir_count = m->kit_count;
    }

    /* 2. Scan presets/ JSON files */
    char presets_dir[MAX_PATH];
    snprintf(presets_dir, sizeof(presets_dir), "%s/presets", m->module_dir);
    DIR *d2 = opendir(presets_dir);
    if (d2) {
        char *names[MAX_KITS];
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d2)) != NULL && n < MAX_KITS) {
            const char *nm = e->d_name;
            size_t len = strlen(nm);
            if (len < 6 || strcmp(nm + len - 5, ".json") != 0) continue;
            names[n++] = strdup(nm);
        }
        closedir(d2);
        qsort(names, n, sizeof(char *), cmp_str);
        for (int i = 0; i < n && m->kit_count < MAX_KITS; i++) {
            int k = m->kit_count;
            strncpy(m->kit_paths[k], names[i], MAX_KIT_NAME - 1);
            /* read "name" from JSON, fall back to filename without .json */
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", presets_dir, names[i]);
            char *json = read_text_file(full);
            if (!json || !json_get_str(json, "name", m->kit_names[k], MAX_KIT_NAME)) {
                strncpy(m->kit_names[k], names[i], MAX_KIT_NAME - 1);
                char *dot = strrchr(m->kit_names[k], '.');
                if (dot) *dot = '\0';
            }
            free(json);
            m->kit_types[k] = KIT_TYPE_JSON;
            m->kit_count++;
            free(names[i]);
        }
    }
}

/* ── patch loading ───────────────────────────────────────────────── */

static void load_instrument(po32_drum_t *m, const char *kit_dir, int inst) {
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "%d", inst);
    size_t prefix_len = strlen(prefix);

    DIR *d = opendir(kit_dir);
    char found[MAX_PATH] = {0};
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
            if (strncmp(name, prefix, prefix_len) != 0) continue;
            char sep = name[prefix_len];
            if (sep != '.' && sep != '_') continue;
            size_t nlen = strlen(name);
            if (nlen < 7 || strcmp(name + nlen - 7, ".mtdrum") != 0) continue;
            snprintf(found, sizeof(found), "%s/%s", kit_dir, name);
            break;
        }
        closedir(d);
    }

    po32_patch_params_t *p = &m->patches[inst - 1];
    po32_patch_params_zero(p);
    if (!found[0]) return;

    int fd = open(found, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) { close(fd); return; }
    char *text = malloc(st.st_size + 1);
    if (!text) { close(fd); return; }
    ssize_t got = read(fd, text, st.st_size);
    close(fd);
    if (got > 0) { text[got] = '\0'; po32_patch_parse_mtdrum_text(text, (size_t)got, p); }
    free(text);
}

static float *patch_field(po32_patch_params_t *p, int idx) {
    return (float *)p + idx;
}

static void load_kit_from_json(po32_drum_t *m, const char *path) {
    char *json = read_text_file(path);
    if (!json) return;
    float fval;
    if (json_get_float(json, "level", &fval)) m->level = fval;
    if (json_get_float(json, "decay", &fval)) m->decay_scale = fval;
    for (int i = 0; i < NUM_INSTRUMENTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "i%d", i);
        char csv[256];
        if (!json_get_str(json, key, csv, sizeof(csv))) continue;
        po32_patch_params_t *p = &m->patches[i];
        float vals[21] = {0};
        char *tok = csv;
        for (int f = 0; f < 21; f++) {
            vals[f] = (float)atof(tok);
            tok = strchr(tok, ',');
            if (!tok) break;
            tok++;
        }
        for (int f = 0; f < 21; f++) *patch_field(p, f) = vals[f];
    }
    free(json);
}

static void load_kit_at(po32_drum_t *m, int idx) {
    if (idx < 0 || idx >= m->kit_count) return;
    if (m->kit_types[idx] == KIT_TYPE_DIR) {
        char kit_dir[MAX_PATH];
        snprintf(kit_dir, sizeof(kit_dir), "%s/kits/%s", m->module_dir, m->kit_paths[idx]);
        for (int i = 1; i <= NUM_INSTRUMENTS; i++)
            load_instrument(m, kit_dir, i);
    } else {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/presets/%s", m->module_dir, m->kit_paths[idx]);
        load_kit_from_json(m, path);
    }
    m->kit_index = idx;
    plogf(m, "po32: loaded kit %s", m->kit_names[idx]);
}

/* ── save kit ────────────────────────────────────────────────────── */

static void save_kit(po32_drum_t *m) {
    char dir_path[MAX_PATH], path[MAX_PATH], filename[64], kitname[32];
    snprintf(dir_path, sizeof(dir_path), "%s/presets", m->module_dir);
    mkdir(dir_path, 0755);

    int n = 1;
    while (n < 1000) {
        snprintf(kitname, sizeof(kitname), "kit%03d", n);
        snprintf(filename, sizeof(filename), "%s.json", kitname);
        snprintf(path, sizeof(path), "%s/%s", dir_path, filename);
        if (access(path, F_OK) != 0) break;
        n++;
    }

    FILE *f = fopen(path, "w");
    if (!f) { plog("po32: failed to save kit"); return; }

    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"%s\",\n", kitname);
    fprintf(f, "  \"level\": %.4f,\n", (double)m->level);
    fprintf(f, "  \"decay\": %.4f", (double)m->decay_scale);
    for (int i = 0; i < NUM_INSTRUMENTS; i++) {
        po32_patch_params_t *p = &m->patches[i];
        fprintf(f, ",\n  \"i%d\": \"%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\"",
            i,
            (double)p->OscWave, (double)p->OscFreq, (double)p->OscAtk, (double)p->OscDcy,
            (double)p->ModMode, (double)p->ModRate, (double)p->ModAmt,
            (double)p->NFilMod, (double)p->NFilFrq, (double)p->NFilQ,
            (double)p->NEnvMod, (double)p->NEnvAtk, (double)p->NEnvDcy,
            (double)p->Mix, (double)p->DistAmt, (double)p->EQFreq, (double)p->EQGain,
            (double)p->Level, (double)p->OscVel, (double)p->NVel, (double)p->ModVel);
    }
    fprintf(f, "\n}\n");
    fclose(f);

    int old_index = m->kit_index;
    scan_unified_kits(m);
    /* Select the newly saved kit */
    m->kit_index = old_index; /* keep current if search fails */
    for (int i = m->kit_dir_count; i < m->kit_count; i++) {
        if (strcmp(m->kit_names[i], kitname) == 0) { m->kit_index = i; break; }
    }
    plogf(m, "po32: saved %s", kitname);
}

/* ── voice pool ──────────────────────────────────────────────────── */

static voice_t *alloc_voice(po32_drum_t *m) {
    voice_t *oldest = NULL;
    size_t   oldest_remaining = SIZE_MAX;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!m->voices[i].active) return &m->voices[i];
        size_t rem = m->voices[i].len - m->voices[i].pos;
        if (rem < oldest_remaining) { oldest_remaining = rem; oldest = &m->voices[i]; }
    }
    return oldest;
}

static void trigger_voice(po32_drum_t *m, int instrument, int velocity) {
    if (instrument < 1 || instrument > NUM_INSTRUMENTS) return;
    po32_patch_params_t params = m->patches[instrument - 1];
    if (m->decay_scale != 1.0f) {
        params.OscDcy  *= m->decay_scale; if (params.OscDcy  > 1.0f) params.OscDcy  = 1.0f;
        params.NEnvDcy *= m->decay_scale; if (params.NEnvDcy > 1.0f) params.NEnvDcy = 1.0f;
    }
    voice_t *v = alloc_voice(m);
    v->pos = 0; v->active = 0;
    po32_status_t st = po32_synth_render(
        &m->synth, &params, velocity, VOICE_DURATION_S,
        v->buf, m->voice_buf_frames, &v->len);
    if (st == PO32_OK && v->len > 0) v->active = 1;
}

/* ── randomizer ──────────────────────────────────────────────────── */

static uint32_t g_rand_state = 0xA3C5E7F1u;

static float rnd(void) {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 17;
    g_rand_state ^= g_rand_state << 5;
    return (float)(g_rand_state & 0x7FFFFFFFu) / (float)0x7FFFFFFFu;
}
static float rnd_range(float lo, float hi) { return lo + rnd() * (hi - lo); }
static float rnd_one_of3(float a, float b, float c) {
    float r = rnd(); return r < 0.333f ? a : (r < 0.666f ? b : c);
}

typedef enum { ROLE_KICK, ROLE_SNARE, ROLE_CLAP, ROLE_HAT, ROLE_TOM, ROLE_PERC } drum_role_t;
static const drum_role_t ROLE_MAP[NUM_INSTRUMENTS] = {
    /* P01 Kick     P02 Rim      P03 Snare    P04 Clap   */
    ROLE_KICK,  ROLE_PERC,  ROLE_SNARE, ROLE_CLAP,
    /* P05 Snare2   P06 Lo Tom   P07 Cls HH   P08 Flr Tom */
    ROLE_SNARE, ROLE_TOM,   ROLE_HAT,   ROLE_TOM,
    /* P09 Pdl HH   P10 Mid Tom  P11 Opn HH   P12 LM Tom */
    ROLE_HAT,   ROLE_TOM,   ROLE_HAT,   ROLE_TOM,
    /* P13 HM Tom   P14 Crash    P15 Hi Tom   P16 Ride   */
    ROLE_TOM,   ROLE_HAT,   ROLE_TOM,   ROLE_HAT,
};

static void randomize_patch(po32_patch_params_t *p, drum_role_t role) {
    switch (role) {
    case ROLE_KICK:
        p->OscWave = rnd() < 0.8f ? 0.0f : 0.5f;
        p->OscFreq = rnd_range(0.03f, 0.35f);
        p->OscAtk  = 0.0f;
        p->OscDcy  = rnd_range(0.30f, 0.90f);
        p->ModMode = 0.0f;
        p->ModRate = rnd() < 0.7f ? 0.0f : rnd_range(0.0f, 0.15f);
        p->ModAmt  = rnd_range(0.0f, 0.45f);
        p->NFilMod = 0.0f;
        p->NFilFrq = rnd_range(0.05f, 0.40f);
        p->NFilQ   = rnd_range(0.0f, 0.25f);
        p->NEnvMod = 0.0f; p->NEnvAtk = 0.0f;
        p->NEnvDcy = rnd_range(0.05f, 0.40f);
        p->Mix     = rnd_range(0.60f, 1.0f);
        p->DistAmt = rnd_range(0.0f, 0.40f);
        p->Level   = rnd_range(0.60f, 0.95f);
        p->OscVel  = rnd_range(0.5f, 1.0f);
        p->NVel    = rnd_range(0.0f, 0.4f);
        break;
    case ROLE_SNARE:
        p->OscWave = rnd_one_of3(0.0f, 0.5f, 1.0f);
        p->OscFreq = rnd_range(0.12f, 0.55f);
        p->OscAtk  = 0.0f;
        p->OscDcy  = rnd_range(0.15f, 0.65f);
        p->ModMode = rnd() < 0.7f ? 0.0f : 0.5f;
        p->ModRate = rnd() < 0.6f ? 0.0f : rnd_range(0.0f, 0.4f);
        p->ModAmt  = rnd_range(0.05f, 0.55f);
        p->NFilMod = rnd_one_of3(0.0f, 0.5f, 1.0f);
        p->NFilFrq = rnd_range(0.30f, 0.85f);
        p->NFilQ   = rnd_range(0.0f, 0.5f);
        p->NEnvMod = rnd() < 0.7f ? 0.0f : 0.5f; p->NEnvAtk = 0.0f;
        p->NEnvDcy = rnd_range(0.15f, 0.65f);
        p->Mix     = rnd_range(0.05f, 0.55f);
        p->DistAmt = rnd_range(0.0f, 0.40f);
        p->Level   = rnd_range(0.50f, 0.90f);
        p->OscVel  = rnd_range(0.4f, 1.0f);
        p->NVel    = rnd_range(0.3f, 1.0f);
        break;
    case ROLE_CLAP:
        /* Pure noise burst — no oscillator contribution */
        p->OscWave = 0.0f;
        p->OscFreq = 0.0f;
        p->OscAtk  = 0.0f;
        p->OscDcy  = 0.0f;
        p->ModMode = 0.0f;
        p->ModRate = 0.0f;
        p->ModAmt  = 0.0f;
        p->NFilMod = rnd() < 0.5f ? 0.0f : 0.5f;
        p->NFilFrq = rnd_range(0.40f, 0.80f);
        p->NFilQ   = rnd_range(0.1f, 0.5f);
        p->NEnvMod = rnd() < 0.6f ? 0.5f : 1.0f;
        p->NEnvAtk = rnd_range(0.0f, 0.08f);
        p->NEnvDcy = rnd_range(0.10f, 0.50f);
        p->Mix     = 0.0f;
        p->DistAmt = rnd_range(0.0f, 0.35f);
        p->Level   = rnd_range(0.55f, 0.95f);
        p->OscVel  = 0.0f;
        p->NVel    = rnd_range(0.5f, 1.0f);
        break;
    case ROLE_HAT:
        p->OscWave = 0.0f;
        p->OscFreq = rnd_range(0.50f, 1.0f);
        p->OscAtk  = 0.0f;
        p->OscDcy  = rnd_range(0.02f, 0.55f);
        p->ModMode = rnd() < 0.6f ? 1.0f : 0.5f;
        p->ModRate = rnd_range(0.2f, 1.0f);
        p->ModAmt  = rnd_range(0.1f, 0.8f);
        p->NFilMod = rnd() < 0.4f ? 0.5f : 1.0f;
        p->NFilFrq = rnd_range(0.40f, 1.0f);
        p->NFilQ   = rnd_range(0.0f, 0.5f);
        p->NEnvMod = rnd() < 0.7f ? 0.0f : 0.5f; p->NEnvAtk = 0.0f;
        p->NEnvDcy = rnd_range(0.02f, 0.55f);
        p->Mix     = rnd_range(0.0f, 0.35f);
        p->DistAmt = rnd_range(0.0f, 0.25f);
        p->Level   = rnd_range(0.35f, 0.80f);
        p->OscVel  = rnd_range(0.3f, 0.9f);
        p->NVel    = rnd_range(0.4f, 1.0f);
        break;
    case ROLE_TOM:
        p->OscWave = rnd() < 0.6f ? 0.0f : 0.5f;
        p->OscFreq = rnd_range(0.08f, 0.55f);
        p->OscAtk  = 0.0f;
        p->OscDcy  = rnd_range(0.25f, 0.75f);
        p->ModMode = rnd() < 0.7f ? 0.0f : 0.5f;
        p->ModRate = rnd() < 0.6f ? 0.0f : rnd_range(0.0f, 0.3f);
        p->ModAmt  = rnd_range(0.05f, 0.50f);
        p->NFilMod = rnd() < 0.6f ? 0.0f : 0.5f;
        p->NFilFrq = rnd_range(0.15f, 0.65f);
        p->NFilQ   = rnd_range(0.0f, 0.4f);
        p->NEnvMod = 0.0f; p->NEnvAtk = 0.0f;
        p->NEnvDcy = rnd_range(0.10f, 0.55f);
        p->Mix     = rnd_range(0.40f, 0.90f);
        p->DistAmt = rnd_range(0.0f, 0.30f);
        p->Level   = rnd_range(0.50f, 0.90f);
        p->OscVel  = rnd_range(0.4f, 1.0f);
        p->NVel    = rnd_range(0.0f, 0.5f);
        break;
    case ROLE_PERC:
        p->OscWave = rnd_one_of3(0.0f, 0.5f, 1.0f);
        p->OscFreq = rnd_range(0.10f, 0.95f);
        p->OscAtk  = rnd() < 0.6f ? 0.0f : rnd_range(0.0f, 0.20f);
        p->OscDcy  = rnd_range(0.05f, 0.70f);
        p->ModMode = rnd_one_of3(0.0f, 0.5f, 1.0f);
        p->ModRate = rnd_range(0.0f, 1.0f);
        p->ModAmt  = rnd_range(0.0f, 1.0f);
        p->NFilMod = rnd_one_of3(0.0f, 0.5f, 1.0f);
        p->NFilFrq = rnd_range(0.1f, 0.95f);
        p->NFilQ   = rnd_range(0.0f, 0.6f);
        p->NEnvMod = rnd_one_of3(0.0f, 0.5f, 1.0f);
        p->NEnvAtk = rnd() < 0.5f ? 0.0f : rnd_range(0.0f, 0.20f);
        p->NEnvDcy = rnd_range(0.05f, 0.70f);
        p->Mix     = rnd_range(0.0f, 1.0f);
        p->DistAmt = rnd_range(0.0f, 0.50f);
        p->Level   = rnd_range(0.40f, 0.90f);
        p->OscVel  = rnd_range(0.2f, 1.0f);
        p->NVel    = rnd_range(0.0f, 0.8f);
        break;
    }
    p->EQFreq = rnd(); p->EQGain = rnd_range(0.4f, 0.6f); p->ModVel = rnd_range(0.0f, 0.4f);
}

static void randomize_kit(po32_drum_t *m) {
    g_rand_state ^= (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)m;
    for (int i = 0; i < NUM_INSTRUMENTS; i++)
        randomize_patch(&m->patches[i], ROLE_MAP[i]);
    plog("po32: randomized kit");
}

/* ── plugin_api_v2 implementation ────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    po32_drum_t *m = calloc(1, sizeof(po32_drum_t));
    if (!m) return NULL;
    strncpy(m->module_dir, module_dir, MAX_PATH - 1);
    po32_synth_init(&m->synth, SAMPLE_RATE);
    m->voice_buf_frames = (size_t)(SAMPLE_RATE * VOICE_DURATION_S) + 1;
    m->voice_bufs = calloc(MAX_VOICES * m->voice_buf_frames, sizeof(float));
    if (!m->voice_bufs) { strncpy(m->error, "out of memory", sizeof(m->error)-1); free(m); return NULL; }
    for (int i = 0; i < MAX_VOICES; i++)
        m->voices[i].buf = m->voice_bufs + i * m->voice_buf_frames;
    m->level = 1.0f; m->decay_scale = 1.0f;
    for (int i = 0; i < NUM_INSTRUMENTS; i++) po32_patch_params_zero(&m->patches[i]);

    /* Ensure presets dir exists, scan all kits */
    char presets_dir[MAX_PATH];
    snprintf(presets_dir, sizeof(presets_dir), "%s/presets", module_dir);
    mkdir(presets_dir, 0755);
    scan_unified_kits(m);
    if (m->kit_count > 0) load_kit_at(m, 0);
    else plog("po32: no kits found");
    return m;
}

static void destroy_instance(void *instance) {
    po32_drum_t *m = (po32_drum_t *)instance;
    if (!m) return;
    free(m->voice_bufs);
    free(m);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    if (len < 3) return;
    po32_drum_t *m = (po32_drum_t *)instance;
    uint8_t status = msg[0] & 0xF0, note = msg[1], velocity = msg[2];
    if (status == 0x90 && velocity > 0)
        trigger_voice(m, (int)note - MIDI_NOTE_BASE + 1, (int)velocity);
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    po32_drum_t *m = (po32_drum_t *)instance;
    if (strcmp(key, "kit") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < m->kit_count) load_kit_at(m, idx);
    } else if (strcmp(key, "kit_query") == 0) {
        int v = atoi(val);
        if (v >= 0 && v < m->kit_count) m->kit_query_idx = v;
    } else if (strcmp(key, "level") == 0) {
        float v = (float)atof(val);
        m->level = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    } else if (strcmp(key, "decay") == 0) {
        float v = (float)atof(val);
        m->decay_scale = v < 0.1f ? 0.1f : (v > 3.0f ? 3.0f : v);
    } else if (strcmp(key, "state") == 0) {
        float fval;
        if (json_get_float(val, "kit", &fval)) m->kit_index = (int)fval;
        if (json_get_float(val, "level", &fval)) m->level = fval;
        if (json_get_float(val, "decay", &fval)) m->decay_scale = fval;
        for (int i = 0; i < NUM_INSTRUMENTS; i++) {
            char ikey[8]; snprintf(ikey, sizeof(ikey), "i%d", i);
            char csv[256];
            if (!json_get_str(val, ikey, csv, sizeof(csv))) continue;
            po32_patch_params_t *p = &m->patches[i];
            char *tok = csv;
            for (int f = 0; f < 21; f++) {
                *patch_field(p, f) = (float)atof(tok);
                tok = strchr(tok, ',');
                if (!tok) break;
                tok++;
            }
        }
    } else if (strcmp(key, "save_kit") == 0) {
        if (atoi(val) == 1) save_kit(m);
    } else if (strcmp(key, "randomize") == 0) {
        if (atoi(val) == 1) randomize_kit(m);
    } else if (strcmp(key, "randomize_inst") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < NUM_INSTRUMENTS)
            randomize_patch(&m->patches[idx], ROLE_MAP[idx]);
    } else if (strcmp(key, "inst") == 0) {
        int v = atoi(val);
        if (v >= 0 && v < NUM_INSTRUMENTS) m->selected_inst = v;
    } else {
        po32_patch_params_t *p = &m->patches[m->selected_inst];
        if (strcmp(key, "inst_wave") == 0) {
            int v = atoi(val);
            p->OscWave = v <= 0 ? 0.0f : (v == 1 ? 0.5f : 1.0f);
        } else if (strcmp(key, "inst_freq") == 0) {
            float v = (float)atoi(val) / 100.0f;
            p->OscFreq = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        } else if (strcmp(key, "inst_dcy") == 0) {
            float v = (float)atoi(val) / 100.0f;
            p->OscDcy = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        } else if (strcmp(key, "inst_mod_mode") == 0) {
            int v = atoi(val);
            p->ModMode = v <= 0 ? 0.0f : (v == 1 ? 0.5f : 1.0f);
        } else if (strcmp(key, "inst_mod_amt") == 0) {
            float v = (float)atoi(val) / 100.0f;
            p->ModAmt = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        } else if (strcmp(key, "inst_noise") == 0) {
            float v = 1.0f - (float)atoi(val) / 100.0f;
            p->Mix = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        } else if (strcmp(key, "inst_dist") == 0) {
            float v = (float)atoi(val) / 100.0f;
            p->DistAmt = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        } else if (strcmp(key, "inst_level") == 0) {
            float v = (float)atoi(val) / 100.0f;
            p->Level = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    po32_drum_t *m = (po32_drum_t *)instance;
    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Libpo32");
    } else if (strcmp(key, "kit") == 0) {
        return snprintf(buf, buf_len, "%d", m->kit_index);
    } else if (strcmp(key, "kit_count") == 0) {
        return snprintf(buf, buf_len, "%d", m->kit_count);
    } else if (strcmp(key, "kit_name") == 0) {
        if (m->kit_index >= 0 && m->kit_index < m->kit_count)
            return snprintf(buf, buf_len, "%s", m->kit_names[m->kit_index]);
        if (buf_len > 0) buf[0] = '\0'; return 0;
    } else if (strcmp(key, "kit_query_name") == 0) {
        if (m->kit_query_idx >= 0 && m->kit_query_idx < m->kit_count)
            return snprintf(buf, buf_len, "%s", m->kit_names[m->kit_query_idx]);
        if (buf_len > 0) buf[0] = '\0'; return 0;
    } else if (strcmp(key, "kit_names_all") == 0) {
        char *p = buf;
        int rem = buf_len - 1;
        for (int i = 0; i < m->kit_count && rem > 0; i++) {
            if (i > 0 && rem > 0) { *p++ = '|'; rem--; }
            int len = (int)strlen(m->kit_names[i]);
            if (len > rem) len = rem;
            memcpy(p, m->kit_names[i], len);
            p += len; rem -= len;
        }
        *p = '\0';
        return (int)(p - buf);
    } else if (strcmp(key, "level") == 0) {
        return snprintf(buf, buf_len, "%.3f", (double)m->level);
    } else if (strcmp(key, "decay") == 0) {
        return snprintf(buf, buf_len, "%.3f", (double)m->decay_scale);
    } else if (strcmp(key, "inst") == 0) {
        return snprintf(buf, buf_len, "%d", m->selected_inst);
    } else if (strcmp(key, "state") == 0) {
        int pos = 0;
        pos += snprintf(buf + pos, buf_len - pos,
            "{\"kit\":%d,\"level\":%.4f,\"decay\":%.4f",
            m->kit_index, (double)m->level, (double)m->decay_scale);
        for (int i = 0; i < NUM_INSTRUMENTS && pos < buf_len - 10; i++) {
            po32_patch_params_t *p = &m->patches[i];
            pos += snprintf(buf + pos, buf_len - pos,
                ",\"i%d\":\"%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\"",
                i,
                (double)p->OscWave, (double)p->OscFreq, (double)p->OscAtk, (double)p->OscDcy,
                (double)p->ModMode, (double)p->ModRate, (double)p->ModAmt,
                (double)p->NFilMod, (double)p->NFilFrq, (double)p->NFilQ,
                (double)p->NEnvMod, (double)p->NEnvAtk, (double)p->NEnvDcy,
                (double)p->Mix, (double)p->DistAmt, (double)p->EQFreq, (double)p->EQGain,
                (double)p->Level, (double)p->OscVel, (double)p->NVel, (double)p->ModVel);
        }
        if (pos < buf_len - 1) buf[pos++] = '}';
        if (pos < buf_len) buf[pos] = '\0';
        return pos;
    } else {
        const po32_patch_params_t *p = &m->patches[m->selected_inst];
        if (strcmp(key, "inst_wave") == 0) {
            return snprintf(buf, buf_len, "%d", p->OscWave < 0.25f ? 0 : (p->OscWave < 0.75f ? 1 : 2));
        } else if (strcmp(key, "inst_freq") == 0) {
            return snprintf(buf, buf_len, "%d", (int)(p->OscFreq * 100.0f + 0.5f));
        } else if (strcmp(key, "inst_dcy") == 0) {
            return snprintf(buf, buf_len, "%d", (int)(p->OscDcy * 100.0f + 0.5f));
        } else if (strcmp(key, "inst_mod_mode") == 0) {
            return snprintf(buf, buf_len, "%d", p->ModMode < 0.25f ? 0 : (p->ModMode < 0.75f ? 1 : 2));
        } else if (strcmp(key, "inst_mod_amt") == 0) {
            return snprintf(buf, buf_len, "%d", (int)(p->ModAmt * 100.0f + 0.5f));
        } else if (strcmp(key, "inst_noise") == 0) {
            return snprintf(buf, buf_len, "%d", (int)((1.0f - p->Mix) * 100.0f + 0.5f));
        } else if (strcmp(key, "inst_dist") == 0) {
            return snprintf(buf, buf_len, "%d", (int)(p->DistAmt * 100.0f + 0.5f));
        } else if (strcmp(key, "inst_level") == 0) {
            return snprintf(buf, buf_len, "%d", (int)(p->Level * 100.0f + 0.5f));
        }
    }
    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    po32_drum_t *m = (po32_drum_t *)instance;
    if (!m->error[0]) return 0;
    return snprintf(buf, buf_len, "%s", m->error);
}

static void v2_render_block(void *instance, int16_t *out, int frames) {
    po32_drum_t *m = (po32_drum_t *)instance;
    float mix[MOVE_FRAMES_PER_BLOCK];
    memset(mix, 0, sizeof(float) * frames);
    for (int v = 0; v < MAX_VOICES; v++) {
        voice_t *voice = &m->voices[v];
        if (!voice->active) continue;
        int remaining = (int)(voice->len - voice->pos);
        int n = remaining < frames ? remaining : frames;
        const float *src = voice->buf + voice->pos;
        for (int i = 0; i < n; i++) mix[i] += src[i];
        voice->pos += n;
        if (voice->pos >= voice->len) voice->active = 0;
    }
    float gain = m->level * 28000.0f;
    for (int i = 0; i < frames; i++) {
        float s = mix[i] * gain;
        if (s >  32767.0f) s =  32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        int16_t sample = (int16_t)s;
        out[i * 2] = sample; out[i * 2 + 1] = sample;
    }
}

/* ── entry point ─────────────────────────────────────────────────── */

static plugin_api_v2_t g_api_v2;

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    g_api_v2.api_version      = MOVE_PLUGIN_API_VERSION_2;
    g_api_v2.create_instance  = create_instance;
    g_api_v2.destroy_instance = destroy_instance;
    g_api_v2.on_midi          = v2_on_midi;
    g_api_v2.set_param        = v2_set_param;
    g_api_v2.get_param        = v2_get_param;
    g_api_v2.get_error        = v2_get_error;
    g_api_v2.render_block     = v2_render_block;
    return &g_api_v2;
}
