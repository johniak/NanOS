#pragma once
#include <string.h>
namespace kernel{
	
	class Console{
		static unsigned short cursorX;
		static unsigned short cursorY;
		static volatile  unsigned short *videoram;
		static void scroll();
		static void moveCursor();
	public:
		static void goToXY(unsigned short x,unsigned short y);
		static void write(char c);
		static void write(int d);
		static void write(const char* text);
		static void writeLine(char c);
		static void writeLine(const char* line);
		static void writeLine(int line);
		static void clearScreen();
		static char *itoa(int i);
	};
}