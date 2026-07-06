#pragma once
#include <string.h>
#include <stdint.h>
namespace kernel{

	// Machine-independent console: formatting over the arch character sink
	// (<arch/console.h>). The VGA/hardware specifics live in the arch layer.
	class Console{
	public:
		static void goToXY(unsigned short x,unsigned short y);
		static void write(char c);
		static void write(int d);
		static void writeHex(int hex);
		static void writeHex(uint64_t hex);        // 64-bit: prints all significant nibbles
		static void write(const char* text);
		static void writeLine(char c);
		static void writeLine(const char* line);
		static void writeLine(int line);
		static void clearScreen();
		static char *itoa(int i,int base);
		static char *itoa(uint64_t v,int base);    // 64-bit unsigned formatter
	};

	// Optional tee for kernel panics. When a ring-0 CPU exception halts the machine, faultHandler
	// also hands one preformatted line to this sink (if registered), so the panic survives on a
	// persistent channel even when the physical display is owned by a driver that has repointed
	// scanout away from the fbcon framebuffer (e.g. i915 after modeset — its bring-up log on the USB
	// root is the readable channel). Registered via knx_set_panic_sink(); NULL = screen only.
	extern void (*g_panicSink)(const char* line);
}
