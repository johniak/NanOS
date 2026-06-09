#include "doctest.h"
#include "UserStack.h"
#include <cstring>
#include <cstdint>

using kernel::buildUserStack;

TEST_CASE("argv image: argc, NULL-terminated argv[], strings, esp at argc, 16-aligned") {
	// Fake user stack as a host buffer; userBase is the VA the buffer represents.
	unsigned char buf[4096];
	const uint32_t userTop = 0x500000;
	const uint32_t userBase = userTop - sizeof(buf);
	auto va2host = [&](uint32_t va) -> unsigned char* { return buf + (va - userBase); };

	const char* argv[] = { "ls", "-l", "/boot", 0 };
	const char* envp[] = { "TERM=xterm-256color", "PATH=/bin", 0 };
	uint32_t esp = buildUserStack(userTop, argv, 3, envp, 2,
		[&](uint32_t va, const void* src, unsigned len) {
			memcpy(va2host(va), src, len);
		});

	// esp points at argc, 16-byte aligned
	CHECK((esp & 0xF) == 0);
	int argc = *(int*) va2host(esp);
	CHECK(argc == 3);

	uint32_t* uargv = (uint32_t*) (va2host(esp) + 4);   // argv = esp + 4
	CHECK(uargv[3] == 0);                                // NULL-terminated
	CHECK(strcmp((char*) va2host(uargv[0]), "ls") == 0);
	CHECK(strcmp((char*) va2host(uargv[1]), "-l") == 0);
	CHECK(strcmp((char*) va2host(uargv[2]), "/boot") == 0);

	// envp follows argv's NULL: esp + 4 + (argc + 1) * 4
	uint32_t* uenvp = uargv + (3 + 1);
	CHECK(uenvp[2] == 0);                                // NULL-terminated
	CHECK(strcmp((char*) va2host(uenvp[0]), "TERM=xterm-256color") == 0);
	CHECK(strcmp((char*) va2host(uenvp[1]), "PATH=/bin") == 0);
}

TEST_CASE("zero args: argc==0, argv[0]==NULL") {
	unsigned char buf[256];
	const uint32_t userTop = 0x500000;
	const uint32_t userBase = userTop - sizeof(buf);
	auto va2host = [&](uint32_t va) -> unsigned char* { return buf + (va - userBase); };

	const char* argv[] = { 0 };
	const char* envp[] = { 0 };
	uint32_t esp = buildUserStack(userTop, argv, 0, envp, 0,
		[&](uint32_t va, const void* src, unsigned len) {
			memcpy(va2host(va), src, len);
		});

	CHECK((esp & 0xF) == 0);
	CHECK(*(int*) va2host(esp) == 0);
	uint32_t* uargv = (uint32_t*) (va2host(esp) + 4);
	CHECK(uargv[0] == 0);                                // argv[0] = NULL
	CHECK(uargv[1] == 0);                                // envp[0] = NULL (no env)
}
