#include "first_log_bench.h"
#include "fmtlog/fmtlog.h"

// The first LOG starts the logger, as bte's does
static bool start_fmtlog()
{
  fmtlog::setLogFile("first_log_fmtlog.log", true);
  fmtlog::startPollingThread(100'000);
  return true;
}

static void log_first(int thread, int line)
{
  static bool const started = start_fmtlog();
  static_cast<void>(started);
  FMTLOG(fmtlog::INF, "Thread {} line {}", thread, line);
}

int main(int argc, char* argv[])
{
  return run_first_log(argc, argv, log_first);
}
