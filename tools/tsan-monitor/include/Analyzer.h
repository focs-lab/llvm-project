#pragma once

#include <atomic>
#include <iostream>

#include "Event.h"

namespace monitor {

enum class AnalyzerResult {
  kContinue,
  kTerminate,
};

class Analyzer {
 public:
  explicit Analyzer(bool verbose) : verbose_(verbose) {}

  AnalyzerResult Process(const Event& event);

 private:
  void PrintEvent(const Event& event);

  bool verbose_ = false;
};

}  // namespace monitor
