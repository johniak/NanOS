/*
 * NetLock.h — the coarse network-stack lock (SMP, Phase 4, Task 15e).
 *
 * One RECURSIVE lock serializes ALL access to the shared net stack — the socket table (g_socks),
 * TCP control blocks (g_tcbs), the ARP cache, routing, and the protocol receive/transmit paths.
 * Three contexts touch the stack, ALL thread context (never a hard IRQ — the NIC IRQ only enqueues
 * to the IRQ-guarded RX backlog): the RX-softirq kernel thread (netRxProcess), the net-timer kernel
 * thread (tcpTick/arpTick), and socket syscalls. It is taken at those BOUNDARIES; internal calls
 * (ipOutput, arpResolve, tcpRx, socketDeliver, ...) run under whichever boundary already holds it,
 * which is why the lock is recursive — a boundary that calls deeper net code re-enters on the same
 * CPU instead of self-deadlocking, while a different CPU blocks.
 *
 * Non-IRQ (RecursiveGuard): net ops never run in a hard IRQ, and a socket op can be lengthy (driver
 * TX), so keeping interrupts enabled avoids starving the BSP timer. Socket recv/accept/connect block
 * at the SYSCALL-DISPATCH layer (sleepOn the socket's wait queue) with NO net op on the stack, so the
 * lock is never held across a sleep. Lock order: g_netLock is taken AFTER m_fdLock (close()/fork copy
 * reach socketClose/socketRef while holding the fd lock) and BEFORE g_rqLock (socketDeliver wakes
 * readers via Scheduler::wakeAll); nothing acquires it in the reverse direction.
 */
#pragma once
#include "Spinlock.h"

namespace kernel {
extern RecursiveSpinlock g_netLock;
}
