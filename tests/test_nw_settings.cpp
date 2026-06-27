#include "doctest.h"
#include "nw_settings.h"
#include <cstring>

TEST_CASE("defaults: blur off, glass on, sane levels") {
	nw_settings s; nw_settings_defaults(&s);
	CHECK(s.blur == 0);
	CHECK(s.transparency == 1);
	CHECK(s.blur_level == 60);
	CHECK(s.transparency_level == 50);
	// off-by-default blur => no radius; glass on => translucent (alpha < 255)
	CHECK(nw_settings_blur_radius(&s) == 0);
	CHECK(nw_settings_win_alpha(&s) < 255);
}

TEST_CASE("parse overlays recognised keys, ignores junk/comments") {
	nw_settings s; nw_settings_defaults(&s);
	const char *txt =
		"# a comment\n"
		"blur: true\n"
		"blur_level: 80\n"
		"transparency: off\n"
		"  transparency_level : 30  \n"
		"unknown_key: 999\n"
		"garbage-without-colon\n";
	nw_settings_parse(txt, (int) strlen(txt), &s);
	CHECK(s.blur == 1);
	CHECK(s.blur_level == 80);
	CHECK(s.transparency == 0);
	CHECK(s.transparency_level == 30);
}

TEST_CASE("parse clamps levels into 0..100") {
	nw_settings s; nw_settings_defaults(&s);
	const char *txt = "blur_level: 9999\n";
	nw_settings_parse(txt, (int) strlen(txt), &s);
	CHECK(s.blur_level == 100);
}

TEST_CASE("serialize then parse round-trips all fields") {
	nw_settings a; nw_settings_defaults(&a);
	a.blur = 1; a.blur_level = 40; a.transparency = 1; a.transparency_level = 25;
	a.accent = 0x7d3ff2u; a.wallpaper = NW_WALL_SOLID; a.wallpaper_color = 0x101820u;
	a.clock_24h = 0; a.clock_seconds = 1; a.shadow = 0; a.corner_radius = 6;
	{ const char *f = "Mono-Regular.ttf"; int i = 0; for (; f[i]; i++) a.ui_font[i] = f[i]; a.ui_font[i] = 0; }
	char buf[256];
	int n = nw_settings_serialize(&a, buf, sizeof buf);
	CHECK(n > 0);
	nw_settings b; nw_settings_defaults(&b);
	nw_settings_parse(buf, n, &b);
	CHECK(b.blur == a.blur);
	CHECK(b.blur_level == a.blur_level);
	CHECK(b.transparency == a.transparency);
	CHECK(b.transparency_level == a.transparency_level);
	CHECK(b.accent == a.accent);
	CHECK(b.wallpaper == a.wallpaper);
	CHECK(b.wallpaper_color == a.wallpaper_color);
	CHECK(b.clock_24h == a.clock_24h);
	CHECK(b.clock_seconds == a.clock_seconds);
	CHECK(b.shadow == a.shadow);
	CHECK(b.corner_radius == a.corner_radius);
	CHECK(strcmp(b.ui_font, a.ui_font) == 0);
}

TEST_CASE("parse colours (0x/#/bare hex), wallpaper names, clamps radius") {
	nw_settings s; nw_settings_defaults(&s);
	const char *txt =
		"accent: #ff9d00\n"
		"wallpaper: gradient\n"
		"wallpaper_color: 0x010203\n"
		"corner_radius: 99\n";
	nw_settings_parse(txt, (int) strlen(txt), &s);
	CHECK(s.accent == 0xff9d00u);
	CHECK(s.wallpaper == NW_WALL_GRADIENT);
	CHECK(s.wallpaper_color == 0x010203u);
	CHECK(s.corner_radius == 20);          // clamped to max
}

TEST_CASE("derive: translucency off => opaque; on => more transparent at higher level") {
	nw_settings s; nw_settings_defaults(&s);
	s.transparency = 0;
	CHECK(nw_settings_win_alpha(&s) == 255);
	CHECK(nw_settings_dark_alpha(&s) == 255);

	s.transparency = 1; s.transparency_level = 10;
	int a_lo = nw_settings_win_alpha(&s);
	s.transparency_level = 60;
	int a_hi = nw_settings_win_alpha(&s);
	CHECK(a_hi < a_lo);            // higher level => lower alpha => more see-through
	CHECK(a_lo <= 255);
	CHECK(a_hi >= 130);            // never fully invisible
}

TEST_CASE("derive: blur radius scales with level when enabled") {
	nw_settings s; nw_settings_defaults(&s);
	s.blur = 1; s.blur_level = 0;
	CHECK(nw_settings_blur_radius(&s) == 1);     // enabled but min radius
	s.blur_level = 100;
	CHECK(nw_settings_blur_radius(&s) == 40);
	s.blur = 0;
	CHECK(nw_settings_blur_radius(&s) == 0);      // disabled overrides
}
