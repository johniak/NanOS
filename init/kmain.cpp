#include "Kernel.h"

using namespace kernel;

extern "C" void kmain(){
  Kernel kernel = Kernel();
  kernel.start();
}

extern "C" void __gxx_personality_v0();
void __gxx_personality_v0(){
  //To do
}   












