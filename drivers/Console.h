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
}
