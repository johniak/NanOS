#pragma once
#include "BlockDevice.h"
#include "String.h"

namespace kernel {

// Runtime registry of block devices. Drivers (in-kernel today, loadable kexts
// later) call registerDevice() to publish a device; the rest of the system
// looks them up by index or name.
class DeviceManager {
public:
	static void registerDevice(BlockDevice* dev);
	static int count();
	static BlockDevice* get(int index);          // 0 if out of range
	static BlockDevice* getByName(String name);   // 0 if not found
};

}
