/*
 * VtConsole.h — one virtual console (Linux vc_data analogue): an fbcon text grid + per-VT
 * input state (line discipline + raw ring + wait queue + termios) + a foreground process group
 * (job control) + a KD_TEXT/KD_GRAPHICS mode + VT_SETMODE process-mode state (the graphics owner).
 *
 * Machine-independent: the active console's input is fed by the arch layer (the PS/2 path), and
 * its fbcon blits only while it is the live (visible) console. Everything that used to be a
 * singleton in arch/x86_64/drivers/input_x86_64.cpp now lives here, once per VT.
 */
#pragma once
#include "FbConsole.h"
#include "Termios.h"
#include "LineDiscipline.h"
#include "KeyDecoder.h"
#include "WaitQueue.h"
#include "vt/VtIoctl.h"

namespace kernel {

class VtConsole {
	int       m_index = 0;        // 1-based VT number
	int       m_mode = KD_TEXT;   // KD_TEXT (kernel draws fbcon) or KD_GRAPHICS (owner draws via /dev/fb0)
	int       m_fgPgrp = 0;       // foreground process group (job control); 0 = none
	FbConsole m_fb;
	Termios   m_termios;

	// --- input state: per-VT (moved out of the arch singleton) ---
	LineDiscipline m_line;
	KeyDecoder     m_decoder;
	bool           m_raw = false;
	WaitQueue      m_inputWq;
	static const int RAWCAP = 256;
	volatile unsigned char m_rawbuf[RAWCAP];
	volatile int   m_rawHead = 0, m_rawTail = 0;

	// --- VT_SETMODE state (graphics owner) ---
	int  m_vtMode = VT_AUTO;     // VT_AUTO (kernel auto-switch) or VT_PROCESS (relsig/acqsig)
	int  m_relsig = 0, m_acqsig = 0;
	int  m_ownerPid = 0;         // pid that did VT_SETMODE(VT_PROCESS)
	bool m_relWait = false;      // a release was requested, awaiting VT_RELDISP

	void rawPush(unsigned char b);
	bool rawEmpty() const { return m_rawHead == m_rawTail; }
	unsigned char rawPop();
	static bool rawReadyPred(void* self);
	static bool lineReadyPred(void* self);
public:
	void init(const FbSurface& s, int index);

	int  index() const { return m_index; }
	int  mode() const { return m_mode; }
	void setMode(int m) { m_mode = m; }
	int  fgPgrp() const { return m_fgPgrp; }
	void setFgPgrp(int p) { m_fgPgrp = p; }
	FbConsole& fbcon() { return m_fb; }

	Termios& termios() { return m_termios; }
	bool raw() const { return m_raw; }
	void setRaw(bool r);

	// Input: feed one scancode (decoder -> line discipline / raw ring, signals, wake readers);
	// blocking read; readiness for poll().
	void feedScancode(unsigned char sc);
	int  read(char* buf, unsigned n, bool nonblock);
	bool inputReady() const;
	WaitQueue* inputWaitQueue() { return &m_inputWq; }

	// Output: write bytes to the fbcon (blits iff this VT is live).
	void write(const char* buf, unsigned n);

	// VT_SETMODE accessors (the graphics handoff protocol).
	int  vtMode() const { return m_vtMode; }
	void setVtMode(int m, int rel, int acq, int owner) { m_vtMode = m; m_relsig = rel; m_acqsig = acq; m_ownerPid = owner; }
	int  relsig() const { return m_relsig; }
	int  acqsig() const { return m_acqsig; }
	int  ownerPid() const { return m_ownerPid; }
	bool relWait() const { return m_relWait; }
	void setRelWait(bool w) { m_relWait = w; }
};

}  // namespace kernel
