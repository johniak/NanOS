#pragma once




namespace kernel {
struct GdtEntry
{
   unsigned short limit_lo;            // Lower 16 bits of the segment limit.
   unsigned short base_lo;             // Lower 16 bits of the base address.
   unsigned char  base_mid;            // Next 8 bits of the base address.
   unsigned char  access;              // Access flags (present, ring, type).
   unsigned char  granularity;         // High 4 bits of limit + flags (gran, size).
   unsigned char  base_hi;             // Last 8 bits of the base address.
} __attribute__((packed));

// A struct describing a pointer to our GDT, suitable for giving to 'lgdt'.
struct GdtPtr
{
   unsigned short limit;
   unsigned base;                      // The address of the first GdtEntry.
} __attribute__((packed));



class Gdt {
	GdtEntry gdtEntries[3];
	GdtPtr gdtPtr;
public:

	void initialize();
private:
	void setGate(int num, unsigned base, unsigned limit, unsigned char access,
			unsigned char gran);
};
}

extern "C" void gdt_flush(unsigned ptr);
