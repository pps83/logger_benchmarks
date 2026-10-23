#include "GetProcAddr.h"
#if defined(_WIN32) || defined(__CYGWIN__)
#include "format.h"
#include <algorithm>
#include <windows.h>
#include <winternl.h>

namespace impl
{
template<typename T1, typename T2>
static inline bool eq_i(T1 x, T2 y)
{
    return (x == y) || ((x | ' ') == (y | ' ') && (x | ' ') >= 'a' && (x | ' ') <= 'z');
}

template<typename T1, typename T2>
static bool iStringEqual(const T1* str1, const T2* str2)
{
    for (;;)
    {
        auto x = *str1++;
        if (!eq_i(x, *str2++))
            return false;
        if (x == 0)
            return true;
    }
}

template<typename T>
static void* getModuleHandleImpl(const T* moduleName)
{
    GCC_WARNING_DISABLE(-Warray-bounds)
    PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    GCC_WARNING_RESTORE()
    PLDR_DATA_TABLE_ENTRY dte = (PLDR_DATA_TABLE_ENTRY)(peb->Ldr->InMemoryOrderModuleList.Flink);
    PLIST_ENTRY listHead = &peb->Ldr->InMemoryOrderModuleList; // head of the linked list (used to get the node & to check the end of the list)
    PLIST_ENTRY listNode = listHead->Flink; // node of the linked list
    if (!moduleName)
        return dte->Reserved2[0];
    do
    {
        if (dte->FullDllName.Length && iStringEqual(dte->FullDllName.Buffer, moduleName))
            return dte->Reserved2[0];
        dte = (PLDR_DATA_TABLE_ENTRY)(listNode->Flink); // next PLDR_DATA_TABLE_ENTRY in the linked list
        listNode = listNode->Flink; // next node in the linked list
    } while (listNode != listHead); // if list node is equal to the head, we reached the end of the linked list
    return nullptr;
}

void* getModuleHandle(const char* dllName)
{
    return getModuleHandleImpl(dllName);
}

uintptr_t getProcAddress(void* dll, const char* funcName)
{
    char* base = reinterpret_cast<char*>(dll);
    IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dosHeader && dosHeader->e_magic == 0x5A4D)
    {
#if defined(_M_IX86) || defined(__i386__)
        IMAGE_NT_HEADERS32* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dosHeader->e_lfanew);
#else
        IMAGE_NT_HEADERS64* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dosHeader->e_lfanew);
#endif
        if (ntHeaders->Signature == 0x4550)
        {
            IMAGE_EXPORT_DIRECTORY* exportDirectory = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
#if 1 // binary search
            auto addrNamePrt = reinterpret_cast<unsigned*>(base + exportDirectory->AddressOfNames);
            auto addrNameEnd = addrNamePrt + exportDirectory->NumberOfNames;
            auto ptr = std::lower_bound(addrNamePrt, addrNameEnd, funcName, [&](const unsigned& nameOffset, const char* funcName) {
                return strcmp(base + nameOffset, funcName) < 0; });
            if (ptr != addrNameEnd)
            {
                if (!strcmp(base + *ptr, funcName))
                {
                    auto funcOffset = reinterpret_cast<unsigned short*>(base + exportDirectory->AddressOfNameOrdinals)[ptr - addrNamePrt];
                    return reinterpret_cast<uintptr_t>(base + reinterpret_cast<unsigned*>(base + exportDirectory->AddressOfFunctions)[funcOffset]);
                }
            }
#else
            for (unsigned int uiIter = 0; uiIter < exportDirectory->NumberOfNames; ++uiIter)
            {
                char* name = base + reinterpret_cast<unsigned*>(base + exportDirectory->AddressOfNames)[uiIter];
                if (!strcmp(name, funcName))
                {
                    auto funcOffset = reinterpret_cast<unsigned short*>(base + exportDirectory->AddressOfNameOrdinals)[uiIter];
                    return reinterpret_cast<uintptr_t>(base + reinterpret_cast<unsigned*>(base + exportDirectory->AddressOfFunctions)[funcOffset]);
                }
            }
#endif
        }
    }
    return 0;
}

uintptr_t getProcAddress(const char* dllName, const char* funcName)
{
    auto dll = getModuleHandleImpl(dllName);
    return dll ? getProcAddress(dll, funcName) : 0;
}
}
#endif
