// Host doctests for the LinuxKPI request_firmware path (linuxkpi/kpi_firmware.c). The VFS read is
// stubbed by host_shims.cpp (lkpi_test_set_file registers a single fake file at an exact path).
#include "doctest.h"
extern "C" {
#include "linux/firmware.h"
#include "linux/errno.h"
}

// From host_shims.cpp: register the one fake file request_firmware will resolve.
extern "C" void lkpi_test_set_file(const char* path, const void* data, unsigned long size);

TEST_CASE("request_firmware: missing blob returns -ENOENT and clears fw") {
	lkpi_test_set_file(0, 0, 0);
	const struct firmware* fw = (const struct firmware*)0xdeadbeef;
	int r = request_firmware(&fw, "i915/nonexistent.bin", 0);
	CHECK(r == -ENOENT);
	CHECK(fw == 0);
}

TEST_CASE("request_firmware: resolves /disks/main/nanos/firmware/<name> and loads bytes") {
	static const unsigned char blob[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	lkpi_test_set_file("/disks/main/nanos/firmware/i915/guc.bin", blob, 4);
	const struct firmware* fw = 0;
	int r = request_firmware(&fw, "i915/guc.bin", 0);
	REQUIRE(r == 0);
	REQUIRE(fw != 0);
	CHECK(fw->size == 4);
	CHECK(fw->data[0] == 0xDE);
	CHECK(fw->data[3] == 0xEF);
	release_firmware(fw);
}

TEST_CASE("request_firmware: a set file with a different name still misses") {
	static const unsigned char blob[2] = { 1, 2 };
	lkpi_test_set_file("/disks/main/nanos/firmware/a.bin", blob, 2);
	const struct firmware* fw = 0;
	CHECK(request_firmware(&fw, "b.bin", 0) == -ENOENT);
	CHECK(fw == 0);
}

TEST_CASE("request_firmware: NULL out/name arguments return -EINVAL") {
	CHECK(request_firmware(0, "x", 0) == -EINVAL);
	const struct firmware* fw = 0;
	CHECK(request_firmware(&fw, 0, 0) == -EINVAL);
	CHECK(fw == 0);
}

TEST_CASE("firmware_request_nowarn / request_firmware_direct share the loader") {
	static const unsigned char blob[1] = { 0x7F };
	lkpi_test_set_file("/disks/main/nanos/firmware/dmc.bin", blob, 1);
	const struct firmware* fw = 0;
	REQUIRE(firmware_request_nowarn(&fw, "dmc.bin", 0) == 0);
	CHECK(fw->size == 1);
	release_firmware(fw);
	fw = 0;
	REQUIRE(request_firmware_direct(&fw, "dmc.bin", 0) == 0);
	CHECK(fw->data[0] == 0x7F);
	release_firmware(fw);
}
