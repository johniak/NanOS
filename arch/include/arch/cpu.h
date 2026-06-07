/*
 * arch/cpu.h — MI/MD contract for core CPU control.
 */
#pragma once

namespace arch {

void cpuDisableInterrupts();
void cpuEnableInterrupts();
void cpuHalt();

}
