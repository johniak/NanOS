#include "doctest.h"
#include "Pipe.h"
#include "Syscall.h"
#include "Vfs.h"
#include <cstring>

using namespace kernel;

static int nullsink(const char*, unsigned n) { return (int) n; }

TEST_CASE("Pipe ring buffer round-trips bytes") {
	Pipe p;
	p.addReader();
	p.addWriter();
	CHECK(p.write("hello", 5) == 5);
	CHECK(p.readable());
	char b[8] = {0};
	CHECK(p.read(b, 8) == 5);
	CHECK(strncmp(b, "hello", 5) == 0);
	CHECK_FALSE(p.readable());
}

TEST_CASE("Pipe reports EOF only after all writers close") {
	Pipe p;
	p.addReader();
	p.addWriter();
	CHECK_FALSE(p.atEof());          // empty but a writer is open -> would block, not EOF
	p.dropWriter();
	CHECK(p.atEof());                // empty + no writers -> EOF
}

TEST_CASE("Syscalls pipe round-trips and reports EOF on write-end close") {
	Vfs vfs;
	Syscalls sys(&vfs, nullsink);
	int fd[2];
	REQUIRE(sys.pipe(fd) == 0);
	CHECK(fd[0] >= 3);
	CHECK(fd[1] >= 3);
	CHECK(fd[0] != fd[1]);
	CHECK(sys.write(fd[1], "abc", 3) == 3);
	char b[8] = {0};
	CHECK(sys.read(fd[0], b, 8) == 3);
	CHECK(strncmp(b, "abc", 3) == 0);
	CHECK(sys.read(fd[0], b, 8) == -EAGAIN);   // empty, writer still open -> would block
	sys.close(fd[1]);
	CHECK(sys.read(fd[0], b, 8) == 0);         // writer closed -> EOF
}

TEST_CASE("Syscalls dup2 aliases a pipe write end (shared backing + refcount)") {
	Vfs vfs;
	Syscalls sys(&vfs, nullsink);
	int fd[2];
	sys.pipe(fd);
	CHECK(sys.dup2(fd[1], 7) == 7);
	CHECK(sys.write(7, "xy", 2) == 2);          // writing the dup reaches the same pipe
	char b[4] = {0};
	CHECK(sys.read(fd[0], b, 4) == 2);
	CHECK(strncmp(b, "xy", 2) == 0);
	sys.close(fd[1]);                           // one writer closed, fd 7 still open
	CHECK(sys.read(fd[0], b, 4) == -EAGAIN);    // not EOF yet
	sys.close(7);                               // last writer gone
	CHECK(sys.read(fd[0], b, 4) == 0);          // now EOF
}

TEST_CASE("Syscalls dup gives a new descriptor for the same end") {
	Vfs vfs;
	Syscalls sys(&vfs, nullsink);
	int fd[2];
	sys.pipe(fd);
	int d = sys.dup(fd[0]);
	CHECK(d >= 3);
	CHECK(d != fd[0]);
	sys.write(fd[1], "q", 1);
	char b[2] = {0};
	CHECK(sys.read(d, b, 2) == 1);              // the dup reads the same pipe
	CHECK(b[0] == 'q');
}

TEST_CASE("Syscalls pollScan reports pipe readiness") {
	Vfs vfs;
	Syscalls sys(&vfs, nullsink);
	int fd[2];
	sys.pipe(fd);
	PollFd pf[1] = {{ fd[0], POLLIN, 0 }};
	CHECK(sys.pollScan(pf, 1) == 0);            // nothing buffered yet
	CHECK(pf[0].revents == 0);
	sys.write(fd[1], "z", 1);
	CHECK(sys.pollScan(pf, 1) == 1);            // now readable
	CHECK((pf[0].revents & POLLIN) != 0);
}

TEST_CASE("Syscalls dup2 of a bad fd fails") {
	Vfs vfs;
	Syscalls sys(&vfs, nullsink);
	CHECK(sys.dup2(20, 7) == -EBADF);           // fd 20 not open
}
