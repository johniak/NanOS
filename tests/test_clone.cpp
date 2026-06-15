#include "doctest.h"
#include "CloneFlags.h"

TEST_CASE("clone flags: thread-create shares the address space; a fork does not") {
	using namespace kernel;
	unsigned thr = CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD;
	CHECK(cloneSharesAddressSpace(thr));
	CHECK(cloneIsThread(thr));
	CHECK(!cloneSharesAddressSpace((unsigned)SIGCHLD));   // a fork
	CHECK(!cloneIsThread((unsigned)SIGCHLD));
}

TEST_CASE("clone flags: VM and THREAD are independent bits") {
	using namespace kernel;
	CHECK(cloneSharesAddressSpace(CLONE_VM));
	CHECK(!cloneIsThread(CLONE_VM));               // vfork-style: shares VM, not a thread
	CHECK(cloneIsThread(CLONE_THREAD));
	CHECK(!cloneSharesAddressSpace(0u));
	CHECK(!cloneIsThread(0u));
}
