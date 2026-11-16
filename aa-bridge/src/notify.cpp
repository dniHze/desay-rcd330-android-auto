// SPDX-License-Identifier: GPL-3.0-or-later
#include "notify.h"

#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace aa {

void notify_shim(const std::string &msg) {
    const char *env = std::getenv("AA_SHIM_NOTIFY");
    const std::string path = env && *env ? env : "/tmp/run/aa/notify.sock";
    if (path == "-") return;
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return;
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr *>(&addr), sizeof addr);
    close(fd);
}

} // namespace aa
