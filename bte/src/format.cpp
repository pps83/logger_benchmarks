#include "format.h"
#include "UninitializedMemoryHacks.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <format>
#include <iterator>
#include <stdexcept>

#undef fmtStr
#undef fmtStrAppend
#undef throw_runtime_error

bool debuggerPresent();

NORETURN void throw_runtime_error(const char* fmt, ...)
{
    std::string ret;
    APPEND_FORMAT(ret, fmt);
    LLOG("RUNTIME ERROR: %s", ret.c_str());
    LLOG("");
#ifndef NDEBUG
    if (debuggerPresent())
    {
        assert(0);
    }
#endif
    throw std::runtime_error(ret);
}

std::string fmtStr(const char* fmt, ...)
{
    std::string ret;
    APPEND_FORMAT(ret, fmt);
    return ret;
}

void fmtStrAppend(std::string& str, const char* fmt, ...)
{
    APPEND_FORMAT(str, fmt);
}

void fmtStrAppendV(std::string& ret, const char *fmt, va_list argPtr)
{
    if (!fmt)
        return;

    // Limit to 2MB string size.
    const size_t maxSize = 2000000;

#if !defined(_MSC_VER) || _MSC_VER >= 1900
#define NEW_CAP(s) ((((s) + 256) & ~255) - 1)
    const size_t sz0 = ret.size();
    folly::resizeWithoutInitialization(ret, ret.capacity());

    va_list argPtrCopy;
    va_copy(argPtrCopy, argPtr);
    int x0 = vsnprintf(&ret[0] + sz0, ret.size() - sz0 + 1, fmt, argPtrCopy);
    va_end(argPtrCopy);
    if (x0 < 0)
    {
        ret.resize(sz0);
        return;
    }
    size_t x = (size_t)x0;
    if (x > ret.size() - sz0)
    {
        size_t capacity = NEW_CAP(x + sz0 + 16);
        ret.reserve(capacity > maxSize ? maxSize : capacity);
        folly::resizeWithoutInitialization(ret, ret.capacity());
        x0 = vsnprintf(&ret[0] + sz0, ret.size() - sz0 + 1, fmt, argPtr);
        if (x0 < 0)
        {
            ret.resize(sz0);
            return;
        }
        x = (size_t)x0;
    }
    if (x > 0 && x <= ret.size() - sz0)
        ret.resize(sz0 + x);
#undef NEW_CAP
#else
    va_list argPtrCopy;
    va_copy(argPtrCopy, argPtr);
    int actualSize = _vscprintf(fmt, argPtrCopy);
    va_end(argPtrCopy);

    if (actualSize < 0)
        actualSize = 0;
    if (actualSize > (int)maxSize)
        actualSize = (int)maxSize;

    size_t sz = ret.size();
    ret.resize(sz + actualSize);
    vsnprintf(&ret[0] + sz, actualSize + 1, fmt, argPtr);
    ret.resize(sz + actualSize);
#endif
}

std::string fmtStrV(const char *fmt, va_list argPtr)
{
    std::string ret;
    fmtStrAppendV(ret, fmt, argPtr);
    return ret;
}

void logdetail::formatPrintf(std::string& out, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    const int length = vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (length <= 0)
    {
        out.clear();
    }
    else
    {
        out.resize(static_cast<size_t>(length) + 1);
        vsnprintf(out.data(), out.size(), format, args);
        out.resize(static_cast<size_t>(length));
    }
    va_end(args);
}

template <typename Out>
static void appendQuotedTo(Out& out, std::string_view text)
{
    out += '"';
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    out += '"';
}

void logdetail::appendQuoted(std::string& out, std::string_view text)
{
    appendQuotedTo(out, text);
}

void logdetail::appendQuoted(BoundedText& out, std::string_view text)
{
    appendQuotedTo(out, text);
}

void logdetail::formatFmt(std::string& out, std::string_view format, std::format_args args)
{
    out.clear();
    // Thrown on the logger thread, it would stop the queue at this record
    try
    {
        std::vformat_to(std::back_inserter(out), format, args);
    }
    catch (const std::format_error& error)
    {
        out = std::format("bad format \"{}\": {}", format, error.what());
    }
}
