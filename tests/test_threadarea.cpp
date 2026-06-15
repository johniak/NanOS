#include "doctest.h"
#include "../kernel/ThreadArea.h"

// set_thread_area() on NanOS does not allocate a TLS slot: there is one fixed GDT
// descriptor (entry 6, selector 0x33) reloaded per thread. userDescToSelector writes
// that fixed entry number back into the user's struct (musl reads it to compute %gs)
// and returns the selector.
TEST_CASE("set_thread_area parses user_desc and assigns the fixed TLS GS selector") {
	kernel::UserDesc ud{};
	ud.entry_number = (unsigned) -1;
	ud.base_addr = 0x40000000;
	ud.limit = 0xFFFFF;
	int sel = kernel::userDescToSelector(&ud);   // fixed TLS slot 6
	CHECK((sel & 7) == 3);                 // RPL 3
	CHECK((sel >> 3) == 6);                // the dedicated TLS entry
	CHECK(ud.entry_number == 6);           // written back (musl reads it to set %gs)
}
