// One LOG call site per argument shape, for reading the generated code
#include "log.h"

void logLiteral()
{
    LOG("connection lost");
}

void logNumbers(int a, int b, double c)
{
    LOG("numbers {} {} {}", a, b, c);
}
