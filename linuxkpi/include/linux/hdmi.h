#ifndef _LKPI_HDMI_H
#define _LKPI_HDMI_H
#include <linux/types.h>
enum hdmi_picture_aspect { HDMI_PICTURE_ASPECT_NONE=0, HDMI_PICTURE_ASPECT_4_3, HDMI_PICTURE_ASPECT_16_9, HDMI_PICTURE_ASPECT_64_27, HDMI_PICTURE_ASPECT_256_135, HDMI_PICTURE_ASPECT_RESERVED };
enum hdmi_colorspace { HDMI_COLORSPACE_RGB=0, HDMI_COLORSPACE_YUV422, HDMI_COLORSPACE_YUV444, HDMI_COLORSPACE_YUV420 };
enum hdmi_quantization_range { HDMI_QUANTIZATION_RANGE_DEFAULT=0, HDMI_QUANTIZATION_RANGE_LIMITED, HDMI_QUANTIZATION_RANGE_FULL };
enum hdmi_quantization_range; enum hdmi_content_type { HDMI_CONTENT_TYPE_GRAPHICS=0 };
struct hdmi_avi_infoframe { unsigned char type, version, length; enum hdmi_colorspace colorspace; enum hdmi_picture_aspect picture_aspect; enum hdmi_quantization_range quantization_range; enum hdmi_quantization_range ycc_quantization_range; enum hdmi_content_type content_type; unsigned char hdmi_type; };
struct hdmi_vendor_infoframe { unsigned char type, version, length; };
union hdmi_infoframe { struct { unsigned char type, version, length; } any; struct hdmi_avi_infoframe avi; struct hdmi_vendor_infoframe vendor; };
struct hdr_sink_metadata { u32 metadata_type; };
#endif
