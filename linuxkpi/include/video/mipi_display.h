/* linuxkpi/include/video/mipi_display.h — MIPI DCS/DSI command opcodes (i915 DSI panel path). */
#ifndef _LKPI_VIDEO_MIPI_DISPLAY_H
#define _LKPI_VIDEO_MIPI_DISPLAY_H
#define MIPI_DCS_GET_DISPLAY_BRIGHTNESS  0x52
#define MIPI_DCS_SET_DISPLAY_BRIGHTNESS  0x51
#define MIPI_DCS_GET_CONTROL_DISPLAY     0x54
#define MIPI_DCS_SET_CONTROL_DISPLAY     0x53
#define MIPI_DCS_WRITE_CONTROL_DISPLAY   0x53
#define MIPI_DCS_READ_CONTROL_DISPLAY    0x54
#define MIPI_DCS_GET_POWER_SAVE          0x56
#define MIPI_DCS_SET_POWER_SAVE          0x55
#define MIPI_DCS_WRITE_POWER_SAVE        0x55
#define MIPI_DCS_READ_POWER_SAVE         0x56
#define MIPI_DCS_SET_DISPLAY_ON          0x29
#define MIPI_DCS_SET_DISPLAY_OFF         0x28
#define MIPI_DCS_ENTER_SLEEP_MODE        0x10
#define MIPI_DCS_EXIT_SLEEP_MODE         0x11

/* MIPI DSI processor-to-peripheral transaction data types (from the MIPI DSI spec;
 * values verbatim from Linux include/video/mipi_display.h). intel_dsi_vbt.c switches on
 * these to emit VBT panel-init sequences over the DSI link. */
enum {
	MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM	= 0x03,
	MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM	= 0x13,
	MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM	= 0x23,
	MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM	= 0x04,
	MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM	= 0x14,
	MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM	= 0x24,
	MIPI_DSI_DCS_SHORT_WRITE		= 0x05,
	MIPI_DSI_DCS_SHORT_WRITE_PARAM		= 0x15,
	MIPI_DSI_DCS_READ			= 0x06,
	MIPI_DSI_GENERIC_LONG_WRITE		= 0x29,
	MIPI_DSI_DCS_LONG_WRITE			= 0x39,
};
#endif
