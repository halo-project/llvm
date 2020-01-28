#pragma once

#include "halomon/XRayEvent.h"
#include "halomon/SummaryStats.h"

namespace halo {

struct FunctionData {
public:
  uint64_t Calls;
  SummaryStats<uint64_t> CallFrequency;

  void addEvent(XRayEvent const& Evt) {
    Calls += Evt.EntryCount;

    if (LastTimestamp)
      CallFrequency.observe(Evt.Time - LastTimestamp);

    LastTimestamp = Evt.Time;
  }

  void serialize(pb::FunctionData *Out) const {
    Out->set_calls(Calls);
    CallFrequency.serialize(Out->mutable_call_frequency());
  }

private:
  uint64_t LastTimestamp;
};



class ThreadData {
private:
  uint64_t TotalCalls;
  SummaryStats<uint64_t> TotalCallFrequency;
  uint64_t LastTimestamp;
  std::unordered_map<uint64_t, FunctionData> FuncData;

public:
  void addEvent(XRayEvent const& Evt) {
    TotalCalls += Evt.EntryCount;

    if (LastTimestamp)
      TotalCallFrequency.observe(Evt.Time - LastTimestamp);

    LastTimestamp = Evt.Time;
    FuncData[Evt.FuncPtr].addEvent(Evt);
  }

  void serialize(pb::ThreadData *Out) const {
    Out->set_total_calls(TotalCalls);
    TotalCallFrequency.serialize(Out->mutable_total_call_frequency());

    auto FuncMap = Out->mutable_func_data();
    for (auto const& Data : FuncData) {
      pb::FunctionData FD;
      Data.second.serialize(&FD);
      FuncMap->insert({Data.first, FD});
    }
  }

  // NOTE: this is NOT the same as the number of call frequency samples,
  // but in practice it will probably be a fixed multiple of it.
  auto getTotalCalls() { return TotalCalls; }

  auto const& getFreqStats() { return TotalCallFrequency; }
};



// an event profiler that tracks call-event frequency.
// The estimate
class XRayProfiler {
public:
  void addEvent(XRayEvent const& Evt) {
    TotalEvents += 1;
    Threads[Evt.Thread].addEvent(Evt);
  }

  auto numEvents() const {
    return TotalEvents;
  }

  void serialize(pb::XRayProfileData &Out) const {
    Out.set_total_events(TotalEvents);

    std::hash<std::thread::id> Hasher;
    auto ThreadMap = Out.mutable_thread_data();

    for (auto const& ThdPair : Threads) {
      auto TID = static_cast<uint64_t>(Hasher(ThdPair.first));
      pb::ThreadData TD;
      ThdPair.second.serialize(&TD);
      ThreadMap->insert({TID, TD});
    }
  }

  void clear() {
    TotalEvents = 0;
    Threads.clear();
  }

private:
  uint64_t TotalEvents;
  std::unordered_map<std::thread::id, ThreadData> Threads;
};

} // end namespace halo