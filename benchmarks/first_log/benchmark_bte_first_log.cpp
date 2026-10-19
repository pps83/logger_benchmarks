#include "first_log_bench.h"
#include "log.h"

static void log_first(int thread, int line)
{
  LOG("Thread {} line {}", thread, line);
}

int main(int argc, char* argv[])
{
  return run_first_log(argc, argv, log_first);
}
