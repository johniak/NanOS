/*
 * LineDiscipline.h — cooked-mode terminal line buffer (machine-independent).
 *
 * Accumulates a line: printable chars are appended and echoed; Backspace erases
 * the last char (echo "\b \b"); Enter commits the line (echo "\n"); Ctrl-D (EOT)
 * on an empty line commits a zero-length line = EOF. takeLine() consumes a
 * committed line. The echo sink is injected (template) so this is host-testable.
 */
#ifndef LINEDISCIPLINE_H_
#define LINEDISCIPLINE_H_

namespace kernel {

class LineDiscipline {
public:
	static constexpr int CAP = 256;   // max chars per line incl. the trailing '\n'

	LineDiscipline() : m_len(0), m_ready(false), m_readyLen(0), m_echo(true) {}

	// Echo control (termios ECHO). When off, line editing still works (chars accumulate, backspace
	// erases) but nothing is displayed — the contract a password prompt relies on. Default on.
	void setEcho(bool on) { m_echo = on; }
	bool echoOn() const { return m_echo; }

	// Feed one input char; `echo` is called for each char to display (only when ECHO is on).
	template <class Echo>
	void push(char c, Echo& echo) {
		if (m_ready)
			return;                          // wait for takeLine

		if (c == '\b' || c == 127) {         // backspace / DEL
			if (m_len > 0) {
				m_len--;
				if (m_echo) { echo('\b'); echo(' '); echo('\b'); }
			}
			return;
		}
		if (c == 4) {                        // EOT (Ctrl-D): commit as-is (EOF if empty)
			commit();
			return;
		}
		if (c == '\n' || c == '\r') {
			if (m_len < CAP)
				m_buf[m_len++] = '\n';
			if (m_echo) echo('\n');
			commit();
			return;
		}
		if (m_len < CAP) {                   // printable; drop silently on overflow
			m_buf[m_len++] = c;
			if (m_echo) echo(c);
		}
	}

	bool lineReady() const { return m_ready; }

	// Copy the committed line into out (up to n). Returns its length, clears ready.
	int takeLine(char* out, int n) {
		int k = m_readyLen < n ? m_readyLen : n;
		for (int i = 0; i < k; i++)
			out[i] = m_buf[i];
		m_ready = false;
		m_readyLen = 0;
		m_len = 0;
		return k;
	}

private:
	void commit() { m_readyLen = m_len; m_ready = true; }

	char m_buf[CAP];
	int  m_len;
	bool m_ready;
	int  m_readyLen;
	bool m_echo;        // termios ECHO: echo typed chars to the display (off for password entry)
};

}  // namespace kernel

#endif /* LINEDISCIPLINE_H_ */
