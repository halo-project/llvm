#pragma once

#include <mutex>
#include <list>

namespace halo {

template<typename ValTy>
class ThreadSafeList {
public:
  /// removes all elements from the list, front to back,
  /// and passes them to the Callback functor.
  ///
  /// @returns the number of elements consumed.
  size_t consume_all(std::function<void(ValTy&)> Callback) {
    std::lock_guard<std::mutex> guard(Lock);
    size_t numConsumed = 0;
    while (!Container.empty()) {
      Callback(Container.front());
      Container.pop_front();
      numConsumed++;
    }
    return numConsumed;
  }

  template<class... Args>
  void emplace_back(Args&&... args) {
    std::lock_guard<std::mutex> guard(Lock);
    Container.emplace_back(std::forward<Args>(args)...);
  }

protected:
  std::mutex Lock;
  std::list<ValTy> Container;
};



template<typename ValTy>
class ThreadSafeVector {
public:
  /// removes all elements from the vector, front to back,
  /// and passes them to the Callback functor.
  ///
  /// @returns the number of elements consumed.
  size_t consume_all(std::function<void(ValTy&)> Callback) {
    std::lock_guard<std::mutex> guard(Lock);
    size_t numConsumed = 0;

    for (auto &Elm : Container) {
      Callback(Elm);
      numConsumed++;
    }

    if (numConsumed)
      Container.clear();

    return numConsumed;
  }

  template<class... Args>
  void emplace_back(Args&&... args) {
    std::lock_guard<std::mutex> guard(Lock);
    Container.emplace_back(std::forward<Args>(args)...);
  }

protected:
  std::mutex Lock;
  std::vector<ValTy> Container;
};

} // end namespace halo