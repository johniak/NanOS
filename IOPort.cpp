#include "IOPort.h"

namespace kernel{
	void IOPort::outb(unsigned short port, unsigned char value)
	{
		__asm__ __volatile__ ("outb %1, %0" : : "dN" (port), "a" (value));
	}
	unsigned char IOPort::inb(unsigned short port)
	{
		unsigned char ret;
		__asm__ __volatile__("inb %1, %0" : "=a" (ret) : "dN" (port));
		return ret;
	}
	unsigned short IOPort::inw(unsigned short port)
	{
		unsigned short ret;
		__asm__ __volatile__ ("inw %1, %0" : "=a" (ret) : "dN" (port));
		return ret;
	}
}