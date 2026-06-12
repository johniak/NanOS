/*
 * NetStats.h — global protocol counters backing /proc/net/snmp. A single zero-initialised POD
 * (lives in .bss, so no global constructor needed) incremented at the main RX/TX points of the
 * IP/ICMP/TCP/UDP modules. Not every Linux SNMP field is tracked — the ones we don't maintain
 * are reported as 0 (honest: we never claim a number we didn't count). Reset for host tests.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetStats {
	// IP
	uint64_t ipInReceives, ipInHdrErrors, ipInAddrErrors, ipInDelivers, ipOutRequests, ipOutNoRoutes;
	uint64_t ipReasmReqds, ipReasmOKs, ipReasmFails, ipFragOKs, ipFragFails, ipFragCreates;
	// ICMP
	uint64_t icmpInMsgs, icmpInErrors, icmpInDestUnreachs, icmpInEchos, icmpInEchoReps;
	uint64_t icmpOutMsgs, icmpOutErrors, icmpOutDestUnreachs, icmpOutEchos, icmpOutEchoReps;
	// TCP
	uint64_t tcpActiveOpens, tcpPassiveOpens, tcpAttemptFails, tcpEstabResets;
	uint64_t tcpInSegs, tcpOutSegs, tcpRetransSegs, tcpInErrs, tcpOutRsts;
	// UDP
	uint64_t udpInDatagrams, udpNoPorts, udpInErrors, udpOutDatagrams;
};

extern NetStats g_netStats;   // zero-initialised in NetStats.cpp

void netStatsReset();          // host tests

}  // namespace kernel
