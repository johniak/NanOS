#ifndef _LKPI_SMP_H
#define _LKPI_SMP_H
#include <linux/types.h>
static inline int smp_processor_id(void){return 0;}
static inline unsigned int num_online_cpus(void){return 1;}
#define raw_smp_processor_id() 0
#define get_cpu() 0
#define put_cpu() do{}while(0)
#define on_each_cpu(f,i,w) do{(f)(i);}while(0)
#endif
