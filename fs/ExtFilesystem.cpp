#include "ExtFilesystem.h"

// All ExtFilesystem methods are defined inline in the header; this translation unit exists so the
// build has an ExtFilesystem.o to link. (It previously held a float ceil() helper, replaced by
// integer ceiling division in the header — the x86_64 kernel forbids FP via -mno-sse.)
namespace kernel {
} /* namespace kernel */
