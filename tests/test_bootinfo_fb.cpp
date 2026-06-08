#include "doctest.h"
#include "MultibootInfo.h"
#include <cstring>
#include <cstddef>

using namespace kernel;

TEST_CASE("multibootFramebuffer parses a linear direct-RGB framebuffer") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof mbi);
	mbi.flags = MB_FLAG_FRAMEBUFFER;
	mbi.framebuffer_addr = 0xFD000000ULL;
	mbi.framebuffer_pitch = 4096;
	mbi.framebuffer_width = 1024;
	mbi.framebuffer_height = 768;
	mbi.framebuffer_bpp = 32;
	mbi.framebuffer_type = 1;   // direct RGB

	MbFramebuffer fb;
	REQUIRE(multibootFramebuffer(&mbi, &fb));
	CHECK(fb.addr == 0xFD000000ULL);
	CHECK(fb.pitch == 4096u);
	CHECK(fb.width == 1024u);
	CHECK(fb.height == 768u);
	CHECK(fb.bpp == 32);
}

TEST_CASE("multibootFramebuffer rejects absent flag / non-RGB / null") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof mbi);
	MbFramebuffer fb;

	mbi.framebuffer_type = 1;
	CHECK(!multibootFramebuffer(&mbi, &fb));        // flag clear

	mbi.flags = MB_FLAG_FRAMEBUFFER;
	mbi.framebuffer_type = 2;
	CHECK(!multibootFramebuffer(&mbi, &fb));        // EGA text
	mbi.framebuffer_type = 0;
	CHECK(!multibootFramebuffer(&mbi, &fb));        // indexed/palette

	CHECK(!multibootFramebuffer(nullptr, &fb));     // no info struct
}

// The framebuffer fields are load-bearing: GRUB writes them at fixed Multiboot-1
// offsets. A wrong struct layout silently reads garbage, so pin the offsets.
TEST_CASE("MultibootInfo framebuffer field offsets match the Multiboot 1 spec") {
	CHECK(offsetof(MultibootInfo, mmap_addr) == 48);
	CHECK(offsetof(MultibootInfo, framebuffer_addr) == 88);
	CHECK(offsetof(MultibootInfo, framebuffer_pitch) == 96);
	CHECK(offsetof(MultibootInfo, framebuffer_width) == 100);
	CHECK(offsetof(MultibootInfo, framebuffer_height) == 104);
	CHECK(offsetof(MultibootInfo, framebuffer_bpp) == 108);
	CHECK(offsetof(MultibootInfo, framebuffer_type) == 109);
}
