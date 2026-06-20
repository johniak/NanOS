#include "doctest.h"
#include "e1000_core.h"

TEST_CASE("ringNext wraps at n") {
	CHECK(ringNext(0, 32) == 1);
	CHECK(ringNext(30, 32) == 31);
	CHECK(ringNext(31, 32) == 0);
}

TEST_CASE("rxDescDone reads the DD status bit") {
	RxDesc d{};
	CHECK(!rxDescDone(&d));
	d.status = 0x01;             // DD
	CHECK(rxDescDone(&d));
	d.status = 0x02;             // EOP without DD
	CHECK(!rxDescDone(&d));
	d.status = 0x03;             // DD | EOP
	CHECK(rxDescDone(&d));
}

TEST_CASE("txEncode sets EOP|IFCS|RS, length and addr, and clears status") {
	TxDesc d{};
	d.status = 0x01;             // pretend a previous "done"
	txEncode(&d, 0x0CAFE000, 100);
	// Copy packed fields to locals — a reference cannot bind to a misaligned packed member.
	uint64_t addr = d.addr; uint16_t length = d.length;
	uint8_t cso = d.cso, cmd = d.cmd, status = d.status;
	CHECK(addr == 0x0CAFE000u);
	CHECK(length == 100);
	CHECK(cso == 0);
	CHECK(cmd == (0x01 | 0x02 | 0x08));   // EOP | IFCS | RS
	CHECK(status == 0);                   // cleared so the tx path can poll DD
}
