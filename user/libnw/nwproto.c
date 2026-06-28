/*
 * nwproto.c — encoder + streaming decoder for the nanowm wire protocol (see nwproto.h).
 * No I/O, no allocation: the caller owns all buffers. Pure logic, host-tested.
 */
#include "nwproto.h"
#include <string.h>

int nw_msg_encode(const struct nw_msg *m, unsigned char *out)
{
	/* nw_msg is asserted to be 28 packed bytes on every target; copy it verbatim. */
	memcpy(out, m, NW_MSG_HDR);
	return NW_MSG_HDR;
}

void nw_decoder_init(struct nw_decoder *d, unsigned char *paybuf, uint32_t paycap)
{
	d->payload  = paybuf;
	d->paycap   = paycap;
	d->hdrgot   = 0;
	d->paygot   = 0;
	d->phase    = NW_DEC_HEADER;
	d->overflow = 0;
}

int nw_decoder_next(struct nw_decoder *d, const unsigned char **p, const unsigned char *end)
{
	const unsigned char *cur = *p;

	for (;;) {
		if (d->phase == NW_DEC_READY) {
			/* the previous message was handed to the caller; begin a fresh one */
			d->phase    = NW_DEC_HEADER;
			d->hdrgot   = 0;
			d->paygot   = 0;
			d->overflow = 0;
		}

		if (d->phase == NW_DEC_HEADER) {
			while (d->hdrgot < NW_MSG_HDR && cur < end)
				d->hdr[d->hdrgot++] = *cur++;
			if (d->hdrgot < NW_MSG_HDR) {       /* header split across reads */
				*p = cur;
				return 0;
			}
			memcpy(&d->msg, d->hdr, NW_MSG_HDR);
			d->paygot   = 0;
			d->overflow = 0;
			d->phase    = NW_DEC_PAYLOAD;
		}

		if (d->phase == NW_DEC_PAYLOAD) {
			while (d->paygot < d->msg.length && cur < end) {
				unsigned char byte = *cur++;
				if (d->paygot < d->paycap)
					d->payload[d->paygot] = byte;   /* fits */
				else
					d->overflow = 1;                /* too big: drop but keep resyncing */
				d->paygot++;
			}
			if (d->paygot < d->msg.length) {    /* payload split across reads */
				*p = cur;
				return 0;
			}
			d->phase = NW_DEC_READY;
			*p = cur;
			return 1;                           /* a whole message is ready */
		}
	}
}
