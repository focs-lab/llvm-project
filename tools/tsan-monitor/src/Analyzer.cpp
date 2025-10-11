#include "Analyzer.h"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace monitor {

AnalyzerResult Analyzer::Process(const Event& event) {
  if (verbose_) {
    PrintEvent(event);
  }
  if (IsTermination(event)) {
    return AnalyzerResult::kTerminate;
  }
  return AnalyzerResult::kContinue;
}

void Analyzer::PrintEvent(const Event& event) {
  std::ostringstream oss;
  oss << "[tid=" << event.tid << "] " << EventName(event.id)
      << " lap=" << static_cast<int>(event.lap)
      << " addr=0x" << std::hex << std::setw(12) << std::setfill('0')
      << event.address << std::dec
      << " nargs=" << event.nargs;
  for (std::size_t i = 0; i < event.nargs; ++i) {
    oss << " arg" << i << "=0x" << std::hex << event.args[i] << std::dec;
  }
  oss << " index=" << event.index;
  std::cout << oss.str() << std::endl;
}

}  // namespace monitor
