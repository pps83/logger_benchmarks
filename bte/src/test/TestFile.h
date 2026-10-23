#pragma once
// Reads back what a test's program wrote; with it the logger's tests need nothing of util.h
#include <stdio.h>
#include <string>

inline bool readTestFile(const std::string& path, std::string& text)
{
    text.clear();
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    char buf[65536];
    for (;;)
    {
        const size_t got = fread(buf, 1, sizeof(buf), file);
        if (got == 0)
            break;
        text.append(buf, got);
    }
    fclose(file);
    return true;
}
