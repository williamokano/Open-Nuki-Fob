// Builds the unmodified external/sha2.c with BYTE_ORDER / LITTLE_ENDIAN from newlib
// (the nRF Makefile passes -DBYTE_ORDER=LITTLE_ENDIAN instead).
#include <machine/endian.h>
// Third-party code: header/definition array bounds differ (harmless), newer GCC flags it
#pragma GCC diagnostic ignored "-Warray-parameter"
#include "../../external/sha2.c"
