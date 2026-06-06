#include "DeviceManager.h"
#include "List.h"

namespace kernel {

// Lazily allocated so we never depend on global constructors running — the
// kernel's loader does not invoke .ctors. A zero-initialized pointer lives in
// .bss (cleared by the multiboot loader), which is safe.
static List<BlockDevice*>* devices = 0;

static List<BlockDevice*>* deviceList() {
	if (devices == 0)
		devices = new List<BlockDevice*>();
	return devices;
}

void DeviceManager::registerDevice(BlockDevice* dev) {
	deviceList()->add(dev);
}

int DeviceManager::count() {
	return deviceList()->getCount();
}

BlockDevice* DeviceManager::get(int index) {
	if (index < 0 || index >= deviceList()->getCount())
		return 0;
	return (*deviceList())[index];
}

BlockDevice* DeviceManager::getByName(String name) {
	List<BlockDevice*>* list = deviceList();
	for (int i = 0; i < list->getCount(); i++) {
		if (name.compareTo((*list)[i]->name()) == 0)
			return (*list)[i];
	}
	return 0;
}

}
