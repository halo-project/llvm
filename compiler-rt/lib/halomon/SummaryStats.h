#pragma once

#include <cmath>
#include "llvm/Support/raw_ostream.h"
#include "Messages.pb.h"

namespace halo {

template <typename ValType>
class SummaryStats {
public:

  void observe(ValType NewSample) {
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm

    Count += 1;
    double Delta = NewSample - Mean;
    Mean += Delta / Count;
    double Delta2 = NewSample - Mean; // not a common-subexpression.
    SumSqDistance += Delta * Delta2;
  }

  uint32_t samples() const { return Count; }

  double mean() const { return Mean; }

  double population_variance() const { return Count == 0
                                        ? 0
                                        : SumSqDistance / Count; }

  // sample variance.
  double variance() const { return Count <= 1
                                          ? 0
                                          : SumSqDistance / (Count - 1); }

  double deviation() const {
    auto S = variance();
    if (S == 0)
      return 0;
    return std::sqrt(S);
  }

  // estimate.
  double error() const { return Count == 0
                                ? 0
                                : deviation() / std::sqrt(Count); }

  double error_pct() const {
    return (error() / mean()) * 100.0;
  }

  void dump(llvm::raw_ostream &out) const {
    auto Avg = mean();
    out << "mean = " << Avg
        << ", deviation = " << deviation()
        << ", error_pct = " << error_pct()
        << ", samples = " << samples()
        << "\n";
  }

  void serialize(pb::SummaryStats *Out) const {
    Out->set_samples(samples());
    Out->set_mean(mean());
    Out->set_population_variance(population_variance());
    Out->set_variance(variance());
    Out->set_deviation(deviation());
    Out->set_error_pct(error_pct());
  }

  void clear() {
    Mean = 0.0;
    SumSqDistance = 0.0;
    Count = 0;
  }

  private:
    double Mean = 0.0;
    double SumSqDistance = 0.0;
    uint64_t Count = 0;
};

} // end namespace halo