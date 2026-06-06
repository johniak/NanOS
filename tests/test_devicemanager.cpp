#include "doctest.h"
#include "DeviceManager.h"
#include "RamBlockDevice.h"

using namespace kernel;

// The registry is process-global AND keeps its entries for the whole run, so
// registered devices must outlive the test case (static storage, never stack)
// and tests are written order-independently (relative to the entry count).
TEST_CASE("DeviceManager registers, counts and retrieves by index") {
	static char buf[512] = {0};
	static RamBlockDevice dev("rmA", buf, sizeof(buf));
	int before = DeviceManager::count();

	DeviceManager::registerDevice(&dev);

	CHECK(DeviceManager::count() == before + 1);
	CHECK(DeviceManager::get(before) == &dev);
}

TEST_CASE("DeviceManager looks up by name and fails cleanly when absent") {
	static char buf[512] = {0};
	static RamBlockDevice dev("rmByName", buf, sizeof(buf));
	DeviceManager::registerDevice(&dev);

	CHECK(DeviceManager::getByName("rmByName") == &dev);
	CHECK(DeviceManager::getByName("no-such-device") == (BlockDevice*)0);
	CHECK(DeviceManager::get(99999) == (BlockDevice*)0);   // out of range
	CHECK(DeviceManager::get(-1) == (BlockDevice*)0);
}
