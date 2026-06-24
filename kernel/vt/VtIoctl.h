/*
 * VtIoctl.h — Linux virtual-terminal ioctl numbers + structs (the subset NanOS implements).
 * Values match Linux so userland (nwm) and any ported tool see the real ABI.
 */
#pragma once
#include <stdint.h>

// KD modes (console graphics/text), arg is an int.
#define KDGETMODE   0x4B3B
#define KDSETMODE   0x4B3A
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

// VT switching control.
#define VT_OPENQRY    0x5600   // arg: int* -> first free VT number (or -1)
#define VT_GETMODE    0x5601   // arg: struct vt_mode*
#define VT_SETMODE    0x5602   // arg: struct vt_mode*
#define VT_GETSTATE   0x5603   // arg: struct vt_stat*
#define VT_RELDISP    0x5605   // arg: int (1 = release granted, 2 = acquire ack)
#define VT_ACTIVATE   0x5606   // arg: int (1-based VT number)
#define VT_WAITACTIVE 0x5607   // arg: int (1-based VT number)

#define VT_AUTO    0x00       // vt_mode.mode: kernel auto-switches
#define VT_PROCESS 0x01       // vt_mode.mode: process-controlled (relsig/acqsig)

struct vt_mode {
	uint8_t mode;     // VT_AUTO or VT_PROCESS
	uint8_t waitv;    // unused (0)
	int16_t relsig;   // signal sent to ask the owner to release
	int16_t acqsig;   // signal sent when the owner acquires
	int16_t frsig;    // unused (0)
};

struct vt_stat {
	uint16_t v_active;   // 1-based active VT
	uint16_t v_signal;   // unused (0)
	uint16_t v_state;    // bitmask of allocated VTs
};
