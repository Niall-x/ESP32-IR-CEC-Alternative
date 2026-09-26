#pragma once
#include <string>

class ITransport {
public:
    virtual bool send(const std::string& cmd) = 0;

    virtual void invalidate() {}

    virtual ~ITransport() = default;
};
