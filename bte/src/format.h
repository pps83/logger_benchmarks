#pragma once
#include <stdarg.h>
#include <string>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#ifndef FORCEINLINE
#define FORCEINLINE inline __attribute__((__always_inline__))
#endif
#define NOINLINE __attribute__((__noinline__))
#define ALIGN(n) __attribute__((__aligned__(n)))
#define NORETURN __attribute__((__noreturn__))
#if defined(_WIN32) && !defined(__clang__)
#define PRINTF_ATTR(fmtIndex, argIndex) __attribute__((__format__(__gnu_printf__, fmtIndex, argIndex)))
#else
#define PRINTF_ATTR(fmtIndex, argIndex) __attribute__((__format__(__printf__, fmtIndex, argIndex)))
#endif
#elif defined(_MSC_VER)
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif
#define NOINLINE __declspec(noinline)
#define ALIGN(n) __declspec(align(n))
#define NORETURN __declspec(noreturn)
#define PRINTF_ATTR(fmtIndex, argIndex)
#else
#error Unsupported compiler
#endif

#if defined(__GNUC__) || defined(__clang__)
#define GCC_WARNING_DISABLE(warn) GCC_WARNING_DISABLE_(GCC diagnostic push) GCC_WARNING_DISABLE_(GCC diagnostic ignored #warn)
#define GCC_WARNING_RESTORE() _Pragma("GCC diagnostic pop")
#define GCC_WARNING_DISABLE_(warn) _Pragma(#warn)
#define MSVC_WARNING_DISABLE(warn)
#define MSVC_WARNING_RESTORE()
#else
#define MSVC_WARNING_DISABLE(warn) __pragma(warning(push)) __pragma(warning(disable: warn))
#define MSVC_WARNING_RESTORE() __pragma(warning(pop))
#define GCC_WARNING_DISABLE(warn)
#define GCC_WARNING_RESTORE()
#endif

/**
  Produces a string from arguments of the style of printf.  This avoids
  problems with buffer overflows when using sprintf and makes it easy
  to use the result functionally.
 */
std::string fmtStr(const char* fmt, ...) PRINTF_ATTR(1, 2);
void fmtStrAppend(std::string& str, const char* fmt, ...) PRINTF_ATTR(2, 3);

#define APPEND_FORMAT(str, fmt)        \
do {                                   \
    va_list argList;                   \
    va_start(argList, fmt);            \
    fmtStrAppendV(str, fmt, argList);  \
    va_end(argList);                   \
} while(0)

// Like fmtStr, but can be called with the argument list from a ... function.
std::string fmtStrV(const char* fmt, va_list args);
void fmtStrAppendV(std::string& str, const char* fmt, va_list args);

NORETURN void throw_runtime_error(const char* fmt, ...) /*throw(std::runtime_error)*/ PRINTF_ATTR(1, 2);

#define _IMPL_PP_CONCAT_(v1, v2) v1 ## v2
#define _IMPL_PP_CONCAT(v1, v2) _IMPL_PP_CONCAT_(v1, v2)

#define _IMPL_PP_CONCAT5_(_0, _1, _2, _3, _4) _0 ## _1 ## _2 ## _3 ## _4

#define _IMPL_PP_IDENTITY_(x) x
#define _IMPL_PP_IDENTITY(x) _IMPL_PP_IDENTITY_(x)

#define _IMPL_PP_VA_ARGS_(...) __VA_ARGS__
#define _IMPL_PP_VA_ARGS(...) _IMPL_PP_VA_ARGS_(__VA_ARGS__)

#define _IMPL_PP_IDENTITY_VA_ARGS_(x, ...) x, __VA_ARGS__
#define _IMPL_PP_IDENTITY_VA_ARGS(x, ...) _IMPL_PP_IDENTITY_VA_ARGS_(x, __VA_ARGS__)

#define _IMPL_PP_IIF_0(x, ...) __VA_ARGS__
#define _IMPL_PP_IIF_1(x, ...) x
#define _IMPL_PP_IIF(c) _IMPL_PP_CONCAT_(_IMPL_PP_IIF_, c)

#define _IMPL_PP_HAS_COMMA(...) _IMPL_PP_IDENTITY(_IMPL_PP_VA_ARGS_TAIL(__VA_ARGS__, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0))
#define _IMPL_PP_IS_EMPTY_TRIGGER_PARENTHESIS_(...) ,

#define _IMPL_PP_IS_EMPTY(...) _IMPL_PP_IS_EMPTY_( \
    /* test if there is just one argument, eventually an empty one */ \
    _IMPL_PP_HAS_COMMA(__VA_ARGS__),                                \
    /* test if _TRIGGER_PARENTHESIS_ together with the argument adds a comma */ \
    _IMPL_PP_HAS_COMMA(_IMPL_PP_IS_EMPTY_TRIGGER_PARENTHESIS_ __VA_ARGS__), \
    /* test if the argument together with a parenthesis adds a comma */ \
    _IMPL_PP_HAS_COMMA(__VA_ARGS__ ()),                             \
    /* test if placing it between _TRIGGER_PARENTHESIS_ and the parenthesis adds a comma */ \
    _IMPL_PP_HAS_COMMA(_IMPL_PP_IS_EMPTY_TRIGGER_PARENTHESIS_ __VA_ARGS__ ()))

#define _IMPL_PP_IS_EMPTY_(_0, _1, _2, _3) _IMPL_PP_HAS_COMMA(_IMPL_PP_CONCAT5_(_IMPL_PP_IS_EMPTY_IS_EMPTY_CASE_, _0, _1, _2, _3))
#define _IMPL_PP_IS_EMPTY_IS_EMPTY_CASE_0001 ,

#define _IMPL_PP_VA_ARGS_SIZE_(...) _IMPL_PP_IDENTITY(_IMPL_PP_VA_ARGS_TAIL(__VA_ARGS__))
#define _IMPL_PP_VA_ARGS_TAIL(_0,_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, x, ...) x
#define _IMPL_PP_VA_ARGS_SEQ64() 17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0

#define PP_VA_ARGS_SIZE(...) _IMPL_PP_IIF(_IMPL_PP_IS_EMPTY(__VA_ARGS__))(0, _IMPL_PP_VA_ARGS_SIZE_(__VA_ARGS__, _IMPL_PP_VA_ARGS_SEQ64()))


#define _IMPL_PP_VA_ARGS_01F(...) _IMPL_PP_IIF(_IMPL_PP_IS_EMPTY(__VA_ARGS__))(S, _IMPL_PP_VA_ARGS_SIZE_(__VA_ARGS__, _IMPL_PP_VA_ARGS_SEQ64_01F()))
#define _IMPL_PP_VA_ARGS_SEQ64_01F() F,F,F,F,F,F,F,F,F,F,F,F,F,F,F,F,S,S

// Where log lines go, any combination; 0 compiles out every LOG and LLOG, arguments too, so they must not change state
enum LogMode : unsigned
{
    LogFileTruncate = 1, // log.txt or LOG_FILE, emptied when a process opens it
    LogFileAppend = 2,   // the same file, appended to
    LogStdout = 4,
    LogStderr = 8,
    LogDebugger = 16, // Windows' debugger output, while a debugger is attached
};
inline constexpr unsigned kLogMode = LogFileAppend | LogStdout | LogDebugger;
static_assert((kLogMode & LogFileTruncate) == 0 || (kLogMode & LogFileAppend) == 0, "the file is emptied or appended");

#define _IMPL_LOG_STATEMENT(...)       \
    do                                 \
    {                                  \
        if constexpr (::kLogMode != 0) \
            __VA_ARGS__;               \
    } while (false)

// LOG for common's own low-level code, not for user code: it never starts the async logger, so it doesn't link it
#define LLOG(...) _IMPL_LOG_STATEMENT(_IMPL_PP_CONCAT(_LLOG, _IMPL_PP_VA_ARGS_01F(__VA_ARGS__))(__VA_ARGS__))
#define _LLOGS(...) ::logdetail::llogOne<::logdetail::isLiteralSpelling(#__VA_ARGS__)>(__VA_ARGS__)
#define _LLOGF(format, ...) ::logdetail::llog<::logdetail::FixedString {format "\0" #__VA_ARGS__}>(__VA_ARGS__)

#if defined(_MSC_VER) && !defined(__clang__)
#define fmtStr(...) ((false && ::printf(__VA_ARGS__)), fmtStr(__VA_ARGS__))
#define fmtStrAppend(str, ...) ((false && ::printf(__VA_ARGS__)), fmtStrAppend(str, __VA_ARGS__))
#define throw_runtime_error(...) ((false && ::printf(__VA_ARGS__)), throw_runtime_error(__VA_ARGS__))
#endif


// PP_FOR_EACH
#define _IMPL_PP_PARENS ()
#define _IMPL_PP_EXPAND0(...) _IMPL_PP_EXPAND4(_IMPL_PP_EXPAND4(_IMPL_PP_EXPAND4(_IMPL_PP_EXPAND4(__VA_ARGS__))))
#define _IMPL_PP_EXPAND4(...) _IMPL_PP_EXPAND3(_IMPL_PP_EXPAND3(_IMPL_PP_EXPAND3(_IMPL_PP_EXPAND3(__VA_ARGS__))))
#define _IMPL_PP_EXPAND3(...) _IMPL_PP_EXPAND2(_IMPL_PP_EXPAND2(_IMPL_PP_EXPAND2(_IMPL_PP_EXPAND2(__VA_ARGS__))))
#define _IMPL_PP_EXPAND2(...) _IMPL_PP_EXPAND1(_IMPL_PP_EXPAND1(_IMPL_PP_EXPAND1(_IMPL_PP_EXPAND1(__VA_ARGS__))))
#define _IMPL_PP_EXPAND1(...) __VA_ARGS__
#define _IMPL_PP_FOR_EACH_HELPER(macro, a1, ...) macro(a1) __VA_OPT__(_IMPL_PP_FOR_EACH_AGAIN _IMPL_PP_PARENS (macro, __VA_ARGS__))
#define _IMPL_PP_FOR_EACH_AGAIN() _IMPL_PP_FOR_EACH_HELPER
#define PP_FOR_EACH(macro, ...)  __VA_OPT__(_IMPL_PP_EXPAND0(_IMPL_PP_FOR_EACH_HELPER(macro, __VA_ARGS__)))

#define _IMPL_PP_LOG_1(x) #x ":{}"
#define _IMPL_PP_LOG_2(x) ", " _IMPL_PP_LOG_1(x)
#define _IMPL_PP_LOG_FORMAT(var, ...) _IMPL_PP_LOG_1(var) PP_FOR_EACH(_IMPL_PP_LOG_2 __VA_OPT__(,) __VA_ARGS__)

#include "log.h"
