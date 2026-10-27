// The one translation unit that compiles dr_flac's implementation (third_party/dr_libs/dr_flac.h,
// mackron/dr_libs d422edf, public domain / MIT-0; see third_party/dr_libs/README.md).
// Only file decoding is used: no stdio-free build, no Ogg container.
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG

#if defined(_MSC_VER)
#	pragma warning(push, 0)
#	pragma warning(disable : 4244 4267 4456 4706 4701 4703)
#elif defined(__GNUC__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wall"
#	pragma GCC diagnostic ignored "-Wextra"
#endif

#include "dr_libs/dr_flac.h"

#if defined(_MSC_VER)
#	pragma warning(pop)
#elif defined(__GNUC__)
#	pragma GCC diagnostic pop
#endif
