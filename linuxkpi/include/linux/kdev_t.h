#ifndef _LKPI_KDEV_T_H
#define _LKPI_KDEV_T_H
#define MINORBITS 20
#define MINORMASK ((1U<<MINORBITS)-1)
#define MAJOR(dev) ((unsigned)((dev)>>MINORBITS))
#define MINOR(dev) ((unsigned)((dev)&MINORMASK))
#define MKDEV(ma,mi) (((ma)<<MINORBITS)|(mi))
#endif
