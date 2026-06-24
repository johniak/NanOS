#include "DeviceManager.h"
#include "List.h"
#include "Spinlock.h"   // SMP: serialize register/lookup of the shared device list

namespace kernel {

// Lazily allocated so we never depend on global constructors running — the
// kernel's loader does not invoke .ctors. A zero-initialized pointer lives in
// .bss (cleared by the multiboot loader), which is safe.
static List<BlockDevice*>* devices = 0;
// SMP: one CPU may registerDevice() while another does getByName(); guard the
// (lazily created) list. .bss-zeroed Spinlock = unlocked, no ctor needed.
static Spinlock devLock;

// Caller must hold devLock (so the lazy `new` is not raced by two CPUs).
static List<BlockDevice*>* deviceList() {
	if (devices == 0)
		devices = new List<BlockDevice*>();
	return devices;
}

void DeviceManager::registerDevice(BlockDevice* dev) {
	SpinIrqGuard g(devLock);
	deviceList()->add(dev);
}

int DeviceManager::count() {
	SpinIrqGuard g(devLock);
	return deviceList()->getCount();
}

BlockDevice* DeviceManager::get(int index) {
	SpinIrqGuard g(devLock);
	if (index < 0 || index >= deviceList()->getCount())
		return 0;
	return (*deviceList())[index];
}

BlockDevice* DeviceManager::getByName(String name) {
	SpinIrqGuard g(devLock);
	List<BlockDevice*>* list = deviceList();
	for (int i = 0; i < list->getCount(); i++) {
		if (name.compareTo((*list)[i]->name()) == 0)
			return (*list)[i];
	}
	return 0;
}

}
