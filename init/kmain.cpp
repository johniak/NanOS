#include <stdint.h>
#include "Kernel.h"

using namespace kernel;

extern "C" void kmain(){
	extern uint32_t magic;
	if ( magic != 0x2BADB002 ){ 
	}
  Kernel kernel = Kernel(); 
  kernel.start();
}

extern "C" void __gxx_personality_v0();
void __gxx_personality_v0(){
  //To do
}   












