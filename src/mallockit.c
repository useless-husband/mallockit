/* Unity build: the whole allocator as one translation unit, so the
 * compiler can inline across layers (e.g. the segment-map test into the
 * interposed free). The layers stay in their own files for reading. */
#include "os.c"
#include "segment.c"
#include "page.c"
#include "heap.c"
#include "alloc.c"
#include "debug.c"
#include "override.c"
