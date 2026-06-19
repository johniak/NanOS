#include "doctest.h"
#include "UsbHid.h"
using namespace kernel;
TEST_CASE("boot keyboard report -> key events via sink") {
    struct Cap { int n; uint8_t code[8]; uint8_t down[8]; } cap{};
    auto sink=[](void* u,uint8_t keycode,uint8_t down){ auto c=(Cap*)u; c->code[c->n]=keycode; c->down[c->n]=down; c->n++; };
    HidKeyboard kb; kb.sink=sink; kb.user=&cap;
    // boot kbd report: [mods][reserved][6 keycodes]. Press 'A' (usage 0x04).
    uint8_t r1[8]={0,0, 0x04,0,0,0,0,0}; hidKeyboardReport(&kb, r1, 8);
    REQUIRE(cap.n==1); CHECK(cap.down[0]==1);
    uint8_t r2[8]={0,0, 0,0,0,0,0,0};    hidKeyboardReport(&kb, r2, 8);   // release
    REQUIRE(cap.n==2); CHECK(cap.down[1]==0); CHECK(cap.code[1]==cap.code[0]);
}
TEST_CASE("boot mouse report -> rel + buttons via sink") {
    struct Cap{int dx,dy,btn,syn;} cap{};
    HidMouse m; m.relSink=[](void*u,int dx,int dy){auto c=(Cap*)u;c->dx=dx;c->dy=dy;};
    m.btnSink=[](void*u,int b,int down){((Cap*)u)->btn=(down<<8)|b;};
    m.synSink=[](void*u){((Cap*)u)->syn++;}; m.user=&cap;
    uint8_t r[3]={0x01, 5, (uint8_t)-3}; hidMouseReport(&m, r, 3);   // button1 down, dx=5, dy=-3
    CHECK(cap.dx==5); CHECK(cap.dy==-3); CHECK(cap.syn==1);
    CHECK(cap.btn==((1<<8)|0x110));   // BTN_LEFT down
    uint8_t r2[3]={0x00, 0, 0}; hidMouseReport(&m, r2, 3);   // release button1, no movement
    CHECK(cap.btn==((0<<8)|0x110));   // BTN_LEFT up
    CHECK(cap.syn==2);
}
TEST_CASE("hidUsageToScancode covers the mapped ranges, 0 elsewhere") {
    // Anchors across each block: letters, digits, named keys, F-keys, extended arrows.
    CHECK(hidUsageToScancode(0x04)==0x1E);   // 'a'
    CHECK(hidUsageToScancode(0x1D)==0x2C);   // 'z'
    CHECK(hidUsageToScancode(0x1E)==0x02);   // '1'
    CHECK(hidUsageToScancode(0x27)==0x0B);   // '0'
    CHECK(hidUsageToScancode(0x28)==0x1C);   // Enter
    CHECK(hidUsageToScancode(0x2C)==0x39);   // Space
    CHECK(hidUsageToScancode(0x3A)==0x3B);   // F1
    CHECK(hidUsageToScancode(0x45)==0x58);   // F12
    CHECK(hidUsageToScancode(0x52)==(0x80|0x48));   // Up (extended)
    CHECK(hidUsageToScancode(0x00)==0);
    CHECK(hidUsageToScancode(0xFF)==0);
    // Every usage in the keyboard block resolves without crashing; mapped ones are nonzero.
    int mapped=0; for (int u=0x04; u<=0x52; u++) if (hidUsageToScancode((uint8_t)u)) mapped++;
    CHECK(mapped>=60);
}
TEST_CASE("keyboard modifier edges emit shift down/up") {
    struct Cap{int n; uint8_t code[8]; uint8_t down[8];} cap{};
    auto sink=[](void*u,uint8_t kc,uint8_t d){auto c=(Cap*)u;c->code[c->n]=kc;c->down[c->n]=d;c->n++;};
    HidKeyboard kb; kb.sink=sink; kb.user=&cap;
    uint8_t r1[8]={0x02,0,0,0,0,0,0,0}; hidKeyboardReport(&kb,r1,8);  // LShift down (bit1)
    REQUIRE(cap.n==1); CHECK(cap.code[0]==0x2A); CHECK(cap.down[0]==1);
    uint8_t r2[8]={0,0,0,0,0,0,0,0};    hidKeyboardReport(&kb,r2,8);  // LShift up
    REQUIRE(cap.n==2); CHECK(cap.code[1]==0x2A); CHECK(cap.down[1]==0);
    uint8_t bad[4]={0,0,0,0}; hidKeyboardReport(&kb,bad,4);           // too short -> ignored
    CHECK(cap.n==2);
}
