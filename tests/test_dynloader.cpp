#include "doctest.h"
#include "DynLoader.h"
#include <cstring>

using namespace kernel;

TEST_CASE("SymTable resolves names to addresses") {
	SymTable t;
	CHECK(t.add("nx_greet", 0x08000010));
	CHECK(t.add("nx_greeting", 0x08000040));
	CHECK(t.count() == 2);
	CHECK(t.find("nx_greet") == 0x08000010);
	CHECK(t.find("nx_greeting") == 0x08000040);
	CHECK(t.find("absent") == 0);
}

TEST_CASE("SymTable copies names (source can change after add)") {
	SymTable t;
	char name[16];
	strcpy(name, "printf");
	t.add(name, 0x1234);
	strcpy(name, "XXXXXX");          // clobber the source buffer
	CHECK(t.find("printf") == 0x1234);
}

TEST_CASE("SymTable rejects duplicate names (first definition wins)") {
	SymTable t;
	CHECK(t.add("dup", 0x1000));
	CHECK_FALSE(t.add("dup", 0x2000));   // already present
	CHECK(t.find("dup") == 0x1000);
	CHECK(t.count() == 1);
}

TEST_CASE("SymTable distinguishes names that share a prefix") {
	SymTable t;
	t.add("read", 0xAA);
	t.add("readv", 0xBB);
	CHECK(t.find("read") == 0xAA);
	CHECK(t.find("readv") == 0xBB);
}
