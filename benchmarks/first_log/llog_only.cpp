// LLOG alone: the program must link no async logger
#include "format.h"

int main()
{
  LLOG("llog only %d", 1);
  return 0;
}
