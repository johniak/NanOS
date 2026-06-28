/*
 * nw_settings.h — NanWM desktop preferences: a tiny key/value model shared by the compositor
 * (nwm, which reads + applies them) and the Settings app (settings, which edits + persists them).
 *
 * Persisted as a simple YAML-ish `key: value` text file at /disks/main/nanos/config/settings.yaml.
 * This module is PURE (no I/O, no allocation — caller owns every buffer), so parse/serialize and
 * the level->pixel mappings are host-tested; the file read/write lives in nwm/settings.
 *
 * Four knobs: two on/off toggles (backdrop blur, glass translucency) and two 0..100 levels
 * (blur strength, translucency strength). The derive helpers turn levels into the alpha/radius
 * the compositor actually uses.
 */
#ifndef NW_SETTINGS_H
#define NW_SETTINGS_H

#define NW_SETTINGS_PATH "/disks/main/nanos/config/settings.yaml"

/* Wallpaper mode (the `wallpaper:` key). */
enum { NW_WALL_BRANDED = 0, NW_WALL_GRADIENT = 1, NW_WALL_SOLID = 2 };

struct nw_settings {
	int blur;                /* backdrop blur enabled (0/1)        */
	int blur_level;          /* blur strength, 0..100              */
	int transparency;        /* glass translucency enabled (0/1)   */
	int transparency_level;  /* translucency strength, 0..100      */
	unsigned accent;         /* UI accent colour, 0xRRGGBB         */
	int wallpaper;           /* NW_WALL_* mode                     */
	unsigned wallpaper_color;/* fill colour for NW_WALL_SOLID      */
	int clock_24h;           /* 1=24-hour, 0=12-hour clock         */
	int clock_seconds;       /* show seconds on the bar clock      */
	int shadow;              /* draw window drop shadows (0/1)     */
	int corner_radius;       /* window corner radius px, 0..20     */
	char ui_font[64];        /* UI font filename under /nanos/share/fonts (proportional, AA) */
};

#define NW_UI_FONT_DEFAULT "UISans-Regular.ttf"
#define NW_FONTS_DIR "/disks/main/nanos/share/fonts"

/* Reset to the shipped defaults (blur OFF — it is expensive; translucency ON). */
void nw_settings_defaults(struct nw_settings *s);

/* Overlay any recognised `key: value` lines found in buf[0,len) onto *s (start from defaults
 * first for a full load). Unknown keys, blank lines and `#` comments are ignored; bool values
 * accept true/false/on/off/yes/no/1/0; levels are clamped to 0..100. */
void nw_settings_parse(const char *buf, int len, struct nw_settings *s);

/* Serialize *s as the canonical file text into out[0,cap). Returns the byte length written
 * (excluding the NUL), or 0 if it would not fit. */
int  nw_settings_serialize(const struct nw_settings *s, char *out, int cap);

/* Effective compositor values. Alpha is the "glass body" coverage (255 = opaque); a lower
 * value shows more of the backdrop. Translucency OFF => fully opaque (255). The blur radius is
 * 0 when blur is OFF. */
int  nw_settings_win_alpha(const struct nw_settings *s);    /* light windows */
int  nw_settings_dark_alpha(const struct nw_settings *s);   /* dark windows  */
int  nw_settings_blur_radius(const struct nw_settings *s);  /* full-res radius, 0 = no blur */

#endif /* NW_SETTINGS_H */
