#pragma once

#include <memory>
#include <string>

#include "mirror_types.h"

class Receiver {
public:
    explicit Receiver(SharedState& state, std::string directHost = std::string(), int directPort = PORT);
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
