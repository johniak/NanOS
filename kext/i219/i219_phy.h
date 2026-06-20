/*
 * i219_phy.h — Intel I219 (ich9lan) PHY/ME bring-up for the i219 NIC kext.
 *
 * The I219 is a PCH-integrated MAC + separate PHY that shares the PHY and NVM with the Management
 * Engine, so the driver must take the SW/FW semaphore before touching the PHY (over MDIC). This is
 * the only machine-dependent-beyond-e1000 piece; it plugs into E1000Core via the phyBringup hook.
 */
#pragma once
#include <stdint.h>
struct E1000Core;

// Pure (host-tested): build the e1000 MDIC command word for a PHY read/write.
//   phyAddr: PHY address (I219 = 1), phyReg: register 0..31, data: write data (16-bit, ignored on read).
uint32_t mdicCmd(bool write, uint8_t phyAddr, uint8_t phyReg, uint16_t data);

// E1000Variant.phyBringup hook: acquire the ME/SW semaphore, disable ULP, confirm the PHY answers
// over MDIC, release the semaphore. Returns true if the PHY is usable (else the kext aborts cleanly).
bool i219PhyBringup(E1000Core* c);
