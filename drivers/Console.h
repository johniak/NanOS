#pragma once
#include <string.h>
namespace kernel{

	// Machine-independent console: formatting over the arch character sink
	// (<arch/console.h>). The VGA/hardware specifics live in the arch layer.
	class Console{
	public:
		static void goToXY(unsigned short x,unsigned short y);
		static void write(char c);
		static void write(int d);
		static void writeHex(int hex);
		static void writeHex(unsigned long hex);   // 64-bit-capable on LP64 (addresses)
		static void write(const char* text);
		static void writeLine(char c);
		static void writeLine(const char* line);
		static void writeLine(int line);
		static void clearScreen();
		static char *itoa(int i,int base);
	};
}
