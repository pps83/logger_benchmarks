// The Release build fails when one of these LOGs makes a call (LogCallSiteCheck.cmake)
#include "log.h"

extern "C" void logCallSiteLiteral()
{
    LOG("call site literal");
}

extern "C" void logCallSiteNumbers(int a, int b, double c)
{
    LOG("call site numbers {} {} {}", a, b, c);
}
