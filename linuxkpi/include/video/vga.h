/* linuxkpi/include/video/vga.h — legacy VGA register ports (i915 disables the VGA plane on init). */
#ifndef _LKPI_VIDEO_VGA_H
#define _LKPI_VIDEO_VGA_H
#define VGA_MIS_W        0x3c2   /* miscellaneous output write */
#define VGA_MIS_R        0x3cc   /* miscellaneous output read */
#define VGA_SEQ_I        0x3c4   /* sequencer index */
#define VGA_SEQ_D        0x3c5   /* sequencer data */
#define VGA_SR01_SCREEN_OFF 0x20 /* SR01 bit: screen disable */
#define VGA_DISP_DISABLE 0x01
#define VGA_MIS_ENB_MEM_ACCESS 0x02
#endif
