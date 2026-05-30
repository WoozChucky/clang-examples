#pragma once

namespace netlib::detail {

// Ref-counted WSAStartup/WSACleanup. Each adapter holds one WinsockGuard for its
// lifetime; the first guard calls WSAStartup, the last destroyed calls WSACleanup.
class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    bool Ok() const { return m_Ok; }
private:
    bool m_Ok = false;
};

} // namespace netlib::detail
