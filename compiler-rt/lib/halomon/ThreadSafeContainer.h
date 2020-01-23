#pragma once

#include <mutex>

template <typename ContainerTy>
class ThreadSafeContainer {

    void access(std::function<void(ContainerTy&)> Callback) {
        std::lock_guard<std::mutex> guard(Lock);
        Callback(Container);
    }

private:
    std::mutex Lock;
    ContainerTy Container;
};