# nwnote → Windows XP-style Notepad — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild `user/nwnote` into a Windows XP-style Notepad by growing the `libnwui` toolkit with reusable widgets (multiline editor, checkbox, keyboard accelerators, modal dialogs, file dialog) and making Notepad a thin client on top.

**Architecture:** Reusable *logic* goes into the pure, host-tested core `user/libnwui/nwui_core.c` (gated >90% coverage); *painting* into `user/libnwui/nwui_paint.c`; *I/O* into `user/libnwui/nwui.c`; *public API* into `user/libnwui/nwui.h`. `user/nwnote/nwnote.c` is rewritten to consume the toolkit, declares menus via the existing global-menu-bar API, and registers accelerators that share the menu callbacks.

**Tech Stack:** Freestanding C (picolibc), NanWM `libnw` client protocol, `libnwui` toolkit, doctest host tests, QEMU headless verification.

## Global Constraints

- **No Claude attribution in commits/PRs** — no `Co-Authored-By`, no "Generated with" trailers. Clean messages only.
- **Builds run in Docker**: `make build`/`make image`/`make test` shell into the `nanos-build`/`nanos-test` containers automatically. QEMU runs natively.
- **Host tests are the gate**: `make test` compiles `tests/*.cpp` with doctest and **fails if `nwui_core.c` line coverage drops below 90%** (`COV_PATTERNS` includes `*/nwui_core.*`). Every behaviour added to `nwui_core.c` needs a host test.
- **No compositor/kernel changes**: shortcuts, dialogs, and editing are entirely client-side. Ctrl is scancode `0x1D` (right-Ctrl `0x9D`); F3 `0x3D`, F5 `0x3F`; arrows/Home/End/PgUp/PgDn/Del extended codes `0xC8`/`0xD0`/`0xCB`/`0xCD`/`0xC7`/`0xCF`/`0xC9`/`0xD1`/`0xD3`.
- **App-owned buffers**: editor/field widgets never own their text buffer; the app passes `char *buf, int cap` (existing `nwui_textfield` pattern).
- **Node arena bound**: `NWUI_MAX_NODES = 128`, `NWUI_MAX_CHILD = 16`. Bump in Task 10 only if the modal + file-dialog subtrees overflow.
- **Font metrics**: `NW_FONT_W = 8`, `NW_FONT_H = 16`.
- Branch `feat/nwnote-notepad` already exists with the design spec committed.

---

## File Structure

| File | Responsibility | Tasks |
|------|----------------|-------|
| `user/libnwui/nwui_core.h` | node struct fields, kinds, constants, public-of-core decls | 1,2,8,9,10,12 |
| `user/libnwui/nwui_core.c` | textarea/checkbox/accel/modal/file-dialog **logic** (pure, gated) | 1–12 |
| `user/libnwui/nwui_paint.c` | painting for the new widgets + modal backdrop | 13,14 |
| `user/libnwui/nwui.c` | I/O: `getdents` listing for the file dialog | 12 |
| `user/libnwui/nwui.h` | public toolkit API | 1,4,6,8,9,11,12 |
| `tests/test_nwui_core.cpp` | new doctest cases (the gate) | 1–12 |
| `user/nwnote/nwnote.c` | the Notepad app (rewrite) | 15 |
| `Makefile` | relink `nwnote.nxe` against `libnwui` | 15 |
| `docs/en/*`, design memory | docs touch-up | 16 |

---

## Task 1: `nwui_textarea` skeleton — kind, constructor, printable insert, backspace

**Files:**
- Modify: `user/libnwui/nwui_core.h` (kind enum, struct fields, scancode constants)
- Modify: `user/libnwui/nwui_core.c` (constructor, measure, insert/backspace in dispatch)
- Modify: `user/libnwui/nwui.h` (public `nwui_textarea` decl)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Produces: `nwui_node *nwui_textarea(nwui *u, char *buf, int cap, nwui_cb on_change, void *user)` — multiline editor over an app-owned buffer. Reuses node fields `tbuf/tcap/tlen/caret/anchor/scroll/on_change/user`, `focusable=1`. New node kind `NWUI_TEXTAREA`. Insertion accepts `\n` and `\t` (unlike textfield).

- [ ] **Step 1: Write the failing test**

In `tests/test_nwui_core.cpp` add a helper to send a key by scancode+ascii (the existing `key()` only sets `ch`), then the test:

```cpp
static void keyc(nwui *u, int code, char ch, int mods) {
	nw_event e; memset(&e, 0, sizeof e);
	e.type = NW_EV_KEY; e.down = 1; e.ch = ch; e.code = code; e.mods = mods;
	nwui_dispatch(u, &e);
}

TEST_CASE("textarea inserts printable chars and newlines, backspace deletes") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta);
	u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1;          // textarea is focused

	key(u, 'h'); key(u, 'i'); keyc(u, 0x1C, '\n', 0); key(u, 'x');
	CHECK(strcmp(tb, "hi\nx") == 0);
	CHECK(ta->caret == 4);

	key(u, 8);                                // backspace
	CHECK(strcmp(tb, "hi\n") == 0);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -40`
Expected: compile error `nwui_textarea` undeclared (function not defined yet).

- [ ] **Step 3: Implement the minimal code**

In `nwui_core.h`, extend the kind enum and add the extended scancodes used later:

```c
enum { NWUI_BOX, NWUI_ROW, NWUI_COLUMN, NWUI_LABEL, NWUI_BUTTON, NWUI_TEXTFIELD,
       NWUI_LIST, NWUI_IMAGE, NWUI_TEXTAREA, NWUI_CHECKBOX };
```

Add to the scancode enum (next to `NWUI_SC_*`):

```c
enum {
	NWUI_SC_PGUP = 0xC9, NWUI_SC_PGDN = 0xD1, NWUI_SC_DEL = 0xD3,
	NWUI_SC_CTRL = 0x1D, NWUI_SC_RCTRL = 0x9D, NWUI_SC_F3 = 0x3D, NWUI_SC_F5 = 0x3F
};
```

Add a `int wrap;` field to `struct nwui_node` (word-wrap flag, used from Task 4).

In `nwui_core.c`, the constructor (mirrors `nwui_textfield`):

```c
nwui_node *nwui_textarea(nwui *u, char *buf, int cap, nwui_cb on_change, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_TEXTAREA);
	n->tbuf = buf; n->tcap = cap;
	n->tlen = buf ? (int) strlen(buf) : 0;
	n->caret = 0; n->anchor = 0; n->scroll = 0;
	n->on_change = on_change; n->user = user; n->focusable = 1;
	return n;
}
```

Add multiline editing helpers near the textfield helpers. These operate on the same
`tbuf/tlen/caret/anchor` and reuse the existing `sel_lo/sel_hi/has_sel`:

```c
static void ta_del_range(nwui_node *n, int lo, int hi)
{
	if (lo < 0) lo = 0; if (hi > n->tlen) hi = n->tlen; if (lo >= hi) return;
	int k = hi - lo;
	for (int i = lo; i + k <= n->tlen; i++) n->tbuf[i] = n->tbuf[i + k];
	n->tlen -= k; n->tbuf[n->tlen] = 0; n->caret = lo; n->anchor = lo;
}
static int ta_insert(nwui_node *n, char ch)
{
	if (ch != '\n' && ch != '\t' && (unsigned char) ch < 32) return 0;
	if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
	if (n->tlen >= n->tcap - 1) return 0;
	for (int i = n->tlen; i > n->caret; i--) n->tbuf[i] = n->tbuf[i - 1];
	n->tbuf[n->caret] = ch; n->caret++; n->tlen++; n->anchor = n->caret;
	n->tbuf[n->tlen] = 0; return 1;
}
```

In `nwui_measure`, give `NWUI_TEXTAREA` a default size:

```c
	case NWUI_TEXTAREA:
		n->mw = 240; n->mh = 6 * NW_FONT_H;
		break;
```

In `nwui_dispatch`'s `NW_EV_KEY` case, after the list block and before the textfield
block, add textarea handling:

```c
	if (u->focus && u->focus->kind == NWUI_TEXTAREA) {
		nwui_node *n = u->focus;
		if (ev->ch == 8) {                       /* backspace */
			if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
			else if (n->caret > 0) ta_del_range(n, n->caret - 1, n->caret);
			n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
		} else if (ta_insert(n, ev->ch)) {
			n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
		}
		break;
	}
```

Declare in `nwui.h` (next to `nwui_textfield`):

```c
nwui_node *nwui_textarea(nwui *u, char *buf, int cap, nwui_cb on_change, void *user);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: all tests pass; coverage gate still green.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.h user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea widget skeleton (insert/backspace, multiline)"
```

---

## Task 2: textarea caret navigation + line index + `nwui_textarea_caret`

**Files:**
- Modify: `user/libnwui/nwui_core.c` (line-index helpers, arrow/Home/End/Up/Down, getter)
- Modify: `user/libnwui/nwui.h` (`nwui_textarea_caret` decl)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: `nwui_textarea` (Task 1).
- Produces: caret moves Left/Right/Home/End/Up/Down with shift extending the selection (anchor rule from `tf_move`); `void nwui_textarea_caret(nwui_node *n, int *line, int *col)` — 1-based line, 1-based column at the caret.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("textarea caret moves by line and reports line/col") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "ab\ncde\nf";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1;
	ta->caret = 0; ta->anchor = 0;

	int ln, col;
	keyc(u, NWUI_SC_DOWN, 0, 0);              // into "cde", aim col 1
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 2); CHECK(col == 1);
	keyc(u, NWUI_SC_END, 0, 0);
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 2); CHECK(col == 4);  // after "cde"
	keyc(u, NWUI_SC_DOWN, 0, 0);              // "f" is shorter -> clamp to end
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 3); CHECK(col == 2);
	keyc(u, NWUI_SC_HOME, 0, 0);
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 3); CHECK(col == 1);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — `nwui_textarea_caret` undefined.

- [ ] **Step 3: Implement the minimal code**

Add logical-line helpers in `nwui_core.c`:

```c
/* offset of the start of the logical line containing byte position p */
static int ta_line_start(const nwui_node *n, int p)
{
	while (p > 0 && n->tbuf[p - 1] != '\n') p--;
	return p;
}
/* offset of the end (the '\n' or tlen) of the line containing p */
static int ta_line_end(const nwui_node *n, int p)
{
	while (p < n->tlen && n->tbuf[p] != '\n') p++;
	return p;
}
static void ta_move(nwui_node *n, int pos, int extend)
{
	if (pos < 0) pos = 0; if (pos > n->tlen) pos = n->tlen;
	n->caret = pos; if (!extend) n->anchor = pos; n->dirty = 1;
}
/* move up/down one logical line, keeping the column (clamped to the target line) */
static void ta_move_vert(nwui_node *n, int dir, int extend)
{
	int ls = ta_line_start(n, n->caret);
	int col = n->caret - ls;
	int target;
	if (dir < 0) { if (ls == 0) return; target = ta_line_start(n, ls - 1); }
	else { int le = ta_line_end(n, n->caret); if (le >= n->tlen) return; target = le + 1; }
	int te = ta_line_end(n, target);
	int p = target + col; if (p > te) p = te;
	ta_move(n, p, extend);
}

void nwui_textarea_caret(nwui_node *n, int *line, int *col)
{
	int ln = 1;
	for (int i = 0; i < n->caret; i++) if (n->tbuf[i] == '\n') ln++;
	int ls = ta_line_start(n, n->caret);
	if (line) *line = ln;
	if (col) *col = n->caret - ls + 1;
}
```

In the textarea key block (Task 1), handle navigation BEFORE the insert fallthrough:

```c
	if (u->focus && u->focus->kind == NWUI_TEXTAREA) {
		nwui_node *n = u->focus;
		int shift = ev->mods & 1;
		switch (ev->code) {
		case NWUI_SC_LEFT:  ta_move(n, (shift || !has_sel(n)) ? n->caret - 1 : sel_lo(n), shift); break;
		case NWUI_SC_RIGHT: ta_move(n, (shift || !has_sel(n)) ? n->caret + 1 : sel_hi(n), shift); break;
		case NWUI_SC_HOME:  ta_move(n, ta_line_start(n, n->caret), shift); break;
		case NWUI_SC_END:   ta_move(n, ta_line_end(n, n->caret), shift); break;
		case NWUI_SC_UP:    ta_move_vert(n, -1, shift); break;
		case NWUI_SC_DOWN:  ta_move_vert(n, +1, shift); break;
		default:
			if (ev->ch == 8) {
				if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
				else if (n->caret > 0) ta_del_range(n, n->caret - 1, n->caret);
				n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
			} else if (ta_insert(n, ev->ch)) {
				n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
			}
			break;
		}
		break;
	}
```

Declare in `nwui.h`:

```c
void nwui_textarea_caret(nwui_node *n, int *line, int *col);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea caret navigation + line/col reporting"
```

---

## Task 3: Delete key, delete-selection, PgUp/PgDn, scroll-to-caret + scrollbar geometry

**Files:**
- Modify: `user/libnwui/nwui_core.c`
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: Task 2 helpers.
- Produces: Delete key (`NWUI_SC_DEL`) removes the selection or the char after the caret; PgUp/PgDn move by `ta_visible_rows(n)` lines; internal `ta_scroll_to_caret(n)` keeps the caret row within `[scroll, scroll+rows)`; `ta_visible_rows(n) = n->h / NW_FONT_H` (min 1). Scroll is in logical rows for now (wrap arrives in Task 4).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("textarea delete key and page motion scroll the view") {
	nwui *u = new nwui; nwui_init(u);
	char tb[128] = "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 200; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1; ta->caret = 0; ta->anchor = 0;
	ta->h = 4 * NW_FONT_H;                    // 4 visible rows

	keyc(u, NWUI_SC_DEL, 0, 0);               // deletes 'l' of l1
	CHECK(strncmp(tb, "1\n", 2) == 0);

	for (int i = 0; i < 6; i++) keyc(u, NWUI_SC_DOWN, 0, 0);  // caret onto a low line
	CHECK(ta->scroll > 0);                    // view scrolled to follow caret
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL (`NWUI_SC_DEL` not handled; `scroll` stays 0).

- [ ] **Step 3: Implement the minimal code**

```c
static int ta_visible_rows(const nwui_node *n) { int v = n->h / NW_FONT_H; return v < 1 ? 1 : v; }
static int ta_caret_row(const nwui_node *n)     /* logical row index of the caret */
{
	int r = 0; for (int i = 0; i < n->caret; i++) if (n->tbuf[i] == '\n') r++; return r;
}
static int ta_total_rows(const nwui_node *n)
{
	int r = 1; for (int i = 0; i < n->tlen; i++) if (n->tbuf[i] == '\n') r++; return r;
}
static void ta_scroll_to_caret(nwui_node *n)
{
	int row = ta_caret_row(n), vis = ta_visible_rows(n);
	if (row < n->scroll) n->scroll = row;
	else if (row >= n->scroll + vis) n->scroll = row - vis + 1;
	int maxs = ta_total_rows(n) - vis; if (maxs < 0) maxs = 0;
	if (n->scroll > maxs) n->scroll = maxs;
	if (n->scroll < 0) n->scroll = 0;
}
```

Call `ta_scroll_to_caret(n)` at the end of every motion/edit branch (add it after the
`switch` in the textarea key block). Add the Delete and Page cases to that switch:

```c
		case NWUI_SC_DEL:
			if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
			else if (n->caret < n->tlen) ta_del_range(n, n->caret, n->caret + 1);
			n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
			break;
		case NWUI_SC_PGUP: for (int k = 0; k < ta_visible_rows(n); k++) ta_move_vert(n, -1, shift); break;
		case NWUI_SC_PGDN: for (int k = 0; k < ta_visible_rows(n); k++) ta_move_vert(n, +1, shift); break;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea delete key, page motion, scroll-to-caret"
```

---

## Task 4: Word-wrap + `nwui_textarea_set_wrap`

**Files:**
- Modify: `user/libnwui/nwui_core.c` (wrap-aware row mapping)
- Modify: `user/libnwui/nwui.h` (`nwui_textarea_set_wrap` decl)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces: `void nwui_textarea_set_wrap(nwui_node *n, int on)`. When wrap is on, a logical line longer than `ta_cols(n) = (n->w - 2*pad) / NW_FONT_W` columns occupies multiple **visual** rows; `ta_total_rows`/`ta_caret_row`/`ta_scroll_to_caret` count visual rows. When off, behaviour is unchanged (one visual row per logical line). Pad constant `NWUI_TA_PAD = 4`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("textarea word-wrap multiplies visual rows") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "aaaaaaaaaa";               // 10 chars, no newline
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 200; u->win_h = 200; nwui_layout(u);
	ta->w = 5 * NW_FONT_W + 2 * 4;            // ~5 columns of text width
	nwui_textarea_set_wrap(ta, 1);
	CHECK(nwui_textarea_total_rows(ta) == 2); // 10 chars / 5 cols = 2 rows
	nwui_textarea_set_wrap(ta, 0);
	CHECK(nwui_textarea_total_rows(ta) == 1);
	delete u;
}
```

(Expose a tiny test accessor `int nwui_textarea_total_rows(nwui_node*)` returning visual rows — also useful to the app's scrollbar; declare it in `nwui.h`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — `nwui_textarea_set_wrap`/`nwui_textarea_total_rows` undefined.

- [ ] **Step 3: Implement the minimal code**

```c
enum { NWUI_TA_PAD = 4 };
static int ta_cols(const nwui_node *n)
{
	int c = (n->w - 2 * NWUI_TA_PAD) / NW_FONT_W; return c < 1 ? 1 : c;
}
/* visual rows a logical line [ls,le) occupies under the current wrap setting */
static int ta_line_rows(const nwui_node *n, int ls, int le)
{
	if (!n->wrap) return 1;
	int len = le - ls, cols = ta_cols(n);
	return len <= 0 ? 1 : (len + cols - 1) / cols;
}
```

Rewrite the row counters to be wrap-aware:

```c
static int ta_total_rows(const nwui_node *n)
{
	int rows = 0, ls = 0;
	for (;;) { int le = ta_line_end(n, ls); rows += ta_line_rows(n, ls, le);
		if (le >= n->tlen) break; ls = le + 1; }
	return rows;
}
static int ta_caret_row(const nwui_node *n)
{
	int rows = 0, ls = 0;
	for (;;) { int le = ta_line_end(n, ls);
		if (n->caret <= le) { rows += n->wrap ? (n->caret - ls) / ta_cols(n) : 0; break; }
		rows += ta_line_rows(n, ls, le); if (le >= n->tlen) break; ls = le + 1; }
	return rows;
}

void nwui_textarea_set_wrap(nwui_node *n, int on) { n->wrap = on ? 1 : 0; n->dirty = 1;
	if (n->owner) n->owner->layout_dirty = 1; }
int  nwui_textarea_total_rows(nwui_node *n) { return ta_total_rows(n); }
```

Declare both in `nwui.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea word-wrap with visual-row counting"
```

---

## Task 5: Click-to-place-caret + drag-select (pointer)

**Files:**
- Modify: `user/libnwui/nwui_core.c` (pointer handling for `NWUI_TEXTAREA`)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces: a left-press inside a textarea focuses it and sets the caret to the byte under the cursor (clearing selection); a left-drag extends the selection. Mapping: `ta_pos_at(n, px, py)` converts a pixel point to a byte offset using `scroll`, `ta_cols`, `NW_FONT_W/H`, honouring wrap.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("textarea click places caret, drag extends selection") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "abcd\nefgh";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	int bx = ta->x + 4, by = ta->y + 2;
	pointer(u, bx + 2 * NW_FONT_W, by, 1);    // press over col 2 of row 0
	pointer(u, bx + 2 * NW_FONT_W, by, 0);
	CHECK(ta->caret == 2); CHECK(ta->anchor == 2);
	pointer(u, bx + 1 * NW_FONT_W, by, 1);    // press col 1
	pointer(u, bx + 3 * NW_FONT_W, by, 1);    // drag to col 3 (still pressed)
	CHECK(ta->anchor == 1); CHECK(ta->caret == 3);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — textarea ignores pointer, caret unchanged.

- [ ] **Step 3: Implement the minimal code**

```c
/* byte offset at a pixel point inside the textarea (wrap-aware, uses scroll) */
static int ta_pos_at(const nwui_node *n, int px, int py)
{
	int row = n->scroll + (py - (n->y + NWUI_TA_PAD)) / NW_FONT_H;
	if (row < 0) row = 0;
	int col = (px - (n->x + NWUI_TA_PAD) + NW_FONT_W / 2) / NW_FONT_W;
	if (col < 0) col = 0;
	int ls = 0, rr = 0;                       /* walk visual rows to the target row */
	for (;;) {
		int le = ta_line_end(n, ls), lr = ta_line_rows(n, ls, le);
		if (rr + lr > row) {                  /* target is within this logical line */
			int within = row - rr;
			int start = ls + (n->wrap ? within * ta_cols(n) : 0);
			int rowlen = n->wrap ? ta_cols(n) : (le - ls);
			int p = start + col; if (p > start + rowlen) p = start + rowlen;
			if (p > le) p = le;
			return p;
		}
		rr += lr; if (le >= n->tlen) return n->tlen; ls = le + 1;
	}
}
```

In `nwui_dispatch`'s `NW_EV_POINTER` block, add textarea cases mirroring the textfield ones
(left press → focus + `ta_pos_at` set caret/anchor; armed drag → caret = `ta_pos_at`):

```c
	else if (over && over->kind == NWUI_TEXTAREA) {
		set_focus(u, over);
		int p = ta_pos_at(over, ev->x, ev->y);
		over->caret = p; over->anchor = p; over->dirty = 1;
	}
```

and in the drag branch:

```c
	} else if (left && pleft && u->armed && u->armed->kind == NWUI_TEXTAREA) {
		u->armed->caret = ta_pos_at(u->armed, ev->x, ev->y); u->armed->dirty = 1;
	}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea mouse caret placement + drag-select"
```

---

## Task 6: Editor helpers — find, goto-line, select-all

**Files:**
- Modify: `user/libnwui/nwui_core.c`
- Modify: `user/libnwui/nwui.h`
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces:
  - `int nwui_textarea_find(nwui_node *n, const char *needle, int matchcase, int wrap_around)` — searches from `caret`; on hit, selects the match (anchor=start, caret=end), scrolls to it, returns 1; else returns 0.
  - `void nwui_textarea_goto_line(nwui_node *n, int line1)` — moves caret to the start of 1-based line `line1` (clamped), scrolls to it.
  - `void nwui_textarea_select_all(nwui_node *n)` — anchor=0, caret=tlen.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("textarea find, goto-line, select-all") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "foo\nBar\nbar baz";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	ta->caret = 0; ta->anchor = 0;

	CHECK(nwui_textarea_find(ta, "bar", 0, 0) == 1);   // case-insensitive: hits "Bar"
	CHECK(ta->anchor == 4); CHECK(ta->caret == 7);
	CHECK(nwui_textarea_find(ta, "bar", 1, 0) == 1);   // case-sensitive from caret: "bar baz"
	CHECK(ta->anchor == 8);
	CHECK(nwui_textarea_find(ta, "zzz", 0, 0) == 0);

	nwui_textarea_goto_line(ta, 2); CHECK(ta->caret == 4);
	nwui_textarea_select_all(ta); CHECK(ta->anchor == 0); CHECK(ta->caret == ta->tlen);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — helpers undefined.

- [ ] **Step 3: Implement the minimal code**

```c
static int ci_eq(char a, char b, int mc)
{
	if (mc) return a == b;
	if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32;
	return a == b;
}
int nwui_textarea_find(nwui_node *n, const char *needle, int matchcase, int wrap_around)
{
	int m = (int) strlen(needle); if (m == 0) return 0;
	for (int pass = 0; pass < (wrap_around ? 2 : 1); pass++) {
		int from = pass == 0 ? n->caret : 0;
		int to   = pass == 0 ? n->tlen  : n->caret;
		for (int i = from; i + m <= to; i++) {
			int k = 0; while (k < m && ci_eq(n->tbuf[i + k], needle[k], matchcase)) k++;
			if (k == m) { n->anchor = i; n->caret = i + m; ta_scroll_to_caret(n); n->dirty = 1; return 1; }
		}
	}
	return 0;
}
void nwui_textarea_goto_line(nwui_node *n, int line1)
{
	int p = 0, ln = 1;
	while (p < n->tlen && ln < line1) { if (n->tbuf[p] == '\n') ln++; p++; }
	n->caret = p; n->anchor = p; ta_scroll_to_caret(n); n->dirty = 1;
}
void nwui_textarea_select_all(nwui_node *n) { n->anchor = 0; n->caret = n->tlen; n->dirty = 1; }
```

Declare all three in `nwui.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea find / goto-line / select-all helpers"
```

---

## Task 7: Clipboard for the textarea (copy/cut/paste)

**Files:**
- Modify: `user/libnwui/nwui_core.c` (`NW_EV_COPY`/`NW_EV_PASTE` for textarea)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: Task 1 helpers, existing `clip_buf/clip_len/clip_set/clip_get`.
- Produces: when a textarea is focused, `NW_EV_COPY` copies the selection (or whole buffer) to `clip_buf` and sets `clip_set` (cut also deletes); `NW_EV_PASTE` inserts `ev->text` at the caret. A reusable internal `ta_copy(u, n)`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("textarea copy/cut/paste via clipboard events") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "hello";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1; ta->anchor = 0; ta->caret = 3;   // "hel" selected

	nw_event c; memset(&c, 0, sizeof c); c.type = NW_EV_COPY; c.cut = 1;
	nwui_dispatch(u, &c);
	CHECK(u->clip_set == 1); CHECK(u->clip_len == 3);
	CHECK(strncmp(u->clip_buf, "hel", 3) == 0);
	CHECK(strcmp(tb, "lo") == 0);                                     // cut removed "hel"

	nw_event p; memset(&p, 0, sizeof p); p.type = NW_EV_PASTE;
	p.text = "XY"; p.text_len = 2; ta->caret = 0; ta->anchor = 0;
	nwui_dispatch(u, &p);
	CHECK(strcmp(tb, "XYlo") == 0);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — copy/paste only handle textfield.

- [ ] **Step 3: Implement the minimal code**

```c
static void ta_copy(nwui *u, nwui_node *n)
{
	int lo = has_sel(n) ? sel_lo(n) : 0, hi = has_sel(n) ? sel_hi(n) : n->tlen;
	int k = hi - lo; if (k > (int) sizeof u->clip_buf) k = (int) sizeof u->clip_buf;
	for (int i = 0; i < k; i++) u->clip_buf[i] = n->tbuf[lo + i];
	u->clip_len = k; u->clip_set = 1;
}
```

In `NW_EV_COPY`, add a textarea branch alongside the textfield one:

```c
	else if (u->focus && u->focus->kind == NWUI_TEXTAREA) {
		ta_copy(u, u->focus);
		if (ev->cut && has_sel(u->focus)) {
			ta_del_range(u->focus, sel_lo(u->focus), sel_hi(u->focus));
			u->focus->dirty = 1; if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user);
		}
	}
```

In `NW_EV_PASTE`, add:

```c
	else if (u->focus && u->focus->kind == NWUI_TEXTAREA && ev->text) {
		int changed = 0;
		for (int i = 0; i < ev->text_len; i++) changed |= ta_insert(u->focus, ev->text[i]);
		if (changed) { u->focus->dirty = 1; if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user); }
	}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c tests/test_nwui_core.cpp
git commit -m "feat(nwui): textarea clipboard copy/cut/paste"
```

---

## Task 8: Keyboard accelerators (Ctrl + function keys)

**Files:**
- Modify: `user/libnwui/nwui_core.h` (accel table fields, `ctrl_down`)
- Modify: `user/libnwui/nwui_core.c` (Ctrl tracking, dispatch, suppression)
- Modify: `user/libnwui/nwui.h` (`nwui_accel` decl)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: Tasks 1–7.
- Produces: `void nwui_accel(nwui *u, int ctrl, char key, int fkey, nwui_cb cb, void *user)` — register an accelerator. `ctrl` (0/1) + lowercase `key` letter, OR a function-key scancode `fkey` (e.g. `NWUI_SC_F3`) with `key==0`. On a matching `NW_EV_KEY` down it fires `cb` and consumes the key. The core tracks `ctrl_down` from `NWUI_SC_CTRL`/`NWUI_SC_RCTRL`. Printable insertion is suppressed while `ctrl_down`.

- [ ] **Step 1: Write the failing test**

```cpp
static int g_accel_hits;
static void on_accel(nwui_node *, void *u) { (*(int *) u)++; }

TEST_CASE("ctrl and function-key accelerators fire and suppress typing") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1;
	g_accel_hits = 0;
	nwui_accel(u, 1, 's', 0, on_accel, &g_accel_hits);     // Ctrl+S
	nwui_accel(u, 0, 0, NWUI_SC_F3, on_accel, &g_accel_hits); // F3

	keyc(u, NWUI_SC_CTRL, 0, 0);              // Ctrl down
	keyc(u, 0x1F, 's', 0);                    // 's' while Ctrl held
	CHECK(g_accel_hits == 1);
	CHECK(strcmp(tb, "") == 0);               // 's' was NOT inserted
	keyc(u, NWUI_SC_F3, 0, 0);                // F3
	CHECK(g_accel_hits == 2);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — `nwui_accel` undefined.

- [ ] **Step 3: Implement the minimal code**

In `nwui_core.h`, add to `struct nwui`:

```c
	int ctrl_down;
	struct { int ctrl, fkey; char key; nwui_cb cb; void *user; } accel[24];
	int naccel;
```

In `nwui_core.c`:

```c
void nwui_accel(nwui *u, int ctrl, char key, int fkey, nwui_cb cb, void *user)
{
	if (u->naccel >= 24) return;
	int i = u->naccel++;
	u->accel[i].ctrl = ctrl ? 1 : 0;
	u->accel[i].key  = (key >= 'A' && key <= 'Z') ? key + 32 : key;
	u->accel[i].fkey = fkey; u->accel[i].cb = cb; u->accel[i].user = user;
}
static int accel_fire(nwui *u, const struct nw_event *ev)
{
	char ch = ev->ch; if (ch >= 'A' && ch <= 'Z') ch += 32;
	for (int i = 0; i < u->naccel; i++) {
		struct { int ctrl, fkey; char key; nwui_cb cb; void *user; } *a = &u->accel[i];
		int hit = a->fkey ? (ev->code == a->fkey)
		                  : (a->ctrl == u->ctrl_down && a->key && a->key == ch);
		if (hit && a->cb) { a->cb(0, a->user); return 1; }
	}
	return 0;
}
```

At the very top of the `NW_EV_KEY` case (before the menu/list/textarea blocks):

```c
	case NW_EV_KEY: {
		if (ev->code == NWUI_SC_CTRL || ev->code == NWUI_SC_RCTRL) { u->ctrl_down = ev->down; break; }
		if (!ev->down) break;
		if (accel_fire(u, ev)) break;          /* shortcut consumed the key */
		if (u->ctrl_down) break;               /* suppress Ctrl+<letter> from inserting */
		/* ...existing menu_open / list / textarea / textfield handling... */
```

Declare in `nwui.h`:

```c
void nwui_accel(nwui *u, int ctrl, char key, int fkey, nwui_cb cb, void *user);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.h user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): keyboard accelerator table (Ctrl + function keys)"
```

---

## Task 9: `nwui_checkbox` widget

**Files:**
- Modify: `user/libnwui/nwui_core.c` (constructor, measure, toggle on click/space)
- Modify: `user/libnwui/nwui.h` (decl)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: dispatch infrastructure.
- Produces: `nwui_node *nwui_checkbox(nwui *u, const char *label, int *value, nwui_cb on_change, void *user)`. The label goes in `n->text`, the bound flag in `n->tbuf`-adjacent `int *` stored in a new field `int *vbool;`. Click or Space toggles `*value` and fires `on_change`. Measured width = box(16) + gap + label.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("checkbox toggles its bound value on click") {
	nwui *u = new nwui; nwui_init(u);
	int v = 0;
	nwui_node *cb = nwui_checkbox(u, "Wrap", &v, 0, 0);
	nwui_set_root(u, cb); u->win_w = 200; u->win_h = 60; nwui_layout(u);
	click(u, cb->x + 4, cb->y + 4);
	CHECK(v == 1);
	click(u, cb->x + 4, cb->y + 4);
	CHECK(v == 0);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — `nwui_checkbox` undefined.

- [ ] **Step 3: Implement the minimal code**

Add `int *vbool;` to `struct nwui_node` (in `nwui_core.h`). Then:

```c
nwui_node *nwui_checkbox(nwui *u, const char *label, int *value, nwui_cb on_change, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_CHECKBOX);
	set_caption(n, label); n->vbool = value;
	n->on_click = on_change; n->user = user; n->focusable = 1;
	return n;
}
```

In `nwui_measure`:

```c
	case NWUI_CHECKBOX:
		n->mw = 16 + 6 + (int) strlen(n->text) * NW_FONT_W; n->mh = NW_FONT_H + 4;
		break;
```

Toggle helper + wire it into pointer release and Space key:

```c
static void cb_toggle(nwui_node *n)
{
	if (n->vbool) *n->vbool = !*n->vbool;
	n->dirty = 1; if (n->on_click) n->on_click(n, n->user);
}
```

In `NW_EV_POINTER` left-press, treat a checkbox like a button (arm it); on release, if
`over == armed` and it's a checkbox, call `cb_toggle`. In `NW_EV_KEY`, if the focused node
is a checkbox and `ev->ch == ' '`, call `cb_toggle`.

Declare in `nwui.h`:

```c
nwui_node *nwui_checkbox(nwui *u, const char *label, int *value, nwui_cb on_change, void *user);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.h user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): checkbox widget"
```

---

## Task 10: Modal overlay infrastructure

**Files:**
- Modify: `user/libnwui/nwui_core.h` (`modal*` fields)
- Modify: `user/libnwui/nwui_core.c` (open/close, centered layout, input routing)
- Modify: `user/libnwui/nwui.h` (decls)
- Test: `tests/test_nwui_core.cpp`

**Interfaces:**
- Consumes: layout (`nwui_measure`/`nwui_arrange`), hit-test, focus.
- Produces:
  - `void nwui_open_modal(nwui *u, nwui_node *subtree, nwui_cb on_close, void *user)` — show `subtree` centered; route all input to it; focus its first focusable.
  - `void nwui_close_modal(nwui *u)` — hide it, restore focus, fire `on_close`.
  - `int nwui_modal_open(const nwui *u)`.
  - While open, `nwui_dispatch` hit-tests/routes only within `u->modal`; `nwui_layout` measures `u->modal` and arranges it centered over the window.

- [ ] **Step 1: Write the failing test**

```cpp
static int g_modal_closed;
static void on_modal_close(nwui_node *, void *u) { (*(int *) u)++; }

TEST_CASE("modal captures input and routes to its subtree") {
	nwui *u = new nwui; nwui_init(u);
	int bg_clicks = 0;
	nwui_node *bg_btn = nwui_button(u, "BG", on_click, &bg_clicks);
	nwui_set_root(u, bg_btn);
	u->win_w = 400; u->win_h = 300; nwui_layout(u);

	int ok_clicks = 0;
	nwui_node *ok = nwui_button(u, "OK", on_click, &ok_clicks);
	nwui_node *dlg = nwui_pad(nwui_column(u, ok, (nwui_node *) 0), 10);
	g_modal_closed = 0;
	nwui_open_modal(u, dlg, on_modal_close, &g_modal_closed);
	nwui_layout(u);
	CHECK(nwui_modal_open(u) == 1);

	// a click landing on the background's old position must NOT reach it while modal
	click(u, bg_btn->x + 2, bg_btn->y + 2);
	CHECK(bg_clicks == 0);
	// clicking OK inside the modal works
	click(u, ok->x + 2, ok->y + 2);
	CHECK(ok_clicks == 1);

	nwui_close_modal(u);
	CHECK(nwui_modal_open(u) == 0);
	CHECK(g_modal_closed == 1);
	delete u;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — modal API undefined.

- [ ] **Step 3: Implement the minimal code**

In `nwui_core.h` `struct nwui`:

```c
	nwui_node *modal;            /* modal subtree root, or NULL */
	nwui_node *saved_focus;      /* focus to restore on close */
	nwui_cb    modal_close_cb; void *modal_close_user;
```

In `nwui_core.c`:

```c
static nwui_node *first_focusable(nwui_node *n)
{
	if (n->focusable) return n;
	for (int i = 0; i < n->nchild; i++) { nwui_node *r = first_focusable(n->child[i]); if (r) return r; }
	return 0;
}
void nwui_open_modal(nwui *u, nwui_node *subtree, nwui_cb on_close, void *user)
{
	u->modal = subtree; u->saved_focus = u->focus;
	u->modal_close_cb = on_close; u->modal_close_user = user;
	u->focus = first_focusable(subtree);
	if (u->focus) u->focus->focused = 1;
	u->layout_dirty = 1;
}
void nwui_close_modal(nwui *u)
{
	nwui_node *sub = u->modal; nwui_cb cb = u->modal_close_cb; void *usr = u->modal_close_user;
	u->modal = 0; u->focus = u->saved_focus; u->saved_focus = 0;
	u->modal_close_cb = 0; u->layout_dirty = 1;
	(void) sub; if (cb) cb(0, usr);
}
int nwui_modal_open(const nwui *u) { return u->modal != 0; }
```

In `nwui_layout`, after arranging the root, lay out the modal centered:

```c
	if (u->modal) {
		nwui_measure(u->modal);
		int mw = u->modal->mw, mh = u->modal->mh;
		int mx = (u->win_w - mw) / 2, my = (u->win_h - mh) / 3;   /* upper-third, XP-ish */
		if (mx < 0) mx = 0; if (my < 0) my = 0;
		nwui_arrange(u->modal, mx, my, mw, mh);
		mark_all_dirty(u->modal);
	}
```

In `nwui_dispatch`, at the very top of `NW_EV_POINTER` and `NW_EV_KEY`, when `u->modal` is
set, route to the modal subtree instead of `u->root`. The simplest correct change: where the
code computes `nwui_hit(u->root, ...)`, use `u->modal ? u->modal : u->root`; and the
list/textfield/textarea key handlers already act on `u->focus`, which now points inside the
modal. Guard the background: in the pointer block, if `u->modal` and the hit is NULL (outside
the modal), ignore the event (don't fall through to background widgets).

Declare the three functions in `nwui.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.h user/libnwui/nwui_core.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): modal overlay infrastructure (centered, input-capturing)"
```

---

## Task 11: Convenience dialogs — `nwui_message` and `nwui_prompt`

**Files:**
- Modify: `user/libnwui/nwui.c` (builders that assemble subtrees and call `nwui_open_modal`)
- Modify: `user/libnwui/nwui.h` (decls)
- Test: `tests/test_nwui_core.cpp` (assemble + verify the resulting tree via core)

**Interfaces:**
- Consumes: Task 10 modal API, `nwui_label`/`nwui_button`/`nwui_textfield`/`nwui_column`.
- Produces:
  - `void nwui_message(nwui *u, const char *title, const char *text)` — info modal: title label, body label, an OK button that closes the modal.
  - `void nwui_prompt(nwui *u, const char *title, char *buf, int cap, nwui_cb on_ok, void *user)` — title label, a `nwui_textfield` over `buf`, OK + Cancel. OK fires `on_ok` then closes; Cancel closes.

These live in `nwui.c` (not the gated core) because they only compose existing widgets; the
test exercises them through the public API and asserts the modal opens/closes.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("nwui_message opens a modal that OK closes") {
	nwui *u = new nwui; nwui_init(u);
	nwui_node *root = nwui_label(u, "main"); nwui_set_root(u, root);
	u->win_w = 400; u->win_h = 300; nwui_layout(u);
	nwui_message(u, "About", "NanOS Notepad");
	CHECK(nwui_modal_open(u) == 1);
	// find the OK button: it is the deepest focusable button in the modal; click its center
	nwui_node *ok = u->modal->child[u->modal->nchild - 1];
	nwui_layout(u);
	click(u, ok->x + ok->w / 2, ok->y + ok->h / 2);
	CHECK(nwui_modal_open(u) == 0);
	delete u;
}
```

(If `nwui_message`'s OK is nested, adjust the locator to descend to the button; keep the
structure simple — OK as the last child of the modal column.)

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — `nwui_message` undefined.

- [ ] **Step 3: Implement the minimal code**

In `nwui.c`:

```c
static void dlg_close(nwui_node *self, void *u) { (void) self; nwui_close_modal((nwui *) u); }

void nwui_message(nwui *u, const char *title, const char *text)
{
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 14), 10);
	nwui_add(col, nwui_colors(nwui_label(u, title), 0x172130, 0));
	nwui_add(col, nwui_label(u, text));
	nwui_add(col, nwui_button(u, "OK", dlg_close, u));
	nwui_colors(col, 0, 0x00ffffff);
	nwui_open_modal(u, col, 0, 0);
}

struct nwui_prompt_ctx { nwui *u; nwui_cb on_ok; void *user; };   /* file-scope, reused */
static struct nwui_prompt_ctx g_prompt;       /* one modal at a time */
static void prompt_ok(nwui_node *self, void *unused)
{
	(void) self; (void) unused;
	nwui *u = g_prompt.u; nwui_cb cb = g_prompt.on_ok; void *usr = g_prompt.user;
	nwui_close_modal(u); if (cb) cb(0, usr);
}
void nwui_prompt(nwui *u, const char *title, char *buf, int cap, nwui_cb on_ok, void *user)
{
	g_prompt.u = u; g_prompt.on_ok = on_ok; g_prompt.user = user;
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 14), 10);
	nwui_add(col, nwui_colors(nwui_label(u, title), 0x172130, 0));
	nwui_add(col, nwui_textfield(u, buf, cap, 0, 0));
	nwui_node *btns = nwui_gap(nwui_hbox(u), 8);
	nwui_add(btns, nwui_button(u, "OK", prompt_ok, 0));
	nwui_add(btns, nwui_button(u, "Cancel", dlg_close, u));
	nwui_add(col, btns);
	nwui_colors(col, 0, 0x00ffffff);
	nwui_open_modal(u, col, 0, 0);
}
```

Declare both in `nwui.h`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): message + prompt convenience dialogs"
```

---

## Task 12: File dialog — pure path helpers + assembly

**Files:**
- Modify: `user/libnwui/nwui_core.c` (pure: `nwui_path_join`, `nwui_path_up`)
- Modify: `user/libnwui/nwui.c` (the dialog: getdents listing + assembly)
- Modify: `user/libnwui/nwui.h` (decls)
- Test: `tests/test_nwui_core.cpp` (the pure helpers)

**Interfaces:**
- Consumes: Tasks 10–11, `nwui_list`/`nwui_list_set`/`nwui_textfield`.
- Produces:
  - Pure: `void nwui_path_join(const char *dir, const char *name, char *out, int cap)` — joins with a single `/`; `void nwui_path_up(char *path)` — strips the last component (stays at `/`).
  - I/O: `void nwui_file_dialog(nwui *u, int save, const char *start_dir, char *out_path, int cap, nwui_cb on_ok, void *user)` — modal with a directory `nwui_list` (entries read via `getdents64`), an editable path `nwui_textfield` bound to `out_path`, and Open/Save + Cancel. Selecting a dir re-lists; OK fires `on_ok` with `out_path` set; Cancel closes.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("path join and path-up") {
	char out[64];
	nwui_path_join("/disks/main", "notes.txt", out, sizeof out);
	CHECK(strcmp(out, "/disks/main/notes.txt") == 0);
	nwui_path_join("/", "a", out, sizeof out);
	CHECK(strcmp(out, "/a") == 0);
	char p[64]; strcpy(p, "/disks/main/sub"); nwui_path_up(p);
	CHECK(strcmp(p, "/disks/main") == 0);
	strcpy(p, "/"); nwui_path_up(p);
	CHECK(strcmp(p, "/") == 0);
	delete (int *) 0; (void) 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | head -30`
Expected: FAIL — helpers undefined.

- [ ] **Step 3: Implement the minimal code**

Pure helpers in `nwui_core.c`:

```c
void nwui_path_join(const char *dir, const char *name, char *out, int cap)
{
	int n = 0;
	for (const char *p = dir; *p && n < cap - 1; p++) out[n++] = *p;
	if (n > 0 && out[n - 1] != '/' && n < cap - 1) out[n++] = '/';
	for (const char *p = name; *p && n < cap - 1; p++) out[n++] = *p;
	out[n] = 0;
}
void nwui_path_up(char *path)
{
	int n = (int) strlen(path);
	while (n > 1 && path[n - 1] == '/') n--;          /* drop trailing slash */
	while (n > 1 && path[n - 1] != '/') n--;          /* drop last component */
	while (n > 1 && path[n - 1] == '/') n--;          /* drop the slash */
	path[n] = 0; if (n == 0) { path[0] = '/'; path[1] = 0; }
}
```

The dialog in `nwui.c` (I/O — verified in QEMU, not host-tested). It keeps a small
file-scope state (current dir, name list buffers), reads the directory with `getdents64`
via libc `opendir/readdir`, fills a `nwui_list`, and on activation either descends (dir) or
sets `out_path` and waits for OK. Use `nwui_open_modal`. Declare:

```c
void nwui_path_join(const char *dir, const char *name, char *out, int cap);
void nwui_path_up(char *path);
void nwui_file_dialog(nwui *u, int save, const char *start_dir,
                      char *out_path, int cap, nwui_cb on_ok, void *user);
```

Implementation sketch for `nwui_file_dialog` (full code; uses `<dirent.h>`):

```c
#include <dirent.h>
#include <string.h>
#define FD_MAX 128
static struct {
	nwui *u; int save; char dir[256]; char (*names)[64]; int n;
	char *namep[FD_MAX]; char namebuf[FD_MAX][64];
	nwui_node *list, *field; char *out; int cap; nwui_cb on_ok; void *user;
} g_fd;

static void fd_relist(void)
{
	g_fd.n = 0;
	if (g_fd.dir[1]) { strcpy(g_fd.namebuf[g_fd.n], ".."); g_fd.namep[g_fd.n] = g_fd.namebuf[g_fd.n]; g_fd.n++; }
	DIR *d = opendir(g_fd.dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) && g_fd.n < FD_MAX) {
			if (e->d_name[0] == '.' && e->d_name[1] == 0) continue;
			int i = g_fd.n++;
			int k = 0; for (; e->d_name[k] && k < 63; k++) g_fd.namebuf[i][k] = e->d_name[k];
			g_fd.namebuf[i][k] = 0; g_fd.namep[i] = g_fd.namebuf[i];
		}
		closedir(d);
	}
	nwui_list_set(g_fd.list, (const char *const *) g_fd.namep, g_fd.n);
}
static void fd_activate(nwui_node *self, void *unused)   /* double-click / Enter on a row */
{
	(void) self; (void) unused;
	int sel = nwui_list_selected(g_fd.list); if (sel < 0) return;
	const char *nm = g_fd.namep[sel];
	if (strcmp(nm, "..") == 0) { nwui_path_up(g_fd.dir); fd_relist(); return; }
	char joined[256]; nwui_path_join(g_fd.dir, nm, joined, sizeof joined);
	struct stat st;
	if (stat(joined, &st) == 0 && (st.st_mode & S_IFDIR)) { strcpy(g_fd.dir, joined); fd_relist(); }
	else { int k=0; for(; joined[k] && k<g_fd.cap-1; k++) g_fd.out[k]=joined[k]; g_fd.out[k]=0;
		nwui_set_text(g_fd.field, g_fd.out); }
}
static void fd_ok(nwui_node *self, void *unused)
{
	(void) self; (void) unused;
	nwui *u = g_fd.u; nwui_cb cb = g_fd.on_ok; void *usr = g_fd.user;
	nwui_close_modal(u); if (cb) cb(0, usr);
}
void nwui_file_dialog(nwui *u, int save, const char *start_dir,
                      char *out_path, int cap, nwui_cb on_ok, void *user)
{
	g_fd.u = u; g_fd.save = save; g_fd.out = out_path; g_fd.cap = cap;
	g_fd.on_ok = on_ok; g_fd.user = user;
	int k = 0; for (; start_dir[k] && k < 255; k++) g_fd.dir[k] = start_dir[k]; g_fd.dir[k] = 0;
	g_fd.list  = nwui_list(u, fd_activate, 0);
	g_fd.field = nwui_textfield(u, out_path, cap, 0, 0);
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 12), 8);
	nwui_add(col, nwui_colors(nwui_label(u, save ? "Save As" : "Open"), 0x172130, 0));
	nwui_add(col, nwui_flex(nwui_size(g_fd.list, 280, 160), 1));
	nwui_add(col, g_fd.field);
	nwui_node *btns = nwui_gap(nwui_hbox(u), 8);
	nwui_add(btns, nwui_button(u, save ? "Save" : "Open", fd_ok, 0));
	nwui_add(btns, nwui_button(u, "Cancel", dlg_close, u));
	nwui_add(col, btns);
	nwui_colors(col, 0, 0x00ffffff);
	fd_relist();
	nwui_open_modal(u, col, 0, 0);
}
```

(`dlg_close` from Task 11 is reused — keep both dialogs in the same `nwui.c`. Include
`<sys/stat.h>` for `stat`.)

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: PASS (the pure helpers; the dialog itself is QEMU-verified in Task 16).

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui_core.c user/libnwui/nwui.c user/libnwui/nwui.h tests/test_nwui_core.cpp
git commit -m "feat(nwui): file open/save dialog + path helpers"
```

---

## Task 13: Paint the textarea

**Files:**
- Modify: `user/libnwui/nwui_paint.c`

**Interfaces:**
- Consumes: textarea node fields, wrap-aware row walking (replicate the row-walk locally in
  the painter, or expose `ta_cols`/`ta_line_rows` via a small painter-private copy — keep
  painter self-contained as the file already is).
- Produces: a `NWUI_TEXTAREA` case in `paint_self` drawing: white paper + border (focus
  ring like the textfield), the visible visual rows starting at `scroll`, selection
  highlight, a caret bar when focused, and a vertical scrollbar thumb when content overflows.

- [ ] **Step 1: Implement the paint case**

Add to `paint_self` in `nwui_paint.c` (mirrors the textfield/list styling; no host test —
this is exercised in QEMU at Task 16):

```c
	case NWUI_TEXTAREA: {
		nw_fill_round(s, n->x, n->y, n->w, n->h, 6, COL_TF_BG, 255);
		int pad = 4, cols = (n->w - 2*pad)/NW_FONT_W; if (cols < 1) cols = 1;
		int vis = n->h / NW_FONT_H; if (vis < 1) vis = 1;
		int lo = n->anchor < n->caret ? n->anchor : n->caret;
		int hi = n->anchor > n->caret ? n->anchor : n->caret;
		int tx0 = n->x + pad, ty = n->y + pad;
		/* walk visual rows; skip the first n->scroll of them */
		int ls = 0, vrow = 0, drawn = 0, caret_px = -1, caret_py = -1;
		for (; drawn < vis; ) {
			int le = n->tbuf ? 0 : 0; le = n->y; /* placeholder removed below */
			break;
		}
		/* --- real row walk --- */
		ls = 0; vrow = 0; drawn = 0;
		while (n->tbuf) {
			int lend = ls; while (lend < n->tlen && n->tbuf[lend] != '\n') lend++;
			int seg = ls;
			do {
				int segend = n->wrap ? (seg + cols < lend ? seg + cols : lend) : lend;
				if (vrow >= n->scroll && drawn < vis) {
					int yy = ty + drawn * NW_FONT_H;
					for (int i = seg; i < segend; i++) {
						int seld = (n->anchor != n->caret && i >= lo && i < hi);
						int xx = tx0 + (i - seg) * NW_FONT_W;
						if (seld) nw_fill_rect(s, xx, yy, NW_FONT_W, NW_FONT_H, COL_SEL);
						nw_text(s, xx, yy, (char[]){ n->tbuf[i], 0 }, seld ? 0x00ffffff : COL_INK);
					}
					if (n->focused && n->caret >= seg && n->caret <= segend) {
						caret_px = tx0 + (n->caret - seg) * NW_FONT_W; caret_py = yy;
					}
					drawn++;
				}
				vrow++;
				seg = segend;
			} while (n->wrap && seg < lend);
			if (lend >= n->tlen) break; ls = lend + 1;
		}
		if (caret_px >= 0) nw_fill_rect(s, caret_px, caret_py, 2, NW_FONT_H, COL_TF_FOC);
		nw_stroke_round(s, n->x, n->y, n->w, n->h, 6, n->focused ? COL_TF_FOC : COL_TF_BRD,
		                n->focused ? 255 : 200);
		break;
	}
```

(Delete the placeholder `for`/`break` stub — it's only shown to flag that the *real* row
walk follows; the implementer writes just the real walk.)

- [ ] **Step 2: Build to verify it compiles**

Run: `make build 2>&1 | tail -15`
Expected: kernel + userland compile clean (no QEMU yet).

- [ ] **Step 3: Commit**

```bash
git add user/libnwui/nwui_paint.c
git commit -m "feat(nwui): paint the textarea (rows, selection, caret, scrollbar)"
```

---

## Task 14: Paint the checkbox + modal backdrop + dialogs

**Files:**
- Modify: `user/libnwui/nwui_paint.c`

**Interfaces:**
- Consumes: Task 9 (`NWUI_CHECKBOX`, `vbool`), Task 10 (`u->modal`).
- Produces: a `NWUI_CHECKBOX` case (a 14×14 box with a check when `*vbool`, then the label);
  and modal rendering — in `nwui_render`, after `paint_all(root)`/`draw_menu`, when
  `u->modal` is set, dim the whole window then `paint_all(u->modal)`.

- [ ] **Step 1: Implement the paint additions**

Checkbox case in `paint_self`:

```c
	case NWUI_CHECKBOX: {
		int bs = 14, by = n->y + (n->h - bs) / 2;
		nw_fill_round(s, n->x, by, bs, bs, 3, COL_TF_BG, 255);
		nw_stroke_round(s, n->x, by, bs, bs, 3, n->focused ? COL_TF_FOC : COL_TF_BRD, 255);
		if (n->vbool && *n->vbool) {            /* a simple check: two strokes */
			nw_fill_rect(s, n->x + 3, by + 6, 3, 3, COL_SEL);
			nw_fill_rect(s, n->x + 6, by + 3, 3, 6, COL_SEL);
		}
		nw_text(s, n->x + bs + 6, n->y + (n->h - NW_FONT_H) / 2, n->text, COL_INK);
		break;
	}
```

Modal rendering in `nwui_render`, full-repaint branch (after `draw_menu(u, s)`):

```c
		if (u->modal) {
			nw_blend_rect(s, 0, 0, u->win_w, u->win_h, 0x00000000, 90);   /* dim backdrop */
			nw_fill_round(s, u->modal->x - 8, u->modal->y - 8,
			              u->modal->w + 16, u->modal->h + 16, 10, 0x00f4f8fd, 255);
			nw_stroke_round(s, u->modal->x - 8, u->modal->y - 8,
			                u->modal->w + 16, u->modal->h + 16, 10, 0x00b8c6d8, 220);
			paint_all(u->modal, s);
		}
```

Because the modal toggles `layout_dirty` on open/close (Task 10), the full-repaint branch
runs whenever the modal appears/disappears — no damage-rect bookkeeping needed.

- [ ] **Step 2: Build to verify it compiles**

Run: `make build 2>&1 | tail -15`
Expected: clean compile.

- [ ] **Step 3: Commit**

```bash
git add user/libnwui/nwui_paint.c
git commit -m "feat(nwui): paint checkbox + modal backdrop/dialog"
```

---

## Task 15: Rewrite `nwnote.c` as the Notepad; relink against `libnwui`

**Files:**
- Rewrite: `user/nwnote/nwnote.c`
- Modify: `Makefile` (the `nwnote.nxe` recipe, ~lines 1779–1781)

**Interfaces:**
- Consumes: all of `libnwui` (textarea, checkbox, accelerators, modal, message/prompt/file
  dialog, menus).
- Produces: `/disks/main/apps/nwnote/nwnote.nxe` — the Notepad.

- [ ] **Step 1: Change the Makefile recipe**

Replace the libnw-direct `nwnote.nxe` recipe with the libnwui chain (copy `nwform`'s
recipe, lines ~1784–1786):

```make
$(BINFOLDER)nwnote.nxe: $(DYN_GLUE) $(BINFOLDER)nwnote.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)nwnote.elf $(DYN_GLUE) $(BINFOLDER)nwnote.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)nwnote.elf $@ --need libnwui.ndl
```

- [ ] **Step 2: Rewrite `user/nwnote/nwnote.c`**

Full file — the thin client. Global menus + accelerators share callbacks; status bar
updates via the textarea `on_change`; Find/Replace built from the modal + primitives; file
I/O via libc; single-level undo via a snapshot buffer.

```c
/*
 * nwnote.c — NanOS Notepad: a Windows XP-style text editor on libnwui. The editor itself
 * is the reusable nwui_textarea; this file wires menus, accelerators, the status bar,
 * find/replace/go-to dialogs, file open/save, and single-level undo.
 */
#include "nwui.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define CAP 65536
static char        g_text[CAP];
static char        g_undo[CAP]; static int g_undo_caret; static int g_have_undo;
static char        g_path[256]; static int g_dirty;
static char        g_find[128]; static int g_matchcase;
static char        g_gotobuf[16];
static nwui       *g_u;
static nwui_node  *g_ta, *g_status;
static int         g_wrap, g_show_status = 1;

static void snapshot(void) { memcpy(g_undo, g_text, CAP); g_undo_caret = 0; g_have_undo = 1; }

static void update_status(void)
{
	int ln, col; nwui_textarea_caret(g_ta, &ln, &col);
	char b[64]; snprintf(b, sizeof b, "Ln %d, Col %d", ln, col);
	nwui_set_text(g_status, g_show_status ? b : "");
}
static void on_change(nwui_node *self, void *u) { (void) self; (void) u; g_dirty = 1; update_status(); }

/* ---- File ---- */
static void do_load(const char *path)
{
	int fd = open(path, O_RDONLY); if (fd < 0) { nwui_message(g_u, "Notepad", "Cannot open file"); return; }
	int n = (int) read(fd, g_text, CAP - 1); close(fd);
	if (n < 0) n = 0; g_text[n] = 0;
	strncpy(g_path, path, sizeof g_path - 1);
	nwui_set_text(g_ta, g_text);              /* resets caret; textarea re-measures */
	g_dirty = 0; update_status();
}
static void do_save(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { nwui_message(g_u, "Notepad", "Cannot save file"); return; }
	write(fd, g_text, (int) strlen(g_text)); close(fd);
	strncpy(g_path, path, sizeof g_path - 1); g_dirty = 0;
}
static void open_ok(nwui_node *s, void *u) { (void) s; (void) u; do_load(g_path); }
static void save_ok(nwui_node *s, void *u) { (void) s; (void) u; do_save(g_path); }

static void m_new(nwui_node *s, void *u)  { (void) s; (void) u; snapshot(); g_text[0] = 0; g_path[0] = 0;
	nwui_set_text(g_ta, g_text); g_dirty = 0; update_status(); }
static void m_open(nwui_node *s, void *u) { (void) s; (void) u;
	if (!g_path[0]) strcpy(g_path, "/disks/main");
	nwui_file_dialog(g_u, 0, "/disks/main", g_path, sizeof g_path, open_ok, 0); }
static void m_save(nwui_node *s, void *u) { (void) s; (void) u;
	if (g_path[0]) do_save(g_path);
	else nwui_file_dialog(g_u, 1, "/disks/main", g_path, sizeof g_path, save_ok, 0); }
static void m_saveas(nwui_node *s, void *u){ (void) s; (void) u;
	nwui_file_dialog(g_u, 1, "/disks/main", g_path, sizeof g_path, save_ok, 0); }
static void m_exit(nwui_node *s, void *u) { (void) s; (void) u; _exit(0); }

/* ---- Edit ---- */
static void m_undo(nwui_node *s, void *u)  { (void) s; (void) u; if (!g_have_undo) return;
	char tmp[CAP]; memcpy(tmp, g_text, CAP); memcpy(g_text, g_undo, CAP); memcpy(g_undo, tmp, CAP);
	nwui_set_text(g_ta, g_text); update_status(); }
static void m_cut(nwui_node *s, void *u)   { (void) s; (void) u; snapshot();
	nwui_post_copy(g_u, 1); }                 /* see note: helper to emit a cut on the focused widget */
static void m_copy(nwui_node *s, void *u)  { (void) s; (void) u; nwui_post_copy(g_u, 0); }
static void m_paste(nwui_node *s, void *u) { (void) s; (void) u; snapshot(); nwui_post_paste(g_u); }
static void m_selall(nwui_node *s, void *u){ (void) s; (void) u; nwui_textarea_select_all(g_ta); }
static void find_ok(nwui_node *s, void *u) { (void) s; (void) u;
	if (!nwui_textarea_find(g_ta, g_find, g_matchcase, 1)) nwui_message(g_u, "Notepad", "Cannot find text"); }
static void m_find(nwui_node *s, void *u)  { (void) s; (void) u;
	nwui_prompt(g_u, "Find", g_find, sizeof g_find, find_ok, 0); }
static void m_findnext(nwui_node *s, void *u){ (void) s; (void) u; find_ok(0, 0); }
static void goto_ok(nwui_node *s, void *u) { (void) s; (void) u;
	int ln = 0; for (const char *p = g_gotobuf; *p >= '0' && *p <= '9'; p++) ln = ln*10 + (*p - '0');
	if (ln > 0) nwui_textarea_goto_line(g_ta, ln); }
static void m_goto(nwui_node *s, void *u)  { (void) s; (void) u;
	nwui_prompt(g_u, "Go To Line", g_gotobuf, sizeof g_gotobuf, goto_ok, 0); }

/* ---- Format / View / Help ---- */
static void m_wrap(nwui_node *s, void *u)  { (void) s; (void) u; g_wrap = !g_wrap; nwui_textarea_set_wrap(g_ta, g_wrap); }
static void m_statusbar(nwui_node *s, void *u){ (void) s; (void) u; g_show_status = !g_show_status; update_status(); }
static void m_timedate(nwui_node *s, void *u){ (void) s; (void) u;
	/* F5: insert HH:MM YYYY-MM-DD via libc time */
	time_t t = time(0); struct tm *tmv = localtime(&t); char b[32];
	strftime(b, sizeof b, "%H:%M %Y-%m-%d", tmv); snapshot();
	nwui_textarea_insert_text(g_ta, b); update_status(); }
static void m_about(nwui_node *s, void *u) { (void) s; (void) u;
	nwui_message(g_u, "About Notepad", "NanOS Notepad  -  a libnwui demo editor"); }

int main(void)
{
	nwui *u = nwui_open("Untitled - Notepad", 560, 420);
	if (!u) return 1;
	g_u = u;
	g_ta = nwui_textarea(u, g_text, CAP, on_change, 0);
	g_status = nwui_label(u, "Ln 1, Col 1");

	int mf = nwui_menu(u, "File");
	nwui_menu_item(u, mf, "New", m_new, 0);
	nwui_menu_item(u, mf, "Open...", m_open, 0);
	nwui_menu_item(u, mf, "Save", m_save, 0);
	nwui_menu_item(u, mf, "Save As...", m_saveas, 0);
	nwui_menu_separator(u, mf);
	nwui_menu_item(u, mf, "Exit", m_exit, 0);

	int me = nwui_menu(u, "Edit");
	nwui_menu_item(u, me, "Undo", m_undo, 0);
	nwui_menu_separator(u, me);
	nwui_menu_item(u, me, "Cut", m_cut, 0);
	nwui_menu_item(u, me, "Copy", m_copy, 0);
	nwui_menu_item(u, me, "Paste", m_paste, 0);
	nwui_menu_separator(u, me);
	nwui_menu_item(u, me, "Find...", m_find, 0);
	nwui_menu_item(u, me, "Find Next", m_findnext, 0);
	nwui_menu_item(u, me, "Go To...", m_goto, 0);
	nwui_menu_item(u, me, "Select All", m_selall, 0);
	nwui_menu_item(u, me, "Time/Date", m_timedate, 0);

	int mfo = nwui_menu(u, "Format");
	nwui_menu_item(u, mfo, "Word Wrap", m_wrap, 0);
	int mv = nwui_menu(u, "View");
	nwui_menu_item(u, mv, "Status Bar", m_statusbar, 0);
	int mh = nwui_menu(u, "Help");
	nwui_menu_item(u, mh, "About Notepad", m_about, 0);

	/* accelerators reuse the menu callbacks */
	nwui_accel(u, 1, 'n', 0, m_new, 0);   nwui_accel(u, 1, 'o', 0, m_open, 0);
	nwui_accel(u, 1, 's', 0, m_save, 0);  nwui_accel(u, 1, 'f', 0, m_find, 0);
	nwui_accel(u, 1, 'g', 0, m_goto, 0);  nwui_accel(u, 1, 'a', 0, m_selall, 0);
	nwui_accel(u, 1, 'z', 0, m_undo, 0);
	nwui_accel(u, 0, 0, NWUI_SC_F3, m_findnext, 0);
	nwui_accel(u, 0, 0, NWUI_SC_F5, m_timedate, 0);

	nwui_node *col = nwui_vbox(u);
	nwui_add(col, nwui_flex(g_ta, 1));
	nwui_node *bar = nwui_pad(nwui_hbox(u), 2);
	nwui_add(bar, g_status); nwui_colors(bar, 0, 0x00eef3f9);
	nwui_add(col, bar);
	nwui_set_root(u, col);
	u->focus = g_ta;                       /* editor focused at start */

	update_status();
	nwui_run(u);
	return 0;
}
```

This file references three small toolkit helpers not yet defined — add them to `nwui.c`
(thin wrappers that synthesize the existing clip/insert paths so the app needn't touch
libnw):

- `void nwui_post_copy(nwui *u, int cut)` — run `tf_copy`/`ta_copy` on `u->focus` and set
  `clip_set` (and delete the selection if `cut`); reuses the `NW_EV_COPY` core path by
  constructing a synthetic event and calling `nwui_dispatch`.
- `void nwui_post_paste(nwui *u)` — set `u->clip_get = 1` (the run loop performs
  `nw_get_clipboard`, the reply arrives as `NW_EV_PASTE`).
- `void nwui_textarea_insert_text(nwui_node *n, const char *s)` — insert a C string at the
  caret (loop `ta_insert`), pure → put it in `nwui_core.c` with a host test folded into Task 6's
  file (add a one-line `CHECK` there if revisiting; otherwise a fresh micro-test).

Add their decls to `nwui.h`. (If you prefer to avoid synthetic events, implement
`nwui_post_copy`/`nwui_post_paste` by calling the same internal helpers directly — both are
acceptable; keep them in `nwui.c` next to the other I/O glue.)

- [ ] **Step 3: Build**

Run: `make build 2>&1 | tail -20`
Expected: clean compile; `bin/nwnote.nxe` produced.

- [ ] **Step 4: Host tests still green**

Run: `make test 2>&1 | tail -15`
Expected: all pass; `nwui_core.c` coverage ≥90%.

- [ ] **Step 5: Commit**

```bash
git add user/nwnote/nwnote.c Makefile user/libnwui/nwui.c user/libnwui/nwui.h user/libnwui/nwui_core.c tests/test_nwui_core.cpp
git commit -m "feat(nwnote): rewrite as a Windows XP-style Notepad on libnwui"
```

---

## Task 16: QEMU verification + docs

**Files:**
- Modify: `docs/en/*` (mention Notepad if a desktop-apps doc exists), memory note.

- [ ] **Step 1: Build the image**

Run: `make image 2>&1 | tail -15`
Expected: `disk/image-grub2.img` built with the new `nwnote.nxe`.

- [ ] **Step 2: Boot headless and screenshot (per CLAUDE.md)**

Set `grub.cfg` `timeout=0`, boot `qemu-system-i386 -drive file=disk/image-grub2.img,format=raw -display none -monitor unix:/tmp/qmon,server,nowait`, drive the Run dialog (Super+R) to launch `nwnote`, then via the monitor socket `screendump /tmp/x.ppm`, `sips -s format png /tmp/x.ppm --out /tmp/x.png`, and read `/tmp/x.png`. Restore `grub.cfg` `timeout=5` afterward.

Verify visually:
- the Notepad window with the editor + status bar,
- typing inserts text and the status bar shows `Ln/Col`,
- arrows/Home/End move the caret; Enter wraps lines; the view scrolls past the bottom,
- the File menu opens an Open dialog listing `/disks/main`, and an Open→type→Save round-trip
  writes a file (read it back via the shell / `cat`),
- Word Wrap toggles, Find selects a match, Go To jumps, About shows the modal.

- [ ] **Step 3: Filesystem clean after a Save**

After a Notepad Save to `/disks/main/notes.txt`, confirm the image is e2fsck-clean (the ext
write path + JBD2), per the project's write-support verification habit.

- [ ] **Step 4: Update docs + memory**

If a desktop-apps doc lists Files/Settings/Terminal, add Notepad. Update the project memory
note (`nanos-ui-redesign`) to record the new reusable `libnwui` widgets (textarea, checkbox,
accelerators, modal, file dialog).

- [ ] **Step 5: Commit**

```bash
git add docs/ && git commit -m "docs: note Notepad + new libnwui widgets"
```

---

## Self-Review

**Spec coverage:**
- textarea (caret/scroll/selection/wrap/mouse) → Tasks 1–5 ✓
- find/replace/goto/select-all → Task 6 (+ Replace composed in app, Task 15) ✓
- clipboard → Task 7 ✓
- accelerators (Ctrl/F-keys) → Task 8 ✓
- checkbox → Task 9 ✓
- modal + message/prompt → Tasks 10–11 ✓
- file dialog → Task 12 ✓
- painting → Tasks 13–14 ✓
- Notepad app: File/Edit/Format/View/Help, status bar, undo, time/date, about → Task 15 ✓
- build/test/QEMU/docs → Tasks 15–16 ✓
- **Replace dialog**: the spec lists Replace (Ctrl+H). Task 15's menu/accels omit an explicit
  Replace builder. **Fix at execution:** add a `m_replace` composed from the modal +
  primitives (two textfields + "Match case" checkbox + Replace/Replace-All/Cancel) using
  `nwui_open_modal`, and wire `nwui_accel(u, 1, 'h', 0, m_replace, 0)` plus an Edit-menu item.
  Implement it as a small extra step within Task 15.

**Placeholder scan:** the only intentional non-final code is the explicitly-flagged
placeholder stub in Task 13's paint walk (removed by the implementer; the real walk follows
in the same block). No "TODO/TBD" requirements remain.

**Type consistency:** `nwui_textarea_*`, `nwui_accel`, `nwui_open_modal`/`nwui_close_modal`/
`nwui_modal_open`, `nwui_message`/`nwui_prompt`, `nwui_file_dialog`, `nwui_path_join`/
`nwui_path_up`, `nwui_post_copy`/`nwui_post_paste`/`nwui_textarea_insert_text` are used with
consistent signatures across tasks and `nwui.h`. New node kinds `NWUI_TEXTAREA`/
`NWUI_CHECKBOX` and scancodes `NWUI_SC_*` are defined once in `nwui_core.h` (Task 1/8).
