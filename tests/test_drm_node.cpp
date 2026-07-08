#include "doctest.h"
#include "DrmDevice.h"
#include "knx_drm_node.h"
using namespace kernel;

static int g_calls = 0; static unsigned g_cmd; static int g_node; static int g_pid_seen;
static long fake_ioctl(int pid, int node, unsigned int cmd, void *arg) {
    g_calls++; g_cmd = cmd; g_node = node; g_pid_seen = pid; (void)arg; return 0;
}
static int fake_mmapoff(int, uint64_t off, uint64_t *phys, uint64_t *len) {
    if (off != 0x10000) return -22;
    *phys = 0xABC000; *len = 0x2000; return 0;
}
static int g_releases = 0;
static void fake_release(int) { g_releases++; }
static const knx_drm_ops FAKE = { fake_ioctl, fake_mmapoff, fake_release };

TEST_CASE("DrmDevice forwards ioctl with node kind and pid") {
    DrmDevice card(&FAKE, KNX_DRM_NODE_PRIMARY);
    DrmDevice rnd (&FAKE, KNX_DRM_NODE_RENDER);
    CHECK(card.ioctl(0xC0106442u /* DRM_IOCTL_VERSION-ish */, nullptr) == 0);
    CHECK(g_node == KNX_DRM_NODE_PRIMARY);
    CHECK(g_pid_seen == 42);   // host curPid() stub
    CHECK(rnd.ioctl(1, nullptr) == 0);
    CHECK(g_node == KNX_DRM_NODE_RENDER);
    CHECK(g_calls == 2);
}
TEST_CASE("DrmDevice with no ops returns -ENODEV") {
    DrmDevice d(nullptr, KNX_DRM_NODE_RENDER);
    CHECK(d.ioctl(1, nullptr) == -19);
}
TEST_CASE("DrmDevice fd close never releases the drm_file") {
    // Regression pin for Dell boot #43: userland churns short-lived fds on an in-use node
    // (dup, Mesa loader reopen, libdrm drmGetDevices2 sniff). A close-time release destroyed
    // the process's GPU state mid-render; the drm_file must die only with the process
    // (kernel::drmProcessExit from procExit/procKill — kernel build, not testable here).
    DrmDevice d(&FAKE, KNX_DRM_NODE_RENDER);
    g_releases = 0;
    d.open(); d.open();    // app fd + a sniffer's short-lived fd
    d.close();             // sniffer closes mid-render...
    d.close();             // ...and even the app's last close
    CHECK(g_releases == 0);
}
TEST_CASE("DrmDevice mmapAt resolves via mmap_offset") {
    DrmDevice d(&FAKE, KNX_DRM_NODE_RENDER);
    uint64_t phys = 0; unsigned len = 0;
    CHECK(d.mmapAt(0x10000, &phys, &len) == 0);
    CHECK(phys == 0xABC000); CHECK(len == 0x2000);
    CHECK(d.mmapAt(0x99999, &phys, &len) < 0);
    CHECK(d.mmapInfo(&phys, &len) < 0);   // offset-less mmap not allowed
}
