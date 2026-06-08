#include "doctest.h"
#include "String.h"
#include <cstring>

// These tests run on the host with libc malloc/free (via host_shims). libc's allocator
// detects double-free/heap corruption, so a broken value-semantics implementation would
// crash here — making this a real guard for the String RAII change.

TEST_CASE("String construction from C-string and int") {
	String a("hello");
	CHECK(a.getLenght() == 5);
	CHECK(strcmp((char*) a, "hello") == 0);
	String n(42);
	CHECK(strcmp((char*) n, "42") == 0);
	String empty;
	CHECK(empty.getLenght() == 0);
}

TEST_CASE("copy constructor makes an independent buffer") {
	String a("original");
	String b(a);                       // deep copy
	CHECK(strcmp((char*) b, "original") == 0);
	a.append("XYZ");                   // mutating a must not touch b
	CHECK(strcmp((char*) b, "original") == 0);
	CHECK(strcmp((char*) a, "originalXYZ") == 0);
}

TEST_CASE("assignment is independent and self-assignment safe") {
	String a("aaa");
	String b("bbbbb");
	b = a;                             // frees b's old buffer, deep-copies a
	CHECK(strcmp((char*) b, "aaa") == 0);
	a.append("ZZ");
	CHECK(strcmp((char*) b, "aaa") == 0);   // b unaffected
	b = b;                             // self-assignment must not corrupt
	CHECK(strcmp((char*) b, "aaa") == 0);
}

TEST_CASE("append concatenates without double-free; empty start works") {
	String s;
	s.append("ab");                    // append onto an empty (textArray==0) String
	s.append("cd");
	CHECK(strcmp((char*) s, "abcd") == 0);
	CHECK(s.getLenght() == 4);
}

TEST_CASE("indexOf / startsWith / compareTo") {
	String s("hello world");
	CHECK(s.indexOf(String("world")) == 6);
	CHECK(s.indexOf(String("xyz")) == -1);
	CHECK(s.startsWith(String("hello")));
	CHECK(!s.startsWith(String("world")));
	CHECK(s.compareTo(String("hello world")) == 0);
	CHECK(s.compareTo(String("hello")) != 0);
}

// A function returning a String by value: exercises copy/RVO + destructor of the
// temporary. With value semantics this must not double-free.
static String makePath(const char* a, const char* b) {
	String s(a);
	s.append("/");
	s.append(b);
	return s;
}

TEST_CASE("return-by-value String round-trips safely") {
	String p = makePath("disks", "main");
	CHECK(strcmp((char*) p, "disks/main") == 0);
	String q = p;                      // another copy
	CHECK(strcmp((char*) q, "disks/main") == 0);
}
