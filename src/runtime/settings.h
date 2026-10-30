/* mcg.ini: the remaster's settings, beside mcg_recomp.exe. See docs/settings.md. */
#pragma once
#include <stdint.h>

/* Read once at startup; writes a commented default file if there is none. */
void settings_init(void);

/* [section] key=value from mcg.ini, or dflt. Never NULL unless dflt is.
 * A command-line flag overrides a setting: check the flag first, then this. */
const char* mcg_setting(const char* section, const char* key, const char* dflt);
int mcg_setting_int(const char* section, const char* key, int dflt);

/* [keys]: the key the engine should see for physical virtual-key vk. */
unsigned settings_key_out(unsigned vk);
/* ...and the inverse, for GetAsyncKeyState/GetKeyState: the physical key to
 * poll when the engine asks about vk (0 when nothing is bound to it). */
unsigned settings_key_in(unsigned vk);

/* [game] options that act on engine state: once before entry, then per pump. */
void settings_apply_game(void);
void settings_tick(void);
