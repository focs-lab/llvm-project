#include "Event.h"

#include <string>

namespace monitor {
std::size_t EventArgCount(EventId id) {
  switch (id) {
    case EventId::kRead:
    case EventId::kWrite:
    case EventId::kVptrUpdate:
      return 1;
    case EventId::kReturn:
      return 0;
    case EventId::kAtomicLoad:
    case EventId::kAtomicStore:
      return 2;
    case EventId::kAtomicRMW:
    case EventId::kAtomicCAS:
      return 3;
    case EventId::kThreadSpawn:
    case EventId::kThreadJoin:
      return 1;  // child tid
    case EventId::kThreadExit:
      return 0;
    case EventId::kVptrLoad:
    case EventId::kMemset:
    case EventId::kMemcpy:
    case EventId::kAtomicFence:
    case EventId::kAtExit:
    case EventId::kMonitorReady:
    case EventId::kIgnoreBegin:
    case EventId::kIgnoreEnd:
    case EventId::kProgramEndMarker:
    case EventId::kUnknown:
    default:
      return 0;
  }
}

std::string_view EventName(EventId id) {
  switch (id) {
    case EventId::kRead:
      return "Read";
    case EventId::kWrite:
      return "Write";
    case EventId::kVptrUpdate:
      return "VptrUpdate";
    case EventId::kVptrLoad:
      return "VptrLoad";
    case EventId::kMemset:
      return "Memset";
    case EventId::kMemcpy:
      return "Memcpy";
    case EventId::kAtomicLoad:
      return "AtomicLoad";
    case EventId::kAtomicStore:
      return "AtomicStore";
    case EventId::kAtomicRMW:
      return "AtomicRMW";
    case EventId::kAtomicCAS:
      return "AtomicCAS";
    case EventId::kAtomicFence:
      return "AtomicFence";
    case EventId::kReturn:
      return "Return";
    case EventId::kAtExit:
      return "AtExit";
    case EventId::kMonitorReady:
      return "MonitorReady";
    case EventId::kThreadSpawn:
      return "ThreadSpawn";
    case EventId::kThreadJoin:
      return "ThreadJoin";
    case EventId::kThreadExit:
      return "ThreadExit";
    case EventId::kIgnoreBegin:
      return "IgnoreBegin";
    case EventId::kIgnoreEnd:
      return "IgnoreEnd";
    case EventId::kProgramEndMarker:
      return "ProgramEnded";
    case EventId::kUnknown:
    default:
      return "Unknown";
  }
}

bool IsTermination(const Event& event) {
  return event.id == EventId::kProgramEndMarker;
}
}  // namespace monitor
