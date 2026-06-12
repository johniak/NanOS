/*
 * NetStats.cpp — the single global protocol-counter instance (see NetStats.h). Zero-init in .bss.
 */
#include "NetStats.h"
#include <string.h>

namespace kernel {

NetStats g_netStats;

void netStatsReset() { memset(&g_netStats, 0, sizeof(g_netStats)); }

}  // namespace kernel
