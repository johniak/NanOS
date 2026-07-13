/*
 * nwproto.h — the nanowm wire protocol, shared by the compositor (nwm) and clients (libnw).
 *
 * One fixed 28-byte header (`nw_msg`) optionally followed by `length` payload bytes. The
 * transport is a byte stream over a pipe whose kernel ring is only 4096 B, so a single
 * COMMIT (a window's pixels) arrives in many fragments and the reader must NEVER block on
 * one peer. Hence the decoder is a STREAMING state machine (`nw_decoder`) fed arbitrary
 * byte chunks: it reassembles header-then-payload across reads and never assumes a whole
 * message landed in one read.
 *
 * Pure logic (no I/O, no libc) — compiles both in the C userland (gcc) and the C++ host
 * test suite (g++); kept in the C/C++ common subset.
 */
#ifndef NW_PROTO_H
#define NW_PROTO_H

#include <stdint.h>

#define NW_PROTO_VERSION 1

/* Message types. Requests (client -> server) and events (server -> client) share the wire
 * but live in disjoint ranges so a decoder/logger can tell direction at a glance. */
enum {
	/* client -> server */
	NW_REQ_HELLO          = 1,   /* a=version                                              */
	NW_REQ_CREATE_WINDOW  = 2,   /* a=w b=h c=style (NW_STYLE_*); payload=title             */
	NW_REQ_COMMIT         = 3,   /* window; a=x b=y c=w d=h; payload = c*d*4 BGRX pixels     */
	NW_REQ_DESTROY_WINDOW = 4,   /* window                                                  */
	NW_REQ_SET_CLIPBOARD  = 5,   /* payload=text (reply to NW_EVT_COPY)                     */
	NW_REQ_GET_CLIPBOARD  = 6,   /* ask for the clipboard -> server replies NW_EVT_PASTE    */
	NW_REQ_SPAWN          = 7,   /* payload=command/path; server launches it (like Run)     */
	NW_REQ_SET_MENU       = 8,   /* payload=menu spec: menus split 0x1e, fields split 0x1f  */
	                             /*   field[0]=title, field[1..]=item labels ("-"=separator) */
	NW_REQ_RELOAD_SETTINGS = 9,  /* no payload; server re-reads settings.yaml + recomposes   */
	NW_REQ_DRAG_BEGIN     = 10,  /* payload=drag data (e.g. a file path); server arbitrates  */
	                             /*   the drag: routes DRAG_MOTION/LEAVE to the window under  */
	                             /*   the cursor and DROP (with this payload) on release.     */
	NW_REQ_SHM_SURFACE    = 11,  /* window; a=w b=h; payload = two little-endian u64 /dev/nwshm
	                              *   tokens (the window's double buffer). Declares that pixels
	                              *   live in shared memory: commits switch to NW_REQ_COMMIT_SHM
	                              *   and carry coordinates only. Replaces a previous pair (the
	                              *   server unmaps + ioctl-frees the old tokens); an EMPTY
	                              *   payload releases the surface (back to pipe commits).      */
	NW_REQ_COMMIT_SHM     = 12,  /* window; a=x b=y c=w d=h; NO payload (length MUST be 0 — on
	                              *   the wire `length` is the payload byte count, so it cannot
	                              *   carry data; a nonzero value desyncs the stream decoder).
	                              *   The frame is in shm buffer 0. The server defers the copy
	                              *   out of the buffer to its next compose, so a fast client
	                              *   costs one copy per composed frame, not one per commit.    */
	NW_REQ_COMMIT_SHM1    = 13,  /* as NW_REQ_COMMIT_SHM, but the frame is in shm buffer 1 —
	                              *   the buffer index is encoded in the TYPE for exactly the
	                              *   wire reason above.                                        */

	/* server -> client */
	NW_EVT_CONFIGURE      = 64,  /* window; a=w b=h (assigned size, incl. first map)        */
	NW_EVT_KEY            = 65,  /* window; a=ascii b=down c=scancode d=mods (bit0 = shift) */
	NW_EVT_POINTER        = 66,  /* window; a=x b=y (rel) c=buttons|mods<<8 (bit8 shift,bit9 cmd) d=wheel */
	NW_EVT_FOCUS          = 67,  /* window; a=1 gained / 0 lost                             */
	NW_EVT_CLOSE          = 68,  /* window; user asked to close (Super+Q / close box)       */
	NW_EVT_COPY           = 69,  /* window; Super+C/X — client should reply SET_CLIPBOARD   */
	NW_EVT_PASTE          = 70,  /* window; Super+V — payload=clipboard text to insert      */
	NW_EVT_MENU           = 71,  /* window; a=top-menu index b=item index (app menu chosen) */
	NW_EVT_DRAG_MOTION    = 72,  /* window under cursor; a=x b=y (rel) c=mods (bit0 shift,1 ctrl) */
	NW_EVT_DRAG_LEAVE     = 73,  /* window the drag just left (clear any drop highlight)       */
	NW_EVT_DROP           = 74,  /* window; a=x b=y (rel) c=mods; payload=the dragged data     */
	NW_EVT_BUFFER_RELEASE = 75   /* window; a=shm buffer index the server is DONE reading —
	                              *   the client may draw into it again (wl_buffer.release /
	                              *   MIT-SHM ShmCompletion equivalent). Consumed inside libnw
	                              *   (never surfaced to the app); with two buffers this also
	                              *   paces a free-running client to the compose rate.          */
};

/* Drag modifier bits carried in NW_EVT_DRAG_MOTION/DROP `c`. */
enum { NW_DND_SHIFT = 1, NW_DND_CTRL = 2 };

/* Pointer button bitmask (matches evdev BTN ordering we care about). */
enum { NW_BTN_LEFT = 1, NW_BTN_RIGHT = 2, NW_BTN_MIDDLE = 4 };

/* Window style bits, sent in NW_REQ_CREATE_WINDOW.c. Style 0 = legacy: light glass slab with an
 * opaque client area. */
enum {
	NW_STYLE_GLASS_CLIENT = 1,   /* client pixels are 0xAARRGGBB: top byte = ink alpha over glass */
	NW_STYLE_DARK         = 2    /* dark slab tint (formal form of the "\x01" title prefix)       */
};

/* /dev/nwshm ioctl ABI (kernel side: drivers/NwShmDevice.h) — the shared window-surface pool
 * behind NW_REQ_SHM_SURFACE. Both ends of the pipeline (libnw clients, the compositor) use it. */
#define NWSHM_IOC_ALLOC 0x4E5701u   /* arg = struct nwshm_ioc{bytes in; token out} */
#define NWSHM_IOC_FREE  0x4E5702u   /* arg = struct nwshm_ioc{token in}            */
struct nwshm_ioc { uint64_t bytes, token; };

/* Largest COMMIT payload a client may send in one message. A full-window repaint of a big
 * window (e.g. 560x360x4 = 806 KB) exceeds the compositor's per-client reassembly buffer, so
 * the client splits a commit into horizontal row bands each <= this many bytes, and the
 * compositor's CLIENT_COMMITCAP is kept comfortably above it. Keeping the cap here makes the
 * two sides agree. */
enum { NW_COMMIT_MAX_BYTES = 256 * 1024 };

/* The fixed wire header. All fields are 4 bytes => 28-byte struct with no padding on every
 * target (i686 userland + the LE host running the tests); serialized by raw copy. */
struct nw_msg {
	uint32_t type;
	uint32_t window;
	int32_t  a, b, c, d;
	uint32_t length;   /* payload bytes that follow this header */
};

enum { NW_MSG_HDR = 28 };

#ifdef __cplusplus
static_assert(sizeof(struct nw_msg) == NW_MSG_HDR, "nw_msg must be 28 packed bytes");
#else
_Static_assert(sizeof(struct nw_msg) == NW_MSG_HDR, "nw_msg must be 28 packed bytes");
#endif

/* ---- encoding ---------------------------------------------------------------------- */

/* Serialize the header into `out` (must hold >= NW_MSG_HDR bytes). Returns NW_MSG_HDR.
 * The payload (if any) is written by the caller right after these bytes. */
int nw_msg_encode(const struct nw_msg *m, unsigned char *out);

/* ---- streaming decode -------------------------------------------------------------- */

/* Decoder phases. */
enum { NW_DEC_HEADER = 0, NW_DEC_PAYLOAD = 1, NW_DEC_READY = 2 };

struct nw_decoder {
	unsigned char  hdr[NW_MSG_HDR];  /* header bytes accumulated so far                  */
	uint32_t       hdrgot;           /* count in hdr[]                                   */
	struct nw_msg  msg;              /* decoded header (valid once payload phase begins)  */
	unsigned char *payload;          /* caller-owned payload buffer                       */
	uint32_t       paycap;           /* capacity of payload[]                             */
	uint32_t       paygot;           /* payload bytes stored / skipped so far             */
	int            phase;            /* NW_DEC_*                                          */
	int            overflow;         /* last message's payload exceeded paycap (skipped)  */
};

/* Bind a payload buffer (sized to the largest message the consumer expects). */
void nw_decoder_init(struct nw_decoder *d, unsigned char *paybuf, uint32_t paycap);

/* Advance through the byte range [*p, end), reassembling one message per call.
 *   returns 1: a complete message is available in d->msg (+ d->payload, d->msg.length
 *              bytes; or d->overflow set if it didn't fit). *p is advanced past it.
 *   returns 0: the range is exhausted mid-message; *p == end. Call again after the next read.
 * Handles headers/payloads split across calls and several messages packed in one range. */
int nw_decoder_next(struct nw_decoder *d, const unsigned char **p, const unsigned char *end);

#endif /* NW_PROTO_H */
