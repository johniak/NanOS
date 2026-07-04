/* linuxkpi/include/video/mipi_display.h — MIPI DCS/DSI command opcodes (i915 DSI panel path). */
#ifndef _LKPI_VIDEO_MIPI_DISPLAY_H
#define _LKPI_VIDEO_MIPI_DISPLAY_H
#define MIPI_DCS_GET_DISPLAY_BRIGHTNESS  0x52
#define MIPI_DCS_SET_DISPLAY_BRIGHTNESS  0x51
#define MIPI_DCS_GET_CONTROL_DISPLAY     0x54
#define MIPI_DCS_SET_CONTROL_DISPLAY     0x53
#define MIPI_DCS_GET_POWER_SAVE          0x56
#define MIPI_DCS_SET_POWER_SAVE          0x55
#define MIPI_DCS_SET_DISPLAY_ON          0x29
#define MIPI_DCS_SET_DISPLAY_OFF         0x28
#define MIPI_DCS_ENTER_SLEEP_MODE        0x10
#define MIPI_DCS_EXIT_SLEEP_MODE         0x11
#endif
