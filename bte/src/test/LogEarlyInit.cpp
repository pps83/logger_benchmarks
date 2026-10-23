#include "format.h"
#include <stdlib.h>

// Runs before most static initializers, possibly before the logger's own; only when LogTest asks for it
struct EarlyLog
{
    EarlyLog()
    {
        if (getenv("LOG_EARLY_INIT_TEST"))
            LOG("logged from an early initializer");
    }
};

#ifdef __GNUC__
static EarlyLog gEarlyLog __attribute__((init_priority(101)));
#else
#ifdef _MSC_VER
#pragma warning(disable : 4073) // initializers put in lib initialization area
#pragma init_seg(lib)
#endif
static EarlyLog gEarlyLog;
#endif
