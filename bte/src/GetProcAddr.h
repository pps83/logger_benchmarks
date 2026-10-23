#pragma once
#if defined(_WIN32) || defined(__CYGWIN__)
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <type_traits>
#include <windows.h>

namespace impl
{
void* getModuleHandle(const char* dllName);
uintptr_t getProcAddress(void* dll, const char* procName);
uintptr_t getProcAddress(const char* dllName, const char* procName);
}

// equivalent to `procPtr = ::GetProcAddress(dll, procName)`
template<typename T, typename = std::enable_if_t<std::is_pointer_v<T>&& std::is_function_v<std::remove_pointer_t<T>>>>
inline bool GetProcAddr(HMODULE dll, const char* procName, T& procPtr)
{
    auto proc = impl::getProcAddress((void*)dll, procName);
    static_assert(sizeof(procPtr) == sizeof(proc));
    static_assert(sizeof(dll) == sizeof(void*));
    memcpy(&procPtr, &proc, sizeof(procPtr));
    return !!proc;
}

// equivalent to `procPtr = ::GetProcAddress(dll, procName)`
template<typename T, typename = std::enable_if_t<!std::is_same_v<void*, HMODULE>&& std::is_pointer_v<T>&& std::is_function_v<std::remove_pointer_t<T>>>>
inline bool GetProcAddr(void* dll, const char* procName, T& procPtr)
{
    return GetProcAddr((HMODULE)dll, procName, procPtr);
}

// equivalent to `procPtr = ::GetProcAddress(::GetModuleHandleA(dllName), procName)`
template<typename T, typename = std::enable_if_t<std::is_pointer_v<T>&& std::is_function_v<std::remove_pointer_t<T>>>>
inline bool GetProcAddr(const char* dllName, const char* procName, T& procPtr)
{
    auto proc = impl::getProcAddress(dllName, procName);
    static_assert(sizeof(procPtr) == sizeof(proc));
    memcpy(&procPtr, &proc, sizeof(procPtr));
    return !!proc;
}

#endif
