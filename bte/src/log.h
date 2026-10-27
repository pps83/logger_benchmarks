#pragma once
// LOG(format, args...): printf or {} format, checked at compile time, formatted on the logger thread. LOG() flushes.
#include "format.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <array>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
extern "C" unsigned __int64 __rdtsc();
extern "C" void _m_prefetchw(volatile const void*);
#pragma intrinsic(__rdtsc)
#pragma intrinsic(_m_prefetchw)
#elif defined(_MSC_VER) && defined(_M_ARM64) && !defined(__aarch64__)
#include <intrin.h>
#endif

void LOG(std::string_view text = std::string_view());

namespace logdetail
{

// The format as a template argument; the macros add the stringized arguments after a NUL, so the spelling is part of it
template <size_t N>
struct FixedString
{
    char data[N] {};
    size_t formatLength = 0;

    consteval FixedString(const char (&text)[N])
    {
        for (size_t i = 0; i < N; ++i)
            data[i] = text[i];
        while (formatLength < N - 1 && data[formatLength] != '\0')
            ++formatLength;
    }

    constexpr std::string_view view() const
    {
        return {data, formatLength};
    }

    constexpr std::string_view args() const
    {
        if (formatLength + 1 >= N)
            return {};
        return {data + formatLength + 1, N - formatLength - 2};
    }
};

enum class Kind { Text, Printf, Fmt, Mixed };

constexpr bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

constexpr bool isPrintfFlag(char c)
{
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

constexpr bool isLengthModifier(char c)
{
    return c == 'h' || c == 'l' || c == 'z' || c == 'j' || c == 't' || c == 'L';
}

struct PrintfSpec
{
    size_t length = 0; // 0: no conversion starts here
    char modifier[3] {};
    char conversion = 0;
    int starArguments = 0; // each '*' width or precision takes one int argument
    bool starPrecision = false;
    int precision = -1;
};

constexpr PrintfSpec parsePrintf(std::string_view text, size_t i)
{
    PrintfSpec spec;
    size_t j = i + 1;
    while (j < text.size() && isPrintfFlag(text[j]))
        ++j;
    if (j < text.size() && text[j] == '*')
    {
        ++spec.starArguments;
        ++j;
    }
    while (j < text.size() && isDigit(text[j]))
        ++j;
    if (j < text.size() && text[j] == '.')
    {
        ++j;
        if (j < text.size() && text[j] == '*')
        {
            ++spec.starArguments;
            spec.starPrecision = true;
            ++j;
        }
        else
        {
            spec.precision = 0;
            for (; j < text.size() && isDigit(text[j]); ++j)
                spec.precision = spec.precision * 10 + (text[j] - '0');
        }
    }
    size_t m = 0;
    while (j < text.size() && m < 2 && isLengthModifier(text[j]))
        spec.modifier[m++] = text[j++];
    if (j >= text.size() || std::string_view("diouxXcspfFeEgGaAn").find(text[j]) == std::string_view::npos)
        return {};
    spec.conversion = text[j];
    spec.length = j + 1 - i;
    return spec;
}

// Length of a std::format field starting at text[i], or 0 if it isn't one ({"a":1}, {min:%d})
constexpr size_t fmtFieldLength(std::string_view text, size_t i)
{
    size_t j = i + 1;
    while (j < text.size() && isDigit(text[j]))
        ++j;
    if (j < text.size() && text[j] == ':')
    {
        int depth = 0;
        for (++j; j < text.size(); ++j)
        {
            if (text[j] == '{')
            {
                ++depth;
            }
            else if (text[j] == '}')
            {
                if (depth == 0)
                    break;
                --depth;
            }
        }
    }
    if (j >= text.size() || text[j] != '}')
        return 0;
    return j + 1 - i;
}

consteval Kind detect(std::string_view text)
{
    bool hasPrintf = false;
    bool hasFmt = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == '%')
        {
            hasPrintf = true;
            ++i;
        }
        else if (text[i] == '%' && parsePrintf(text, i).length != 0)
        {
            hasPrintf = true;
            i += parsePrintf(text, i).length - 1;
        }
        else if ((text[i] == '{' || text[i] == '}') && i + 1 < text.size() && text[i + 1] == text[i])
        {
            hasFmt = true;
            ++i;
        }
        else if (text[i] == '{' && fmtFieldLength(text, i) != 0)
        {
            hasFmt = true;
            i += fmtFieldLength(text, i) - 1;
        }
    }
    if (hasPrintf && hasFmt)
        return Kind::Mixed;
    if (hasPrintf)
        return Kind::Printf;
    return hasFmt ? Kind::Fmt : Kind::Text;
}

template <typename T>
using Bare = std::remove_cvref_t<T>;

// Text arguments: their bytes are copied at the call
template <typename D>
constexpr bool isString = std::is_same_v<D, std::string> || std::is_same_v<D, std::string_view>
                          || std::is_same_v<D, const char*> || std::is_same_v<D, char*>
                          || (std::is_array_v<D> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<D>>, char>);

template <typename D>
constexpr bool isStringPointer = std::is_same_v<D, const char*> || std::is_same_v<D, char*> || std::is_array_v<D>;

template <typename D>
constexpr bool isPointer = (std::is_pointer_v<D> && !isString<D>) || std::is_same_v<D, std::nullptr_t>;

// Copied by value and formatted later
template <typename D>
constexpr bool isScalar = std::is_arithmetic_v<D> || std::is_enum_v<D> || isPointer<D>;

template <typename D>
constexpr bool isCharArray = std::is_array_v<D> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<D>>, char>;

// An argument spelled as a string literal (isLiteralSpelling), kept by its address: it lives as long as the program
struct LiteralText
{
    const char* text;
    size_t length;

    bool operator==(const LiteralText&) const = default;
};

// A string literal or adjacent ones and nothing else: a leading quote can also start an expression giving a local array
consteval bool isLiteralSpelling(std::string_view text)
{
    bool literal = false;
    for (size_t i = 0;; ++i)
    {
        while (i < text.size() && text[i] == ' ')
            ++i;
        if (i == text.size())
            return literal;
        if (text[i] != '"')
            return false;
        for (++i; i < text.size() && text[i] != '"'; ++i)
        {
            if (text[i] == '\\')
                ++i;
        }
        if (i >= text.size())
            return false;
        literal = true;
    }
}

// Arguments spelled as string literals; a template's commas split the list too, the count then differs and none count
template <FixedString S, size_t N>
consteval std::array<bool, N> literalArgs()
{
    std::array<bool, N> quoted {};
    const std::string_view text = S.args();
    size_t piece = 0;
    size_t pieceBegin = 0;
    int depth = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        const char c = i < text.size() ? text[i] : ',';
        // A raw string's quotes and commas are its text, not the list's: every argument is then copied
        if (c == '"' && i > 0 && text[i - 1] == 'R')
            return {};
        if (c == '"' || c == '\'')
        {
            for (++i; i < text.size() && text[i] != c; ++i)
            {
                if (text[i] == '\\')
                    ++i;
            }
            if (i >= text.size())
                return {};
        }
        else if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == ',' && depth == 0)
        {
            if (piece < N)
                quoted[piece] = isLiteralSpelling(text.substr(pieceBegin, i - pieceBegin));
            ++piece;
            pieceBegin = i + 1;
        }
    }
    if (piece != N)
        return {};
    return quoted;
}

// The type printf receives through `...`, after default promotions
template <typename D>
auto printfStoredType()
{
    if constexpr (std::is_enum_v<D>)
        return decltype(+std::declval<std::underlying_type_t<D>>()) {};
    else if constexpr (std::is_same_v<D, float>)
        return double {};
    else if constexpr (std::is_floating_point_v<D>)
        return D {};
    else if constexpr (std::is_integral_v<D>)
        return decltype(+std::declval<D>()) {};
    else
        return static_cast<const void*>(nullptr);
}

template <typename D>
using PrintfStored = decltype(printfStoredType<D>());

// fmt keeps the exact type; pointers become const void*
template <typename D>
using FmtStored = std::conditional_t<isPointer<D>, const void*, D>;

template <Kind K, typename D>
using Stored = std::conditional_t<K == Kind::Printf, PrintfStored<D>, FmtStored<D>>;

// Integer type after promotion, matched exactly like GCC's check (%ld needs a long)
enum class IntType { None, Int, UInt, Long, ULong, LongLong, ULongLong };

template <typename P>
consteval IntType intType()
{
    if constexpr (std::is_same_v<P, int>)
        return IntType::Int;
    else if constexpr (std::is_same_v<P, unsigned>)
        return IntType::UInt;
    else if constexpr (std::is_same_v<P, long>)
        return IntType::Long;
    else if constexpr (std::is_same_v<P, unsigned long>)
        return IntType::ULong;
    else if constexpr (std::is_same_v<P, long long>)
        return IntType::LongLong;
    else if constexpr (std::is_same_v<P, unsigned long long>)
        return IntType::ULongLong;
    else
        return IntType::None;
}

struct ArgInfo
{
    bool integral = false;
    bool floating = false;
    bool longDouble = false;
    bool string = false;
    bool stringPointer = false;
    bool pointer = false;
    IntType intType = IntType::None;
};

template <typename T>
consteval ArgInfo argInfo()
{
    using D = Bare<T>;
    ArgInfo info;
    if constexpr (isString<D>)
    {
        info.string = true;
        info.stringPointer = isStringPointer<D>;
    }
    else if constexpr (isPointer<D>)
    {
        info.pointer = true;
    }
    else if constexpr (std::is_floating_point_v<D>)
    {
        info.floating = true;
        info.longDouble = std::is_same_v<D, long double>;
    }
    else if constexpr (std::is_integral_v<D> || std::is_enum_v<D>)
    {
        info.integral = true;
        info.intType = intType<PrintfStored<D>>();
    }
    return info;
}

consteval bool integerMatches(std::string_view modifier, IntType type)
{
    if (modifier.empty() || modifier == "h" || modifier == "hh")
        return type == IntType::Int || type == IntType::UInt;
    if (modifier == "l")
        return type == IntType::Long || type == IntType::ULong;
    if (modifier == "ll")
        return type == IntType::LongLong || type == IntType::ULongLong;
    if (modifier == "z")
        return type == intType<size_t>() || type == intType<std::make_signed_t<size_t>>();
    if (modifier == "j")
        return type == intType<intmax_t>() || type == intType<uintmax_t>();
    if (modifier == "t")
        return type == intType<ptrdiff_t>() || type == intType<std::make_unsigned_t<ptrdiff_t>>();
    return false;
}

enum class PrintfCheck { Ok, MoreConversionsThanArguments, MoreArgumentsThanConversions, PercentN, Mismatch };

consteval bool matches(const PrintfSpec& spec, const ArgInfo& arg)
{
    const std::string_view modifier(spec.modifier);
    switch (spec.conversion)
    {
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
        return arg.integral && integerMatches(modifier, arg.intType);
    case 'c':
        return arg.integral && modifier.empty() && integerMatches(modifier, arg.intType);
    case 's':
        return arg.string && modifier.empty();
    case 'p':
        return (arg.pointer || arg.stringPointer) && modifier.empty();
    default:
        return arg.floating && arg.longDouble == (modifier == "L");
    }
}

template <typename... Args>
consteval PrintfCheck checkPrintf(std::string_view text)
{
    constexpr std::array<ArgInfo, sizeof...(Args)> args {argInfo<Args>()...};
    size_t next = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%')
            continue;
        if (i + 1 < text.size() && text[i + 1] == '%')
        {
            ++i;
            continue;
        }
        const PrintfSpec spec = parsePrintf(text, i);
        if (spec.length == 0)
            continue;
        i += spec.length - 1;
        for (int star = 0; star < spec.starArguments; ++star)
        {
            if (next >= args.size())
                return PrintfCheck::MoreConversionsThanArguments;
            if (!args[next].integral || args[next].intType != IntType::Int)
                return PrintfCheck::Mismatch;
            ++next;
        }
        if (spec.conversion == 'n')
            return PrintfCheck::PercentN;
        if (next >= args.size())
            return PrintfCheck::MoreConversionsThanArguments;
        if (!matches(spec, args[next++]))
            return PrintfCheck::Mismatch;
    }
    return next == args.size() ? PrintfCheck::Ok : PrintfCheck::MoreArgumentsThanConversions;
}

// %p with a string pointer logs the address; %.Ns and %.*s read at most N chars
struct ArgUse
{
    bool pointerValue = false;
    int limit = -1; // -1: no limit, -2: the previous int argument
};

template <size_t N>
consteval std::array<ArgUse, N> printfArgUses(std::string_view text)
{
    std::array<ArgUse, N> uses {};
    size_t next = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%')
            continue;
        if (i + 1 < text.size() && text[i + 1] == '%')
        {
            ++i;
            continue;
        }
        const PrintfSpec spec = parsePrintf(text, i);
        if (spec.length == 0)
            continue;
        i += spec.length - 1;
        next += static_cast<size_t>(spec.starArguments);
        if (next >= N)
            break;
        if (spec.conversion == 'p')
            uses[next].pointerValue = true;
        else if (spec.conversion == 's')
            uses[next].limit = spec.starPrecision ? -2 : spec.precision;
        ++next;
    }
    return uses;
}

template <Kind K, size_t N>
consteval std::array<ArgUse, N> argUses(std::string_view text)
{
    if constexpr (K == Kind::Printf)
        return printfArgUses<N>(text);
    else
        return std::array<ArgUse, N> {};
}

// Format checks done at compile time
template <FixedString S, typename... Args>
consteval Kind checkedKind()
{
    constexpr Kind kind = detect(S.view());
    static_assert(kind != Kind::Mixed, "the format string mixes printf conversions and fmt replacement fields");
    static_assert(kind != Kind::Text, "arguments given to a format string with no placeholders");
    if constexpr (kind == Kind::Printf)
    {
        constexpr PrintfCheck check = checkPrintf<Args...>(S.view());
        static_assert(check != PrintfCheck::MoreConversionsThanArguments, "printf: more conversions than arguments");
        static_assert(check != PrintfCheck::MoreArgumentsThanConversions, "printf: more arguments than conversions");
        static_assert(check != PrintfCheck::PercentN, "printf: %n is not allowed");
        static_assert(check != PrintfCheck::Mismatch, "printf: an argument does not match its conversion");
    }
    return kind;
}

class Reader
{
public:
    explicit Reader(const unsigned char* buffer) : next(buffer) {}

    template <typename T>
    T scalar()
    {
        T value;
        memcpy(&value, next, sizeof(value));
        next += sizeof(value);
        return value;
    }

    // Points into the record, where a NUL follows it
    std::string_view string()
    {
        const uint32_t length = scalar<uint32_t>();
        const std::string_view text(reinterpret_cast<const char*>(next), length);
        next += length + 1;
        return text;
    }

    const unsigned char* position() const
    {
        return next;
    }

private:
    const unsigned char* next;
};

template <typename D>
const char* textPointer(const D& value)
{
    if constexpr (std::is_array_v<D>)
        return value;
    else if constexpr (std::is_pointer_v<D>)
        return value;
    else
        return value.data();
}

// Reads at most `limit` chars, so the buffer needs no terminator
template <typename D>
std::string_view textView(const D& value, long long limit)
{
    if constexpr (std::is_array_v<D>)
    {
        const size_t extent = std::extent_v<D>;
        const size_t bound = limit >= 0 && static_cast<size_t>(limit) < extent ? static_cast<size_t>(limit) : extent;
        return std::string_view(value, strnlen(value, bound));
    }
    else if constexpr (std::is_pointer_v<D>)
    {
        if (value == nullptr)
            return std::string_view("(null)");
        if (limit >= 0)
            return std::string_view(value, strnlen(value, static_cast<size_t>(limit)));
        return std::string_view(value);
    }
    else
    {
        const std::string_view text(value);
        return limit >= 0 ? text.substr(0, static_cast<size_t>(limit)) : text;
    }
}

// What a record stores for an argument: a scalar, a literal's address, or a string's text
template <Kind K, ArgUse U, bool Quoted, typename T>
FORCEINLINE auto field(const T& value, long long precision)
{
    using D = Bare<T>;
    if constexpr (isString<D> && U.pointerValue)
        return static_cast<const void*>(textPointer(value));
    else if constexpr (Quoted && isCharArray<D>)
        return LiteralText {value, std::extent_v<D> - 1};
    else if constexpr (isString<D>)
        return textView(value, U.limit == -2 ? precision : U.limit);
    else
        return static_cast<Stored<K, D>>(value);
}

// %.*s takes its precision from the int argument before the string
template <size_t I, typename... Args>
FORCEINLINE long long previousInteger(const Args&... args)
{
    if constexpr (I == 0)
        return -1;
    else if constexpr (std::is_integral_v<Bare<std::tuple_element_t<I - 1, std::tuple<Args...>>>>)
        return static_cast<long long>(std::get<I - 1>(std::tie(args...)));
    else
        return -1;
}

template <Kind K, ArgUse U, bool Quoted, typename D>
auto unpack(Reader& reader)
{
    if constexpr (isString<D> && U.pointerValue)
        return reader.scalar<const void*>();
    else if constexpr (Quoted && isCharArray<D> && K == Kind::Printf)
        return reader.scalar<LiteralText>().text;
    else if constexpr (Quoted && isCharArray<D>)
        return std::string_view(reader.scalar<LiteralText>().text); // a C string, as a copied char array formats
    else if constexpr (isString<D> && K == Kind::Printf)
        return reader.string().data();
    else if constexpr (isString<D>)
        return reader.string();
    else
        return reader.scalar<Stored<K, D>>();
}

// Defined in format.cpp
void formatPrintf(std::string& out, const char* format, ...);
void formatFmt(std::string& out, std::string_view format, std::format_args args);
void appendQuoted(std::string& out, std::string_view text);

// Defined in log.cpp
void logFormatted(std::string_view format, std::format_args args);

// Formats a record's arguments on the logger thread and returns where they end
using FormatFn = const unsigned char* (*)(const unsigned char* bytes, std::string& out);

// One static Site per format, argument spelling, argument types and LOG or LLOG; its address identifies them
struct Site
{
    FormatFn format;
    FormatFn values = nullptr;  // the stored values as they are and their end, for the line a failed formatting writes
    const char* text = nullptr; // the format, a NUL, then the arguments as written
    bool llog = false;
};

// The record's arguments as the formatting takes them; braced init evaluates left to right
template <Kind K, FixedString S, typename... D, size_t... I>
auto unpackAll(Reader& reader, std::index_sequence<I...>)
{
    constexpr std::array<ArgUse, sizeof...(D)> uses = argUses<K, sizeof...(D)>(S.view());
    constexpr std::array<bool, sizeof...(D)> quoted = literalArgs<S, sizeof...(D)>();
    return std::tuple<decltype(unpack<K, uses[I], quoted[I], D>(reader))...> {
        unpack<K, uses[I], quoted[I], D>(reader)...};
}

template <Kind K, FixedString S, typename... D>
const unsigned char* replay(const unsigned char* bytes, std::string& out)
{
    Reader reader(bytes);
    auto values = unpackAll<K, S, D...>(reader, std::index_sequence_for<D...> {});
    if constexpr (K == Kind::Printf)
        std::apply([&](auto... value) { formatPrintf(out, S.data, value...); }, values);
    else
        std::apply([&](auto&... value) { formatFmt(out, S.view(), std::make_format_args(value...)); }, values);
    return reader.position();
}

// A stored value as it is, without the formatter its type may have, after a comma unless it is the first
template <typename T>
void appendValue(std::string& out, const T& value)
{
    if (!out.empty())
        out += ", ";
    if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, const char*>)
        appendQuoted(out, value);
    else if constexpr (std::is_enum_v<T>)
        out += std::format("{}", std::to_underlying(value));
    else
        out += std::format("{}", value);
}

template <Kind K, FixedString S, typename... D>
const unsigned char* storedValues(const unsigned char* bytes, std::string& out)
{
    Reader reader(bytes);
    const auto values = unpackAll<K, S, D...>(reader, std::index_sequence_for<D...> {});
    out.clear();
    std::apply([&](const auto&... value) { (appendValue(out, value), ...); }, values);
    return reader.position();
}

// The calling thread's queue as LOG writes it; AsyncLog.cpp keeps Quill's view of the queue in step with it
struct Writer
{
    unsigned char* storage;
    size_t mask;
    size_t position;
    std::atomic<size_t> limit; // the logger's end sets it from another thread
    std::atomic<size_t>* published;
    std::atomic<const unsigned char*> last; // the last record, for the repeat check; the logger's end clears it
    std::atomic<uint64_t> repeats;          // records equal to it, counted instead of written; the logger reads it
};

// A thread's Writer is on the heap: other threads may still read it after a runtime skipped the thread's destructors.
// Until its first LOG a thread points at this one, which never has room
inline constinit Writer kNoWriter {};

// Defined in the header: MSVC checks an extern thread_local's initialization on every use
inline constinit thread_local Writer* tWriter = &kNoWriter;

// A record: the timestamp, the Site's address with its low bit set, then the arguments
constexpr size_t kRecordHeader = 2 * sizeof(uint64_t);

FORCEINLINE uint64_t recordTag(const Site& site)
{
    return reinterpret_cast<uintptr_t>(&site) | 1;
}

FORCEINLINE const Site& siteOf(uint64_t tag)
{
    return *reinterpret_cast<const Site*>(static_cast<uintptr_t>(tag - 1));
}

uint64_t quillCounter();

// The counter Quill converts a Tsc logger's timestamps from
FORCEINLINE uint64_t timestamp()
{
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#elif defined(_MSC_VER) && defined(_M_ARM64)
    // CNTVCT_EL0, as above: MSVC has no GCC-style asm
    return static_cast<uint64_t>(_ReadStatusReg(ARM64_SYSREG(3, 3, 14, 0, 2)));
#else
    // Quill's own clock: a steady one on 32-bit ARM, which LOG would otherwise have to copy exactly
    return quillCounter();
#endif
}

FORCEINLINE void prefetchForWrite(const unsigned char* at)
{
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
    _m_prefetchw(at);
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
    __prefetch2(at, 0x10); // 0x10: prefetch for writing, as __builtin_prefetch(at, 1)
#else
    __builtin_prefetch(at, 1);
#endif
}

FORCEINLINE size_t fieldSize(std::string_view text)
{
    return sizeof(uint32_t) + text.size() + 1;
}

template <typename T>
constexpr size_t fieldSize(T)
{
    return sizeof(T);
}

FORCEINLINE unsigned char* put(unsigned char* at, std::string_view text)
{
    const uint32_t length = static_cast<uint32_t>(text.size());
    memcpy(at, &length, sizeof(length));
    memcpy(at + sizeof(length), text.data(), text.size());
    at[sizeof(length) + text.size()] = 0;
    return at + fieldSize(text);
}

template <typename T>
FORCEINLINE unsigned char* put(unsigned char* at, T value)
{
    memcpy(at, &value, sizeof(value));
    return at + sizeof(value);
}

// Floating values compare by their bits, as they print: -0.0 is not 0.0, a NaN repeats
template <typename T>
FORCEINLINE auto comparable(T value)
{
    if constexpr (std::is_floating_point_v<T> && (sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t)))
    {
        std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t, uint64_t> bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }
    else
        return value;
}

FORCEINLINE bool same(const unsigned char*& at, std::string_view text)
{
    uint32_t length;
    memcpy(&length, at, sizeof(length));
    const bool equal = length == text.size() && memcmp(at + sizeof(length), text.data(), text.size()) == 0;
    at += fieldSize(text);
    return equal;
}

template <typename T>
FORCEINLINE bool same(const unsigned char*& at, T value)
{
    T previous;
    memcpy(&previous, at, sizeof(previous));
    at += sizeof(previous);
    return comparable(previous) == comparable(value);
}

template <typename... F>
FORCEINLINE void writeRecord(unsigned char* at, uint64_t tag, F... fields)
{
    const uint64_t time = timestamp();
    memcpy(at, &time, sizeof(time));
    memcpy(at + sizeof(time), &tag, sizeof(tag));
    at += kRecordHeader;
    ((at = put(at, fields)), ...);
}

template <typename... F>
FORCEINLINE bool sameRecord(const unsigned char* record, uint64_t tag, F... fields)
{
    uint64_t previousTag;
    memcpy(&previousTag, record + sizeof(uint64_t), sizeof(previousTag));
    const unsigned char* at = record + kRecordHeader;
    return previousTag == tag && (same(at, fields) && ...);
}

// The logger's end takes a thread's room, then clears its last record: a LOG that found it cleared finds no room either
FORCEINLINE void acquireIfCleared(const unsigned char* last)
{
    if (last == nullptr)
        std::atomic_thread_fence(std::memory_order_acquire);
}

// Signed, so a limit the logger's end set to the published position fails a record written after it too
FORCEINLINE bool fits(const Writer& writer, size_t size)
{
    const size_t room = writer.limit.load(std::memory_order_relaxed) - writer.position;
    return static_cast<ptrdiff_t>(room) >= static_cast<ptrdiff_t>(size);
}

FORCEINLINE void publish(Writer& writer, size_t size)
{
    const size_t position = writer.position + size;
    writer.position = position;
    writer.published->store(position, std::memory_order_release);
    // Owns the line a page ahead: after a pause the next LOGs find their lines owned instead of stalling on them
    prefetchForWrite(writer.storage + ((position + 4096) & writer.mask));
}

// Defined in AsyncLog.cpp
unsigned char* reserve(size_t size); // asks Quill for room; null: the record is written synchronously
void commitAfterRepeats(unsigned char* record, size_t size);
void writeSync(const unsigned char* record);
void submitText(std::string_view text);

// A record that differs from the thread's last one
template <typename... F>
FORCEINLINE void commitRecord(Writer& writer, unsigned char* at, size_t size, uint64_t tag, F... fields)
{
    writeRecord(at, tag, fields...);
    if (writer.repeats.load(std::memory_order_relaxed) != 0) [[unlikely]]
    {
        commitAfterRepeats(at, size);
        return;
    }
    writer.last.store(at, std::memory_order_relaxed);
    publish(writer, size);
}

// Defined in SyncLog.cpp: an LLOG record goes to the async logger while it runs, else it is written synchronously
void submitLlog(const unsigned char* record, size_t size);
void llogText(std::string_view text);
void llogFormatted(std::string_view format, std::format_args args);
bool syncLinesDropped(); // in SyncLog.cpp: another thread ended the logger, or ExitProcess runs

// LLOG's record, built on the stack, so LLOG neither starts nor links the async logger
template <typename... F>
void llogFields(const Site& site, F... fields)
{
    const uint64_t tag = recordTag(site);
    const size_t size = (kRecordHeader + ... + fieldSize(fields));
    if (size <= 512)
    {
        alignas(uint64_t) unsigned char record[512];
        writeRecord(record, tag, fields...);
        submitLlog(record, size);
        return;
    }
    std::vector<unsigned char> record(size);
    writeRecord(record.data(), tag, fields...);
    submitLlog(record.data(), size);
}

// A thread's first record, a full queue, or no logger thread
template <typename... F>
NOINLINE void logSlow(uint64_t tag, size_t size, F... fields)
{
    unsigned char* at = reserve(size);
    if (at != nullptr)
    {
        commitRecord(*tWriter, at, size, tag, fields...);
        return;
    }
    // Dropped, so not built: at exit the CRT's heap can be gone before a detached thread's last LOG
    if (syncLinesDropped())
        return;
    std::vector<unsigned char> record(size);
    writeRecord(record.data(), tag, fields...);
    writeSync(record.data());
}

// Compared first, while the values are still in registers: a repeat costs no timestamp, no room check and no write
template <typename... F>
FORCEINLINE void logFields(const Site& site, F... fields)
{
    const uint64_t tag = recordTag(site);
    Writer& writer = *tWriter;
    const unsigned char* last = writer.last.load(std::memory_order_relaxed);
    if (last != nullptr && sameRecord(last, tag, fields...))
    {
        // No timestamp for a repeat: the logger thread times pending counts itself (AsyncLog.cpp, writeIdleRepeats)
        writer.repeats.store(writer.repeats.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        return;
    }
    acquireIfCleared(last);
    const size_t size = (kRecordHeader + ... + fieldSize(fields));
    if (!fits(writer, size)) [[unlikely]]
    {
        logSlow(tag, size, fields...);
        return;
    }
    commitRecord(writer, writer.storage + (writer.position & writer.mask), size, tag, fields...);
}

// LOG's record goes into the thread's ring, LLOG's to SyncLog: an LLOG instantiates nothing of the async logger
template <bool Llog, typename... F>
FORCEINLINE void recordFields(const Site& site, F... fields)
{
    if constexpr (Llog)
        llogFields(site, fields...);
    else
        logFields(site, fields...);
}

template <Kind K, FixedString S, bool Llog, typename... Args, size_t... I>
FORCEINLINE void logArgs(const Site& site, std::index_sequence<I...>, const Args&... args)
{
    constexpr std::array<ArgUse, sizeof...(Args)> uses = argUses<K, sizeof...(Args)>(S.view());
    constexpr std::array<bool, sizeof...(Args)> quoted = literalArgs<S, sizeof...(Args)>();
    recordFields<Llog>(site, field<K, uses[I], quoted[I]>(args, previousInteger<I>(args...))...);
}

template <FixedString S, bool Llog = false, typename... Args>
FORCEINLINE void log(const Args&... args)
{
    constexpr Kind kind = checkedKind<S, Args...>();
    if constexpr (kind == Kind::Fmt)
        static_assert((std::format_string<const Args&...>(S.view()), true));

    constexpr bool copyable = ((isString<Bare<Args>> || isScalar<Bare<Args>>) && ...);
    if constexpr (copyable)
    {
        static constexpr Site site {
            &replay<kind, S, Bare<Args>...>, &storedValues<kind, S, Bare<Args>...>, S.data, Llog};
        logArgs<kind, S, Llog>(site, std::index_sequence_for<Args...> {}, args...);
    }
    else if constexpr (Llog)
        llogFormatted(S.view(), std::make_format_args(args...));
    else
    {
        // Custom types are formatted at the call: only the caller can safely read them
        logFormatted(S.view(), std::make_format_args(args...));
    }
}

// LLOG's own name, so its expansion has no comma outside parentheses and works inside another macro's argument
template <FixedString S, typename... Args>
FORCEINLINE void llog(const Args&... args)
{
    log<S, true>(args...);
}

// A literal LOG's record holds the literal's address and length
inline const unsigned char* formatLiteral(const unsigned char* bytes, std::string& out)
{
    Reader reader(bytes);
    const char* text = reader.scalar<const char*>();
    static_cast<void>(reader.scalar<uint32_t>()); // the length serves the repeat check; the text ends at its first NUL
    out.assign(text);
    return reader.position();
}

// The literal's text is not read: reading it may be what failed
inline const unsigned char* literalValues(const unsigned char* bytes, std::string& out)
{
    Reader reader(bytes);
    static_cast<void>(reader.scalar<const char*>());
    static_cast<void>(reader.scalar<uint32_t>());
    out.clear();
    return reader.position();
}

inline constexpr Site kLiteralSite {&formatLiteral, &literalValues};

template <bool Llog>
FORCEINLINE void recordText(std::string_view text)
{
    if constexpr (Llog)
        llogText(text);
    else
        (LOG)(text);
}

// Quoted: the argument is spelled as a string literal, which lives as long as the program; any other text is copied
template <bool Quoted, bool Llog = false, size_t N>
FORCEINLINE void logOne(const char (&text)[N])
{
    if constexpr (Quoted && N > 1)
        recordFields<Llog>(kLiteralSite, static_cast<const char*>(text), static_cast<uint32_t>(N - 1));
    else
        recordText<Llog>(std::string_view(text)); // LOG("") flushes, as LOG() does
}

template <bool Quoted, bool Llog = false, typename... T>
FORCEINLINE void logOne(const T&... text)
{
    recordText<Llog>(std::string_view(text...));
}

// LLOG's own name, as llog is
template <bool Quoted, typename... T>
FORCEINLINE void llogOne(const T&... text)
{
    logOne<Quoted, true>(text...);
}

// A text record holds the finished line
inline const unsigned char* formatText(const unsigned char* bytes, std::string& out)
{
    Reader reader(bytes);
    out.assign(reader.string());
    return reader.position();
}

inline constexpr Site kTextSite {&formatText, &formatText}; // its one value is the text

} // namespace logdetail

#define LOG(...) _IMPL_LOG_STATEMENT(_IMPL_PP_CONCAT(_LOG, _IMPL_PP_VA_ARGS_01F(__VA_ARGS__))(__VA_ARGS__))
#define _LOGS(...) ::logdetail::logOne<::logdetail::isLiteralSpelling(#__VA_ARGS__)>(__VA_ARGS__)
#define _LOGF(format, ...) ::logdetail::log<::logdetail::FixedString {format "\0" #__VA_ARGS__}>(__VA_ARGS__)

#define LOG_ARGS(...) _IMPL_LOG_STATEMENT(_LOGF(_IMPL_PP_LOG_FORMAT(__VA_ARGS__), __VA_ARGS__))
