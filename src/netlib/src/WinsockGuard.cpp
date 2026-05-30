#include "WinsockGuard.h"

#include <mutex>
#include <winsock2.h>

#include "lib.h"           // SM_WARN / SM_ERROR (header-only)
#include "netlib/netlib.h" // NETLIB_API declaration for Version() (so it exports)

namespace netlib::detail {

namespace {
std::mutex g_Mx;
int        g_RefCount = 0;
}

WinsockGuard::WinsockGuard() {
    std::scoped_lock lk(g_Mx);
    if (g_RefCount == 0) {
        WSADATA wsa{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            SM_ERROR("netlib: WSAStartup failed (rc=%d)", rc);
            return;
        }
    }
    ++g_RefCount;
    m_Ok = true;
}

WinsockGuard::~WinsockGuard() {
    if (!m_Ok) return;
    std::scoped_lock lk(g_Mx);
    if (--g_RefCount == 0) {
        WSACleanup();
    }
}

} // namespace netlib::detail

namespace netlib {
const char* Version() { return "netlib 0.1"; }
}
