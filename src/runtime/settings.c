/*
 * mcg.ini -- the remaster's settings file (docs/settings.md).
 *
 * Kept beside mcg_recomp.exe, as ra2.ini is beside ra2.exe, and read with the
 * Win32 profile API: the format is the INI everyone already knows and there is
 * no parser of our own to get wrong. Missing on first run -> written with
 * every key at its default, commented, so the file documents itself.
 *
 * The game's OWN preferences (difficulty, music/sfx/radio volume, brightness)
 * stay in game/PREFS.CFG, where its options screen reads and writes them.
 * mcg.ini is only for what the 1999 game had no setting for.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "recomp_types.h"
#include "settings.h"

static char g_ini[MAX_PATH];

static const char DEFAULT_INI[] =
    "; MechCommander Gold recomp settings. Delete this file to get the defaults\n"
    "; back. A command-line flag overrides the matching setting. docs/settings.md\n"
    "\n"
    "[display]\n"
    "; fullscreen=0      1 = borderless fullscreen (same as --fullscreen)\n"
    "; scale=            window scale (same as --scale)\n"
    "; res=              WxH (same as --res)\n"
    "\n"
    "[sound]\n"
    "; master_volume=100 0-100, on top of the game's own music/sfx/radio sliders\n"
    "; mute=0\n"
    "\n"
    "[game]\n"
    "; 1 = jump to any campaign mission from the briefing screen by typing its\n"
    ";     two-digit number (the developers' own mission jump; docs/settings.md)\n"
    "unlock_missions=0\n"
    "\n"
    "[keys]\n"
    "; physical=what the game sees, e.g.  Q=A  makes Q the ammo-conservation key.\n"
    "; Names: a letter or digit, F1-F12, SPACE TAB ENTER ESC BACKSPACE SHIFT CTRL\n"
    "; ALT UP DOWN LEFT RIGHT HOME END PGUP PGDN INS DEL, or 0xNN (a virtual key).\n";

void settings_init_keys(void);

void settings_init(void) {
    GetModuleFileNameA(NULL, g_ini, MAX_PATH);
    char* slash = strrchr(g_ini, '\\');
    strcpy_s(slash ? slash + 1 : g_ini, MAX_PATH - (slash ? (slash + 1 - g_ini) : 0),
             "mcg.ini");
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(g_ini, "w");
        if (f) { fputs(DEFAULT_INI, f); fclose(f); }
        fprintf(stderr, "[settings] wrote defaults to %s\n", g_ini);
    } else {
        fprintf(stderr, "[settings] %s\n", g_ini);
    }
    settings_init_keys();
}

/* ponytail: 16 rotating buffers so a caller can hold a few results at once;
 * nobody keeps one past startup. */
const char* mcg_setting(const char* section, const char* key, const char* dflt) {
    static char buf[16][256];
    static int n;
    char* b = buf[n++ & 15];
    if (!g_ini[0]) return dflt;
    GetPrivateProfileStringA(section, key, "\x01", b, sizeof buf[0], g_ini);
    return (b[0] == '\x01' || !b[0]) ? dflt : b;
}

int mcg_setting_int(const char* section, const char* key, int dflt) {
    const char* v = mcg_setting(section, key, NULL);
    return v ? (int)strtol(v, NULL, 0) : dflt;
}

/* ============================================================
 * [keys] remap. Applied host-side: PeekMessageA rewrites WM_KEYDOWN/UP before
 * the engine (and its TranslateMessage) sees them, GetAsyncKeyState/
 * GetKeyState poll the physical key bound to what the engine asks about, and
 * scripted typing goes through the same table so a run can prove it.
 * ============================================================ */
static uint8_t g_out[256], g_in[256];

static unsigned key_name(const char* s) {
    static const struct { const char* n; unsigned vk; } names[] = {
        {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN},
        {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK}, {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL}, {"ALT", VK_MENU}, {"UP", VK_UP},
        {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PGUP", VK_PRIOR},
        {"PGDN", VK_NEXT}, {"INS", VK_INSERT}, {"DEL", VK_DELETE},
    };
    while (*s == ' ') s++;
    if (s[0] && (!s[1] || s[1] == ' ')) {
        char c = (char)toupper((unsigned char)s[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (unsigned)c;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtoul(s, NULL, 16) & 0xFF;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(s, names[i].n)) return names[i].vk;
    return 0;
}

int settings_bind(const char* line) {   /* "PHYS=GAME"; 0 if malformed */
    char k[32];
    const char* eq = strchr(line, '=');
    if (!eq || eq - line >= (int)sizeof k) return 0;
    memcpy(k, line, eq - line); k[eq - line] = 0;
    for (char* e = k + strlen(k); e > k && e[-1] == ' '; ) *--e = 0;
    unsigned from = key_name(k), to = key_name(eq + 1);
    if (!from || !to) return 0;
    g_out[from] = (uint8_t)to;
    return 1;
}

void settings_init_keys(void) {
    for (int i = 0; i < 256; i++) g_out[i] = (uint8_t)i;
    char sec[4096];
    DWORD n = GetPrivateProfileSectionA("keys", sec, sizeof sec, g_ini);
    for (char* p = sec; p < sec + n && *p; p += strlen(p) + 1) {
        if (*p == ';') continue;
        if (settings_bind(p)) fprintf(stderr, "[settings] key %s\n", p);
        else fprintf(stderr, "[settings] [keys] cannot read \"%s\"\n", p);
    }
    memset(g_in, 0, sizeof g_in);
    for (int i = 255; i > 0; i--) g_in[g_out[i]] = (uint8_t)i;   /* lowest phys wins */
}

unsigned settings_key_out(unsigned vk) { return vk < 256 ? g_out[vk] : vk; }
unsigned settings_key_in(unsigned vk)  { return vk < 256 ? g_in[vk] : vk; }

/* ============================================================
 * [game] unlock_missions: the developers' mission jump.
 *
 * Logistics::processCheatCode (0x006FFA50), called from the briefing screen
 * for every key, has a branch that reads two typed digits as a scenario index
 * into mechcmdr1.fit, saves the current roster as start<N> and reloads it as
 * the campaign at mission N -- then the briefing for that mission comes up.
 * Its enable flag (0x00808654) is only ever CLEARED in the retail build (each
 * jump clears it); the code that set it did not ship. It also sits behind
 * cheatsOn (0x0080879C), the ixtlriimceourl switch. Setting both is the
 * whole feature: the jump itself is the game's code.
 * ============================================================ */
#define MCG_CHEATS_ON      0x0080879Cu
#define MCG_MISSION_JUMP   0x00808654u
static int g_unlock;

void settings_apply_game(void) {
    g_unlock = mcg_setting_int("game", "unlock_missions", 0);
    if (g_unlock) {
        MEM32(MCG_CHEATS_ON) = 1;
        fprintf(stderr, "[settings] unlock_missions: briefing-screen mission jump on\n");
    }
}

void settings_tick(void) {
    if (g_unlock) MEM32(MCG_MISSION_JUMP) = 1;   /* re-armed after each jump */
}
