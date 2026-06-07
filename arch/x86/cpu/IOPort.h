#pragma once

namespace kernel{
	class IOPort{
	public:
		static void outb(unsigned short port, unsigned char value);
		static unsigned char inb(unsigned short port);
		static unsigned short inw(unsigned short port);
	};
}