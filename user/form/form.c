/*
 * form.c — a nanowm demo app built with libnwui: a tiny greeting form. Shows composition
 * (nested column/row), the flex layout, a text field, buttons with callbacks, and MVU-style
 * state -> view updates (clicking "Greet" sets a label via nwui_set_text). The whole UI is
 * the toolkit's; this app is just the tree + a little state.
 */
#include "nwui.h"
#include <string.h>
#include <stdio.h>

static char       g_name[64];
static nwui_node *g_field;
static nwui_node *g_result;

static void on_greet(nwui_node *self, void *user)
{
	char buf[96];
	if (g_name[0])
		snprintf(buf, sizeof buf, "Hello, %s!", g_name);
	else
		snprintf(buf, sizeof buf, "Hello there!");
	nwui_set_text(g_result, buf);
}

static void on_clear(nwui_node *self, void *user)
{
	g_name[0] = 0;
	nwui_set_text(g_field, "");          /* reset the field's value + caret */
	nwui_set_text(g_result, "");
}

int main(void)
{
	nwui *u = nwui_open("Greeter", 360, 200);
	if (!u)
		return 1;

	g_field  = nwui_textfield(u, g_name, sizeof g_name, 0, 0);
	g_result = nwui_label(u, "");

	nwui_set_root(u, nwui_pad(nwui_gap(nwui_column(u,
		nwui_label(u, "Your name:"),
		nwui_flex(g_field, 0),
		nwui_gap(nwui_row(u,
			nwui_button(u, "Greet", on_greet, 0),
			nwui_button(u, "Clear", on_clear, 0),
			(nwui_node *) 0), 8),
		g_result,
		(nwui_node *) 0), 8), 12));

	nwui_run(u);
	return 0;
}
