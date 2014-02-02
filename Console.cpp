#include "Console.h"
#include "IOPort.h"
namespace kernel{

	unsigned short Console::cursorX=0;
	unsigned short Console::cursorY=0;
	volatile unsigned short *Console::videoram = (unsigned short *)0xB8000;
	void Console::scroll()
	{

		unsigned char attributeByte = (0<< 4) | (15& 0x0F);
		unsigned short blank =  ' ' | (attributeByte << 8);

		if(cursorY >= 25)
		{
			int i;
			for (i = 0*80; i < 24*80; i++)
			{
				videoram[i] = videoram[i+80];
			}
			for (i = 24*80; i < 25*80; i++)
			{
				videoram[i] = blank;
			}
			cursorY = 24;
		}
	}
	void Console::goToXY(unsigned short x,unsigned short y){
		cursorX = x;
		cursorY = y;
		moveCursor();
	}

	void Console::write(char c)
	{
		unsigned char backColour = 0;
		unsigned char foreColour = 15;

		unsigned char  attributeByte = (backColour << 4) | (foreColour & 0x0F);
		unsigned short attribute = attributeByte << 8;
		unsigned short *location;


		if (c == 0x08 && cursorX)
		{
			cursorX--;
		}

		else if (c == 0x09)
		{
			cursorX = (cursorX+8) & ~(8-1);
		}
		else if (c == '\r')
		{
			cursorX = 0;
		}
		else if (c == '\n')
		{
			cursorX = 0;
			cursorY++;
		}
		else if(c >= ' ')
		{
			location = ((unsigned short *)videoram) + (cursorY*80 + cursorX);
			*location = c | attribute;
			cursorX++;
		}
		if (cursorX >= 80)
		{
			cursorX = 0;
			cursorY ++;
		}

		scroll();
		moveCursor();
	}
	void Console::write(const char* text){
		int length=strlen(text);
		for(int i=0;i<length;i++){
			write(text[i]);
		}
	}
	void Console::write(int d){
		write(itoa(d));
	}
	void Console::writeLine(char line){
		write(line);
		write('\n');
	}
	void Console::writeLine(const char* line){
		write(line);
		write('\n');
	}
	void Console::writeLine(int line){
		write(line);
		write('\n');
	}
	void Console::moveCursor()
	{
		unsigned short cursorLocation = cursorY * 80 + cursorX;
		IOPort::outb(0x3D4, 14);                  // Tell the VGA board we are setting the high cursor byte.
		IOPort::outb(0x3D5, cursorLocation >> 8); // Send the high cursor byte.
		IOPort::outb(0x3D4, 15);                  // Tell the VGA board we are setting the low cursor byte.
		IOPort::outb(0x3D5, cursorLocation);      // Send the low cursor byte.
	}
	void Console::clearScreen(){
		unsigned char attributeByte = (0<< 4) | (15& 0x0F);
		unsigned short blank =  ' ' | (attributeByte << 8);
		for(int i=0;i<80*25;i++){
			videoram[i] = blank;
		}
		cursorX = 0;
		cursorY = 0;
		moveCursor();
	}
	char *Console::itoa(int i)
	{
  		#define INT_DIGITS 19 
		static char buf[INT_DIGITS + 2];
		char *p = buf + INT_DIGITS + 1; 
		if (i >= 0) {
			do {
				*--p = '0' + (i % 10);
				i /= 10;
			} while (i != 0);
			return p;
		}
		else {
			do {
				*--p = '0' - (i % 10);
				i /= 10;
			} while (i != 0);
			*--p = '-';
		}
		return p;
	}
}