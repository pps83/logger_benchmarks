// LLOG and LOG from the exiting thread after the async logger has ended (DestructionManager level 5)
#include "DestructionManager.h"
#include "format.h"
#include <chrono>
#include <memory>
#include <stdlib.h>
#include <string>

struct LogsAtExit
{
  ~LogsAtExit()
  {
    LLOG("literal line");
    LLOG("printf %d %s %.2f", 7, "text", 1.5);
    LLOG("fmt {} {} {}", 7, std::string("text"), 1.5);
    LLOG("custom {}", std::chrono::seconds(3));
    LLOG(std::string("a string"));
    LOG("literal line");
    LOG("printf %d %s %.2f", 7, "text", 1.5);
    LOG("fmt {} {} {}", 7, std::string("text"), 1.5);
    LOG("custom {}", std::chrono::seconds(3));
    LOG(std::string("a string"));
  }
};

int main()
{
  LOG("before the logger ends");
  DestructionManager::manage(5, std::make_shared<LogsAtExit>());
  exit(0);
}
