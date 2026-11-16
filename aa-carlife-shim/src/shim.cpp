// SPDX-License-Identifier: GPL-2.0-only
//
// aa-carlife-shim: stand-in for /svp/bin/carlifeservice.
//
// It serves the stock CarLife RCF contract (carlife_idl.hpp) so that the unchanged
// appconnect / connectionservice front end drives Android Auto instead of Baidu CarLife:
//
//   * RcfServer on <bind>:30407 with ISVP_CarLifeService bound (stock binds 0.0.0.0);
//   * RcfServer on 127.0.0.1:30408 publishing ISVP_CarLifeListener (clients subscribe);
//   * /tmp/run/service/registry/svp.svc.carlife, the empty file SVP_RegisterService creates
//     and the clients' StartService() waits for.
//
// The shim never touches USB. It tells the AA receiver what to do over a line-based Unix
// socket (AA_CTL_SOCK) and turns the receiver's state lines into connect-status callbacks:
//
//   shim -> receiver   "start" | "stop"
//                      "touch <down|up|move> <x> <y>"   (appconnect screen coordinates)
//                      "rotary <code> <value>"          (raw RotateKeyEvent, mapping TBD)
//                      "key <keyfeed token>"            (play / pause, see keyfeed.rs)
//                      "show <n>" | "foreground"
//   receiver -> shim   "state <connecting|connected|projecting|failed|disconnected>"
//                      "audio <media|speech|system|telephony> <0|1>"   phone audio on / off
//                      "mic <0|1>"                                     phone wants the mic / done
//                      "hostui"                  the phone's "back to the car" icon: OnGoToDeskTop
//   both ways          "hello <protocol>"        first line on a connection (protocol 2 now)
//   liveness           shim "ping <n>" every 2 s, receiver "pong <n>", only once the receiver
//                      said hello 2 or later; 3 unanswered: the receiver is killed (procguard
//                      restarts it) and the connection dropped. An old receiver is never pinged.
//   Lines either side does not know are ignored, so old and new builds can be mixed.
//
// Connect statuses follow stock (see carlife_idl.hpp): CarLifeConnection() publishes
// connecting (2, 0) at once; the receiver's first video frame gives (3, 0); a failed start gives
// (1, 1), no video in time gives the timeout (5, 1), and the end of a session (4, 0).
// With AA_SHIM_FAKE=1 (or --fake) no receiver is needed: CarLifeConnection() plays
// connecting -> video, to check how appconnect reacts on the unit.
//
// procguard launches services as "procguard NAME --debug-output none" and restarts them after
// an exit 0 or a signal, every 50 ms; any other exit leaves them down for the boot. So:
//   * --debug-output console logs to stderr; any other mode (none, the unit's default) logs to
//     syslog, through a socket that never blocks (see logf): under procguard stderr is the serial
//     console;
//   * a start-up failure writes its reason to AA_SHIM_FATAL_FILE and exits 0: the launcher then
//     runs the stock service in the slot (fatalStart). Exit 1 only when another server already
//     holds our ports, which then serves the slot;
//   * a crash logs a backtrace and dies by its signal (onCrash), so procguard restarts it;
//   * SIGTERM ends a running session first (CarLifeShim::endSession), then exits 0.
// unknown arguments only warn. "aa-carlife-shim --log-relay TAG" is a separate small mode: it
// copies stdin to syslog line by line (runLogRelay), for the processes the launcher starts.

#include "appc_patch.hpp"
#include "audio_idl.hpp"
#include "bt_idl.hpp"
#include "can_idl.hpp"
#include "carlife_idl.hpp"
#include "ilm_idl.hpp"
#include "power_idl.hpp"
#include "source_idl.hpp"

#include <RCF/RCF.hpp>

// See build.sh: RCF's FutureImpl destructor must be allowed to throw in every file, or an RCF
// error from it ends the process.
static_assert(!std::is_nothrow_destructible<RCF::FutureImpl<RCF::Void> >::value,
              "build without -include boost/version.hpp: RCF errors would terminate the shim");

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef AA_VERSION
#define AA_VERSION "dev"   // build.sh passes the sources' md5
#endif

namespace {

using namespace SVPCarLife;

const char* const kTag = "aa-carlife-shim";
const char* const kServiceName = "svp.svc.carlife";
const int kExitFatal = 1;   // only when another server holds our ports (see fatalStart)

bool gVerbose = false;

// ---------------------------------------------------------------------------------------
// Logging. Every thread logs, the RCF dispatch threads and sourcemgr's synchronous callback
// among them, so a line must never wait on its reader. On the unit's syslog (busybox syslogd,
// /dev/log) the line is one datagram, sent without blocking: a full socket gets 5 ms once,
// then the line is dropped and counted, and the count is logged once lines get through again;
// a syslogd that is not there (yet), or restarted, is reconnected at most once a second.
// --debug-output console writes to stderr instead (the SD-card flow's shim.log). Each line
// carries the monotonic time, as the receiver's and bridge's own stamps do.
// ---------------------------------------------------------------------------------------
namespace logsink {

const int kUser = 1 << 3;                      // LOG_USER, as the unit's own services log
enum { kErr = 3, kWarning = 4, kInfo = 6, kDebug = 7 };

std::mutex gMutex;                             // the socket; held at most 5 ms
int gFd = -1;
std::atomic<int> gCrashFd{-1};                 // the same socket, for onCrash (no locks there)
bool gStderr = true;                           // until main knows --debug-output
std::string gIdent = kTag;
bool gWithPid = true;
std::string gPath = "/dev/log";
unsigned gDropped = 0;
bool gDropping = false;                        // the last line was dropped: do not wait again
std::chrono::steady_clock::time_point gNextConnect;

void stamp(long& s, long& ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s = (long)ts.tv_sec;
    ms = (long)(ts.tv_nsec / 1000000);
}

const char* levelName(int level) {
    return level <= kErr ? "ERROR" : level == kWarning ? "WARN" : level == kDebug ? "DEBUG" : "INFO";
}

// gMutex held.
void connectLocked() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now < gNextConnect) return;
    gNextConnect = now + std::chrono::seconds(1);
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un a;
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, gPath.c_str(), sizeof a.sun_path - 1);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) {
        ::close(fd);
        return;
    }
    gFd = fd;
    gCrashFd = fd;
}

// gMutex held. One datagram; false when it was dropped.
bool sendLocked(const char* data, size_t len) {
    if (gFd < 0) connectLocked();
    if (gFd < 0) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (::send(gFd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) {
            gDropping = false;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            if (gDropping || attempt > 0) break;
            pollfd p;
            p.fd = gFd;
            p.events = POLLOUT;
            p.revents = 0;
            if (::poll(&p, 1, 5) <= 0) break;
            continue;
        }
        // ECONNREFUSED / ENOTCONN / ENOENT: syslogd went away or was restarted.
        gCrashFd = -1;
        ::close(gFd);
        gFd = -1;
        break;
    }
    gDropping = true;
    return false;
}

// "<pri>ident[pid]: [stamp] LEVEL: text" in buf; its length.
size_t format(char* buf, size_t size, int level, long s, long ms, const char* text) {
    int n;
    if (gWithPid) n = std::snprintf(buf, size, "<%d>%s[%d]: [%ld.%03ld] %s: %s", kUser | level, gIdent.c_str(),
                                    (int)getpid(), s, ms, levelName(level), text);
    else          n = std::snprintf(buf, size, "<%d>%s: [%ld.%03ld] %s: %s", kUser | level, gIdent.c_str(), s, ms,
                                    levelName(level), text);
    return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

// One line to syslog with this ident, or to stderr.
void line(int level, const char* text) {
    long s, ms;
    stamp(s, ms);
    if (gStderr) {
        std::fprintf(stderr, "[%s %ld.%03ld] %s: %s\n", gIdent.c_str(), s, ms, levelName(level), text);
        return;
    }
    char buf[1024];
    const size_t len = format(buf, sizeof buf, level, s, ms, text);
    std::lock_guard<std::mutex> lk(gMutex);
    if (!sendLocked(buf, len)) {
        ++gDropped;
        return;
    }
    if (gDropped) {
        char what[64], note[160];
        std::snprintf(what, sizeof what, "%u log lines dropped (syslog busy or down)", gDropped);
        if (sendLocked(note, format(note, sizeof note, kWarning, s, ms, what))) gDropped = 0;
    }
}

// The drops not yet reported (the log relay's exit).
unsigned dropped() {
    std::lock_guard<std::mutex> lk(gMutex);
    return gDropped;
}

} // namespace logsink

void logf(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logf(int level, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logsink::line(level, msg);
}
#define LOGI(...) logf(logsink::kInfo, __VA_ARGS__)
#define LOGW(...) logf(logsink::kWarning, __VA_ARGS__)
#define LOGE(...) logf(logsink::kErr, __VA_ARGS__)
#define LOGV(...) do { if (gVerbose) logf(logsink::kDebug, __VA_ARGS__); } while (0)

// A string from another device (a Bluetooth name) for a log line: control characters out.
std::string printable(const std::string& s) {
    std::string out = s.substr(0, 64);
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    return out;
}

// ---------------------------------------------------------------------------------------
// Timed waits on the monotonic clock. This toolchain's std::condition_variable waits on the
// wall clock (GCC 6.5: wait_for/wait_until become system_clock deadlines), and the unit's wall
// clock steps by hours twice early in every boot (about 2.3 s and 4.9 s, step 3 runs 45-47),
// just after the shim starts: a step forward ended every pending wait at once, a step back
// would stretch them by the step. Same interface as std::condition_variable, for
// std::unique_lock<std::mutex> and steady_clock (which is CLOCK_MONOTONIC here).
// ---------------------------------------------------------------------------------------
class MonoCv {
public:
    typedef std::chrono::steady_clock Clock;

    MonoCv() {
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&mCond, &a);
        pthread_condattr_destroy(&a);
    }
    ~MonoCv() { pthread_cond_destroy(&mCond); }
    MonoCv(const MonoCv&) = delete;
    MonoCv& operator=(const MonoCv&) = delete;

    void notify_one() { pthread_cond_signal(&mCond); }
    void notify_all() { pthread_cond_broadcast(&mCond); }

    void wait(std::unique_lock<std::mutex>& lk) { pthread_cond_wait(&mCond, lk.mutex()->native_handle()); }
    template <class Pred> void wait(std::unique_lock<std::mutex>& lk, Pred pred) {
        while (!pred()) wait(lk);
    }

    std::cv_status wait_until(std::unique_lock<std::mutex>& lk, Clock::time_point until) {
        const timespec ts = deadline(until);
        pthread_cond_timedwait(&mCond, lk.mutex()->native_handle(), &ts);
        return Clock::now() >= until ? std::cv_status::timeout : std::cv_status::no_timeout;
    }
    template <class Pred> bool wait_until(std::unique_lock<std::mutex>& lk, Clock::time_point until, Pred pred) {
        while (!pred())
            if (wait_until(lk, until) == std::cv_status::timeout) return pred();
        return true;
    }

    template <class Rep, class Period>
    std::cv_status wait_for(std::unique_lock<std::mutex>& lk, const std::chrono::duration<Rep, Period>& d) {
        return wait_until(lk, Clock::now() + std::chrono::duration_cast<Clock::duration>(d));
    }
    template <class Rep, class Period, class Pred>
    bool wait_for(std::unique_lock<std::mutex>& lk, const std::chrono::duration<Rep, Period>& d, Pred pred) {
        return wait_until(lk, Clock::now() + std::chrono::duration_cast<Clock::duration>(d), pred);
    }

private:
    static timespec deadline(Clock::time_point until) {
        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(until - Clock::now()).count();
        if (ns < 0) ns = 0;
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += static_cast<time_t>(ns / 1000000000LL);
        ts.tv_nsec += static_cast<long>(ns % 1000000000LL);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        return ts;
    }

    pthread_cond_t mCond;
};

std::string envOr(const char* name, const char* dflt) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(dflt);
}

// An integer setting within [lo, hi]; anything else is logged and gives the default.
int envInt(const char* name, int dflt, int lo = INT_MIN, int hi = INT_MAX) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    char* end = nullptr;
    errno = 0;
    const long n = std::strtol(v, &end, 10);
    if (!end || *end != '\0' || errno == ERANGE || n < lo || n > hi) {
        LOGW("%s=%s is not a number in %d..%d; using %d", name, v, lo, hi, dflt);
        return dflt;
    }
    return (int)n;
}

const int kMaxPort = 65535;

const char* statusName(S_CL_CONNECT_STATUS st) {
    switch (st) {
    case CL_ST_NONE:           return "none";
    case CL_ST_CONNECT_FAILED: return "connect failed";
    case CL_ST_CONNECTING:     return "connecting";
    case CL_ST_VIDEO:          return "video";
    case CL_ST_DISCONNECTED:   return "disconnected";
    case CL_ST_TIMEOUT:        return "timeout";
    default:                   return "?";
    }
}

const char* touchName(S_CL_TOUCH_EVENT ev) {
    switch (ev) {
    case CL_TOUCH_DOWN: return "down";
    case CL_TOUCH_UP:   return "up";
    case CL_TOUCH_MOVE: return "move";
    default:            return nullptr;
    }
}

struct Config {
    std::string bindHost;
    int svcPort;
    int pubPort;
    std::string registryDir;   // "-" disables
    std::string ctlSock;       // "-" disables
    bool fake;
    int fakeVideoMs;           // fake mode: connecting (2) -> video (3) delay
    int connectTimeoutMs;
    int receiverWaitMs;
    std::string ilmHost;
    int ilmPort;               // 0 disables the ILM calls
    std::string bridgeComm;    // comma-separated process names of the video bridge
    std::string videoSource;   // "x,y,w,h" of the bridge's surface to show; "-" leaves it
    std::string videoDest;     // "x,y,w,h" on the screen; "-" leaves it
    std::string sourceHost;
    int sourcePort;            // svp.svc.source; 0 disables the audio source claim
    int sourceListenPort;      // its status publisher (interruption ends); 0 disables
    std::string canHost;
    int canPort;               // svp.svc.can; 0 disables day/night, gear, right-hand drive, speed
    int canListenPort;         // the CAN service's publisher (speed); 0 disables speed
    std::string btHost;
    int btPort;                // svp.svc.bluetooth; 0 leaves the mic path to appconnect
    int btListenPort;          // its event publisher (pairing)
    bool btPairing;            // the AA pairing loop
    bool btA2dpDrop;           // Bluetooth music off while AA plays, back after
    bool btLock;               // no other device on Bluetooth while AA's phone holds the phone link
    std::string btLockFile;    // says the lock is ours, so a restart takes it off; "" never marks
    std::string onTopFile;     // "<pid> <0|1>": AA on screen, for the Bluetooth app (no pop-ups)
    bool callFix;              // keep an outgoing call on AA's screen (NeedBlockBTPopup)
    bool appcPatch;            // appconnect's code changes in memory (appc_patch.hpp)
    int callFixMs;
    std::string audioHost;
    int audioPort;             // audiomgr; 0 disables RegisterAudio
    std::string notifySock;    // datagrams from the bridge ("open <stream>"); "-" disables
    std::string powerHost;
    int powerPort;             // svp.svc.power: whether a switch-on is temporary
    int powerListenPort;       // its publisher: the unit switched off / on; 0 disables
    std::string lastModeFile;  // whether AA had the speakers at the last switch-off; "" disables
    std::string fatalFile;     // a start-up failure's reason, for the launcher (fatalStart)
    std::string receiverComm;  // the receiver's process names: the only ones the liveness check kills
    std::string bootsFile;     // the launcher's boot-loop counter: a clean switch-off clears it
};

// Creates the directories of `path` that are missing, with `mode`.
bool mkdirs(const std::string& path, mode_t mode) {
    std::string cur;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        cur = path.substr(0, pos);
        if (cur.empty()) continue;
        if (::mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
    }
    return true;
}

// The directory part of a file path ("" when there is none).
std::string dirName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : slash == 0 ? std::string("/") : path.substr(0, slash);
}

// ---------------------------------------------------------------------------------------
// Control channel to the AA receiver: one Unix stream socket, one client at a time.
// Each side says "hello <protocol>" first. A receiver that says protocol 2 or later is pinged
// every 2 s ("ping <n>", answered "pong <n>"); after 3 pings without an answer it is taken as
// hung: killed (only a process whose name is in AA_RECEIVER_COMM, found by SO_PEERCRED;
// procguard restarts it) and the connection shut down, which reports "state gone" as any end
// of the connection does. hello / ping / pong stay in here; every other line goes to the LineFn.
// ---------------------------------------------------------------------------------------
class ControlChannel {
public:
    // Gets every line the receiver sends, and "state gone" when it goes away.
    typedef void (*LineFn)(void* ctx, const std::string& line);

    enum { kProtocol = 2 };

    ControlChannel() : mListenFd(-1), mClientFd(-1), mConnections(0), mStop(false), mOnLine(nullptr), mCtx(nullptr) {}

    // `receiverNames`: the process names (comma-separated) the liveness check may kill.
    bool open(const std::string& path, LineFn onLine, void* ctx, const std::string& receiverNames) {
        mPath = path;
        mOnLine = onLine;
        mCtx = ctx;
        mReceiverNames = "," + receiverNames + ",";
        if (path.size() >= sizeof(((sockaddr_un*)0)->sun_path)) {
            LOGE("control socket path too long: %s", path.c_str());
            return false;
        }
        mListenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (mListenFd < 0) { LOGE("control socket(): %s", std::strerror(errno)); return false; }
        ::unlink(path.c_str());
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
        if (::bind(mListenFd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(mListenFd, 1) != 0) {
            LOGE("control socket %s: %s", path.c_str(), std::strerror(errno));
            ::close(mListenFd);
            mListenFd = -1;
            return false;
        }
        ::chmod(path.c_str(), 0600); // the receiver runs as root, as every service on the unit
        mThread = std::thread(&ControlChannel::run, this);
        LOGI("control socket listening on %s", path.c_str());
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all(); // a waitConnected() gives up
        if (mThread.joinable()) mThread.join();
        std::lock_guard<std::mutex> lk(mMutex);
        if (mClientFd >= 0) { ::close(mClientFd); mClientFd = -1; }
        if (mListenFd >= 0) { ::close(mListenFd); mListenFd = -1; ::unlink(mPath.c_str()); }
    }

    bool connected() {
        std::lock_guard<std::mutex> lk(mMutex);
        return mClientFd >= 0;
    }

    // Receiver connections accepted so far: changes whenever the receiver (re)connects, for
    // what it must hear once per connection.
    unsigned connections() {
        std::lock_guard<std::mutex> lk(mMutex);
        return mConnections;
    }

    bool waitConnected(int timeoutMs) {
        std::unique_lock<std::mutex> lk(mMutex);
        mCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return mClientFd >= 0 || mStop; });
        return mClientFd >= 0;
    }

    // Called from RCF dispatch threads and sourcemgr's synchronous callback, so it never waits
    // long: a receiver that stops reading gets 1 s (SO_SNDTIMEO), then the connection is shut
    // down. Only the reader thread closes the socket; it then reports "state gone".
    bool send(const std::string& line) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mClientFd < 0 || mSendFailed) return false;
        std::string out = line + "\n";
        const char* p = out.data();
        size_t left = out.size();
        while (left > 0) {
            ssize_t n = ::send(mClientFd, p, left, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                LOGW("receiver write failed (%s), dropping connection", n < 0 ? std::strerror(errno) : "nothing written");
                mSendFailed = true;
                ::shutdown(mClientFd, SHUT_RDWR);
                return false;
            }
            p += n;
            left -= (size_t)n;
        }
        return true;
    }

private:
    typedef std::chrono::steady_clock Clock;

    // What one connection said: its protocol, the pings, a line being cut.
    struct Peer {
        int protocol = 0;          // from its hello; 0: an old receiver, never pinged
        unsigned pinged = 0;       // the last ping sent
        unsigned answered = 0;     // the last pong received
        Clock::time_point nextPing;
        std::string buf;
        bool discarding = false;   // the rest of an over-long line
    };

    // Reader thread: one received line.
    void handle(Peer& peer, const std::string& line) {
        unsigned n = 0;
        if (std::sscanf(line.c_str(), "hello %u", &n) == 1) {
            peer.protocol = (int)n;
            LOGI("receiver: protocol %u%s", n, n >= 2 ? " (pinged every 2 s)" : "");
            if (n >= 2) {
                peer.pinged = peer.answered = 0;
                peer.nextPing = Clock::now() + std::chrono::seconds(2);
            }
        } else if (std::sscanf(line.c_str(), "pong %u", &n) == 1) {
            if (n <= peer.pinged && n > peer.answered) peer.answered = n;
        } else if (mOnLine) {
            mOnLine(mCtx, line);
        }
    }

    // Reader thread: received bytes, cut into lines. A line over 4096 bytes is dropped whole.
    void feed(Peer& peer, const char* data, size_t n) {
        peer.buf.append(data, n);
        size_t pos;
        while ((pos = peer.buf.find('\n')) != std::string::npos) {
            std::string line = peer.buf.substr(0, pos);
            peer.buf.erase(0, pos + 1);
            if (peer.discarding) {
                peer.discarding = false;
                continue;
            }
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            if (!line.empty()) handle(peer, line);
        }
        if (peer.buf.size() > 4096) {
            LOGW("receiver: a line over 4096 bytes, dropped");
            peer.buf.clear();
            peer.discarding = true;
        }
    }

    // Reader thread: the next ping, or the end of a receiver that stopped answering.
    void keepAlive(Peer& peer, int fd) {
        if (peer.protocol < 2) return;
        const Clock::time_point now = Clock::now();
        if (now < peer.nextPing) return;
        peer.nextPing = now + std::chrono::seconds(2);
        if (peer.pinged - peer.answered < 3) {
            send("ping " + std::to_string(++peer.pinged));
            return;
        }
        LOGW("receiver: no answer to 3 pings, taken as hung");
        peer.protocol = 0;
        killPeer(fd);
        std::lock_guard<std::mutex> lk(mMutex);
        if (mClientFd == fd && !mSendFailed) {
            mSendFailed = true;
            ::shutdown(fd, SHUT_RDWR); // the reader sees the end: "state gone"
        }
    }

    // SIGKILL for the process at the other end, if it is the receiver.
    void killPeer(int fd) {
        ucred cred;
        socklen_t len = sizeof cred;
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.pid <= 0) {
            LOGW("receiver: its pid is unknown (%s); not killed", std::strerror(errno));
            return;
        }
        char path[64], comm[32] = {0};
        std::snprintf(path, sizeof path, "/proc/%d/comm", (int)cred.pid);
        if (FILE* f = std::fopen(path, "r")) {
            if (std::fgets(comm, sizeof comm, f)) comm[std::strcspn(comm, "\n")] = '\0';
            std::fclose(f);
        }
        if (!comm[0] || mReceiverNames.find("," + std::string(comm) + ",") == std::string::npos) {
            LOGW("receiver: pid %d is \"%s\", not %s; not killed", (int)cred.pid, comm,
                 mReceiverNames.substr(1, mReceiverNames.size() - 2).c_str());
            return;
        }
        if (::kill(cred.pid, SIGKILL) == 0) LOGW("receiver: pid %d (%s) killed; procguard restarts it", (int)cred.pid, comm);
        else LOGW("receiver: killing pid %d: %s", (int)cred.pid, std::strerror(errno));
    }

    void run() {
        Peer peer;
        while (!mStop) {
            int clientFd;
            {
                std::lock_guard<std::mutex> lk(mMutex);
                clientFd = mClientFd;
            }
            pollfd fds[2];
            int nfds = 0;
            fds[nfds].fd = mListenFd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds;
            if (clientFd >= 0) {
                fds[nfds].fd = clientFd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds;
            }
            const int r = ::poll(fds, nfds, 200);
            if (r < 0) continue;

            if (fds[0].revents & POLLIN) {
                int fd = ::accept4(mListenFd, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd >= 0) {
                    const timeval sendTimeout = {1, 0};
                    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof sendTimeout);
                    bool replaced = false;
                    {
                        std::lock_guard<std::mutex> lk(mMutex);
                        if (mClientFd >= 0) {
                            LOGW("receiver reconnected, replacing the previous connection");
                            ::close(mClientFd);
                            replaced = true;
                        }
                        mClientFd = fd;
                        mSendFailed = false;
                        ++mConnections;
                        LOGI("receiver connected");
                    }
                    mCv.notify_all();
                    peer = Peer();
                    // The connection it replaced ended without its end being read: whatever
                    // session ran on it is over (outside the lock, as below).
                    if (replaced && mOnLine) mOnLine(mCtx, "state gone");
                    send("hello " + std::to_string((int)kProtocol));
                }
                continue;
            }
            if (nfds > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                char tmp[512];
                ssize_t n = ::recv(clientFd, tmp, sizeof tmp, 0);
                if (n <= 0) {
                    {
                        std::lock_guard<std::mutex> lk(mMutex);
                        if (mClientFd == clientFd) {
                            ::close(mClientFd);
                            mClientFd = -1;
                        }
                    }
                    peer = Peer();
                    LOGW("receiver disconnected");
                    // Outside the lock: the handler stops the session, and the parts it stops
                    // send to the receiver (send() takes the lock) while holding their own.
                    if (mOnLine) mOnLine(mCtx, "state gone");
                    continue;
                }
                feed(peer, tmp, (size_t)n);
            }
            if (clientFd >= 0) keepAlive(peer, clientFd);
        }
    }

    std::string mPath;
    std::string mReceiverNames;  // ",name,name,"
    int mListenFd;
    int mClientFd;
    bool mSendFailed = false;  // this connection is being shut down (send)
    unsigned mConnections;
    std::atomic<bool> mStop;
    std::mutex mMutex;
    MonoCv mCv;
    std::thread mThread;
    LineFn mOnLine;
    void* mCtx;
};

// ---------------------------------------------------------------------------------------
// The servant. Method names and signatures must match ISVP_CarLifeService exactly.
// ---------------------------------------------------------------------------------------
// The GENIVI ILM control library on the unit (/usr/lib/libilmControl.so.1.9.1, ilm_init in its
// libilmCommon), loaded at run time: SVP_iLMService::ShowSurface sets visibility, opacity and
// a destination rectangle (the surface's own size), but never the source rectangle. The
// decoder's buffers are padded (896x576 for an 800x480 picture, which starts at 32,24 in the
// sink's surface: gst-ducati's PADX/PADY, and the step 3 screen captures), so without these the
// picture is off-centre with a green edge and its bottom cut off.
class IlmControl {
public:
    // "x,y,w,h" -> rectangle; false for "-" or anything malformed.
    static bool parse(const std::string& s, int r[4]) {
        return std::sscanf(s.c_str(), "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4 && r[2] > 0 && r[3] > 0;
    }

    // Sets the rectangles and commits; logs once if the library is missing.
    void setRegions(unsigned int id, const std::string& source, const std::string& dest) {
        int src[4], dst[4];
        const bool hasSrc = parse(source, src), hasDst = parse(dest, dst);
        if ((!hasSrc && !hasDst) || !load()) return;
        int a = hasSrc ? mSetSource(id, src[0], src[1], src[2], src[3]) : 0;
        int b = hasDst ? mSetDest(id, dst[0], dst[1], dst[2], dst[3]) : 0;
        int c = mCommit();
        LOGV("ILM control: surface %u source %s, destination %s -> %d/%d/%d", id, source.c_str(), dest.c_str(), a, b, c);
    }

private:
    typedef int (*InitFn)();
    typedef int (*RectFn)(unsigned int, int, int, int, int);
    typedef int (*CommitFn)();

    bool load() {
        if (mTried) return mReady;
        mTried = true;
        const char* names[] = {"libilmControl.so.1.9.1", "libilmControl.so.1", "libilmControl.so"};
        void* lib = nullptr;
        for (const char* n : names) {
            if ((lib = dlopen(n, RTLD_NOW | RTLD_GLOBAL))) break;
        }
        if (!lib) {
            LOGW("ILM control: libilmControl not available (%s); surface rectangles left as they are", dlerror());
            return false;
        }
        InitFn init = reinterpret_cast<InitFn>(dlsym(lib, "ilm_init"));
        mSetSource = reinterpret_cast<RectFn>(dlsym(lib, "ilm_surfaceSetSourceRectangle"));
        mSetDest = reinterpret_cast<RectFn>(dlsym(lib, "ilm_surfaceSetDestinationRectangle"));
        mCommit = reinterpret_cast<CommitFn>(dlsym(lib, "ilm_commitChanges"));
        if (!init || !mSetSource || !mSetDest || !mCommit) {
            LOGW("ILM control: libilmControl lacks the functions needed");
            return false;
        }
        int r = init();
        LOGI("ILM control: ilm_init -> %d", r);
        mReady = (r == 0);
        return mReady;
    }

    bool mTried = false;
    bool mReady = false;
    RectFn mSetSource = nullptr;
    RectFn mSetDest = nullptr;
    CommitFn mCommit = nullptr;
};

// ---------------------------------------------------------------------------------------
// The video surface. appconnect calls Show(1) when its CarLife screen opens and Show(0) when
// it leaves; stock carlifeservice answers with SVP_iLMService::ShowSurface(getpid() +
// 20000000, 1 / 0) on its own video surface, and the ILM service sets visibility and opacity
// and places the video. Here the video is drawn by the bridge, whose waylandsink creates its
// ILM surface with the same convention, so the id is 20000000 + the bridge's pid. The calls
// run on a worker: WaitSurface blocks until the surface exists (it appears after the first
// decoded frame), and Show() itself must return at once. While the screen is up the worker
// looks for the bridge every 2 s: a bridge restarted by procguard (a new pid, so a new surface),
// or one that was not running yet, is shown without waiting for the next Show(1).
class VideoSurface {
public:
    explicit VideoSurface(const Config& cfg) : mCfg(cfg), mWant(false), mDirty(false), mStop(false) {
        if (mCfg.ilmPort > 0) mWorker = std::thread(&VideoSurface::run, this);
    }

    ~VideoSurface() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

    // From Show(n).
    void show(bool on) { request(on, true); }

    // A new AA session means a new pipeline and a new surface, and so does the bridge's video
    // rebuild (BridgeNotify "video"): show it again if the screen is up.
    void newSession() { request(false, false); }

private:
    void request(bool on, bool set) {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (set) mWant = on;
            else if (!mWant) return;
            mDirty = true;
        }
        mCv.notify_all();
    }

    // 20000000 + the pid of the newest process whose name is in bridgeComm (a bridge being
    // replaced may still be there); 0 if none runs.
    unsigned int findSurface() {
        DIR* d = opendir("/proc");
        if (!d) return 0;
        const std::string names = "," + mCfg.bridgeComm + ",";
        long best = 0;
        unsigned long long bestStart = 0;
        while (dirent* e = readdir(d)) {
            char* end = nullptr;
            long pid = std::strtol(e->d_name, &end, 10);
            if (!end || *end != '\0' || pid <= 0) continue;
            char path[64];
            std::snprintf(path, sizeof path, "/proc/%ld/comm", pid);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            char comm[32] = {0};
            if (std::fgets(comm, sizeof comm, f)) comm[std::strcspn(comm, "\n")] = '\0';
            std::fclose(f);
            if (!comm[0] || names.find("," + std::string(comm) + ",") == std::string::npos) continue;
            const unsigned long long start = appc_patch::startTime(static_cast<int>(pid));
            if (!best || start > bestStart) {
                best = pid;
                bestStart = start;
            }
        }
        closedir(d);
        return best ? kILMVideoSurfaceBase + static_cast<unsigned int>(best) : 0;
    }

    void run() {
        unsigned int shown = 0;  // the surface last shown; 0 while hidden
        unsigned int failed = 0; // the surface whose calls failed, tried again up to 3 times
        int failures = 0;
        for (;;) {
            bool on, asked;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                if (mWant) mCv.wait_for(lk, std::chrono::seconds(2), [this] { return mStop || mDirty; });
                else       mCv.wait(lk, [this] { return mStop || mDirty; });
                if (mStop) return;
                asked = mDirty;
                mDirty = false;
                on = mWant;
            }
            unsigned int id = findSurface();
            if (!asked) {
                // The look every 2 s while shown: only a surface not shown yet needs anything.
                if (!on || !id || id == shown || (id == failed && failures >= 3)) continue;
                LOGI("ILM: the bridge's surface is now %u (the bridge started or restarted)", id);
            }
            if (!id) {
                LOGW("ILM: no video bridge running (%s), cannot %s its surface%s", mCfg.bridgeComm.c_str(),
                     on ? "show" : "hide", on ? " yet" : "");
                shown = 0;
                continue;
            }
            try {
                RcfClient<SVP_iLMService> ilm(RCF::TcpEndpoint(mCfg.ilmHost, mCfg.ilmPort));
                if (on) {
                    ilm.getClientStub().setRemoteCallTimeoutMs(15000);
                    int w = ilm.WaitSurface(id);
                    LOGV("ILM: WaitSurface(%u) -> %d", id, w);
                }
                ilm.getClientStub().setRemoteCallTimeoutMs(5000);
                int r = ilm.ShowSurface(id, on ? 1 : 0);
                LOGI("ILM: ShowSurface(%u, %d) -> %d", id, on ? 1 : 0, r);
                shown = on ? id : 0;
                failures = 0;
                if (on) mControl.setRegions(id, mCfg.videoSource, mCfg.videoDest);
                // Show(1) comes just before the surface exists (the sink creates it after the
                // first frame). In case WaitSurface does not wait for a surface it has not seen,
                // show it once more a little later, unless something new was asked meanwhile.
                if (on) {
                    std::unique_lock<std::mutex> lk(mMutex);
                    if (!mCv.wait_for(lk, std::chrono::seconds(3), [this] { return mStop || mDirty; })) {
                        lk.unlock();
                        if (findSurface() != id) continue; // the bridge was replaced: the next look shows it
                        r = ilm.ShowSurface(id, 1);
                        LOGV("ILM: ShowSurface(%u, 1) again -> %d", id, r);
                        mControl.setRegions(id, mCfg.videoSource, mCfg.videoDest);
                    }
                }
            } catch (const RCF::Exception& e) {
                LOGW("ILM: %s surface %u failed: %s", on ? "showing" : "hiding", id, e.getErrorString().c_str());
                shown = 0; // the next look tries again, up to 3 times for this surface
                if (id != failed) failures = 0;
                failed = id;
                ++failures;
            }
        }
    }

    const Config& mCfg;
    IlmControl mControl;   // used from the worker thread only
    std::mutex mMutex;
    MonoCv mCv;
    bool mWant;
    bool mDirty;
    bool mStop;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// Whether something accepts TCP connections on host:port (within timeoutMs). Checked before
// RCF subscriptions: RCF 2.1's createSubscription sends its request as a temporary FutureImpl
// (SubscriptionService::doRequestSubscription: clientStub.ping()), so a failure throws from a
// destructor, which terminates the process (step 3 desktop test: the shim aborted with no
// publisher to subscribe to).
static bool tcpReachable(const std::string& host, int port, int timeoutMs) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    bool ok = ::inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1;
    if (ok && ::connect(fd, (sockaddr*)&a, sizeof a) != 0) {
        ok = false;
        if (errno == EINPROGRESS) {
            pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            int err = 0;
            socklen_t len = sizeof err;
            ok = ::poll(&p, 1, timeoutMs) == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
    }
    ::close(fd);
    return ok;
}


// ---------------------------------------------------------------------------------------
// Last mode: whether Android Auto had the speakers when the unit was last switched off, kept in
// a file (AA_LAST_MODE_FILE: on the SD card with start.sh, /storage/data/data/aa/ in the image).
// CarPlay's service keeps the same in connectionservice (Set_/Get_Carplay_LastMode, persist
// 100-102) and restores the audio owner at the next session; the CarLife slot has nothing like
// it, and the phone's autoplay took the speakers from the radio whatever was playing before
// (step 3 run 41: AA media 2.4 s after projecting). At the next connection the receiver gets
// "autoplay 0" when the car's own audio had them, and refuses the phone's autoplay once
// (AaConfig::refuse_autoplay): the source the unit restored keeps playing until the user
// starts AA's media.
//   * AA has the speakers once its media source is granted, and loses them when a main source
//     (radio, USB, Bluetooth audio...) takes over for good (AudioSource). Calls, prompts, the
//     mute and the power pause leave it.
//   * A change is written once it has held for 5 s, or at the switch-off if it is older than
//     1 s there, so the source changes of a switch-off itself are not taken for the user's; from
//     PS_USER_OFF / PS_SHUT_DOWN to PS_USER_ON nothing changes.
//   * Unknown (no file yet): AA may autoplay, as before.
// The file is replaced whole (a temporary file, fsync, rename, fsync of the directory), so a
// power cut leaves the old or the new record; its directory is made (0700) if missing. A
// change still waiting at exit is written if it is older than 1 s.
// Replaces `path` whole with `text`: a temporary file, fsync, rename, fsync of the directory, so
// a power cut leaves the old file or the new one. Returns "" or what failed.
std::string replaceFile(const std::string& path, const char* text) {
    const std::string tmp = path + ".tmp";
    const size_t len = std::strlen(text);
    const char* failed = nullptr;
    int err = 0;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { failed = "open"; err = errno; }
    else if (::write(fd, text, len) != (ssize_t)len) { failed = "write"; err = errno; }
    else if (::fsync(fd) != 0) { failed = "fsync"; err = errno; }
    if (fd >= 0) ::close(fd);
    if (!failed && ::rename(tmp.c_str(), path.c_str()) != 0) { failed = "rename"; err = errno; }
    if (failed) return std::string(failed) + ": " + std::strerror(err);
    // The rename itself is safe only once the directory is on the disk.
    const std::string dir = dirName(path);
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return std::string();
}

class LastMode {
public:
    explicit LastMode(const Config& cfg) : mPath(cfg.lastModeFile) {
        if (mPath.empty()) return;
        const std::string dir = dirName(mPath);
        if (!dir.empty() && !mkdirs(dir, 0700)) LOGW("last mode: cannot create %s: %s", dir.c_str(), std::strerror(errno));
        char buf[64] = {0};
        const int fd = ::open(mPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            const ssize_t n = ::read(fd, buf, sizeof buf - 1);
            ::close(fd);
            if (n > 0 && !std::strncmp(buf, "speakers car", 12)) mSaved = false;
        }
        LOGI("last mode: %s (%s)", mSaved ? "Android Auto had the speakers, or nothing saved yet"
                                          : "the car's own audio had the speakers", mPath.c_str());
        mWorker = std::thread(&LastMode::run, this);
    }

    ~LastMode() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

    bool enabled() const { return !mPath.empty(); }

    // At the last switch-off (true when unknown).
    bool aaHadSpeakers() {
        std::lock_guard<std::mutex> lk(mMutex);
        return mSaved;
    }

    // AA took (true) or lost (false) the speakers.
    void speakers(bool aa) {
        if (mPath.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (mFrozen) return;
            mPending = aa ? 1 : 0;
            mPendingAt = std::chrono::steady_clock::now();
        }
        mCv.notify_all();
    }

    // The unit is switched off (true) or on again: keep what it was at the switch-off. A change
    // from just before (1 s) is the switch-off's own; an older one is written now.
    void freeze(bool on) {
        if (mPath.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (on == mFrozen) return;
            mFrozen = on;
            if (!on || mPending < 0) return;
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (now - mPendingAt < std::chrono::seconds(1)) mPending = -1;
            else mPendingAt = now - std::chrono::seconds(5);
        }
        mCv.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(mMutex);
        while (!mStop) {
            if (mPending < 0) {
                mCv.wait(lk);
                continue;
            }
            const std::chrono::steady_clock::time_point due = mPendingAt + std::chrono::seconds(5);
            if (std::chrono::steady_clock::now() < due) {
                mCv.wait_until(lk, due);
                continue;
            }
            const bool aa = mPending == 1;
            mPending = -1;
            if (aa == mSaved) continue;
            mSaved = aa;
            lk.unlock();
            write(aa);
            lk.lock();
        }
        // Stopping: a change that has held for 1 s is the user's, as at a switch-off (freeze).
        if (mPending >= 0 && !mFrozen && std::chrono::steady_clock::now() - mPendingAt >= std::chrono::seconds(1) &&
            (mPending == 1) != mSaved) {
            mSaved = mPending == 1;
            lk.unlock();
            write(mSaved);
        }
    }

    void write(bool aa) {
        const std::string err = replaceFile(mPath, aa ? "speakers aa\n" : "speakers car\n");
        if (!err.empty()) {
            LOGW("last mode: writing %s: %s", mPath.c_str(), err.c_str());
            return;
        }
        LOGI("last mode: %s now has the speakers", aa ? "Android Auto" : "the car's own audio");
    }

    const std::string mPath;
    std::mutex mMutex;
    MonoCv mCv;
    bool mSaved = true;   // what the file says
    int mPending = -1;    // a change to write: 1 AA, 0 the car, -1 none
    std::chrono::steady_clock::time_point mPendingAt;
    bool mFrozen = false;
    bool mStop = false;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// The CarLife audio sources in the SVP source service (sourcemgr, svp.svc.source; see
// source_idl.hpp). Which source is active decides what the speakers play and where the media
// keys go: without this the radio keeps both (step 3 on the unit: no AA sound, steering-wheel
// next/previous changed the radio station). As stock carlifeservice does:
//   * a session starts: SID_CARLIFE_MEDIA/NAVI/PROMPT/ASR SS_ATTACHED, then SS_VALID;
//   * the phone starts media audio: RequestSource(SID_CARLIFE_MEDIA) and, when it answers 1,
//     ConfirmSource (stock also did this on every Show(1); we do only to resume after a
//     temporary interruption, see Show);
//   * a voice prompt plays (AA speech audio): SID_CARLIFE_NAVI is requested, and released when
//     it ends (stock ttsInit / ttsStop). Unlike stock, the assistant uses it too: while the
//     phone has the mic and while AA system sounds play (see navi());
//   * sourcemgr calls back cbSourceOp(SID_CARLIFE_MEDIA, PAUSE) when another source takes
//     over; it never calls back PLAY. Instead its SVP_SourceListener topic publishes who became
//     active, and (34, SS_RESTORED) when the interruption is over and the owner should take the
//     source back (source_idl.hpp). The phone hears it as Android Auto audio focus (receiver
//     "focus" lines), as CarPlay's service tells the iPhone: a temporary loss when the media is
//     interrupted, a permanent one once a main source (radio, USB, Bluetooth audio...) turns
//     out to have taken over, a gain when the media source is ours again;
//   * the car's mute: appconnect's SendModuleControlCmd(3, 0 / 1) while the media source is
//     active (see musicControl); the unit switched off and on: the same, as stock does (power);
//   * the session ends: SS_DETACHED.
// The callback arrives over a callback connection: register with getpid() and turn that
// connection around (RCF::createCallbackConnection), exactly as libsource's client does.
// Every call runs on one worker thread, in order; a failed call drops the connection and the
// next try makes a new one. Attached or detached is a wanted state: the worker sets it until a
// whole Attach (or Detach) has gone through, every 2 s while sourcemgr fails (one lost call
// meant no AA sound for that session, or sources left attached after it); a request or a
// release is tried 3 times, 1 s apart. Requests wait until the attach has gone through.
class AudioSource {
public:
    AudioSource(const Config& cfg, ControlChannel& ctl, LastMode& last)
        : mCfg(cfg), mCtl(ctl), mLast(last), mStop(false), mAttached(false), mListener(*this) {
        if (mCfg.sourcePort > 0) mWorker = std::thread(&AudioSource::run, this);
    }

    ~AudioSource() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
        mCallbackClient.reset();
        mClient.reset();
        if (mServer) mServer->stop();
        try {
            if (mSubscription) mSubscription->close();
            mSubscription.reset();
            if (mSubServer) mSubServer->stop();
        } catch (const std::exception& e) {
            LOGW("source: closing the status subscription: %s", e.what());
        }
    }

    // A session started (on) or ended (off). Repeats are ignored.
    void attach(bool on) {
        if (mCfg.sourcePort <= 0) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (mAttached == on) return;
            mAttached = on;
            // Detaching gives every source back; what was still queued for the old state is moot.
            mNaviUses = 0;
            mNaviHeld = false;
            mReleasePending = false;
            mLoss = kNoLoss;
            mMuted = false;
            mPowerPaused = false;
            mMediaPlaying = false;
            mTasks.clear();
            mRetryAt = Clock::time_point();
        }
        mCv.notify_all();
    }

    // Nothing queued, nothing running, and sourcemgr has the wanted state (shutdown waits for it).
    bool idle() {
        if (mCfg.sourcePort <= 0) return true;
        std::lock_guard<std::mutex> lk(mMutex);
        return mTasks.empty() && !mBusy && mApplied == (mAttached ? kApplied : kDetachedState);
    }

    // Ask for a source: RequestSource and, when it answers 1, ConfirmSource.
    void request(int source) {
        if (mCfg.sourcePort <= 0) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (!mAttached) return;
            mTasks.push_back(Task(Request, source));
        }
        mCv.notify_all();
    }

    // AA media playback started (on) or stopped (off), from the receiver: on asks for the media
    // source, as stock mediaPrepared.
    void media(bool on) {
        if (mCfg.sourcePort <= 0) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mMediaPlaying = on;
        }
        if (on) request(SVPSource::SID_CARLIFE_MEDIA);
    }

    // Back on the AA screen while another source interrupts the media for a while (a call, a
    // prompt): ask for the media source again (granted, the phone gets the audio focus back; see
    // doTask). While the interruption still runs, sourcemgr keeps it waiting and publishes
    // SS_RESTORED later. Not after a main source took over (the radio, USB, Bluetooth audio):
    // the user chose it, and opening the AA screen with the APP key, say for the map, silenced
    // the radio, even with no AA media to play (a friend's unit, rc3). AA takes the speakers
    // back when its media plays again (media), as CarPlay does; its prompts mix over the radio.
    void resumeIfInterrupted() {
        bool interrupted;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            interrupted = mLoss == kTransientLoss;
        }
        if (interrupted) request(SVPSource::SID_CARLIFE_MEDIA);
    }

    // appconnect's SendModuleControlCmd(3, cmd), CarLife's music module: 0 pause, 1 play. It
    // sends 0 on the car's mute while the media source is active, on its Disconnect button and
    // when it goes to the background for the radio or media player during voice; 1 on unmute
    // while the media source is active, and at times that are not a request to play (a Mute OFF
    // broadcast at session start, a call ending) (CCarlifeDataMgr::OnAudioMuteStateChanged
    // 0x476dc, CAppconnectMainDlg::OnButtonClick 0x301c0, SVP_AppFw_IsReady 0x29af4). As
    // CarPlay's service borrows the iPhone's main audio on mute and gives it back on unmute: a
    // temporary focus loss, and the gain only if that loss was ours (the latch drops the other
    // 1s; repeats are harmless). The DSP mutes the media in hardware either way.
    void musicControl(bool play) {
        if (mCfg.sourcePort <= 0) return;
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mAttached) return;
        if (!play) {
            if (mMuted) return;
            mMuted = true;
            if (mLoss == kNoLoss) sendFocus("transient", "the car paused the music (mute)");
        } else {
            if (!mMuted) return;
            mMuted = false;
            if (mLoss == kNoLoss && !mPowerPaused) sendFocus("gain", "the car resumed the music (unmute)");
        }
    }

    // The unit switched off (false) or on (true), as stock carlifeservice's OnPowerStateChanged
    // (power_idl.hpp): off pauses AA's media unless it is not playing or already paused (mute,
    // another source), and remembers that it did; on resumes only that, and not on a temporary
    // switch-on (resume false).
    void power(bool on, bool resume = true) {
        if (mCfg.sourcePort <= 0) return;
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mAttached) return;
        if (!on) {
            if (mPowerPaused) return;
            if (!mMediaPlaying || mMuted || mLoss != kNoLoss) {
                LOGI("power: Android Auto's media is not playing; nothing to pause");
                return;
            }
            mPowerPaused = true;
            sendFocus("transient", "the unit was switched off");
        } else {
            if (!mPowerPaused) return;
            mPowerPaused = false;
            if (!resume) {
                LOGI("power: a temporary switch-on; the media stays paused, as stock CarLife leaves it");
                return;
            }
            if (mLoss == kNoLoss && !mMuted) sendFocus("gain", "the unit was switched on");
        }
    }

    // What holds the navigation source: AA's speech (navigation prompts and the assistant's
    // replies and chimes), its system sounds, and the mic while the phone listens.
    enum NaviUse { kNaviSpeech = 1, kNaviSystem = 2, kNaviMic = 4 };

    // Hold SID_CARLIFE_NAVI while anything uses it (the first use requests it) and give it back
    // 2 s after the last use ends, so one assistant turn (mic, then a reply, then maybe the mic
    // again) keeps it throughout. Stock voice recognition took SID_CARLIFE_ASR for the mic,
    // but on this unit that source plays at the speech-recognition volume, full by default:
    // source.conf gives it its own audio source, and audiomgr sets the prompt stream's volume
    // only for CL_NAVI (the navigation volume; SVPRmtAudio::ActiveSource), so the assistant's
    // first chime came out very loud (step 3). ASR also paused the radio; navigation mixes
    // with it. The mic does not need ASR: it follows the voice module status (MicPath).
    void navi(NaviUse use, bool on) {
        if (mCfg.sourcePort <= 0) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (!mAttached) return;
            const bool before = mNaviUses != 0;
            if (on) mNaviUses |= use;
            else    mNaviUses &= ~static_cast<unsigned>(use);
            const bool now = mNaviUses != 0;
            if (before == now) return;
            if (now) {
                if (mReleasePending) mReleasePending = false; // never given back: keep it
                else if (!mNaviHeld) {
                    mNaviHeld = true;
                    mTasks.push_back(Task(Request, SVPSource::SID_CARLIFE_NAVI));
                }
            } else {
                mReleasePending = true;
                mReleaseAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNaviLingerMs);
            }
        }
        mCv.notify_all();
    }

    // SVP_SourceCallback, on the callback server's thread.
    int cbSourceOp(int source, SVPSource::SourceOp op) {
        const char* name = op == SVPSource::OP_PLAY ? "play" : op == SVPSource::OP_PAUSE ? "pause"
                         : op == SVPSource::OP_STOP ? "stop" : "?";
        LOGI("source: cbSourceOp(%d, %d %s)", source, (int)op, name);
        if (source != SVPSource::SID_CARLIFE_MEDIA || op == SVPSource::OP_PLAY) return 1;
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mAttached) return 1; // STOP at the session's end
        if (mNaviHeld) {
            // CL_NAVI coexists with the media source, so our own prompts should not pause it.
            LOGI("source: media paused while our navigation source is held; left to Android Auto");
            return 1;
        }
        if (mLoss != kNoLoss) return 1;
        mLossResumePlay = mMediaPlaying;
        // Who took over is published after this answer (source_idl.hpp); one that already
        // showed up is a main source: for good.
        if (Clock::now() - mMainActiveAt < std::chrono::seconds(1)) {
            mLoss = kPermanentLoss;
            sendFocus("loss", "a main source took the audio");
            mCtl.send("key pause");
            mLast.speakers(false);
        } else {
            mLoss = kTransientLoss;
            sendFocus("transient", "another source interrupted the media");
        }
        return 1;
    }

    // SVP_SourceListener::pubSourceStatus, on the subscription's thread.
    void onSourceStatus(int source, SVPSource::SourceStatus status) {
        const bool ours = source >= SVPSource::SID_CARLIFE_MEDIA && source <= SVPSource::SID_CARLIFE_ASR;
        if (!ours && !isMainSource(source)) {
            LOGV("source: status %d -> %d", source, (int)status);
            return;
        }
        LOGI("source: status %d -> %d", source, (int)status);
        bool resume = false;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (!mAttached) return;
            if (!ours && (status == SVPSource::SS_ACTIVED || status == SVPSource::SS_INBACKGROUND ||
                          status == SVPSource::SS_RESTORED)) {
                if (status == SVPSource::SS_ACTIVED) mMainActiveAt = Clock::now();
                if (mLoss == kTransientLoss) {
                    // The radio or another main source took the speakers, or is next in line.
                    mLoss = kPermanentLoss;
                    sendFocus("loss", "a main source took the audio");
                    mCtl.send("key pause");
                    mLast.speakers(false);
                }
            } else if (source == SVPSource::SID_CARLIFE_MEDIA && status == SVPSource::SS_RESTORED &&
                       mLoss != kNoLoss) {
                resume = true; // the interruption is over: take the source back (doTask)
            }
        }
        if (resume) request(SVPSource::SID_CARLIFE_MEDIA);
    }

private:
    enum Kind { Request, Release };
    enum Loss { kNoLoss, kTransientLoss, kPermanentLoss };
    enum Applied { kDetachedState, kApplied, kUnknown }; // what sourcemgr has (kUnknown: an attach or detach failed midway)
    typedef std::chrono::steady_clock Clock;

    class Listener {
    public:
        explicit Listener(AudioSource& owner) : mOwner(owner) {}
        void pubSourceStatus(int source, SVPSource::SourceStatus status) { mOwner.onSourceStatus(source, status); }

    private:
        AudioSource& mOwner;
    };

    // Main (background) sources in svp/etc/source.conf (FG 0), other than CL_MEDIA: tuner, disc,
    // aux, terminal, Bluetooth audio, iPod, SD, USB, stream, QQ music, CarPlay and AA media,
    // "no media". Taking over from CL_MEDIA, they keep the speakers until the user switches.
    static bool isMainSource(int source) {
        static const int kMain[] = {1, 2, 3, 4, 8, 9, 10, 11, 14, 15, 16, 17, 22, 24, 25, 30, 39};
        for (int m : kMain)
            if (m == source) return true;
        return false;
    }

    // mMutex held.
    void sendFocus(const char* state, const char* why) {
        LOGI("focus: %s (%s)", state, why);
        mCtl.send(std::string("focus ") + state);
    }

    // The media source is ours again (granted after a loss).
    void regained() {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mLoss == kNoLoss) return;
        const bool permanent = mLoss == kPermanentLoss;
        mLoss = kNoLoss;
        if (mMuted || mPowerPaused) return; // the unmute / switch-on gives it back
        sendFocus("gain", permanent ? "the media source is ours again" : "the interruption is over");
        // After a permanent loss the phone does not resume by itself: resume what was playing,
        // unless the phone already plays again (the usual way back: the user played AA media).
        if (permanent && mLossResumePlay && !mMediaPlaying) mCtl.send("key play");
    }

    // Subscribe to sourcemgr's status topic, unless subscribed; at most every 10 s. Worker only.
    void subscribe() {
        if (mCfg.sourceListenPort <= 0) return;
        if (mSubscription && mSubscription->isConnected()) return;
        const Clock::time_point now = Clock::now();
        if (mSubscribeTried != Clock::time_point() && now - mSubscribeTried < std::chrono::seconds(10)) return;
        mSubscribeTried = now;
        if (!tcpReachable(mCfg.sourceHost, mCfg.sourceListenPort, 1000)) {
            if (!mSubscribeFailing) LOGW("source: no status publisher on %s:%d; interruptions end only when the "
                                         "AA screen is opened again", mCfg.sourceHost.c_str(), mCfg.sourceListenPort);
            mSubscribeFailing = true;
            return;
        }
        try {
            if (mSubscription) {
                LOGW("source: status subscription lost; subscribing again");
                mSubscription->close();
                mSubscription.reset();
            }
            if (!mSubServer) {
                mSubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint(-1)));
                mSubServer->start();
            }
            RCF::SubscriptionParms parms;
            parms.setPublisherEndpoint(RCF::TcpEndpoint(mCfg.sourceHost, mCfg.sourceListenPort));
            mSubscription = mSubServer->createSubscription<SVP_SourceListener>(mListener, parms);
            LOGI("source: subscribed to the source status topic (%s:%d)", mCfg.sourceHost.c_str(),
                 mCfg.sourceListenPort);
            mSubscribeFailing = false;
        } catch (const std::exception& e) {
            if (!mSubscribeFailing) LOGW("source: status subscription failed: %s", e.what());
            mSubscribeFailing = true;
            mSubscription.reset();
        }
    }
    struct Task {
        Task(Kind k = Request, int s = 0) : kind(k), source(s), tries(0) {}
        Kind kind;
        int source;
        int tries;
    };
    static const int kSources[4];
    enum { kNaviLingerMs = 2000 };

    // Worker. Logged once per stretch of failures.
    void failed(const char* what, const std::string& why) {
        if (!mFailing) LOGW("source: %s: %s (retrying quietly)", what, why.c_str());
        mFailing = true;
    }

    bool connect() {
        if (mClient) return true;
        try {
            // A server only for the callback connection; libsource uses TcpEndpoint(0) too.
            mServer.reset(new RCF::RcfServer(RCF::TcpEndpoint(0)));
            mServer->bind<SVP_SourceCallback>(*this);
            mServer->start();
            mCallbackClient.reset(new RcfClient<SVP_SourceService>(RCF::TcpEndpoint(mCfg.sourceHost, mCfg.sourcePort)));
            mCallbackClient->getClientStub().setRemoteCallTimeoutMs(5000);
            // .get(): a void RCF call otherwise runs in its FutureImpl's destructor, and a
            // failure thrown from there (no source service yet) terminates the process.
            mCallbackClient->RegisterClient(static_cast<int>(getpid())).get();
            RCF::createCallbackConnection(*mCallbackClient, *mServer);
            mClient.reset(new RcfClient<SVP_SourceService>(RCF::TcpEndpoint(mCfg.sourceHost, mCfg.sourcePort)));
            mClient->getClientStub().setRemoteCallTimeoutMs(5000);
            LOGI("source: registered as client %d, callback connection up", (int)getpid());
            subscribe();
            return true;
        } catch (const RCF::Exception& e) {
            failed(("cannot reach the source service on " + mCfg.sourceHost + ":" + std::to_string(mCfg.sourcePort)).c_str(),
                   e.getErrorString());
        } catch (const std::exception& e) {
            failed(("cannot reach the source service on " + mCfg.sourceHost + ":" + std::to_string(mCfg.sourcePort)).c_str(),
                   e.what());
        }
        drop();
        return false;
    }

    void drop() {
        try {
            mClient.reset();
            mCallbackClient.reset();
            if (mServer) mServer->stop();
            mServer.reset();
        } catch (const std::exception& e) {
            LOGW("source: dropping the connection: %s", e.what());
        }
    }

    // Worker: the four sources attached and valid (true), or detached; true when every call went
    // through.
    bool applyState(bool attach) {
        if (!connect()) return false;
        try {
            if (attach) {
                // The unit's own record of the last main source (diagnostic, for last mode).
                if (gVerbose) {
                    try {
                        LOGV("source: GetLastBGSource() -> %d", (int)mClient->GetLastBGSource());
                    } catch (const std::exception& e) {
                        LOGV("source: GetLastBGSource failed: %s", e.what());
                    }
                }
                for (int src : kSources)
                    LOGI("source: SetSourceStatus(%d, attached) -> %d", src, (int)mClient->SetSourceStatus(src, SVPSource::SS_ATTACHED));
                for (int src : kSources)
                    LOGI("source: SetSourceStatus(%d, valid) -> %d", src, (int)mClient->SetSourceStatus(src, SVPSource::SS_VALID));
            } else {
                for (int src : kSources)
                    LOGI("source: SetSourceStatus(%d, detached) -> %d", src, (int)mClient->SetSourceStatus(src, SVPSource::SS_DETACHED));
            }
            if (mFailing) LOGI("source: the source service answers again");
            mFailing = false;
            return true;
        } catch (const RCF::Exception& e) {
            failed(attach ? "attaching the sources" : "detaching the sources", e.getErrorString());
        } catch (const std::exception& e) {
            failed(attach ? "attaching the sources" : "detaching the sources", e.what());
        }
        drop();
        return false;
    }

    // Worker: a request or a release; true when it went through.
    bool doTask(Task t) {
        if (!connect()) return false;
        try {
            const int pid = static_cast<int>(getpid());
            if (t.kind == Request) {
                int r = mClient->RequestSource(t.source, pid);
                LOGI("source: RequestSource(%d, %d) -> %d", t.source, pid, r);
                if (r == 1) LOGI("source: ConfirmSource(%d) -> %d", t.source, (int)mClient->ConfirmSource(t.source));
                // Getting the media source back after a loss is the phone's gain (sourcemgr
                // never calls back PLAY). 3: parked behind a foreground source (a call); it
                // publishes SS_RESTORED when that ends.
                if (r == 1 && t.source == SVPSource::SID_CARLIFE_MEDIA) {
                    mLast.speakers(true);
                    regained();
                }
                if (r == 3 && t.source == SVPSource::SID_CARLIFE_MEDIA)
                    LOGI("source: media source waits behind another source until it is restored");
            } else {
                // (source, client, restore), as libsource sends it (source_idl.hpp).
                LOGI("source: ReleaseSource(%d, %d, 1) -> %d", t.source, pid,
                     (int)mClient->ReleaseSource(t.source, pid, 1));
            }
            if (mFailing) LOGI("source: the source service answers again");
            mFailing = false;
            return true;
        } catch (const RCF::Exception& e) {
            failed(t.kind == Request ? "RequestSource" : "ReleaseSource", e.getErrorString());
        } catch (const std::exception& e) {
            failed(t.kind == Request ? "RequestSource" : "ReleaseSource", e.what());
        }
        drop();
        return false;
    }

    void run() {
        for (;;) {
            Task t;
            bool reconcile = false, want = false;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                for (;;) {
                    if (mStop) return;
                    const Clock::time_point now = Clock::now();
                    if (mReleasePending && now >= mReleaseAt) {
                        mReleasePending = false;
                        mNaviHeld = false;
                        mTasks.push_back(Task(Release, SVPSource::SID_CARLIFE_NAVI));
                    }
                    const Applied target = mAttached ? kApplied : kDetachedState;
                    if (now >= mRetryAt) {
                        if (mApplied != target) {
                            reconcile = true;
                            want = mAttached;
                            break;
                        }
                        if (!mTasks.empty()) break;
                    }
                    // Wake for the navigation release, a retry, and every 10 s while attached to
                    // keep the status subscription up.
                    Clock::time_point until = now + std::chrono::seconds(10);
                    if (mReleasePending && mReleaseAt < until) until = mReleaseAt;
                    const bool retrying = mApplied != target || !mTasks.empty();
                    if (retrying && mRetryAt < until) until = mRetryAt;
                    if (mCv.wait_until(lk, until) == std::cv_status::timeout && mAttached && mClient &&
                        mApplied == kApplied && !retrying && !(mReleasePending && Clock::now() >= mReleaseAt)) {
                        lk.unlock();
                        subscribe();
                        lk.lock();
                    }
                }
                if (!reconcile) {
                    t = mTasks.front();
                    mTasks.pop_front();
                }
                mBusy = true;
            }
            const bool ok = reconcile ? applyState(want) : doTask(t);
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mBusy = false;
                if (reconcile) {
                    if (ok) mApplied = want ? kApplied : kDetachedState;
                    else {
                        mApplied = kUnknown;
                        mRetryAt = Clock::now() + std::chrono::seconds(2);
                    }
                } else if (!ok && mAttached && ++t.tries < 3) {
                    mTasks.push_front(t);
                    mRetryAt = Clock::now() + std::chrono::seconds(1);
                }
            }
        }
    }

    const Config& mCfg;
    ControlChannel& mCtl;
    LastMode& mLast;
    std::mutex mMutex;
    MonoCv mCv;
    std::deque<Task> mTasks;
    Applied mApplied = kDetachedState;   // what sourcemgr has (the worker sets it)
    Clock::time_point mRetryAt;          // no call before this (after a failure)
    bool mBusy = false;                  // the worker is in a call
    bool mFailing = false;               // the worker's: a failure was logged
    Loss mLoss = kNoLoss;            // the media source taken by another (see cbSourceOp)
    bool mLossResumePlay = false;    // AA media was playing when lost
    bool mMuted = false;             // the car's mute paused the music (musicControl)
    bool mPowerPaused = false;       // the unit's switch-off paused it (power)
    bool mMediaPlaying = false;      // AA media audio on (receiver)
    Clock::time_point mMainActiveAt; // the last main source's SS_ACTIVED
    unsigned mNaviUses = 0;          // NaviUse bits
    bool mNaviHeld = false;          // requested and not (yet) given back
    bool mReleasePending = false;    // all uses ended: give it back at mReleaseAt
    std::chrono::steady_clock::time_point mReleaseAt;
    bool mStop;
    bool mAttached;
    Listener mListener;
    // The status subscription: the worker's only.
    std::unique_ptr<RCF::RcfServer> mSubServer;
    RCF::SubscriptionPtr mSubscription;
    Clock::time_point mSubscribeTried;
    bool mSubscribeFailing = false;
    std::thread mWorker;
    // Used from the worker only (and the server's own threads for the callback).
    std::unique_ptr<RCF::RcfServer> mServer;
    std::unique_ptr<RcfClient<SVP_SourceService> > mCallbackClient;
    std::unique_ptr<RcfClient<SVP_SourceService> > mClient;
};

// ---------------------------------------------------------------------------------------
// Vehicle data for Android Auto from the CAN service (can_idl.hpp):
//   * the CarPlay info record, polled with GetCanCarplayInfo every 2 s (canservice republishes
//     it on every MCU message anyway): DayNight -> "night <0|1>" (AA's night mode, the dark map
//     once the headlights are on; where stock appconnect gets the night mode it hands to
//     CarPlay), RightHandDrive -> "rhd <0|1>" (the driver's side in service discovery; stock
//     appconnect hands it to CarPlay's SetRightHandDrive), both when they change and again every
//     30 s in case the receiver restarted (it ignores repeats); TransmissionState -> "gear
//     <park|reverse|neutral|drive>" every poll, mapped as ipodservice maps it for the iPhone
//     (SVPIpodCANListener::SendThreadProc: 1 P, 2 R, 3 N, 4..6 D, else unknown, not sent);
//   * the vehicle speed, which canservice only publishes (OnCanVehicleSpeedChanged on its
//     SVP_CanListener publisher; GetCanVehicleSpeed asks it to publish the current value), in
//     0.01 km/h: ipodservice divides by 100 and by 3.6 for the iPhone's m/s
//     (SVPIpodCANListener::OnCanVehicleSpeedChanged 0xc8f78). Subscribed as the stock clients
//     do (libcan SVPCanService_Client::InitListener) and sent as "speed <m/s>" at most every
//     200 ms while it changes and every 2 s otherwise; the receiver derives the driving status
//     from it.
class CarInfo {
public:
    CarInfo(const Config& cfg, ControlChannel& ctl) : mCfg(cfg), mCtl(ctl), mStop(false), mListener(*this) {
        if (mCfg.canPort > 0) mWorker = std::thread(&CarInfo::run, this);
    }

    ~CarInfo() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

    // Night and the driver's side as last read, for a session about to start (its service
    // discovery reads the driver's side): a receiver that connected since the last poll has
    // them before "start".
    void sendCurrent() {
        const int night = mNight.load(), rhd = mRhd.load();
        if (night < 0 || rhd < 0) return;
        mCtl.send(night ? "night 1" : "night 0");
        mCtl.send(rhd ? "rhd 1" : "rhd 0");
    }

private:
    typedef std::chrono::steady_clock Clock;

    // SVP_CanListener servant: every method must exist; only the speed is used.
    class Listener {
    public:
        explicit Listener(CarInfo& car) : mCar(car) {}
        void OnCanIgnitionModeChanged(SVPCanType::SVPCanIgnModeType) {}
        void OnCanVehicleSpeedChanged(unsigned short raw) { mCar.onSpeed(raw); }
        // Logged (each change) to compare with the polled record: night with the headlights on
        // stayed DayNight 0 in step 3 runs 35-38.
        void OnCanCarplayInfoChanged(SVPCanType::SVPCanCarplayInfoType i) {
            const int v = i.DayNight | (i.TransmissionState << 8) | (i.RightHandDrive << 16);
            if (v == mLastInfo) return;
            mLastInfo = v;
            LOGV("can: event CarplayInfo DayNight %u, transmission %u, right-hand drive %u", (unsigned)i.DayNight,
                 (unsigned)i.TransmissionState, (unsigned)i.RightHandDrive);
        }
        void OnCanAudioLoweringChange(int) {}
        void OnDescAdaptionBtSettingChanged(SVPCanType::SVPDescAdaptionBtSettingType) {}
        void OnDescAdaptionVehicleCfgChanged(SVPCanType::SVPDescAdaptionVehicleConfigType) {}
        void OnDescAdaptionStickerBitsChanged(unsigned short) {}
        void OnDescCodingVehicleImageChanged(SVPCanType::SVPDescCodingVehicleImageType) {}
        void OnDescCodingBtSettingChanged(SVPCanType::SVPDescCodingBtSettingType) {}
        void OnDescActuatorTestEvent(SVPCanType::SVPDescActuatorIdType, SVPCanType::SVPDescActuatorCtrlType) {}
        void OnDescDatasetDownloadEvent(SVPCanType::SVPDescDatasetIdType, std::vector<unsigned char>&) {}
        void OnDescRoutineTestEvent(SVPCanType::SVPDescRoutineIdType, SVPCanType::SVPDescRoutineCtrlType,
                                    unsigned char) {}
        void OnDescMeasuredValueEvent(SVPCanType::SVPDescMeasuredIdType) {}
        void OnBapScreenViewOptionEvent(SVPCanType::SVPBapScreenViewIdType, std::vector<unsigned char>&) {}
        void OnBapScreenViewDataEvent(SVPCanType::SVPBapScreenViewIdType, std::vector<unsigned char>&) {}
        void OnBapScreenViewRequestEvent(SVPCanType::SVPBapScreenViewIdType, SVPCanType::SVPBapScreenCtrlType) {}
        void OnBapPhoneCallActionChange(SVPCanType::SVPBapPhoneCallActionType, unsigned char) {}
        void OnBapStatusBarChanged(std::vector<unsigned char>&) {}
        void OnBapFsgStateChanged(SVPCanType::SVPBapFsgStateType, int) {}
        void OnBapGeneralPopupChanged(SVPCanType::SVPBapGeneralPopupType) {}
        void OnBapStartStopValueChange(SVPCanType::SVPStartStopValueIdType, std::vector<unsigned char>&) {}
        void OnBapScreenOffTimerEvent(SVPCanType::SVPBapScreenOffFuncType, SVPCanType::SVPBapScreenOffTimerType) {}
        // The MCU messages canservice passes through (0x73E1..0x73FE; no stock consumer):
        // logged when one changes, to look for a headlight / night signal (at most 300 lines).
        void OnCanServiceMessage(unsigned short id, std::vector<unsigned char>& data) {
            if (mServiceLines >= 300) return;
            std::string hex;
            char b[4];
            for (size_t i = 0; i < data.size() && i < 32; ++i) {
                std::snprintf(b, sizeof b, "%02x", data[i]);
                hex += b;
            }
            std::string& last = mServiceMsg[id];
            if (last == hex) return;
            last = hex;
            ++mServiceLines;
            LOGV("can: event ServiceMessage 0x%04x %s", (unsigned)id, hex.c_str());
        }

    private:
        CarInfo& mCar;
        int mLastInfo = -1;
        std::map<unsigned short, std::string> mServiceMsg;
        int mServiceLines = 0;
    };

    static const char* gearName(unsigned char transmission) {
        switch (transmission) {
        case 1: return "park";
        case 2: return "reverse";
        case 3: return "neutral";
        case 4: case 5: case 6: return "drive";
        default: return nullptr;
        }
    }

    // On the subscription's thread, and from the poll (resend).
    void onSpeed(unsigned short raw) {
        std::lock_guard<std::mutex> lk(mSpeedMutex);
        const Clock::time_point now = Clock::now();
        if (!mSpeedSeen) LOGI("can: vehicle speed %u.%02u km/h (first value)", raw / 100u, raw % 100u);
        mSpeedSeen = true;
        mSpeedRaw = raw;
        mSpeedAt = now;
        maybeSendSpeed(now);
    }

    // mSpeedMutex held.
    void maybeSendSpeed(Clock::time_point now) {
        const bool changed = mSpeedRaw != mSpeedSentRaw;
        if ((changed && now - mSpeedSentAt >= std::chrono::milliseconds(200)) ||
            now - mSpeedSentAt >= std::chrono::seconds(2)) {
            char line[32];
            std::snprintf(line, sizeof line, "speed %.3f", mSpeedRaw / 360.0); // 0.01 km/h -> m/s
            if (mCtl.send(line)) {
                mSpeedSentRaw = mSpeedRaw;
                mSpeedSentAt = now;
            }
        }
    }

    // Subscribe to the CAN service's publisher for the speed, unless subscribed; at most every
    // 10 s. Then ask for the current speed (published back).
    void subscribe(RcfClient<SVP_CanService>& client) {
        if (mCfg.canListenPort <= 0) return;
        if (mSubscription && mSubscription->isConnected()) return;
        const Clock::time_point now = Clock::now();
        if (mSubscribeTried != Clock::time_point() && now - mSubscribeTried < std::chrono::seconds(10)) return;
        mSubscribeTried = now;
        // Only when the publisher is there (see tcpReachable).
        if (!tcpReachable(mCfg.canHost, mCfg.canListenPort, 1000)) {
            if (!mSubscribeFailing) LOGW("can: no CAN publisher on %s:%d; no speed until there is one",
                                         mCfg.canHost.c_str(), mCfg.canListenPort);
            mSubscribeFailing = true;
            if (mSubscription) {
                mSubscription->close();
                mSubscription.reset();
            }
            return;
        }
        try {
            if (mSubscription) {
                LOGW("can: speed subscription lost; subscribing again");
                mSubscription->close();
                mSubscription.reset();
            }
            if (!mSubServer) {
                // As libcan: a TCP transport that does not listen, to host the subscription.
                mSubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint(-1)));
                mSubServer->start();
            }
            RCF::SubscriptionParms parms;
            parms.setPublisherEndpoint(RCF::TcpEndpoint(mCfg.canHost, mCfg.canListenPort));
            mSubscription = mSubServer->createSubscription<SVP_CanListener>(mListener, parms);
            LOGI("can: subscribed to the CAN service's publisher (%s:%d) for the speed", mCfg.canHost.c_str(),
                 mCfg.canListenPort);
        } catch (const RCF::Exception& e) {
            if (!mSubscribeFailing) LOGW("can: speed subscription on %s:%d failed: %s", mCfg.canHost.c_str(),
                                         mCfg.canListenPort, e.getErrorString().c_str());
            mSubscribeFailing = true;
            mSubscription.reset();
            return;
        } catch (const std::exception& e) {
            if (!mSubscribeFailing) LOGW("can: speed subscription failed: %s", e.what());
            mSubscribeFailing = true;
            mSubscription.reset();
            return;
        }
        mSubscribeFailing = false;
        try {
            client.GetCanVehicleSpeed().get();
        } catch (const std::exception& e) {
            LOGW("can: GetCanVehicleSpeed failed: %s", e.what());
        }
    }

    void run() {
        std::unique_ptr<RcfClient<SVP_CanService> > client;
        int last = -1, lastRhd = -1, lastTransmission = -1, sinceSentS = 0;
        int logged = -1, loggedRhd = -1;
        unsigned sentTo = 0; // ControlChannel::connections() when night and rhd last reached the receiver
        bool failing = false;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mMutex);
                if (mCv.wait_for(lk, std::chrono::seconds(2), [this] { return mStop; })) break;
            }
            try {
                if (!client) {
                    client.reset(new RcfClient<SVP_CanService>(RCF::TcpEndpoint(mCfg.canHost, mCfg.canPort)));
                    client->getClientStub().setConnectTimeoutMs(2000);
                    client->getClientStub().setRemoteCallTimeoutMs(2000);
                }
                SVPCanType::SVPCanCarplayInfoType info;
                client->GetCanCarplayInfo(info).get(); // .get(): see AudioSource::connect
                if (failing) LOGI("can: the CAN service answers again");
                failing = false;
                const int night = info.DayNight != 0;
                const int rhd = info.RightHandDrive != 0;
                mNight = night;
                mRhd = rhd;
                sinceSentS += 2;
                if (night != logged || rhd != loggedRhd) {
                    LOGI("can: DayNight %u (transmission %u, right-hand drive %u) -> %s",
                         (unsigned)info.DayNight, (unsigned)info.TransmissionState,
                         (unsigned)info.RightHandDrive, night ? "night" : "day");
                    logged = night;
                    loggedRhd = rhd;
                }
                // Each receiver connection gets them at once (it reads the driver's side at
                // the next session's service discovery, and starts left-hand), then on a change
                // and every 30 s; they count as sent only once the receiver had them.
                const unsigned conn = mCtl.connections();
                if (night != last || rhd != lastRhd || conn != sentTo || sinceSentS >= 30) {
                    if (mCtl.send(night ? "night 1" : "night 0") && mCtl.send(rhd ? "rhd 1" : "rhd 0")) {
                        last = night;
                        lastRhd = rhd;
                        sentTo = conn;
                        sinceSentS = 0;
                    }
                }
                const char* gear = gearName(info.TransmissionState);
                if (info.TransmissionState != lastTransmission) {
                    LOGI("can: transmission %u -> %s", (unsigned)info.TransmissionState, gear ? gear : "unknown, not sent");
                    lastTransmission = info.TransmissionState;
                }
                if (gear) mCtl.send(std::string("gear ") + gear);
                subscribe(*client);
                {
                    // Resend a speed that is still current (a new receiver or session gets it).
                    std::lock_guard<std::mutex> lk(mSpeedMutex);
                    if (mSpeedSeen && Clock::now() - mSpeedAt < std::chrono::seconds(5)) maybeSendSpeed(Clock::now());
                }
            } catch (const RCF::Exception& e) {
                if (!failing) LOGW("can: GetCanCarplayInfo on %s:%d failed: %s", mCfg.canHost.c_str(),
                                   mCfg.canPort, e.getErrorString().c_str());
                failing = true;
                client.reset();
            } catch (const std::exception& e) {
                if (!failing) LOGW("can: GetCanCarplayInfo failed: %s", e.what());
                failing = true;
                client.reset();
            }
        }
        try {
            if (mSubscription) mSubscription->close();
            mSubscription.reset();
            if (mSubServer) mSubServer->stop();
        } catch (const std::exception& e) {
            LOGW("can: closing the speed subscription: %s", e.what());
        }
    }

    const Config& mCfg;
    ControlChannel& mCtl;
    std::mutex mMutex;
    MonoCv mCv;
    bool mStop;
    std::atomic<int> mNight{-1}, mRhd{-1}; // the last poll's (-1: none yet)
    Listener mListener;
    // The worker's only.
    std::unique_ptr<RCF::RcfServer> mSubServer;
    RCF::SubscriptionPtr mSubscription;
    Clock::time_point mSubscribeTried;
    bool mSubscribeFailing = false;
    // The speed: written by the subscription's thread, read by the worker.
    std::mutex mSpeedMutex;
    bool mSpeedSeen = false;
    unsigned short mSpeedRaw = 0, mSpeedSentRaw = 0xffff;
    Clock::time_point mSpeedAt, mSpeedSentAt;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// The car microphone's route through the Bluetooth module (bt_idl.hpp). Its echo-cancelling
// stage sits between the mic and MIB2BtOptIn and gives the mic to the SoC only in
// EAP_WIRED_PHONE_MODE; in EAP_BT_MODE (the default) the mic stays with Bluetooth calls and
// MIB2BtOptIn reads zeros (step 3 runs 12/13: 5 s of capture, peak level 0). Stock, appconnect
// switches it on CarLife's module-4 (voice) status, which the shim now publishes too; this
// sets it directly as well, as MirrorLink's ASR does, in case appconnect does not. Calls run
// on one worker; only the latest wanted state counts, and it is retried every 2 s until
// btservice takes it. At start it is put back to EAP_BT_MODE: a shim that died while the
// phone had the mic left it with the SoC, and btservice itself resets it only on a Bluetooth
// (re)connect, so calls on a phone already connected had no mic.
class MicPath {
public:
    explicit MicPath(const Config& cfg) : mCfg(cfg), mWant(false), mDirty(true), mStop(false) {
        if (mCfg.btPort > 0) mWorker = std::thread(&MicPath::run, this);
    }

    ~MicPath() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

    void set(bool phone) {
        if (mCfg.btPort <= 0) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mWant = phone;
            mDirty = true;
            mRetryAt = std::chrono::steady_clock::time_point();
        }
        mCv.notify_all();
    }

    // The wanted path is set (shutdown waits for it).
    bool idle() {
        if (mCfg.btPort <= 0) return true;
        std::lock_guard<std::mutex> lk(mMutex);
        return !mDirty && !mBusy;
    }

private:
    void run() {
        std::unique_ptr<RcfClient<SVP_BtService> > client;
        bool failing = false;
        for (;;) {
            bool want;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                for (;;) {
                    if (mStop) return;
                    if (mDirty && std::chrono::steady_clock::now() >= mRetryAt) break;
                    if (mDirty) mCv.wait_until(lk, mRetryAt);
                    else        mCv.wait(lk);
                }
                want = mWant;
                mDirty = false;
                mBusy = true;
            }
            const SVPBtType::ECNR_AUDIO_PATH path =
                want ? SVPBtType::EAP_WIRED_PHONE_MODE : SVPBtType::EAP_BT_MODE;
            bool ok = false;
            try {
                if (!client) {
                    client.reset(new RcfClient<SVP_BtService>(RCF::TcpEndpoint(mCfg.btHost, mCfg.btPort)));
                    client->getClientStub().setConnectTimeoutMs(2000);
                    client->getClientStub().setRemoteCallTimeoutMs(3000);
                }
                client->SetECNRAudioPath(path).get(); // .get(): see AudioSource::connect
                LOGI("bt: SetECNRAudioPath(%d %s)", (int)path, want ? "wired phone: the mic to the SoC" : "bt");
                ok = true;
                failing = false;
            } catch (const RCF::Exception& e) {
                if (!failing) LOGW("bt: SetECNRAudioPath(%d) on %s:%d failed: %s (retrying every 2 s)", (int)path,
                                   mCfg.btHost.c_str(), mCfg.btPort, e.getErrorString().c_str());
                failing = true;
                client.reset();
            } catch (const std::exception& e) {
                if (!failing) LOGW("bt: SetECNRAudioPath(%d) failed: %s (retrying every 2 s)", (int)path, e.what());
                failing = true;
                client.reset();
            }
            std::lock_guard<std::mutex> lk(mMutex);
            mBusy = false;
            if (!ok && !mDirty) {
                mDirty = true; // the same wanted path again, unless a newer one came meanwhile
                mRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
    }

    const Config& mCfg;
    std::mutex mMutex;
    MonoCv mCv;
    bool mWant, mDirty, mStop;
    bool mBusy = false;
    std::chrono::steady_clock::time_point mRetryAt;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// The car's Bluetooth address, for the receiver's service discovery response (else
// 00:00:00:00:00:00). A wired phone ignores it, but a wireless adapter's phone identifies and
// pairs the car by it (step 3 run 29: an adapter stopped after service discovery).
// btservice answers GetAttribute(3) with "%02x:%02x:%02x:%02x:%02x:%02x" from
// CBtManager::GetBtAddr (SVPBtServer::GetAttribute 0x1843dc), all zeros while Bluetooth is
// not up yet: asked every 10 s until it is real, then sent to the receiver ("btmac ...",
// upper case) once per receiver connection.
class BtInfo {
public:
    BtInfo(const Config& cfg, ControlChannel& ctl) : mCfg(cfg), mCtl(ctl), mStop(false) {
        if (mCfg.btPort > 0) mWorker = std::thread(&BtInfo::run, this);
    }

    ~BtInfo() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

private:
    enum { kAttrLocalAddress = 3 };

    std::string ask() {
        try {
            RcfClient<SVP_BtService> client(RCF::TcpEndpoint(mCfg.btHost, mCfg.btPort));
            client.getClientStub().setConnectTimeoutMs(2000);
            client.getClientStub().setRemoteCallTimeoutMs(3000);
            std::string addr;
            int r = client.GetAttribute(SVPBtType::Attribute(kAttrLocalAddress), addr);
            if (r != 1 || addr.size() != 17 || addr == "00:00:00:00:00:00") return std::string();
            for (char& c : addr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return addr;
        } catch (const RCF::Exception& e) {
            if (!mFailing) LOGW("bt: GetAttribute(3) failed: %s", e.getErrorString().c_str());
            mFailing = true;
        } catch (const std::exception& e) {
            if (!mFailing) LOGW("bt: GetAttribute(3) failed: %s", e.what());
            mFailing = true;
        }
        return std::string();
    }

    void run() {
        std::string addr;
        unsigned sentTo = 0; // ControlChannel::connections() when last sent
        auto nextAsk = std::chrono::steady_clock::now();
        for (;;) {
            if (addr.empty() && std::chrono::steady_clock::now() >= nextAsk) {
                addr = ask();
                if (!addr.empty()) LOGI("bt: the unit's Bluetooth address is %s", addr.c_str());
                nextAsk = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
            if (!addr.empty()) {
                const unsigned c = mCtl.connections();
                if (c != sentTo && mCtl.send("btmac " + addr)) sentTo = c;
            }
            std::unique_lock<std::mutex> lk(mMutex);
            if (mCv.wait_for(lk, std::chrono::seconds(1), [this] { return mStop; })) return;
        }
    }

    const Config& mCfg;
    ControlChannel& mCtl;
    std::mutex mMutex;
    MonoCv mCv;
    bool mStop;
    bool mFailing = false;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// appconnect's code changes (appc_patch.hpp), kept on: applied within about a second of an
// appconnect process appearing, and again if it is replaced. The iPhone change must be in place
// before an iPhone is plugged in, which no CarLife call announces, so this does not wait for one.
// Each change is logged once; "no appconnect process" only while the unit is still booting. A
// process found with every change on (the patched file in the image, or patched here) is not
// opened again; one whose code is not the expected appconnect is looked at every 30 s.
class AppcWatch {
public:
    explicit AppcWatch(const Config& cfg) : mCfg(cfg) {}

    void start() {
        if (mCfg.appcPatch) mWorker = std::thread(&AppcWatch::run, this);
    }

    ~AppcWatch() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

    // Look now (a CarLife call came: appconnect may be a new process).
    void now() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mNow = true;
        }
        mCv.notify_all();
    }

    // Every change is on in every appconnect process.
    bool patched() const { return mPatched; }

private:
    void run() {
        std::string last, previous;
        appc_patch::Seen seen;
        for (;;) {
            std::string msg;
            const bool ok = appc_patch::apply(msg, "appconnect", &seen);
            // The same refusal twice in a row: not a process caught mid-exec, its code differs.
            const bool differs = !ok && seen.found > 0 && msg == previous;
            previous = msg;
            mPatched = ok;
            if (!msg.empty() && msg != last) {
                if (ok) LOGI("appconnect patch: %s", msg.c_str());
                else LOGW("appconnect patch: %s", msg.c_str());
            }
            if (!msg.empty()) last = msg;
            checkSubscribed();
            std::unique_lock<std::mutex> lk(mMutex);
            // Every second while no appconnect runs (the unit starting), every 5 s once all is on,
            // every 30 s while its code is not what the changes expect (another firmware).
            const int waitS = ok ? 5 : differs ? 30 : 1;
            mCv.wait_for(lk, std::chrono::seconds(waitS), [this] { return mStop || mNow; });
            mNow = false;
            if (mStop) return;
        }
    }

    // appconnect subscribes to the CarLife status events once, when it starts
    // (SVPCarLifeService_Client::InitListener: no retry, no reconnect). One that started before
    // the shim took the slot is subscribed to the stopped stock service and never hears Android
    // Auto connect: no AA screen after picking it, "connecting" for good (step 3 runs 45, 46).
    // Nothing here can mend that (appmgr does not restart apps); it is logged once per process,
    // so a run shows it at once. Subscribed means holding a TCP connection to our publisher.
    void checkSubscribed() {
        if (mCfg.pubPort <= 0) return;
        const int pid = firstPid("appconnect");
        if (pid <= 0 || pid == mCheckedPid) return;
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (pid != mSeenPid) {
            mSeenPid = pid;
            mSeenAt = now;
        }
        if (connectedTo(pid, mCfg.pubPort)) {
            mCheckedPid = pid;
            LOGI("appconnect (pid %d) is subscribed to our status events", pid);
        } else if (now - mSeenAt >= std::chrono::seconds(20)) {
            // Not yet subscribed 20 s after it was first seen: it will not be.
            mCheckedPid = pid;
            LOGW("appconnect (pid %d) is NOT subscribed to our status events: it started before the shim took the "
                 "CarLife slot, so it will not see Android Auto connect (no AA screen by itself, \"connecting\" "
                 "stays) until the unit restarts with the shim in time", pid);
        }
    }

    static int firstPid(const char* comm) {
        DIR* d = opendir("/proc");
        if (!d) return 0;
        int found = 0;
        while (dirent* e = readdir(d)) {
            char* end = nullptr;
            const long pid = std::strtol(e->d_name, &end, 10);
            if (!end || *end != '\0' || pid <= 0) continue;
            char path[64], name[32] = {0};
            std::snprintf(path, sizeof path, "/proc/%ld/comm", pid);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            if (std::fgets(name, sizeof name, f)) name[std::strcspn(name, "\n")] = '\0';
            std::fclose(f);
            if (!std::strcmp(name, comm)) {
                found = static_cast<int>(pid);
                break;
            }
        }
        closedir(d);
        return found;
    }

    // Process `pid` has an established TCP connection to local port `port` (/proc/net/tcp).
    static bool connectedTo(int pid, int port) {
        std::set<unsigned long> inodes;
        FILE* f = std::fopen("/proc/net/tcp", "r");
        if (!f) return false;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned remAddr = 0, remPort = 0, state = 0;
            unsigned long inode = 0;
            if (std::sscanf(line, " %*d: %*x:%*x %x:%x %x %*x:%*x %*x:%*x %*x %*d %*d %lu", &remAddr, &remPort,
                            &state, &inode) == 4 &&
                (int)remPort == port && state == 1 && inode)
                inodes.insert(inode);
        }
        std::fclose(f);
        if (inodes.empty()) return false;
        char dir[64];
        std::snprintf(dir, sizeof dir, "/proc/%d/fd", pid);
        DIR* d = opendir(dir);
        if (!d) return false;
        bool yes = false;
        while (dirent* e = readdir(d)) {
            char path[sizeof dir + sizeof e->d_name], target[64] = {0};
            std::snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            const ssize_t n = ::readlink(path, target, sizeof target - 1);
            unsigned long ino = 0;
            if (n > 0 && std::sscanf(target, "socket:[%lu]", &ino) == 1 && inodes.count(ino)) {
                yes = true;
                break;
            }
        }
        closedir(d);
        return yes;
    }

    const Config& mCfg;
    int mCheckedPid = 0;                     // judged: subscribed or not
    int mSeenPid = 0;                        // the appconnect process being watched
    std::chrono::steady_clock::time_point mSeenAt;
    std::mutex mMutex;
    MonoCv mCv;
    bool mStop = false;
    bool mNow = false;
    std::atomic<bool> mPatched{false};
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// The unit's power state (power_idl.hpp): powermgr's publisher tells when the unit is switched
// off with the ignition on (PS_USER_OFF), on again (PS_USER_ON), or going down (PS_SHUT_DOWN).
// Subscribed (again every 10 s if lost) on a worker; each event goes to the callback, on the
// subscription's thread.
class PowerWatch {
public:
    typedef std::function<void(int state, int a, int b)> Callback;

    PowerWatch(const Config& cfg, Callback cb) : mCfg(cfg), mCb(cb), mListener(*this) {}

    void start() {
        if (mCfg.powerListenPort > 0) mWorker = std::thread(&PowerWatch::run, this);
    }

    ~PowerWatch() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
        try {
            if (mSubscription) mSubscription->close();
            mSubscription.reset();
            if (mSubServer) mSubServer->stop();
        } catch (const std::exception& e) {
            LOGW("power: closing the subscription: %s", e.what());
        }
    }

    // Stock's test for a temporary switch-on (an incoming call while the unit is off):
    // GetPowerState answers (PS_USER_ON, 1).
    bool temporaryOn() {
        int state = 0, sub = 0;
        if (!query(state, sub)) return false;
        return state == SVPPower::PS_USER_ON && sub == 1;
    }

private:
    // SVP_PowerListener servant: every method must exist (power_idl.hpp).
    class Listener {
    public:
        explicit Listener(PowerWatch& w) : mW(w) {}
        void OnScreenOffChanged(unsigned char v) { LOGI("power: event ScreenOffChanged %u", (unsigned)v); }
        void OnDisplayoffChanged(int v) { LOGI("power: event DisplayoffChanged %d", v); }
        void OnPowerStateChanged(SVPPower::PowerState s, unsigned char a, unsigned char b) {
            if (mW.mCb) mW.mCb(static_cast<int>(s), a, b);
        }
        void OnBkLightLevelChanged(SVPPower::BkLight::BkLightMode, SVPPower::BkLight::BkLightType, unsigned char) {}

    private:
        PowerWatch& mW;
    };

    bool query(int& state, int& sub) {
        if (mCfg.powerPort <= 0) return false;
        try {
            RcfClient<SVP_PowerService> client(RCF::TcpEndpoint(mCfg.powerHost, mCfg.powerPort));
            client.getClientStub().setConnectTimeoutMs(1000);
            client.getClientStub().setRemoteCallTimeoutMs(1000);
            SVPPower::PowerState st = SVPPower::PS_UNDEFINED;
            unsigned char s = 0;
            const int r = client.GetPowerState(st, s);
            state = static_cast<int>(st);
            sub = s;
            LOGI("power: GetPowerState -> %d: %s (%d), %d", r, SVPPower::name(state), state, sub);
            return true;
        } catch (const std::exception& e) {
            LOGW("power: GetPowerState failed: %s", e.what());
            return false;
        }
    }

    void subscribe() {
        if (mSubscription && mSubscription->isConnected()) return;
        if (!tcpReachable(mCfg.powerHost, mCfg.powerListenPort, 1000)) {
            if (!mFailing) LOGW("power: no power publisher on %s:%d; switch-offs are not followed",
                                mCfg.powerHost.c_str(), mCfg.powerListenPort);
            mFailing = true;
            return;
        }
        try {
            if (mSubscription) {
                LOGW("power: subscription lost; subscribing again");
                mSubscription->close();
                mSubscription.reset();
            }
            if (!mSubServer) {
                mSubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint(-1)));
                mSubServer->start();
            }
            RCF::SubscriptionParms parms;
            parms.setPublisherEndpoint(RCF::TcpEndpoint(mCfg.powerHost, mCfg.powerListenPort));
            mSubscription = mSubServer->createSubscription<SVP_PowerListener>(mListener, parms);
            LOGI("power: subscribed to the power events (%s:%d)", mCfg.powerHost.c_str(), mCfg.powerListenPort);
            mFailing = false;
            int state = 0, sub = 0;
            query(state, sub);
        } catch (const std::exception& e) {
            if (!mFailing) LOGW("power: subscription failed: %s", e.what());
            mFailing = true;
            mSubscription.reset();
        }
    }

    void run() {
        for (;;) {
            subscribe();
            std::unique_lock<std::mutex> lk(mMutex);
            if (mCv.wait_for(lk, std::chrono::seconds(10), [this] { return mStop; })) return;
        }
    }

    const Config& mCfg;
    Callback mCb;
    Listener mListener;
    std::mutex mMutex;
    MonoCv mCv;
    bool mStop = false;
    bool mFailing = false;
    std::unique_ptr<RCF::RcfServer> mSubServer;
    RCF::SubscriptionPtr mSubscription;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// Android Auto's Bluetooth pairing with the car (bt_idl.hpp has btservice's side). The phone
// gets the car's address in service discovery and asks whether it is paired with it (the
// receiver's "btpair <address> <method>"). Answering "already paired" without asking made an
// unpaired phone connect to the car, and the unit's Bluetooth app then asked the user to pair
// (step 3 runs 30, 31). Now:
//   * paired: the car's paired list (GetDeviceNum / GetDevice of BT_DEVICE_PAIRED) has the
//     phone's address -> "btpaired 1": the phone connects for calls and the car knows it;
//   * not paired -> "btpaired 0", and for 60 s a pairing window: SetAutoAcceptConnect(1) (the
//     unit's own setting saved first and put back after), so btservice takes the phone's link
//     without OnRemoteConnectRequest and the app's "pairing request" dialog; the phone bonds,
//     btservice publishes OnPairingComfirmRequest(passkey, address): the passkey goes to the
//     phone ("btauth %06u", BluetoothAuthenticationRequest), which confirms by itself, and
//     AcceptSspRequest(1) confirms on the car. The app still opens its numeric dialog until
//     OnPairStatusChanged(0) closes it, so it flashes briefly.
// Every pairing and connection event btservice publishes is logged ("bt:"), so a run shows what
// the car's Bluetooth did. Calls to btservice run on one worker.
//
// Also the phone's Bluetooth music (A2DP) while Android Auto plays, as stock CarLife and CarPlay
// do: AA's media comes over USB, and a music link to the same (or another) phone could make the
// car switch to its Bluetooth audio source. btservice itself refuses new A2DP links while our
// status is 3 (OnHandleAuthoriseInd), but not one that was up before, nor its link-loss
// reconnect: when the session starts, the device holding A2DP (GetAvpConnectAddr) is
// remembered and Disconnect(A2DP) (HFP stays); an A2DP connect during the session is dropped
// again; 2 s after the session ends, Connect(that address, A2DP) puts it back (stock CarLife
// never did). AA_BT_A2DP_DROP=0 leaves A2DP alone.
//
// And the lock: while the session runs and its phone (the address of its btpair) holds the
// car's phone link (GetHfpConnectAddr), capability 10 is on: btservice refuses every other
// device's pairing and connection without asking anyone (bt_idl.hpp), so no other phone joins
// or takes over, and the Bluetooth app has nothing to ask about. Links already up stay up. It is
// looked at on each pairing or connection event, the btpair, the session's start and end, and
// every 10 s, and set again each time while it holds (the Bluetooth app clears it when a CarPlay
// session ends, and it cannot be read back). It comes off when that phone lets go of the link
// (so it can come back) or the session ends, and at the shim's stop; a file (AA_BT_LOCK_FILE)
// marks it as ours, so a shim that died holding it takes it off at its next start, and one set
// by the Bluetooth app for CarPlay is left alone. AA_BT_LOCK=0 never sets it.
class BtPairing {
public:
    // Off with AA_BT_PAIRING=0. The method and event IDs are the unit's own (bt_idl.hpp).
    BtPairing(const Config& cfg, ControlChannel& ctl) : mCfg(cfg), mCtl(ctl), mListener(*this) {
        if (mCfg.btPort > 0 && mCfg.btListenPort > 0 && (mCfg.btPairing || mCfg.btA2dpDrop || mCfg.btLock))
            mWorker = std::thread(&BtPairing::run, this);
    }

    // The AA session is projecting (on) or over (off).
    void session(bool on) {
        if (!mWorker.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            if (mSession == on) return;
            mSession = on;
            mSessionChanged = true;
            if (!on) mSessionPhone.clear();
        }
        mCv.notify_all();
    }

    ~BtPairing() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
    }

    // The receiver's "btpair <address> <method>": answered with "btpaired 0|1". The address is
    // also the session's phone, for the lock.
    void query(const std::string& address, int method) {
        if (mWorker.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mSessionPhone = lower(address);
                mLinkChanged = true;
            }
            mCv.notify_all();
        }
        if (!mWorker.joinable() || !mCfg.btPairing) {
            LOGI("bt: pairing loop off: phone %s answered \"already paired\" (as before)", address.c_str());
            mCtl.send("btpaired 1");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mQuery = address;
            mQueryMethod = method;
            // Whatever the answer, the session's phone may bond now: it asks this every session,
            // even when both sides are paired, and when it has forgotten the car while the car
            // still lists it, the answer is "paired" and then it bonds anyway (step 3 run 38:
            // confirm requests left to the Bluetooth app, which failed every time).
            mWatchPhone = address;
            mWatchEnd = Clock::now() + std::chrono::seconds(60);
        }
        mCv.notify_all();
    }

private:
    typedef std::chrono::steady_clock Clock;

    // Every event has a method (SVPBtListenerNoop); these hide the ones the loop logs or uses.
    class Listener : public SVPBtListenerNoop {
    public:
        explicit Listener(BtPairing& o) : mO(o) {}
        void OnPairingComfirmRequest(unsigned int passkey, const std::string& raw) { mO.onConfirm(passkey, raw); }
        void OnPairingPinCodeRequest(unsigned int a, const std::string& b, const std::string& c) {
            mO.onPinRequest(a, b, c);
        }
        void OnRemoteConnectRequest(const std::string& addr) { LOGI("bt: event RemoteConnectRequest(%s)", addr.c_str()); }
        void OnReturnRemoteDeviceName(const std::string& addr, const std::string& name) {
            LOGI("bt: event RemoteDeviceName(%s, \"%s\")", printable(addr).c_str(), printable(name).c_str());
        }
        void OnPairStatusChanged(SVPBtType::PAIR_STATUS st, const std::string& addr) { mO.onPairStatus((int)st, addr); }
        void OnDeviceNeedPair(const std::string& addr) { LOGI("bt: event DeviceNeedPair(%s)", addr.c_str()); }
        void OnPairListChanged() { LOGI("bt: event PairListChanged"); }
        void OnCallStatusChanged(SVPBtType::COMBINE_CALL_STATUS st) { mO.onCallStatus((int)st); }
        void OnConnectStatusChanged(SVPBtType::BT_PROFILE_CONNECT p, int v, const std::string& addr) {
            LOGI("bt: event ConnectStatusChanged(profile %d, %d, %s)", (int)p, v, addr.c_str());
            if ((int)p == 2 && v == 1) mO.onA2dpConnected(addr);
            mO.onLink();
        }
        void OnDisconnectStatusChanged(SVPBtType::BT_PROFILE_CONNECT p, int v) {
            LOGI("bt: event DisconnectStatusChanged(profile %d, %d)", (int)p, v);
            mO.onLink();
        }

    private:
        BtPairing& mO;
    };

    // OnPairingComfirmRequest's address: 6 raw bytes, most significant first, cut at the first 0.
    static std::string hexAddr(const std::string& raw) {
        std::string out;
        char b[4];
        for (size_t i = 0; i < raw.size(); ++i) {
            std::snprintf(b, sizeof b, i ? ":%02X" : "%02X", (unsigned char)raw[i]);
            out += b;
        }
        return out;
    }

    // Six random digits (a PIN for legacy pairing).
    static std::string randomPin() {
        unsigned v = 0;
        FILE* f = std::fopen("/dev/urandom", "rb");
        if (!f || std::fread(&v, sizeof v, 1, f) != 1) v = static_cast<unsigned>(time(nullptr)) * 2654435761u;
        if (f) std::fclose(f);
        char b[8];
        std::snprintf(b, sizeof b, "%06u", v % 1000000u);
        return b;
    }

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Whether OnPairingComfirmRequest's raw address is `addr` (it may be cut at a 0 byte).
    static bool sameAddr(const std::string& raw, const std::string& addr) {
        const std::string h = lower(hexAddr(raw)), a = lower(addr);
        return !h.empty() && a.compare(0, h.size(), h) == 0;
    }

    // On the subscription's thread. Only the phone that asked (btpair) is confirmed here: while
    // a window is open, btservice takes any device's connection (auto-accept), and
    // AcceptSspRequest answers whichever request came last (bt_idl.hpp), so another device's
    // request is left to the Bluetooth app and its dialog. The key is logged at debug only.
    void onConfirm(unsigned int passkey, const std::string& raw) {
        std::lock_guard<std::mutex> lk(mMutex);
        const Clock::time_point now = Clock::now();
        const bool window = !mPhone.empty() && now < mWindowEnd && sameAddr(raw, mPhone);
        // The session's phone bonding again although the car lists it: it lost its pairing.
        const bool rebond = !window && !mWatchPhone.empty() && now < mWatchEnd && sameAddr(raw, mWatchPhone);
        const bool ours = window || rebond;
        LOGV("bt: pairing key %06u", passkey);
        LOGI("bt: event PairingComfirmRequest(%s)%s", hexAddr(raw).c_str(),
             window   ? " -> Android Auto's pairing: key to the phone, accepted here"
             : rebond ? " -> the session's phone pairs again (it forgot the car): key to the phone, accepted here"
                      : " (not ours: left to the Bluetooth app)");
        if (!ours) return;
        if (rebond) mRebond = mWatchPhone;
        char line[32];
        std::snprintf(line, sizeof line, "btauth %06u", passkey);
        mCtl.send(line);
        mAccept = true;
        mCv.notify_all();
    }

    // On the subscription's thread: the calls' combined status (0 none, 1 one outgoing dialing or
    // ringing out, 4 one incoming ringing; btservice 0x15395c, 0x1535d0).
    void onCallStatus(int st) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (st == mCallStatus) return;
        LOGI("bt: event CallStatus %d", st);
        mCallStatus = st;
        mCallStatusAt = Clock::now();
    }

  public:
    // Whether a call status event that starts an outgoing call is about to be, or was just,
    // handled: no call for a second (its event not here yet) or dialing for under 600 ms. False
    // when unknown (no events), while a call runs, as one ends, or for an incoming call.
    bool outgoingCallStarting() {
        if (!mSubscribed) return false;
        std::lock_guard<std::mutex> lk(mMutex);
        const Clock::duration since = Clock::now() - mCallStatusAt;
        if (mCallStatus == 0) return mCallStatusAt == Clock::time_point() || since >= std::chrono::seconds(1);
        return mCallStatus == 1 && since < std::chrono::milliseconds(600);
    }
  private:

    // On the subscription's thread: a link came up or went down (the lock is looked at again).
    void onLink() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mLinkChanged = true;
        }
        mCv.notify_all();
    }

    // Capability 10: every link key refused (bt_idl.hpp).
    static const int kCapAutoEject = 10;

    // Worker: the lock on while the session runs and its phone holds the phone link, else off;
    // set again while it holds.
    void updateLock(bool session, const std::string& phone) {
        bool want = false;
        std::string hfp;
        if (session && !phone.empty()) {
            if (client().GetHfpConnectAddr(hfp) != 1) hfp.clear();
            want = !hfp.empty() && lower(hfp) == phone;
        }
        if (!want && !mLocked) return;
        const int r = client().SetCapability(SVPBtType::Capability(kCapAutoEject), want ? 1 : 0);
        if (want == mLocked) return; // set again
        mLocked = want;
        if (!mCfg.btLockFile.empty()) {
            if (want) {
                const std::string err = replaceFile(mCfg.btLockFile, "10\n");
                if (!err.empty()) LOGW("bt: %s: %s", mCfg.btLockFile.c_str(), err.c_str());
            } else {
                ::unlink(mCfg.btLockFile.c_str());
            }
        }
        if (want) {
            LOGI("bt: Android Auto's phone %s holds the phone link: no other device may pair or connect "
                 "(SetCapability(10, 1) -> %d)", phone.c_str(), r);
        } else {
            const std::string why = !session        ? "the session is over"
                                    : hfp.empty()   ? "Android Auto's phone let go of the phone link"
                                                    : "the phone link is " + hfp + "'s";
            LOGI("bt: other devices may pair and connect again: %s (SetCapability(10, 0) -> %d)", why.c_str(), r);
        }
    }

    // On the subscription's thread: a music link (A2DP) came up.
    void onA2dpConnected(const std::string& addr) {
        if (!mCfg.btA2dpDrop) return;
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mSession) return;
        mA2dpAddr = lower(addr);
        mDropA2dp = true;
        mCv.notify_all();
    }

    // On the subscription's thread: the car's Bluetooth wants a PIN (legacy pairing; the phone
    // asked for BLUETOOTH_PAIRING_METHOD_PIN). The phone already has the PIN ("btauth").
    void onPinRequest(unsigned int a, const std::string& b, const std::string& c) {
        std::lock_guard<std::mutex> lk(mMutex);
        const bool ours = !mPhone.empty() && Clock::now() < mWindowEnd && !mPin.empty() && sameAddr(b, mPhone);
        LOGI("bt: event PinCodeRequest(%u, %s, %s)%s", a, hexAddr(b).c_str(), printable(c).c_str(),
             ours ? " -> Android Auto's pairing: our PIN" : " (not ours: left to the Bluetooth app)");
        if (!ours) return;
        mAcceptPin = true;
        mCv.notify_all();
    }

    void onPairStatus(int status, const std::string& addr) {
        std::lock_guard<std::mutex> lk(mMutex);
        LOGI("bt: event PairStatusChanged(%d, %s)", status, addr.c_str());
        if (!mRebond.empty()) {
            LOGI("bt: %s paired again %s", mRebond.c_str(), status == 0 ? "(done)" : "failed");
            mRebond.clear();
        }
        if (mPhone.empty()) return;
        if (status == 0 && lower(addr) != lower(mPhone)) return;
        LOGI("bt: Android Auto pairing with %s %s", mPhone.c_str(), status == 0 ? "done" : "failed");
        mWindowEnd = Clock::now(); // close the window (the worker restores the setting)
        mCv.notify_all();
    }

    RcfClient<SVP_BtService>& client() {
        if (!mClient) {
            mClient.reset(new RcfClient<SVP_BtService>(RCF::TcpEndpoint(mCfg.btHost, mCfg.btPort)));
            mClient->getClientStub().setConnectTimeoutMs(2000);
            mClient->getClientStub().setRemoteCallTimeoutMs(3000);
        }
        return *mClient;
    }

    // Worker: whether the car's paired list has the address (-1: could not ask).
    int isPaired(const std::string& address) {
        const int n = client().GetDeviceNum(SVPBtType::BT_DEVICE_PAIRED);
        std::string list;
        int found = 0;
        for (int i = 0; i < n && i < 32; ++i) {
            SVPBtType::SVPBtDevice d;
            if (client().GetDevice(i, d, SVPBtType::BT_DEVICE_PAIRED) != 1 && d.address.empty()) continue;
            list += (list.empty() ? "" : ", ") + d.address;
            if (lower(d.address) == lower(address)) found = 1;
        }
        LOGI("bt: paired with the car: %d (%s)", n, list.c_str());
        return found;
    }

    void subscribe() {
        if (mSubscription && mSubscription->isConnected()) return;
        const Clock::time_point now = Clock::now();
        if (mSubscribeTried != Clock::time_point() && now - mSubscribeTried < std::chrono::seconds(10)) return;
        mSubscribeTried = now;
        if (!tcpReachable(mCfg.btHost, mCfg.btListenPort, 1000)) {
            if (!mSubscribeFailing) LOGW("bt: no Bluetooth event publisher on %s:%d", mCfg.btHost.c_str(), mCfg.btListenPort);
            mSubscribeFailing = true;
            return;
        }
        try {
            if (mSubscription) {
                LOGW("bt: event subscription lost; subscribing again");
                mSubscription->close();
                mSubscription.reset();
            }
            if (!mSubServer) {
                mSubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint(-1)));
                mSubServer->start();
            }
            RCF::SubscriptionParms parms;
            parms.setPublisherEndpoint(RCF::TcpEndpoint(mCfg.btHost, mCfg.btListenPort));
            mSubscription = mSubServer->createSubscription<SVP_BtListener>(mListener, parms);
            mSubscribed = true;
            LOGI("bt: subscribed to the Bluetooth events (%s:%d)", mCfg.btHost.c_str(), mCfg.btListenPort);
            mSubscribeFailing = false;
        } catch (const std::exception& e) {
            if (!mSubscribeFailing) LOGW("bt: event subscription failed: %s", e.what());
            mSubscribeFailing = true;
            mSubscription.reset();
        }
    }

    void run() {
        // A shim that died with a pairing window open left the unit's auto-accept on; its boot
        // default is off and no stock program sets it (bt_idl.hpp).
        if (mCfg.btPairing) try {
            client().SetAutoAcceptConnect(0).get();
            LOGI("bt: SetAutoAcceptConnect(0) (the unit's default)");
        } catch (const std::exception& e) {
            LOGW("bt: SetAutoAcceptConnect(0) failed: %s", e.what());
            mClient.reset();
        }
        // A shim that died holding the lock left the unit refusing every device; one that the
        // Bluetooth app set for CarPlay has no file and is left alone.
        if (!mCfg.btLockFile.empty() && ::access(mCfg.btLockFile.c_str(), F_OK) == 0) try {
            const int r = client().SetCapability(SVPBtType::Capability(kCapAutoEject), 0);
            ::unlink(mCfg.btLockFile.c_str());
            LOGI("bt: the lock of an earlier run taken off (SetCapability(10, 0) -> %d)", r);
        } catch (const std::exception& e) {
            LOGW("bt: taking off an earlier run's lock failed: %s", e.what());
            mClient.reset();
        }
        for (;;) {
            std::string query;
            int method = 0;
            bool accept = false, restore = false;
            bool sessionChanged = false, session = false, dropA2dp = false, a2dpBack = false;
            std::string acceptPin, a2dpAddr, sessionPhone;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                // Wake for work, the window's end, the music link's return, and every 10 s to keep
                // the subscription up.
                Clock::time_point until = Clock::now() + std::chrono::seconds(10);
                if (!mPhone.empty() && mWindowEnd < until) until = mWindowEnd;
                if (mA2dpBackPending && mA2dpBackAt < until) until = mA2dpBackAt;
                mCv.wait_until(lk, until, [this] {
                    return mStop || !mQuery.empty() || mAccept || mAcceptPin || mSessionChanged || mDropA2dp || mLinkChanged ||
                           (!mPhone.empty() && Clock::now() >= mWindowEnd) ||
                           (mA2dpBackPending && Clock::now() >= mA2dpBackAt);
                });
                sessionChanged = mSessionChanged;
                mSessionChanged = false;
                mLinkChanged = false;
                session = mSession;
                sessionPhone = mSessionPhone;
                dropA2dp = mDropA2dp;
                mDropA2dp = false;
                a2dpBack = mA2dpBackPending && Clock::now() >= mA2dpBackAt;
                if (a2dpBack) mA2dpBackPending = false;
                if (sessionChanged && session) mA2dpBackPending = false; // a new session before it returned
                a2dpAddr = mA2dpAddr;
                if (mStop) break;
                query.swap(mQuery);
                method = mQueryMethod;
                accept = mAccept;
                mAccept = false;
                acceptPin = mAcceptPin ? mPin : std::string();
                mAcceptPin = false;
                if (!mPhone.empty() && Clock::now() >= mWindowEnd) {
                    mPhone.clear();
                    mRestorePending = true;
                }
                restore = mRestorePending;
            }
            try {
                // First, and until it succeeds: a failed call below must not leave the unit
                // accepting connections from any device.
                if (restore) {
                    client().SetAutoAcceptConnect(mSavedAutoAccept).get();
                    mRestorePending = false;
                    LOGI("bt: pairing window over; SetAutoAcceptConnect(%d) (as it was)", mSavedAutoAccept);
                }
                if (sessionChanged && session && mCfg.btA2dpDrop) {
                    std::string addr;
                    const int r = client().GetAvpConnectAddr(addr);
                    if (r == 1 && addr.size() == 17) {
                        {
                            std::lock_guard<std::mutex> lk(mMutex);
                            mA2dpAddr = lower(addr);
                        }
                        client().Disconnect(SVPBtType::BT_SERVICE_A2DP).get();
                        LOGI("bt: Bluetooth music (A2DP) of %s off for the session", addr.c_str());
                    } else {
                        LOGI("bt: no Bluetooth music (A2DP) link at the session's start");
                    }
                }
                if (sessionChanged && !session) {
                    std::lock_guard<std::mutex> lk(mMutex);
                    if (!mA2dpAddr.empty()) {
                        mA2dpBackPending = true;
                        mA2dpBackAt = Clock::now() + std::chrono::seconds(2);
                    }
                }
                if (dropA2dp && session) {
                    client().Disconnect(SVPBtType::BT_SERVICE_A2DP).get();
                    LOGI("bt: Bluetooth music (A2DP) of %s connected during the session: off again", a2dpAddr.c_str());
                }
                if (a2dpBack && !session && !a2dpAddr.empty()) {
                    client().Connect(a2dpAddr, SVPBtType::BT_SERVICE_A2DP).get();
                    LOGI("bt: session over: Bluetooth music (A2DP) back to %s", a2dpAddr.c_str());
                    std::lock_guard<std::mutex> lk(mMutex);
                    mA2dpAddr.clear();
                }
                if (accept) {
                    client().AcceptSspRequest(1).get(); // .get(): see AudioSource::connect
                    LOGI("bt: AcceptSspRequest(1)");
                }
                if (!acceptPin.empty()) {
                    client().AcceptPinCodeRequest(acceptPin).get();
                    LOGI("bt: AcceptPinCodeRequest(the PIN)");
                }
                if (!query.empty()) {
                    const int paired = isPaired(query);
                    LOGI("bt: phone %s (method %d) %s the car", query.c_str(), method,
                         paired ? "is paired with" : "is not paired with");
                    mCtl.send(paired ? "btpaired 1" : "btpaired 0");
                    if (!paired) {
                        bool open;
                        {
                            std::lock_guard<std::mutex> lk(mMutex);
                            open = !mPhone.empty();
                        }
                        if (!open) mSavedAutoAccept = client().GetAutoAcceptConnect();
                        client().SetAutoAcceptConnect(1).get();
                        std::lock_guard<std::mutex> lk(mMutex);
                        mPhone = query;
                        mWindowEnd = Clock::now() + std::chrono::seconds(60);
                        LOGI("bt: pairing window for %s: 60 s, SetAutoAcceptConnect(1) (was %d)", query.c_str(),
                             mSavedAutoAccept);
                        // PIN pairing (method 4): the phone gets the PIN now and bonds with it; the
                        // car's PIN request is answered with the same one (onPinRequest). Numeric
                        // comparison (2): the key comes from the car's confirm request (onConfirm).
                        mPin.clear();
                        if (method == 4) {
                            mPin = randomPin();
                            mCtl.send("btauth " + mPin);
                            LOGI("bt: PIN pairing: a PIN to the phone");
                            LOGV("bt: the PIN is %s", mPin.c_str());
                        }
                    }
                }
                if (mCfg.btLock) updateLock(session, sessionPhone);
            } catch (const std::exception& e) {
                LOGW("bt: pairing call failed: %s", e.what());
                mClient.reset();
            }
            subscribe();
        }
        // Put the unit's setting back if a window is still open, or its restore failed, and
        // the phone's music link if a session ended (or was ended by the shutdown) without it.
        bool open;
        std::string a2dpAddr;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            open = !mPhone.empty() || mRestorePending;
            if (!mSession) a2dpAddr = mA2dpAddr; // dropped for a session, not yet given back
        }
        try {
            if (open) client().SetAutoAcceptConnect(mSavedAutoAccept).get();
            if (mLocked) {
                const int r = client().SetCapability(SVPBtType::Capability(kCapAutoEject), 0);
                mLocked = false;
                if (!mCfg.btLockFile.empty()) ::unlink(mCfg.btLockFile.c_str());
                LOGI("bt: stopping: other devices may pair and connect again (SetCapability(10, 0) -> %d)", r);
            }
            if (!a2dpAddr.empty() && mCfg.btA2dpDrop) {
                client().Connect(a2dpAddr, SVPBtType::BT_SERVICE_A2DP).get();
                LOGI("bt: stopping: Bluetooth music (A2DP) back to %s", a2dpAddr.c_str());
            }
            if (mSubscription) mSubscription->close();
            mSubscription.reset();
            if (mSubServer) mSubServer->stop();
        } catch (const std::exception& e) {
            LOGW("bt: closing: %s", e.what());
        }
    }

    const Config& mCfg;
    ControlChannel& mCtl;
    std::mutex mMutex;
    MonoCv mCv;
    bool mStop = false;
    bool mRestorePending = false;    // the window is over; auto-accept not yet put back
    std::string mQuery;              // a btpair to answer
    int mQueryMethod = 0;
    std::string mPhone;              // the phone being paired (a window is open)
    Clock::time_point mWindowEnd;
    bool mAccept = false;            // AcceptSspRequest(1) to send
    bool mAcceptPin = false;         // AcceptPinCodeRequest(mPin) to send
    std::string mPin;                // PIN pairing: the PIN given to the phone
    std::string mWatchPhone;         // the session's phone (last btpair), for a re-bond
    Clock::time_point mWatchEnd;
    std::string mRebond;             // a re-bond being confirmed (for the log)
    int mCallStatus = 0;             // OnCallStatusChanged
    Clock::time_point mCallStatusAt; // when it last changed (never: epoch)
    std::atomic<bool> mSubscribed{false};
    bool mSession = false;           // AA projecting
    bool mSessionChanged = false;
    bool mDropA2dp = false;          // a Disconnect(A2DP) to send
    std::string mA2dpAddr;           // the device whose music link was dropped
    std::string mSessionPhone;       // the session's phone (its btpair), lower case; "" none
    bool mLinkChanged = false;       // a link event, a btpair: the lock to look at
    bool mA2dpBackPending = false;   // Connect(mA2dpAddr, A2DP) at mA2dpBackAt
    Clock::time_point mA2dpBackAt;
    Listener mListener;
    // The worker's only.
    int mSavedAutoAccept = 0;
    bool mLocked = false;            // capability 10 set by us
    std::unique_ptr<RcfClient<SVP_BtService> > mClient;
    std::unique_ptr<RCF::RcfServer> mSubServer;
    RCF::SubscriptionPtr mSubscription;
    Clock::time_point mSubscribeTried;
    bool mSubscribeFailing = false;
    std::thread mWorker;
};

// ---------------------------------------------------------------------------------------
// The bridge's notify socket (AA_SHIM_NOTIFY, datagrams of one line each):
//   * "open <stream name>": the volume of the prompt streams (audio_idl.hpp). audiomgr sets a
//     CarLife source's volume on the "sv-as-carlife-prompt" streams that exist when the source
//     becomes active, and stock carlifeservice registers each stream it opens (RegisterAudio
//     right after pa_simple_new), which gives a stream opened while CL_NAVI is active the
//     navigation volume. The bridge opens our streams, so it sends this each time, and this
//     calls RegisterAudio for it. Without it, a stream opened after the source was taken started
//     at PulseAudio's default, full volume (the assistant's first chime of each boot: CL_NAVI is
//     taken when the mic opens, before the chime's stream exists). Not with AA_AUDIO_PORT=0;
//   * "video": the bridge rebuilt its video pipeline in place (after a video error or stall, the
//     same process) and the new one has its first frame. waylandsink2 then makes its ILM surface
//     again, with the same id, hidden: the surface is shown again if the screen is up (as for a
//     new session; nothing to do while it is not, the next Show(1) shows it).
// Anything else is ignored, so a newer bridge can say more.
class BridgeNotify {
public:
    BridgeNotify(const Config& cfg, std::function<void()> onVideo) : mCfg(cfg), mOnVideo(onVideo), mStop(false) {
        if (mCfg.notifySock != "-") mWorker = std::thread(&BridgeNotify::run, this);
    }

    ~BridgeNotify() {
        mStop = true;
        if (mWorker.joinable()) mWorker.join();
    }

private:
    void run() {
        int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        if (fd < 0 || mCfg.notifySock.size() >= sizeof addr.sun_path) {
            LOGE("bridge: notify socket %s: %s", mCfg.notifySock.c_str(), fd < 0 ? std::strerror(errno) : "path too long");
            if (fd >= 0) ::close(fd);
            return;
        }
        std::strncpy(addr.sun_path, mCfg.notifySock.c_str(), sizeof addr.sun_path - 1);
        const std::string dir = dirName(mCfg.notifySock);
        if (!dir.empty()) mkdirs(dir, 0755);
        ::unlink(addr.sun_path);
        if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0) {
            LOGE("bridge: notify socket %s: %s", mCfg.notifySock.c_str(), std::strerror(errno));
            ::close(fd);
            return;
        }
        ::chmod(addr.sun_path, 0600); // the bridge runs as root, as every service on the unit
        LOGI("bridge: notify socket %s (prompt streams%s, video rebuilds)", mCfg.notifySock.c_str(),
             mCfg.audioPort > 0 ? "" : " off");
        std::unique_ptr<RcfClient<ISVP_AudioService> > client;
        bool failing = false;
        while (!mStop) {
            pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            if (::poll(&p, 1, 300) <= 0) continue;
            char buf[128];
            const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string msg(buf);
            while (!msg.empty() && (msg[msg.size() - 1] == '\n' || msg[msg.size() - 1] == '\r')) msg.erase(msg.size() - 1);
            if (msg == "video") {
                LOGI("bridge: video pipeline rebuilt; its surface is shown again if the screen is up");
                if (mOnVideo) mOnVideo();
                continue;
            }
            if (msg.compare(0, 5, "open ") != 0 || mCfg.audioPort <= 0) {
                LOGV("bridge: notify \"%s\" ignored", printable(msg).c_str());
                continue;
            }
            const std::string name = msg.substr(5);
            if (name.compare(0, 6, "sv-as-") != 0) continue; // only the platform's prompt streams
            try {
                if (!client) {
                    client.reset(new RcfClient<ISVP_AudioService>(RCF::TcpEndpoint(mCfg.audioHost, mCfg.audioPort)));
                    client->getClientStub().setConnectTimeoutMs(1000);
                    client->getClientStub().setRemoteCallTimeoutMs(2000);
                }
                const int r = client->RegisterAudio(name, kRegisterAllPids);
                LOGI("audio: RegisterAudio(%s) -> %d", name.c_str(), r);
                failing = false;
            } catch (const RCF::Exception& e) {
                if (!failing) LOGW("audio: RegisterAudio(%s) on %s:%d failed: %s", name.c_str(), mCfg.audioHost.c_str(),
                                   mCfg.audioPort, e.getErrorString().c_str());
                failing = true;
                client.reset();
            } catch (const std::exception& e) {
                if (!failing) LOGW("audio: RegisterAudio(%s) failed: %s", name.c_str(), e.what());
                failing = true;
                client.reset();
            }
        }
        ::close(fd);
        ::unlink(addr.sun_path);
    }

    const Config& mCfg;
    std::function<void()> mOnVideo;
    std::atomic<bool> mStop;
    std::thread mWorker;
};

const int AudioSource::kSources[4] = {SVPSource::SID_CARLIFE_MEDIA, SVPSource::SID_CARLIFE_NAVI,
                                      SVPSource::SID_CARLIFE_PROMPT, SVPSource::SID_CARLIFE_ASR};

// ---------------------------------------------------------------------------------------
class CarLifeShim {
public:
    typedef RCF::Publisher<ISVP_CarLifeListener> ListenerPublisher;

    CarLifeShim(const Config& cfg, ControlChannel& ctl)
        : mCfg(cfg), mCtl(ctl), mVideo(cfg), mLast(cfg), mAudio(cfg, ctl, mLast), mCar(cfg, ctl), mMic(cfg), mBt(cfg, ctl), mPair(cfg, ctl),
          mNotify(cfg, [this] { mVideo.newSession(); }),
          mPower(cfg, [this](int s, int a, int b) { onPower(s, a, b); }), mAppc(cfg), mStatus(CL_ST_NONE), mConnecting(false), mStopping(false) {
        mPower.start(); // after every member: its events call onPower
        mAppc.start();
        mAnnouncer = std::thread(&CarLifeShim::runAnnouncer, this);
        writeOnTop(0);
    }

    ~CarLifeShim() {
        writeOnTop(0);
        {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            mStopping = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
        if (mAnnouncer.joinable()) mAnnouncer.join();
    }

    void setPublisher(const boost::shared_ptr<ListenerPublisher>& pub) {
        std::lock_guard<std::mutex> lk(mPubMutex);
        mPub = pub;
    }

    // A client subscribed to our status events (RCF's OnSubscriberConnect, on the publisher's
    // thread, before RCF adds it: the announcement must come later, and never from here).
    // appconnect subscribes once, when it starts, and shows Android Auto only on the 2 -> 3
    // events, never from GetCarLifeConnectStatus (step 3 run 46). With a phone attached at
    // boot the session can reach 2 or 3 before appconnect starts, so a newcomer hears the
    // running session announced again (announceSession).
    void subscriberJoined() {
        LOGI("a client subscribed to the status events");
        {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            mAnnounceAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAnnounceDelayMs);
            mAnnounce = true;
        }
        mCv.notify_all();
    }

    // SIGTERM: a running session ends as the user's Disconnect ends it, while the publisher is
    // still up (appconnect hears (4, 0): after a restart it would hear nothing, being
    // subscribed to this process only), then the sources and the mic path are given back,
    // within `ms`. The receiver gets "stop" and the phone's music link comes back as the
    // Bluetooth worker stops.
    void endSession(int ms) {
        {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            mStopping = true; // no bring-up or announcement goes on from here
        }
        mCv.notify_all();
        const S_CL_CONNECT_STATUS st = currentStatus();
        if (st == CL_ST_CONNECTING || st == CL_ST_VIDEO) {
            LOGI("stopping: ending the session (status %d)", (int)st);
            mCtl.send("stop");
            setStatus(CL_ST_DISCONNECTED);
        }
        setVoice(false);
        const std::chrono::steady_clock::time_point until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!(mAudio.idle() && mMic.idle())) {
            if (std::chrono::steady_clock::now() >= until) {
                LOGW("stopping: the audio sources or the mic path not given back within %d ms", ms);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // ---- ISVP_CarLifeService, in wire order --------------------------------------------

    int Play(int n) {                                                         // 0
        LOGI("Play(%d)", n);
        mCtl.send("key play");
        return 0;
    }

    int Pause(int n) {                                                        // 1
        LOGI("Pause(%d)", n);
        mCtl.send("key pause");
        return 0;
    }

    int Show(int n) {                                                         // 2
        LOGI("Show(%d)", n);
        mShown = n != 0;
        publishOnTop();
        mVideo.show(n != 0);
        // Not the media source on every Show(1), as stock CarLife does: that switched the
        // speakers to a silent AA (the owner wants CarPlay's way: AA takes them when its media
        // plays, "audio media 1"). Only to resume what another source interrupted for a while;
        // never over a main source the user chose (the radio): see resumeIfInterrupted.
        if (n) mAudio.resumeIfInterrupted();
        if (n) patchAppconnect(); // again if appconnect restarted since
        mCtl.send("show " + std::to_string(n));
        // Back after the phone's "back to the car" icon: open on Android Auto's home screen,
        // not on the launcher the user left from.
        if (n && mHomeOnShow.exchange(false)) mCtl.send("home");
        return 0;
    }

    // The unit's power state (PowerWatch), as stock carlifeservice follows it (power_idl.hpp):
    // switched off, AA's media pauses and the phone's video stops (the screen is dark); on
    // again, both come back; last mode keeps what it was at the switch-off.
    void onPower(int state, int a, int b) {
        LOGI("power: %s (%d, %d, %d)", SVPPower::name(state), state, a, b);
        // A boot that reaches a switch-off was a healthy one, however short: the launcher's
        // boot-loop counter (aa_run.sh, AA_BOOTS_FILE) is cleared, once per boot.
        if ((state == SVPPower::PS_USER_OFF || state == SVPPower::PS_SHUT_DOWN) && !mCfg.bootsFile.empty() &&
            !mBootsCleared.exchange(true)) {
            const std::string err = replaceFile(mCfg.bootsFile, "0\n");
            if (err.empty()) LOGI("power: switch-off: boot-loop counter cleared (%s)", mCfg.bootsFile.c_str());
            else LOGW("power: clearing %s: %s", mCfg.bootsFile.c_str(), err.c_str());
        }
        if (state == SVPPower::PS_USER_OFF) {
            mLast.freeze(true);
            mAudio.power(false);
            if (mShown) mCtl.send("show 0");
        } else if (state == SVPPower::PS_USER_ON) {
            mAudio.power(true, !mPower.temporaryOn());
            mLast.freeze(false);
            if (mShown) mCtl.send("show 1");
        } else if (state == SVPPower::PS_SHUT_DOWN || state == SVPPower::PS_RESET) {
            mLast.freeze(true);
        }
    }

    // appconnect's code changes (appc_patch.hpp, AppcWatch): look again now. Until the
    // outgoing-call change is on, the call fix in NeedBlockBTPopup() stands in.
    void patchAppconnect() { mAppc.now(); }

    int StartApp(int n) {                                                     // 3
        LOGI("StartApp(%d)", n);
        return 0;
    }

    int CarLifeConnection() {                                                 // 4
        patchAppconnect(); // before any call of this session
        // Called synchronously from connectionservice's USB hot-plug handler: return at
        // once and do the bring-up on a worker, as stock does (CCarLifeSetup::setupRunThread).
        // connectionservice calls this on every USB attach of a phone set to CarLife, and the
        // receiver's own accessory-mode switch re-attaches the phone. During a bring-up the
        // call is ignored. During a running session it is answered without touching the
        // session: connecting, then video, published again (reannounce).
        std::lock_guard<std::mutex> lk(mWorkerMutex);
        if (mConnecting) {
            LOGI("CarLifeConnection(): bring-up already running, ignored");
            return 0;
        }
        if (mWorker.joinable()) mWorker.join();
        mConnecting = true;
        if (currentStatus() == CL_ST_VIDEO) {
            LOGI("CarLifeConnection(): session running; announcing it again");
            mWorker = std::thread(&CarLifeShim::reannounce, this);
        } else {
            LOGI("CarLifeConnection()");
            mWorker = std::thread(&CarLifeShim::runConnect, this);
        }
        return 0;
    }

    // The unit asked for a connection while one runs. Since the receiver waits for the unit
    // after a session ends (Disconnect), the session now often reaches video before the unit's
    // second call, the one for the phone re-attached in accessory mode (step 3 run 45: video at
    // 145.8 s, the call at 146.2 s). That call was ignored and nothing was published, so
    // appconnect's "connecting" for it never ended: AA not brought to the screen, no
    // "connected", until the APP key. Answering it as the stock flow sees a connection, 2 then 3,
    // ends it (run 42: the call came before video, and AA came up 0.1 s after 3).
    void reannounce() {
        announceSession();
        std::lock_guard<std::mutex> lk(mWorkerMutex);
        mConnecting = false;
    }

    // The running session told again: connecting (2), then 0.3 s later video (3) if it is still
    // 3, and the voice module's status. Each only while the status is what it announces, checked
    // under mPubMutex as setStatus publishes, so a session that ends meanwhile is never
    // announced after its (4, 0). The session's own state and its audio, Bluetooth and mic
    // set-up are left as they are (unlike setStatus).
    void announceSession() {
        if (!announce(CL_ST_CONNECTING, true)) return;
        if (sleepUnlessStopping(300)) announce(CL_ST_VIDEO, false);
        std::lock_guard<std::mutex> lk(mPubMutex);
        const S_CL_CONNECT_STATUS st = currentStatus();
        if (!mPub || (st != CL_ST_CONNECTING && st != CL_ST_VIDEO)) return;
        try {
            mPub->publish().OnModuleStatusChange(kModuleVr, mVoice.load());
        } catch (const RCF::Exception& e) {
            LOGW("publish OnModuleStatusChange failed: %s", e.getErrorString().c_str());
        }
    }

    // OnCarLifeConnectStatusChange(st, 0) to the subscribers if the status is st (or, with
    // orVideo, st or 3); false when it was not.
    bool announce(S_CL_CONNECT_STATUS st, bool orVideo) {
        std::lock_guard<std::mutex> lk(mPubMutex);
        const S_CL_CONNECT_STATUS cur = currentStatus();
        if (cur != st && !(orVideo && cur == CL_ST_VIDEO)) return false;
        LOGI("OnCarLifeConnectStatusChange(%d %s, 0) (announced again)", (int)st, statusName(st));
        if (!mPub) return true;
        try {
            mPub->publish().OnCarLifeConnectStatusChange(st, CL_ERR_NONE);
        } catch (const RCF::Exception& e) {
            LOGW("publish OnCarLifeConnectStatusChange failed: %s", e.getErrorString().c_str());
        }
        return true;
    }

    // The announcer thread: announceSession() kAnnounceDelayMs after a client subscribed, while
    // a session runs (several subscribers close together get one announcement).
    void runAnnouncer() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mStatusMutex);
                for (;;) {
                    if (mStopping) return;
                    if (mAnnounce && std::chrono::steady_clock::now() >= mAnnounceAt) break;
                    if (mAnnounce) mCv.wait_until(lk, mAnnounceAt);
                    else           mCv.wait(lk);
                }
                mAnnounce = false;
                if (mStatus != CL_ST_CONNECTING && mStatus != CL_ST_VIDEO) continue;
                LOGI("a session is running (status %d): announcing it to the new subscriber", (int)mStatus);
            }
            announceSession();
        }
    }

    int CarLifeDisConnection() {                                              // 5
        LOGI("CarLifeDisConnection()");
        mCtl.send("stop");
        setStatus(CL_ST_DISCONNECTED);
        return 0;
    }

    void SingleTouchInfo(unsigned int x, unsigned int y, S_CL_TOUCH_EVENT ev) { // 6
        const char* name = touchName(ev);
        if (!name) {
            LOGW("SingleTouchInfo(%u, %u, %d): unknown action", x, y, (int)ev);
            return;
        }
        LOGV("SingleTouchInfo(%u, %u, %s)", x, y, name);
        char line[64];
        std::snprintf(line, sizeof line, "touch %s %u %u", name, x, y);
        mCtl.send(line);
    }

    void RotateKeyEvent(unsigned int code, int value) {                       // 7
        LOGV("RotateKeyEvent(0x%04x, %d)", code, value);
        char line[64];
        std::snprintf(line, sizeof line, "rotary %u %d", code, value);
        mCtl.send(line);
    }

    S_CL_CONNECT_STATUS GetCarLifeConnectStatus() {                           // 8
        std::lock_guard<std::mutex> lk(mStatusMutex);
        if (mStatusOnce) {
            mStatusOnce = false;
            if (mStatus == CL_ST_VIDEO && std::chrono::steady_clock::now() < mStatusOnceUntil) {
                LOGI("GetCarLifeConnectStatus() -> 2 once: an outgoing call starts; appconnect keeps Android Auto "
                     "on screen (see NeedBlockBTPopup)");
                return CL_ST_CONNECTING;
            }
        }
        LOGV("GetCarLifeConnectStatus() -> %d", (int)mStatus);
        return mStatus;
    }

    void GoToForeGround() {                                                   // 9
        LOGI("GoToForeGround()");
        mCtl.send("foreground");
    }

    // Asked by the Bluetooth app on every call status (ShowCallPop) and by appconnect for the
    // steering-wheel phone key. 1 while Android Auto is on screen: the app shows no incoming-call
    // pop-up over it (AA shows the call), and appconnect answers / ends calls with the phone key
    // itself through btservice (MFLPhoneFunction). 0 otherwise, or the key would go unhandled.
    //
    // It is also the first sign of a call status change we get: the Bluetooth app asks it as
    // soon as one arrives, ~14 ms before appconnect's CAppconnectDataMgr::OnCallStatusChanged
    // (unit 0x33590) asks GetCarLifeConnectStatus() for an outgoing call (status 1): when that
    // is 3 and appconnect is in front, appconnect sleeps 1 s and launches the Bluetooth app in
    // "bt_carlife_temp_call" mode, which covers Android Auto for the whole call (step 3 runs
    // 38, 39). So when an outgoing call is starting (BtPairing::outgoingCallStarting), the next
    // GetCarLifeConnectStatus() within AA_CALL_FIX_MS (400) answers 2 once: appconnect skips the
    // launch and the call stays on AA's own screen. Lost race: the launch happens, as before
    // (step 3 run 41: by 9 ms). An incoming call (status 4) and a call ending are left alone.
    // AA_CALL_FIX=0 turns it off. It stands in only while appconnect is not patched
    // (appc_patch.hpp), which removes the launch itself.
    int NeedBlockBTPopup() {                                                  // 10
        const int block = currentStatus() == CL_ST_VIDEO && mShown ? 1 : 0;
        LOGI("NeedBlockBTPopup() -> %d", block);
        if (block && mCfg.callFix && !mAppc.patched() && mPair.outgoingCallStarting()) {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            mStatusOnceUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(mCfg.callFixMs);
            mStatusOnce = true;
        }
        return block;
    }

    int NeedSwitchCarLifeAppToFG() {                                          // 11
        LOGI("NeedSwitchCarLifeAppToFG() -> 0");
        return 0;
    }

    unsigned int GetModuleStatus(unsigned int module) {                       // 12
        const unsigned int st = module == kModuleVr ? mVoice.load() : 0u;
        LOGV("GetModuleStatus(%u) -> %u", module, st);
        return st;
    }

    void SetSourceAvailable(int n) {                                          // 13
        // TODO(source): claim the media/nav/VR sources through svp.svc.source.
        LOGI("SetSourceAvailable(%d)", n);
    }

    void SendModuleControlCmd(unsigned int module, unsigned int cmd) {        // 14
        LOGI("SendModuleControlCmd(%u, %u)", module, cmd);
        // CarLife modules: 3 music (0 pause, 1 play; see AudioSource::musicControl), 4 voice
        // (0 stop: appconnect going to the background mid-assistant; AA ends its own voice
        // session, nothing to do).
        if (module == 3 && cmd <= 1) mAudio.musicControl(cmd == 1);
    }

    std::string GetConnectCarlifePhoneBtAddress() {                           // 15
        LOGI("GetConnectCarlifePhoneBtAddress() -> \"\"");
        return std::string();
    }

    int ConnectCarlifePhoneIsiOS() {                                          // 16
        LOGI("ConnectCarlifePhoneIsiOS() -> 0");
        return 0;
    }

    int NeedChangeHandFreeMode() {                                           // 18
        // See carlife_idl.hpp: the call stays on the hands-free mode the Bluetooth app chose.
        LOGI("NeedChangeHandFreeMode() -> 0");
        return 0;
    }

    void ClearCarLifeStatus() {                                               // 17
        // Stock writes 4 to the status global here; whether it also publishes is unknown,
        // so only the stored value changes.
        LOGI("ClearCarLifeStatus()");
        {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            mStatus = CL_ST_DISCONNECTED;
        }
        publishOnTop();
    }

    // Android Auto on screen, as NeedBlockBTPopup tells it (status 3 and shown), to
    // AA_ONTOP_FILE as "<pid> <0|1>", for the Bluetooth app, which shows no pop-up while it is 1
    // and this process lives (i18n/lib/bluetooth.cpp). Written when it changes; 0 at the start
    // and the stop.
    void publishOnTop() { writeOnTop(currentStatus() == CL_ST_VIDEO && mShown ? 1 : 0); }

    void writeOnTop(int on) {
        if (mCfg.onTopFile.empty()) return;
        std::lock_guard<std::mutex> lk(mOnTopMutex);
        if (on == mOnTop) return;
        char line[32];
        std::snprintf(line, sizeof line, "%d %d\n", static_cast<int>(::getpid()), on);
        const std::string err = replaceFile(mCfg.onTopFile, line);
        if (!err.empty()) {
            if (!mOnTopFailed) LOGW("%s: %s; the Bluetooth app's pop-ups are not held back", mCfg.onTopFile.c_str(), err.c_str());
            mOnTopFailed = true;
            return;
        }
        mOnTop = on;
        mOnTopFailed = false;
        LOGI("Android Auto %s screen: Bluetooth pop-ups %s", on ? "on" : "off", on ? "held back" : "allowed");
    }

    // ---- receiver events ---------------------------------------------------------------

    static void onReceiverLine(void* ctx, const std::string& line) {
        static_cast<CarLifeShim*>(ctx)->receiverLine(line);
    }

private:
    void receiverLine(const std::string& line) {
        char what[16];
        int on = 0;
        if (line.compare(0, 6, "state ") == 0) {
            receiverState(line.substr(6));
        } else if (std::sscanf(line.c_str(), "audio %15s %d", what, &on) == 2) {
            LOGI("receiver: %s audio %s", what, on ? "on" : "off");
            // As stock: media playback asks for the media source (mediaPrepared), the voice
            // prompts for the navigation one (ttsInit / ttsStop); system sounds too.
            if (!std::strcmp(what, "media")) mAudio.media(on != 0);
            else if (!std::strcmp(what, "speech")) mAudio.navi(AudioSource::kNaviSpeech, on != 0);
            else if (!std::strcmp(what, "system")) mAudio.navi(AudioSource::kNaviSystem, on != 0);
        } else if (line.compare(0, 7, "btpair ") == 0) {
            char addr[32] = {0};
            int method = 0;
            if (std::sscanf(line.c_str(), "btpair %31s %d", addr, &method) >= 1) {
                LOGI("receiver: the phone %s asks whether it is paired with the car", addr);
                mPair.query(addr, method);
            }
        } else if (line == "hostui") {
            // The phone's "back to the car" icon (the make logo in Android Auto's launcher).
            // Stock carlifeservice publishes OnGoToDeskTop when CarLife's phone asks for the
            // same; appconnect then sends its window system event 85 and leaves the screen.
            LOGI("receiver: back to the car's home screen -> OnGoToDeskTop");
            mHomeOnShow = true;
            goToDeskTop();
        } else if (std::sscanf(line.c_str(), "mic %d", &on) == 1) {
            LOGI("receiver: mic %s", on ? "on" : "off");
            // The voice module's status, on which appconnect routes the car mic to us (see
            // MicPath), as stock voice recognition (vrInit / vrStop); for the sound, the
            // navigation source rather than stock's ASR one (see AudioSource::navi).
            mAudio.navi(AudioSource::kNaviMic, on != 0);
            setVoice(on != 0);
        } else {
            LOGV("receiver: %s", line.c_str());
        }
    }

    void receiverState(const std::string& state) {
        LOGI("receiver state: %s", state.c_str());
        S_CL_CONNECT_STATUS cur = currentStatus();
        if (state == "connecting" || state == "connected") {
            // Stock stays at connecting (2) from ConnectionInit until the first video frame.
            if (cur != CL_ST_CONNECTING) setStatus(CL_ST_CONNECTING);
        } else if (state == "projecting") {
            setStatus(CL_ST_VIDEO);
            mVideo.newSession();
            mAudio.attach(true);
        } else if (state == "failed") {
            if (cur == CL_ST_VIDEO) setStatus(CL_ST_DISCONNECTED, CL_ERR_FAILED);
            else                    setStatus(CL_ST_CONNECT_FAILED, CL_ERR_FAILED);
        } else if (state == "disconnected" || state == "gone") {
            // Only the end of a bring-up or a session is news. After a failure (1 or 5) the
            // receiver's "disconnected" answer to "stop" leaves the failure as the final state.
            if (cur == CL_ST_CONNECTING || cur == CL_ST_VIDEO) setStatus(CL_ST_DISCONNECTED);
        } else {
            LOGW("unknown receiver state '%s'", state.c_str());
        }
    }

    S_CL_CONNECT_STATUS currentStatus() {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        return mStatus;
    }

    // Mirrors the stock emit: store the value the getter returns, then publish. mPubMutex is
    // held from the store to the publish, so the order of the published statuses is the order
    // they were stored in, whichever threads set them (the receiver's lines, appconnect's calls,
    // the bring-up worker); the fan-out under it only queues work for the parts' own threads.
    void setStatus(S_CL_CONNECT_STATUS st, S_CL_CONNECT_ERROR err = CL_ERR_NONE) {
        std::lock_guard<std::mutex> pub(mPubMutex);
        {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            mStatus = st;
        }
        publishOnTop();
        // A session ended: give the audio sources back, as stock SetSourceAvailable(0) does,
        // and the mic back to Bluetooth.
        if (st == CL_ST_DISCONNECTED || st == CL_ST_CONNECT_FAILED || st == CL_ST_TIMEOUT) {
            mAudio.attach(false);
            setVoiceLocked(false);
            mPair.session(false);
        }
        if (st == CL_ST_VIDEO) mPair.session(true);
        mCv.notify_all();
        LOGI("OnCarLifeConnectStatusChange(%d %s, %d)", (int)st, statusName(st), (int)err);
        if (!mPub) return;
        try {
            mPub->publish().OnCarLifeConnectStatusChange(st, err);
        } catch (const RCF::Exception& e) {
            LOGW("publish OnCarLifeConnectStatusChange failed: %s", e.getErrorString().c_str());
        }
    }

    // CarLife's module 4 is voice recognition: stock carlifeservice publishes its status
    // (cmdModuleStatus) and appconnect answers OnModuleStatusChange(4, 1) with
    // SetECNRAudioPath(EAP_WIRED_PHONE_MODE) and (4, 0) with EAP_BT_MODE. While the session runs
    // it also puts the path back to what GetModuleStatus(4) says whenever btservice reports a
    // change, so GetModuleStatus must tell the truth. Running only during a session: a late
    // "mic 1" after the session's end must not take the car mic from Bluetooth calls.
    void setVoice(bool on) {
        std::lock_guard<std::mutex> pub(mPubMutex);
        if (on) {
            const S_CL_CONNECT_STATUS st = currentStatus();
            if (st != CL_ST_CONNECTING && st != CL_ST_VIDEO) {
                LOGW("voice module (4): the phone wants the mic outside a session (status %d); ignored", (int)st);
                return;
            }
        }
        setVoiceLocked(on);
    }

    // mPubMutex held.
    void setVoiceLocked(bool on) {
        if (mVoice.exchange(on ? 1u : 0u) == (on ? 1u : 0u)) return;
        LOGI("voice module (4): %s -> OnModuleStatusChange(4, %d)", on ? "running" : "idle", on ? 1 : 0);
        mMic.set(on);
        if (!mPub) return;
        try {
            mPub->publish().OnModuleStatusChange(kModuleVr, on ? 1u : 0u);
        } catch (const RCF::Exception& e) {
            LOGW("publish OnModuleStatusChange failed: %s", e.getErrorString().c_str());
        }
    }

    void goToDeskTop() {
        std::lock_guard<std::mutex> lk(mPubMutex);
        if (!mPub) return;
        try {
            mPub->publish().OnGoToDeskTop();
        } catch (const RCF::Exception& e) {
            LOGW("publish OnGoToDeskTop failed: %s", e.getErrorString().c_str());
        }
    }

    bool sleepUnlessStopping(int ms) {
        std::unique_lock<std::mutex> lk(mStatusMutex);
        return !mCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return mStopping.load(); });
    }

    void runConnect() {
        if (mCfg.fake) runFakeConnect();
        else           runReceiverConnect();
        std::lock_guard<std::mutex> lk(mWorkerMutex);
        mConnecting = false;
    }

    // Plays the happy path without a phone, to see what appconnect does with it. Stock sends
    // no OnConnectingStatusPercentage on the way, so neither does this.
    void runFakeConnect() {
        setStatus(CL_ST_CONNECTING);
        if (!sleepUnlessStopping(mCfg.fakeVideoMs)) return;
        setStatus(CL_ST_VIDEO);
    }

    void runReceiverConnect() {
        setStatus(CL_ST_CONNECTING);
        if (!mCtl.waitConnected(mCfg.receiverWaitMs)) {
            if (mStopping) return; // endSession has published the end
            LOGW("AA receiver not connected to %s after %d ms", mCfg.ctlSock.c_str(),
                 mCfg.receiverWaitMs);
            setStatus(CL_ST_CONNECT_FAILED, CL_ERR_FAILED);
            return;
        }
        if (mStopping) return;
        // Last mode for the session about to start (LastMode), and the car's night mode and
        // driver's side, which its service discovery reads.
        if (mLast.enabled()) mCtl.send(mLast.aaHadSpeakers() ? "autoplay 1" : "autoplay 0");
        mCar.sendCurrent();
        if (!mCtl.send("start")) {
            setStatus(CL_ST_CONNECT_FAILED, CL_ERR_FAILED);
            return;
        }
        // The receiver's state lines move the status (receiverState); wait for an outcome.
        std::unique_lock<std::mutex> lk(mStatusMutex);
        bool done = mCv.wait_for(lk, std::chrono::milliseconds(mCfg.connectTimeoutMs), [this] {
            return mStopping.load() || mStatus != CL_ST_CONNECTING;
        });
        lk.unlock();
        if (!done) {
            // As stock ConnectionTimeOutProcess does when its connection timer expires.
            LOGW("no video after %d ms, giving up", mCfg.connectTimeoutMs);
            mCtl.send("stop");
            setStatus(CL_ST_TIMEOUT, CL_ERR_FAILED);
        }
    }

    const Config& mCfg;
    ControlChannel& mCtl;
    VideoSurface mVideo;
    LastMode mLast;                         // before mAudio, which reports to it
    AudioSource mAudio;
    CarInfo mCar;
    MicPath mMic;
    BtInfo mBt;
    BtPairing mPair;
    BridgeNotify mNotify;                   // after mVideo, which its "video" datagrams show again
    PowerWatch mPower;
    AppcWatch mAppc;
    std::atomic<bool> mHomeOnShow{false};   // the next Show(1) sends "home"
    std::atomic<bool> mShown{false};        // the last Show(n): AA on screen
    std::mutex mOnTopMutex;
    int mOnTop = -1;                         // what AA_ONTOP_FILE says (-1: not written yet)
    bool mOnTopFailed = false;
    std::atomic<bool> mBootsCleared{false}; // the boot-loop counter cleared at this boot's switch-off
    bool mStatusOnce = false;               // GetCarLifeConnectStatus: answer 2 once (mStatusMutex)
    std::chrono::steady_clock::time_point mStatusOnceUntil;
    std::atomic<unsigned int> mVoice{0};    // CarLife module 4 (voice) status: 1 while the phone has the mic
    enum { kModuleVr = 4 };                 // CarLife module 4: voice recognition

    std::mutex mStatusMutex;
    MonoCv mCv;
    S_CL_CONNECT_STATUS mStatus;

    std::mutex mPubMutex;
    boost::shared_ptr<ListenerPublisher> mPub;

    std::mutex mWorkerMutex;
    std::thread mWorker;
    bool mConnecting;
    std::atomic<bool> mStopping;

    // A late subscriber's announcement (subscriberJoined); mStatusMutex.
    enum { kAnnounceDelayMs = 500 };
    bool mAnnounce = false;
    std::chrono::steady_clock::time_point mAnnounceAt;
    std::thread mAnnouncer;
};

// ---------------------------------------------------------------------------------------

// SVP_RegisterService (libservice.so) only creates this empty file. Clients watch the
// directory for IN_CREATE | IN_DELETE (SVPServiceMonitor, mask 0x300); connectionservice makes
// its client lazily on a call, while appconnect subscribes to our events once, when it starts
// (step 3 run 46). A file left behind by a killed carlifeservice would make our O_CREAT a
// silent no-op, so delete it first: clients then see "down", then "up".
bool registerService(const std::string& dir, std::string& pathOut) {
    if (dir == "-") return true;
    if (!mkdirs(dir, 0777)) {
        LOGE("cannot create %s: %s", dir.c_str(), std::strerror(errno));
        return false;
    }
    pathOut = dir + "/" + kServiceName;
    if (::unlink(pathOut.c_str()) == 0) {
        LOGI("removed a stale %s", pathOut.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    // 0700, as SVP_RegisterService leaves the other services' files.
    int fd = ::open(pathOut.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    if (fd < 0) {
        LOGE("cannot create %s: %s", pathOut.c_str(), std::strerror(errno));
        pathOut.clear();
        return false;
    }
    ::close(fd);
    LOGI("registered %s", pathOut.c_str());
    return true;
}

// Port 30407/30408 can still be held by a previous instance that procguard is replacing.
// Each attempt uses a fresh RcfServer: after a failed start(), RCF 2.1 lets a second start()
// on the same object "succeed" without a listening socket.
template <typename Setup>
std::unique_ptr<RCF::RcfServer> startServer(const std::string& host, int port, const char* what,
                                            Setup setup) {
    for (int attempt = 1; ; ++attempt) {
        std::unique_ptr<RCF::RcfServer> server(new RCF::RcfServer(RCF::TcpEndpoint(host, port)));
        try {
            setup(*server);
            server->start();
            return server;
        } catch (const RCF::Exception& e) {
            if (attempt >= 10) {
                LOGE("%s %s:%d: start failed: %s", what, host.c_str(), port, e.getErrorString().c_str());
                return std::unique_ptr<RCF::RcfServer>();
            }
            LOGW("%s %s:%d: start failed (%s), retrying", what, host.c_str(), port,
                 e.getErrorString().c_str());
        }
        server.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// The shim cannot serve the slot. Under procguard a non-zero exit leaves the slot empty for the
// whole boot, and appconnect then waits on a service that never answers (step 3 runs 34-36: the
// unit lagged); so the reason goes to cfg.fatalFile, the registry file goes (it is ours, or a
// crashed instance's), and the exit is 0: procguard runs the launcher again, which finds the
// file and starts the stock service in our place. When another live server holds our port,
// that one serves the slot: exit 1, stay down, and leave its registry file alone.
int fatalStart(const Config& cfg, const std::string& reason, const std::string& busyHost = std::string(),
               int busyPort = 0) {
    if (busyPort > 0 && tcpReachable(busyHost == "0.0.0.0" ? "127.0.0.1" : busyHost, busyPort, 500)) {
        LOGE("start-up failed: %s; another server holds %s:%d and serves the slot, staying down", reason.c_str(),
             busyHost.c_str(), busyPort);
        return kExitFatal;
    }
    if (cfg.registryDir != "-") ::unlink((cfg.registryDir + "/" + kServiceName).c_str());
    const std::string dir = dirName(cfg.fatalFile);
    if (!dir.empty()) mkdirs(dir, 0755);
    const std::string tmp = cfg.fatalFile + ".tmp";
    const std::string text = reason + "\n";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const bool ok = fd >= 0 && ::write(fd, text.data(), text.size()) == (ssize_t)text.size();
    if (fd >= 0) ::close(fd);
    if (ok && ::rename(tmp.c_str(), cfg.fatalFile.c_str()) == 0)
        LOGE("start-up failed: %s; written to %s for the launcher (the stock service takes the slot)", reason.c_str(),
             cfg.fatalFile.c_str());
    else
        LOGE("start-up failed: %s; and writing %s failed: %s", reason.c_str(), cfg.fatalFile.c_str(), std::strerror(errno));
    return 0;
}

// ---------------------------------------------------------------------------------------
// Crashes. A fault logs the signal, where it happened (the PC and LR, to look up in
// out/aa-carlife-shim.unstripped.armhf with addr2line: the binary is not position-
// independent) and a backtrace, to syslog (or stderr), using only async-signal-safe calls;
// then the signal is raised again with its default action, so the process dies by it and
// procguard restarts it within the launcher's budget. An uncaught exception logs its text
// first (onTerminate), then aborts into the same path.
// ---------------------------------------------------------------------------------------
namespace crash {

char* put(char* p, char* end, const char* s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

char* putNum(char* p, char* end, unsigned long v, unsigned base) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v && n < (int)sizeof tmp);
    if (base == 16) p = put(p, end, "0x");
    while (n > 0 && p < end) *p++ = tmp[--n];
    return p;
}

// One line, as logsink formats it, to the syslog socket or stderr.
void emit(const char* text) {
    char buf[256];
    char* end = buf + sizeof buf - 2;
    char* p = buf;
    const int fd = logsink::gStderr ? -1 : logsink::gCrashFd.load();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (fd >= 0) {
        p = put(p, end, "<11>"); // LOG_USER | LOG_ERR
        p = put(p, end, kTag);
        p = put(p, end, "[");
        p = putNum(p, end, (unsigned long)getpid(), 10);
        p = put(p, end, "]: [");
    } else {
        p = put(p, end, "[");
        p = put(p, end, kTag);
        p = put(p, end, " ");
    }
    p = putNum(p, end, (unsigned long)ts.tv_sec, 10);
    p = put(p, end, ".");
    const unsigned long ms = (unsigned long)(ts.tv_nsec / 1000000);
    if (ms < 100) p = put(p, end, "0");
    if (ms < 10) p = put(p, end, "0");
    p = putNum(p, end, ms, 10);
    p = put(p, end, "] ERROR: ");
    p = put(p, end, text);
    if (fd >= 0) {
        ssize_t r = ::send(fd, buf, (size_t)(p - buf), MSG_DONTWAIT | MSG_NOSIGNAL);
        (void)r;
    } else {
        *p++ = '\n';
        ssize_t r = ::write(2, buf, (size_t)(p - buf));
        (void)r;
    }
}

void onCrash(int sig, siginfo_t* info, void* ctx) {
    char line[160];
    char* end = line + sizeof line - 1;
    char* p = put(line, end, "crash: signal ");
    p = putNum(p, end, (unsigned long)sig, 10);
    p = put(p, end, " (");
    p = put(p, end, sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL"
                    : sig == SIGFPE ? "SIGFPE" : sig == SIGABRT ? "SIGABRT" : "?");
    p = put(p, end, "), build " AA_VERSION);
    if (info && (sig == SIGSEGV || sig == SIGBUS)) {
        p = put(p, end, ", address ");
        p = putNum(p, end, (unsigned long)info->si_addr, 16);
    }
    *p = '\0';
    emit(line);
#if defined(__arm__)
    if (ctx) {
        const ucontext_t* uc = static_cast<const ucontext_t*>(ctx);
        p = put(line, end, "crash: pc ");
        p = putNum(p, end, (unsigned long)uc->uc_mcontext.arm_pc, 16);
        p = put(p, end, " lr ");
        p = putNum(p, end, (unsigned long)uc->uc_mcontext.arm_lr, 16);
        p = put(p, end, " sp ");
        p = putNum(p, end, (unsigned long)uc->uc_mcontext.arm_sp, 16);
        *p = '\0';
        emit(line);
    }
#else
    (void)ctx;
#endif
    void* frames[32];
    const int n = backtrace(frames, 32);
    for (int i = 0; i < n; ++i) {
        p = put(line, end, "crash: frame ");
        p = putNum(p, end, (unsigned long)i, 10);
        p = put(p, end, " ");
        p = putNum(p, end, (unsigned long)frames[i], 16);
        *p = '\0';
        emit(line);
    }
    // Die by the signal (procguard restarts after a signal, not after exit 1).
    signal(sig, SIG_DFL);
    raise(sig);
}

void onTerminate() {
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const RCF::Exception& x) {
            LOGE("uncaught RCF exception: %s", x.getErrorString().c_str());
        } catch (const std::exception& x) {
            LOGE("uncaught exception: %s", x.what());
        } catch (...) {
            LOGE("uncaught exception of an unknown type");
        }
    } else {
        LOGE("std::terminate called without an exception");
    }
    std::abort();
}

char gAltStack[16384];

void install() {
    void* warm[2];
    backtrace(warm, 2); // loads libgcc_s now, not inside a signal handler
    stack_t ss;
    std::memset(&ss, 0, sizeof ss);
    ss.ss_sp = gAltStack;
    ss.ss_size = sizeof gAltStack;
    sigaltstack(&ss, nullptr); // the main thread's own stack overflowing still gets logged
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = onCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
    std::set_terminate(onTerminate);
}

} // namespace crash

// ---------------------------------------------------------------------------------------
// --log-relay TAG: stdin to syslog, one datagram per line, ident TAG, through the same sink
// (never blocks: a full or missing syslog drops lines and counts them); lines are cut at 900
// bytes. For the receiver's and the bridge's output, which the launcher sends here through a
// FIFO instead of to the serial console. Exits 0 at the end of the input.
// ---------------------------------------------------------------------------------------
int runLogRelay(const std::string& tag) {
    std::signal(SIGPIPE, SIG_IGN);
    logsink::gStderr = false;
    logsink::gIdent = tag;
    logsink::gWithPid = false;
    std::string buf;
    char tmp[4096];
    for (;;) {
        const ssize_t n = ::read(0, tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos || buf.size() >= 900) {
            const size_t cut = pos != std::string::npos && pos < 900 ? pos : 900;
            std::string line = buf.substr(0, cut);
            buf.erase(0, cut < buf.size() && buf[cut] == '\n' ? cut + 1 : cut);
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            if (!line.empty()) logsink::line(logsink::kInfo, line.c_str());
        }
    }
    if (!buf.empty()) logsink::line(logsink::kInfo, buf.c_str());
    const unsigned lost = logsink::dropped();
    if (lost) {
        char note[96];
        std::snprintf(note, sizeof note, "log relay: %u lines dropped (syslog busy or down)", lost);
        logsink::line(logsink::kWarning, note);
    }
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "usage: aa-carlife-shim [--fake] [--verbose] [--debug-output <mode>]\n"
        "       aa-carlife-shim --log-relay TAG    stdin to syslog, ident TAG, until the end of input\n"
        "--debug-output console logs to stderr; any other mode (none, syslog, kmsg, file) to syslog\n"
        "environment:\n"
        "  AA_CL_BIND=127.0.0.1           service bind address (stock uses 0.0.0.0)\n"
        "  AA_CL_SVC_PORT=30407           ISVP_CarLifeService port\n"
        "  AA_CL_PUB_PORT=30408           ISVP_CarLifeListener publisher port\n"
        "  AA_CL_REGISTRY_DIR=/tmp/run/service/registry   '-' to skip registration\n"
        "  AA_CTL_SOCK=/tmp/run/aa-carlife.sock           '-' to disable the receiver link\n"
        "  AA_SHIM_FATAL_FILE=/tmp/run/aa/shim.fatal      a start-up failure's reason, for the launcher\n"
        "  AA_RECEIVER_COMM=aa-headless,aa-receiver       receiver process names a hung receiver is\n"
        "                                 killed by (SIGKILL after 3 unanswered pings)\n"
        "  AA_SHIM_FAKE=1                 play connecting (2) then video (3), no receiver\n"
        "  AA_SHIM_FAKE_VIDEO_MS=1500     fake mode: delay from connecting to video\n"
        "  AA_SHIM_CONNECT_TIMEOUT_MS=30000  wait for 'state projecting' after 'start'\n"
        "  AA_SHIM_RECEIVER_WAIT_MS=5000     wait for the receiver to connect\n"
        "  AA_SHIM_VERBOSE=1              log touches, keys, getters and the like (debug level)\n"
        "  AA_SHIM_SYSLOG_SOCK=/dev/log   the syslog socket (tests)\n"
        "  AA_ILM_HOST=127.0.0.1 AA_ILM_PORT=30005   SVP iLM service for Show(n); port 0 disables\n"
        "  AA_BRIDGE_COMM=androidautobrid,aa-bridge  video bridge process names (its ILM surface\n"
        "                                 is 20000000 + its pid)\n"
        "  AA_VIDEO_SOURCE=32,24,800,480  part of the bridge's surface to show ('-': leave it)\n"
        "  AA_VIDEO_DEST=0,0,800,480      where on the screen ('-': leave it)\n"
        "  AA_BT_HOST=127.0.0.1 AA_BT_PORT=30315  SVP Bluetooth service: the car mic's route\n"
        "                                 while the phone has the mic, and the unit's Bluetooth address\n"
        "                                 for the receiver (receiver mode only); port 0 disables\n"
        "  AA_BT_PAIRING=1                Android Auto's Bluetooth pairing loop with the car\n"
        "                                 (receiver btpair / btpaired / btauth); 0: every phone is\n"
        "                                 answered \"already paired\"\n"
        "  AA_BT_LISTEN_PORT=30316        btservice's event publisher (pairing loop, music link)\n"
        "  AA_BT_A2DP_DROP=1              Bluetooth music (A2DP) off during the session, back after\n"
        "  AA_CALL_FIX=1 AA_CALL_FIX_MS=400  keep an outgoing call on Android Auto's screen\n"
        "  AA_BT_LOCK=1                   while Android Auto's phone holds the car's phone link, no\n"
        "                                 other device may pair or connect (capability 10)\n"
        "  AA_BT_LOCK_FILE=/tmp/run/aa/btlock  marks that lock as ours: a restart takes it off\n"
        "  AA_ONTOP_FILE=/tmp/run/aa/ontop  \"<pid> <0|1>\": Android Auto on screen; the Bluetooth\n"
        "                                 app shows no pop-ups while it is 1; empty disables\n"
        "  AA_APPC_PATCH=1                appconnect's code changes in memory: outgoing calls stay on\n"
        "                                 Android Auto; an iPhone goes straight to CarPlay\n"
        "  AA_CAN_HOST=127.0.0.1 AA_CAN_PORT=30705  SVP CAN service: the car's day/night, gear and\n"
        "                                 driver's side for Android Auto (receiver mode only); port 0\n"
        "                                 disables all CAN data\n"
        "  AA_CAN_LISTEN_PORT=30706       the CAN service's publisher: the vehicle speed; 0 disables\n"
        "  AA_AUDIO_HOST=127.0.0.1 AA_AUDIO_PORT=30203  audiomgr: RegisterAudio for the bridge's\n"
        "                                 prompt streams (receiver mode only); port 0 disables\n"
        "  AA_SHIM_NOTIFY=/tmp/run/aa/notify.sock  datagram socket the bridge reports opened streams to\n"
        "  AA_SOURCE_HOST=127.0.0.1 AA_SOURCE_PORT=30205  SVP source service for the CarLife\n"
        "                                 audio sources (receiver mode only); port 0 disables\n"
        "  AA_SOURCE_LISTEN_PORT=30206    its status topic: when an interruption ends; 0 disables\n"
        "  AA_POWER_HOST=127.0.0.1 AA_POWER_PORT=30201  SVP power service: is a switch-on temporary\n"
        "  AA_POWER_LISTEN_PORT=30202     its publisher: the unit switched off / on (pause and resume\n"
        "                                 AA's media as stock CarLife does); 0 disables\n"
        "  AA_LAST_MODE_FILE=             last mode: whether AA had the speakers at the last\n"
        "                                 switch-off; if not, the phone's autoplay is refused once\n"
        "                                 at the next connection; empty disables\n"
        "  AA_BOOTS_FILE=                 the launcher's boot-loop counter, set to 0 at a switch-off\n");
}

} // namespace

int main(int argc, char** argv) {
    // The log relay is a mode of its own: nothing below runs for it.
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--log-relay")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                usage();
                return 2;
            }
            logsink::gPath = envOr("AA_SHIM_SYSLOG_SOCK", "/dev/log");
            return runLogRelay(argv[i + 1]);
        }
    // Where the log goes, before anything is logged: stderr only for --debug-output console
    // (and when started by hand without --debug-output); the unit's procguard passes "none".
    {
        const char* mode = nullptr;
        for (int i = 1; i + 1 < argc; ++i)
            if (!std::strcmp(argv[i], "--debug-output")) mode = argv[i + 1];
        logsink::gStderr = !mode || !std::strcmp(mode, "console");
        logsink::gPath = envOr("AA_SHIM_SYSLOG_SOCK", "/dev/log");
    }
    gVerbose = envInt("AA_SHIM_VERBOSE", 0, 0, 1) != 0;
    crash::install();

    Config cfg;
    cfg.bindHost = envOr("AA_CL_BIND", "127.0.0.1");
    cfg.svcPort = envInt("AA_CL_SVC_PORT", kCarLifeServicePort, 1, kMaxPort);
    cfg.pubPort = envInt("AA_CL_PUB_PORT", kCarLifePublisherPort, 1, kMaxPort);
    cfg.registryDir = envOr("AA_CL_REGISTRY_DIR", "/tmp/run/service/registry");
    cfg.ctlSock = envOr("AA_CTL_SOCK", "/tmp/run/aa-carlife.sock");
    cfg.fake = envInt("AA_SHIM_FAKE", 0, 0, 1) != 0;
    cfg.fakeVideoMs = envInt("AA_SHIM_FAKE_VIDEO_MS", 1500, 0, 60000);
    // 30 s: what every car run used (start.sh); a first session that dies after the phone's
    // accessory switch and the second one that works take about 4 s together (step 3 run 47).
    cfg.connectTimeoutMs = envInt("AA_SHIM_CONNECT_TIMEOUT_MS", 30000, 1000, 120000);
    cfg.receiverWaitMs = envInt("AA_SHIM_RECEIVER_WAIT_MS", 5000, 100, 60000);
    cfg.ilmHost = envOr("AA_ILM_HOST", "127.0.0.1");
    cfg.ilmPort = envInt("AA_ILM_PORT", kILMServicePort, 0, kMaxPort);
    // /proc/<pid>/comm keeps 15 characters: "androidautobridge" reads "androidautobrid".
    cfg.bridgeComm = envOr("AA_BRIDGE_COMM", "androidautobrid,aa-bridge");
    // Measured on the unit with waylandsink2 (896x576 surface, picture at 32,24).
    cfg.videoSource = envOr("AA_VIDEO_SOURCE", "32,24,800,480");
    cfg.videoDest = envOr("AA_VIDEO_DEST", "0,0,800,480");
    cfg.sourceHost = envOr("AA_SOURCE_HOST", "127.0.0.1");
    cfg.sourcePort = envInt("AA_SOURCE_PORT", kSourceServicePort, 0, kMaxPort);
    cfg.sourceListenPort = envInt("AA_SOURCE_LISTEN_PORT", kSourceListenerPort, 0, kMaxPort);
    cfg.canHost = envOr("AA_CAN_HOST", "127.0.0.1");
    cfg.canPort = envInt("AA_CAN_PORT", kCanServicePort, 0, kMaxPort);
    cfg.canListenPort = envInt("AA_CAN_LISTEN_PORT", kCanListenerPublisherPort, 0, kMaxPort);
    cfg.btHost = envOr("AA_BT_HOST", "127.0.0.1");
    cfg.btPort = envInt("AA_BT_PORT", kBtServicePort, 0, kMaxPort);
    cfg.btListenPort = envInt("AA_BT_LISTEN_PORT", kBtListenerPort, 0, kMaxPort);
    cfg.btPairing = envInt("AA_BT_PAIRING", 1, 0, 1) != 0;
    cfg.btA2dpDrop = envInt("AA_BT_A2DP_DROP", 1, 0, 1) != 0;
    cfg.btLock = envInt("AA_BT_LOCK", 1, 0, 1) != 0;
    cfg.btLockFile = envOr("AA_BT_LOCK_FILE", "/tmp/run/aa/btlock");
    // The path is fixed in the Bluetooth app's side (i18n/lib/bluetooth.cpp); this is for tests.
    cfg.onTopFile = envOr("AA_ONTOP_FILE", "/tmp/run/aa/ontop");
    cfg.callFix = envInt("AA_CALL_FIX", 1, 0, 1) != 0;
    cfg.appcPatch = envInt("AA_APPC_PATCH", 1, 0, 1) != 0;
    cfg.callFixMs = envInt("AA_CALL_FIX_MS", 400, 0, 10000);
    cfg.audioHost = envOr("AA_AUDIO_HOST", "127.0.0.1");
    cfg.audioPort = envInt("AA_AUDIO_PORT", kAudioServicePort, 0, kMaxPort);
    cfg.notifySock = envOr("AA_SHIM_NOTIFY", "/tmp/run/aa/notify.sock");
    cfg.powerHost = envOr("AA_POWER_HOST", "127.0.0.1");
    cfg.powerPort = envInt("AA_POWER_PORT", kPowerServicePort, 0, kMaxPort);
    cfg.powerListenPort = envInt("AA_POWER_LISTEN_PORT", kPowerListenerPort, 0, kMaxPort);
    cfg.lastModeFile = envOr("AA_LAST_MODE_FILE", "");
    cfg.fatalFile = envOr("AA_SHIM_FATAL_FILE", "/tmp/run/aa/shim.fatal");
    // /proc/<pid>/comm of the receiver: its build's name, or its name on the unit (start.sh and
    // the system image run it as aa-receiver).
    cfg.receiverComm = envOr("AA_RECEIVER_COMM", "aa-headless,aa-receiver");
    cfg.bootsFile = envOr("AA_BOOTS_FILE", "");

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--debug-output")) {
            if (i + 1 < argc) ++i;
        } else if (!std::strcmp(argv[i], "--fake")) {
            cfg.fake = true;
        } else if (!std::strcmp(argv[i], "--verbose")) {
            gVerbose = true;
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            LOGW("ignoring unknown argument %s", argv[i]);
        }
    }
    // Fake mode has no sound: leave the radio its source.
    if (cfg.fake) cfg.appcPatch = false;
    if (cfg.fake) cfg.sourcePort = cfg.canPort = cfg.canListenPort = cfg.btPort = cfg.audioPort = 0;
    if (cfg.canPort <= 0) cfg.canListenPort = 0;
    if (cfg.fake || cfg.sourcePort <= 0) cfg.sourceListenPort = 0;
    if (cfg.fake || cfg.btPort <= 0) cfg.btListenPort = 0;
    if (cfg.fake) cfg.powerPort = cfg.powerListenPort = 0;
    if (cfg.fake) cfg.lastModeFile.clear();
    if (cfg.fake) cfg.notifySock = "-"; // no bridge to hear from

    // Signals go to sigwait() below; every thread created after this inherits the mask.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    std::string mode = cfg.fake ? "FAKE mode (no receiver), video after " +
                                      std::to_string(cfg.fakeVideoMs) + " ms"
                                : "receiver via " + cfg.ctlSock;
    LOGI("starting: build %s, service %s:%d, publisher 127.0.0.1:%d, %s, log to %s%s", AA_VERSION, cfg.bindHost.c_str(),
         cfg.svcPort, cfg.pubPort, mode.c_str(), logsink::gStderr ? "stderr" : logsink::gPath.c_str(),
         gVerbose ? " (verbose)" : "");
    LOGI("config: connect timeout %d ms, receiver wait %d ms, ILM %d, source %d/%d, CAN %d/%d, BT %d/%d "
         "(pairing %d, A2DP drop %d), audio %d, power %d/%d, call fix %d (%d ms), appconnect patch %d, "
         "video %s -> %s, bridge %s, notify %s, last mode %s, fatal file %s",
         cfg.connectTimeoutMs, cfg.receiverWaitMs, cfg.ilmPort, cfg.sourcePort, cfg.sourceListenPort, cfg.canPort,
         cfg.canListenPort, cfg.btPort, cfg.btListenPort, cfg.btPairing ? 1 : 0, cfg.btA2dpDrop ? 1 : 0, cfg.audioPort,
         cfg.powerPort, cfg.powerListenPort, cfg.callFix ? 1 : 0, cfg.callFixMs, cfg.appcPatch ? 1 : 0,
         cfg.videoSource.c_str(), cfg.videoDest.c_str(), cfg.bridgeComm.c_str(), cfg.notifySock.c_str(),
         cfg.lastModeFile.empty() ? "off" : cfg.lastModeFile.c_str(), cfg.fatalFile.c_str());

    RCF::RcfInitDeinit rcfInit;
    ControlChannel ctl;
    CarLifeShim shim(cfg, ctl);

    // Runs before shim is destroyed on every path, so no receiver callback outlives it.
    struct Teardown {
        ControlChannel& ctl;
        ~Teardown() { ctl.close(); }
    } teardown = {ctl};

    if (!cfg.fake && cfg.ctlSock != "-") {
        const std::string dir = dirName(cfg.ctlSock);
        if (!dir.empty() && !mkdirs(dir, 0755)) return fatalStart(cfg, "cannot create " + dir + ": " + std::strerror(errno));
        if (!ctl.open(cfg.ctlSock, &CarLifeShim::onReceiverLine, &shim, cfg.receiverComm))
            return fatalStart(cfg, "cannot listen on the control socket " + cfg.ctlSock);
    }

    std::string registryPath;
    bool ready = false;
    try {
        // A client that subscribes while a session runs hears it announced (subscriberJoined).
        RCF::PublisherParms pubParms;
        pubParms.setOnSubscriberConnect([&shim](RCF::RcfSession&, const std::string&) {
            shim.subscriberJoined();
            return true;
        });
        std::unique_ptr<RCF::RcfServer> pubServer =
            startServer("127.0.0.1", cfg.pubPort, "publisher", [](RCF::RcfServer&) {});
        if (!pubServer) return fatalStart(cfg, "publisher port " + std::to_string(cfg.pubPort), "127.0.0.1", cfg.pubPort);
        shim.setPublisher(pubServer->createPublisher<ISVP_CarLifeListener>(pubParms));
        // The publisher refers to pubServer's publishing service: drop it before pubServer
        // goes out of scope (locals are destroyed in reverse order).
        struct PublisherReset {
            CarLifeShim& shim;
            ~PublisherReset() { shim.setPublisher(boost::shared_ptr<CarLifeShim::ListenerPublisher>()); }
        } publisherReset = {shim};

        std::unique_ptr<RCF::RcfServer> svcServer =
            startServer(cfg.bindHost, cfg.svcPort, "service", [&shim](RCF::RcfServer& s) {
                s.setThreadPool(RCF::ThreadPoolPtr(new RCF::ThreadPool(1, 4)));
                s.bind<ISVP_CarLifeService>(shim);
            });
        if (!svcServer) return fatalStart(cfg, "service port " + std::to_string(cfg.svcPort), cfg.bindHost, cfg.svcPort);

        if (!registerService(cfg.registryDir, registryPath))
            return fatalStart(cfg, "registry file in " + cfg.registryDir + ": " + std::strerror(errno));
        ready = true;
        LOGI("ready");

        int sig = 0;
        sigwait(&sigs, &sig);
        LOGI("signal %d, shutting down", sig);

        if (!registryPath.empty()) ::unlink(registryPath.c_str());
        svcServer->stop();
        // A running session ends while the publisher can still tell appconnect (bounded).
        shim.endSession(3000);
    } catch (const RCF::Exception& e) {
        if (!registryPath.empty()) ::unlink(registryPath.c_str());
        if (!ready) return fatalStart(cfg, "RCF: " + e.getErrorString());
        LOGE("RCF: %s", e.getErrorString().c_str());
    } catch (const std::exception& e) {
        if (!registryPath.empty()) ::unlink(registryPath.c_str());
        if (!ready) return fatalStart(cfg, std::string("start-up: ") + e.what());
        LOGE("%s", e.what());
    }
    LOGI("stopped");
    return 0;
}
