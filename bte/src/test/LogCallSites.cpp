// The Release build fails when a LOG here makes a call, a logCallSiteOne one other than one (LogCallSiteCheck.cmake)
#include "log.h"
#include <format>
#include <string>

struct CallSitePoint
{
    int x;
};

template <>
struct std::formatter<CallSitePoint> : std::formatter<int>
{
    auto format(CallSitePoint point, std::format_context& context) const
    {
        return std::formatter<int>::format(point.x, context);
    }
};

extern "C" void logCallSiteLiteral()
{
    LOG("call site literal");
}

extern "C" void logCallSiteNumbers(int a, int b, double c)
{
    LOG("call site numbers {} {} {}", a, b, c);
}

extern "C" void logCallSiteQuotedArg(int a)
{
    LOG("call site quoted arg %s %d", "a literal", a);
}

extern "C" void logCallSitePointer(const void* pointer, int a)
{
    LOG("call site pointer %p %d", pointer, a);
}

extern "C" void logCallSiteArgs(int x, int y)
{
    LOG_ARGS(x, y);
}

extern "C" void logCallSiteOneText(const char* text, int a)
{
    LOG("call site text %s %d", text, a);
}

extern "C" void logCallSiteOneString(const std::string& text, const char* other)
{
    LOG("call site string {} {} {}", text, other, "literal");
}

extern "C" void logCallSiteOneLone(const char* text)
{
    LOG(text);
}

extern "C" void logCallSiteOneStdString(const std::string& text)
{
    LOG("call site std string {}", text);
}

extern "C" void logCallSiteOneWholeString(const std::string& text)
{
    LOG(text);
}

extern "C" void logCallSiteOneCStr(const std::string& text)
{
    LOG("call site c string %s", text.c_str());
}

extern "C" void logCallSiteOneCustom(int x)
{
    LOG("call site custom {}", CallSitePoint {x});
}

extern "C" void logCallSiteOneLlog(const char* text)
{
    LLOG("call site llog %s", text);
}
