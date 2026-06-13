// FAZA G — AF_UNIX (local) sockets: socketpair byte channel, named stream bind/listen/connect/
// accept, EADDRINUSE/ECONNREFUSED, and DGRAM message boundaries. Pure MI — driven directly, no
// VFS/scheduler (the wake hook stays unset; these cases never block).
#include "doctest.h"
#include "Unix.h"
#include "Socket.h"
#include "NetBuf.h"
#include <cstring>

using namespace kernel;

static void reset() { socketReset(); unixReset(); }

// ---- SOCK_STREAM via socketpair: the simplest connected pair (also FAZA G3) -----------------

TEST_CASE("AF_UNIX socketpair: bidirectional byte stream + partial reads") {
	reset();
	Socket *a = nullptr, *b = nullptr;
	REQUIRE(unixSocketpair(SOCK_STREAM, 0, &a, &b) == 0);
	REQUIRE(a); REQUIRE(b);

	CHECK(unixSend(a, "hello", 5, 0, 0) == 5);
	char buf[16];
	CHECK(unixRecv(b, buf, 2, 0, 0, 0) == 2);            // partial read
	CHECK(std::memcmp(buf, "he", 2) == 0);
	CHECK(unixRecv(b, buf, 16, 0, 0, 0) == 3);           // remainder
	CHECK(std::memcmp(buf, "llo", 3) == 0);

	CHECK(unixSend(b, "PONG", 4, 0, 0) == 4);            // other direction
	CHECK(unixRecv(a, buf, 16, 0, 0, 0) == 4);
	CHECK(std::memcmp(buf, "PONG", 4) == 0);

	CHECK(unixRecv(a, buf, 16, 0, 0, 0) == -SOCK_EAGAIN); // nothing pending

	// The same exchange via the generic socketSendTo/socketRecvFrom seam (the path kernel
	// write()/read() actually take for an AF_UNIX fd).
	CHECK(socketSendTo(a, "Z", 1, 0, 0) == 1);
	uint32_t sip = 99; uint16_t sp = 99;
	CHECK(socketRecvFrom(b, buf, 16, &sip, &sp, 0) == 1);
	CHECK(buf[0] == 'Z');
	CHECK(sip == 0); CHECK(sp == 0);                     // AF_UNIX has no IP source address
	socketClose(a); socketClose(b);
}

TEST_CASE("AF_UNIX stream: peer close yields buffered bytes then EOF") {
	reset();
	Socket *a = nullptr, *b = nullptr;
	REQUIRE(unixSocketpair(SOCK_STREAM, 0, &a, &b) == 0);
	CHECK(unixSend(a, "bye", 3, 0, 0) == 3);
	socketClose(a);                                       // a gone; b can still drain then sees EOF
	char buf[16];
	CHECK(unixRecv(b, buf, 16, 0, 0, 0) == 3);            // buffered data survives the close
	CHECK(std::memcmp(buf, "bye", 3) == 0);
	CHECK(unixRecv(b, buf, 16, 0, 0, 0) == 0);            // EOF (not EAGAIN)
	CHECK(unixSend(b, "x", 1, 0, 0) == -SOCK_EPIPE);      // writing to a gone peer
	socketClose(b);
}

TEST_CASE("AF_UNIX stream: MSG_PEEK leaves bytes queued") {
	reset();
	Socket *a = nullptr, *b = nullptr;
	REQUIRE(unixSocketpair(SOCK_STREAM, 0, &a, &b) == 0);
	CHECK(unixSend(a, "data", 4, 0, 0) == 4);
	char buf[16];
	CHECK(unixRecv(b, buf, 16, MSG_PEEK, 0, 0) == 4);
	CHECK(unixRecv(b, buf, 16, 0, 0, 0) == 4);            // still there after the peek
	socketClose(a); socketClose(b);
}

// ---- named SOCK_STREAM: bind / listen / connect / accept (FAZA G1) --------------------------

TEST_CASE("AF_UNIX named stream: full connect/accept cycle + exchange") {
	reset();
	Socket* srv = socketCreate(AF_UNIX, SOCK_STREAM, 0, nullptr);
	REQUIRE(srv);
	const char* path = "/tmp/test.sock"; unsigned pl = std::strlen(path) + 1;
	CHECK(unixBind(srv, path, pl) == 0);
	CHECK(unixListen(srv, 5) == 0);
	CHECK(unixIsListening(srv));

	Socket* cli = socketCreate(AF_UNIX, SOCK_STREAM, 0, nullptr);
	REQUIRE(cli);
	CHECK(unixConnect(cli, path, pl) == 0);

	int err = -1;
	Socket* conn = unixAccept(srv, &err);                 // server-side endpoint
	REQUIRE(conn); CHECK(err == 0);
	CHECK(unixPeerOf(cli) == conn);
	CHECK(unixPeerOf(conn) == cli);

	CHECK(unixSend(cli, "ping", 4, 0, 0) == 4);
	char buf[16];
	CHECK(unixRecv(conn, buf, 16, 0, 0, 0) == 4);
	CHECK(std::memcmp(buf, "ping", 4) == 0);
	CHECK(unixSend(conn, "pong", 4, 0, 0) == 4);
	CHECK(unixRecv(cli, buf, 16, 0, 0, 0) == 4);
	CHECK(std::memcmp(buf, "pong", 4) == 0);

	socketClose(cli); socketClose(conn); socketClose(srv);
}

TEST_CASE("AF_UNIX named stream: EADDRINUSE and ECONNREFUSED") {
	reset();
	const char* path = "/tmp/dup.sock"; unsigned pl = std::strlen(path) + 1;

	Socket* s1 = socketCreate(AF_UNIX, SOCK_STREAM, 0, nullptr);
	CHECK(unixBind(s1, path, pl) == 0);
	Socket* s2 = socketCreate(AF_UNIX, SOCK_STREAM, 0, nullptr);
	CHECK(unixBind(s2, path, pl) == -SOCK_EADDRINUSE);    // already bound

	Socket* cli = socketCreate(AF_UNIX, SOCK_STREAM, 0, nullptr);
	CHECK(unixConnect(cli, "/tmp/nope.sock", 15) == -SOCK_ECONNREFUSED);  // nobody bound
	CHECK(unixConnect(cli, path, pl) == -SOCK_ECONNREFUSED);              // bound but not listening
	socketClose(s1); socketClose(s2); socketClose(cli);
}

TEST_CASE("AF_UNIX stream readable/writable for poll") {
	reset();
	Socket *a = nullptr, *b = nullptr;
	REQUIRE(unixSocketpair(SOCK_STREAM, 0, &a, &b) == 0);
	CHECK_FALSE(socketReadable(b));
	CHECK(socketWritable(a));
	CHECK(unixSend(a, "z", 1, 0, 0) == 1);
	CHECK(socketReadable(b));                              // data arrived -> POLLIN
	char buf[4]; unixRecv(b, buf, 4, 0, 0, 0);
	CHECK_FALSE(socketReadable(b));
	socketClose(a);
	CHECK(socketReadable(b));                              // EOF is also readable (recv returns 0)
	socketClose(b);
}

// ---- SOCK_DGRAM: message boundaries + named sendto (FAZA G2) ---------------------------------

TEST_CASE("AF_UNIX dgram: sendto by path preserves message boundaries") {
	reset();
	Socket* rx = socketCreate(AF_UNIX, SOCK_DGRAM, 0, nullptr);
	const char* path = "/tmp/dg.sock"; unsigned pl = std::strlen(path) + 1;
	CHECK(unixBind(rx, path, pl) == 0);

	Socket* tx = socketCreate(AF_UNIX, SOCK_DGRAM, 0, nullptr);
	CHECK(unixSend(tx, "aaa", 3, path, pl) == 3);
	CHECK(unixSend(tx, "bb", 2, path, pl) == 2);

	char buf[16];
	CHECK(unixRecv(rx, buf, 16, 0, 0, 0) == 3);           // first datagram whole
	CHECK(std::memcmp(buf, "aaa", 3) == 0);
	CHECK(unixRecv(rx, buf, 16, 0, 0, 0) == 2);           // second datagram separate
	CHECK(std::memcmp(buf, "bb", 2) == 0);
	CHECK(unixRecv(rx, buf, 16, 0, 0, 0) == -SOCK_EAGAIN);
	socketClose(rx); socketClose(tx);
}

TEST_CASE("AF_UNIX dgram: sendto unbound path is refused; connect sets default dest") {
	reset();
	Socket* tx = socketCreate(AF_UNIX, SOCK_DGRAM, 0, nullptr);
	CHECK(unixSend(tx, "x", 1, "/tmp/ghost.sock", 16) == -SOCK_ECONNREFUSED);

	Socket* rx = socketCreate(AF_UNIX, SOCK_DGRAM, 0, nullptr);
	const char* path = "/tmp/c.sock"; unsigned pl = std::strlen(path) + 1;
	CHECK(unixBind(rx, path, pl) == 0);
	CHECK(unixConnect(tx, path, pl) == 0);
	CHECK(unixSend(tx, "hi", 2, 0, 0) == 2);              // connected: no path needed
	char buf[8];
	CHECK(unixRecv(rx, buf, 8, 0, 0, 0) == 2);
	socketClose(rx); socketClose(tx);
}
