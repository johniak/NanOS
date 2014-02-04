#pragma once

namespace kernel{

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

struct Registers
{
   unsigned ds;                  // Data segment selector
   unsigned edi, esi, ebp, esp, ebx, edx, ecx, eax; // Pushed by pusha.
   unsigned int_no, err_code;    // Interrupt number and error code (if applicable)
   unsigned eip, cs, eflags, useresp, ss; // Pushed by the processor automatically.
};

typedef void (*IsrHandler)(Registers);
class Interrupt{
public:
	static void registerInterruptHandler(unsigned char n, IsrHandler handler);

};
}
