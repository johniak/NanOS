/*
 * NetCore.h — kernel-side bring-up of the net stack: installs the MI core's wake + IRQ-guard
 * hooks, creates the loopback device, and spawns the RX softirq thread (ksoftirqd-net) that
 * drains the backlog outside IRQ context. Called once during scheduler setup.
 */
#pragma once
#include "knx_net.h"   // the driver-facing exports live here (also in the KernelExports table)

namespace kernel {

struct Task;

// Install hooks + lo + spawn the softirq task (id 2). Returns the softirq Task* so the caller
// can registerKthread() it for /proc visibility. Call AFTER Scheduler::init(), before start().
Task* netCoreInit();

// The periodic protocol-timer thread (TCP RTO/TIME-WAIT, ARP/IP-reasm aging). Register as a kthread.
Task* netTimerThread();

// Configure the primary interface + default route (static fallback; FAZA 10 adds DHCP).
void netBringUp();

}  // namespace kernel
