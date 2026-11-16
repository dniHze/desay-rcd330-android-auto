// SPDX-License-Identifier: GPL-2.0-only
//
// Test of src/appc_patch.hpp on a native Linux host (not under qemu-user, whose guest memory is
// not at its guest addresses in /proc/<pid>/mem):
//   docker run --rm -v "$PWD":/w rust:bookworm sh -c 'g++ -std=c++11 -Wall -O2 -o /tmp/t
//       /w/aa-carlife-shim/tools/appc_patch_test.cpp && /tmp/t'   (one line, from the work dir)
//
// A child process named "appconnect" maps a file holding one build's instruction words at their
// addresses (0x2e000..0x4a000, read + exec, private, as the loader maps appconnect's text), and
// the test patches it as the shim does, for the 0358, 0394 and 0317 builds in turn: stock code gets every change and keeps it, a second run
// has nothing to do, a change whose code differs in one word is left alone while the other is
// still made, a change of several runs (callkey) gets every run, code already patched (the
// rebranded appconnect from kzb-re/patch_appconnect.py) is
// taken as is, no process is reported, and the file behind the mapping is never written; with a
// record (appc_patch::Seen) a process found with every change on is reported once and skipped.
// An appconnect of neither build is left alone.
#include "../src/appc_patch.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>

namespace {

using appc_patch::kPatches;
using appc_patch::kNumPatches;

const unsigned long kBase = 0x2e000, kSize = 0x1c000; // every build's code words
const char* const kBuilds[] = {"0358", "0394", "0317"};
const size_t kNumBuilds = sizeof kBuilds / sizeof kBuilds[0];
int fails = 0;

void check(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

void put(std::vector<char>& img, unsigned long addr, uint32_t w) { std::memcpy(&img[addr - kBase], &w, 4); }

// The index in kPatches of change `name`'s variant for `build`.
size_t idx(const char* name, const char* build) {
    for (size_t p = 0; p < kNumPatches; ++p)
        if (!std::strcmp(kPatches[p].name, name) && !std::strcmp(kPatches[p].build, build)) return p;
    std::printf("FAIL: no %s variant for %s\n", name, build);
    ++fails;
    return 0;
}

// One build's appconnect words; `patched` gives every edit its new word; `spoil` changes one guard
// word of that patch (by index into kPatches; -1 none).
std::vector<char> image(const char* build, bool patched, int spoil = -1) {
    std::vector<char> img(kSize, 0);
    for (size_t p = 0; p < kNumPatches; ++p) {
        if (std::strcmp(kPatches[p].build, build) != 0) continue;
        for (size_t i = 0; i < kPatches[p].nGuards; ++i) put(img, kPatches[p].guards[i].addr, kPatches[p].guards[i].word);
    }
    for (size_t p = 0; p < kNumPatches; ++p) {
        if (std::strcmp(kPatches[p].build, build) != 0) continue;
        for (size_t i = 0; i < kPatches[p].nEdits; ++i)
            put(img, kPatches[p].edits[i].addr, patched ? kPatches[p].edits[i].patched : kPatches[p].edits[i].stock);
    }
    if (spoil >= 0) put(img, kPatches[spoil].guards[0].addr, kPatches[spoil].guards[0].word ^ 1);
    return img;
}

// A child named "appconnect" with `img` mapped at kBase from file `path`; its pid.
int spawn(const std::string& path, const std::vector<char>& img) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    int ready[2];
    if (pipe(ready) != 0) return -1;
    const int pid = fork();
    if (pid == 0) {
        prctl(PR_SET_NAME, "appconnect", 0, 0, 0);
        const int fd = ::open(path.c_str(), O_RDONLY);
        void* p = mmap(reinterpret_cast<void*>(kBase), kSize, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
        const char ok = p == reinterpret_cast<void*>(kBase) ? 1 : 0;
        if (write(ready[1], &ok, 1) != 1) _exit(1);
        for (;;) pause();
    }
    char ok = 0;
    if (read(ready[0], &ok, 1) != 1 || !ok) {
        std::printf("FAIL: child could not map 0x%lx\n", kBase);
        ++fails;
    }
    close(ready[0]);
    close(ready[1]);
    return pid;
}

void reap(int pid) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

uint32_t wordIn(int pid, unsigned long addr) {
    uint32_t w = 0;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/mem", pid);
    const int fd = ::open(path, O_RDONLY);
    if (::pread(fd, &w, sizeof w, (off_t)addr) != (ssize_t)sizeof w) w = 0;
    ::close(fd);
    return w;
}

uint32_t wordInFile(const std::string& path, unsigned long addr) {
    uint32_t w = 0;
    FILE* f = std::fopen(path.c_str(), "rb");
    std::fseek(f, (long)(addr - kBase), SEEK_SET);
    if (std::fread(&w, sizeof w, 1, f) != 1) w = 0;
    std::fclose(f);
    return w;
}

// Every edit of patch p reads `patched` (or stock) words in the process.
bool edits(int pid, size_t p, bool patched) {
    for (size_t i = 0; i < kPatches[p].nEdits; ++i) {
        const appc_patch::Edit& e = kPatches[p].edits[i];
        if (wordIn(pid, e.addr) != (patched ? e.patched : e.stock)) return false;
    }
    return true;
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

} // namespace

// Every change in one build's appconnect: made, kept, left alone when a guard differs, taken
// as is when the file has them, left alone when half done.
void runBuild(const std::string& file, const char* b) {
    std::string msg;
    char what[160];
    const size_t cf = idx("callfix", b), ip = idx("iphone", b), t1 = idx("iphonetile", b), t2 = idx("iphonetile2", b);
    const size_t sh = idx("seekhold", b), ck = idx("callkey", b);
#define CHECK(text, cond) (std::snprintf(what, sizeof what, "%s: %s", b, text), check(what, cond))
    int pid = spawn(file, image(b, false));
    bool ok = appc_patch::apply(msg);
    std::printf("      %s\n", msg.c_str());
    CHECK("stock code: every change made", ok && has(msg, "callfix: pid") && has(msg, "iphone: pid") &&
                                               has(msg, "straight to CarPlay") && has(msg, "iphonetile: pid") &&
                                               has(msg, "iphonetile2: pid") && has(msg, "seekhold: pid") &&
                                               has(msg, "callkey: pid"));
    CHECK("their words are the patched ones, every run of each",
          edits(pid, cf, true) && edits(pid, ip, true) && edits(pid, t1, true) && edits(pid, t2, true) &&
              edits(pid, sh, true) && edits(pid, ck, true));
    CHECK("the guard words are unchanged",
          wordIn(pid, kPatches[cf].guards[10].addr) == kPatches[cf].guards[10].word &&
              wordIn(pid, kPatches[ip].guards[7].addr) == kPatches[ip].guards[7].word);
    CHECK("the file behind the mapping is not written",
          wordInFile(file, kPatches[cf].edits[0].addr) == kPatches[cf].edits[0].stock &&
              wordInFile(file, kPatches[ip].edits[3].addr) == kPatches[ip].edits[3].stock);
    ok = appc_patch::apply(msg);
    CHECK("second run: already on, nothing to say", ok && msg.empty());
    reap(pid);

    pid = spawn(file, image(b, false, (int)ip));
    ok = appc_patch::apply(msg);
    std::printf("      %s\n", msg.c_str());
    CHECK("one iphone guard word differs: iphone left alone, callfix still made",
          !ok && has(msg, "iphone: pid") && has(msg, "not a known appconnect build") && edits(pid, ip, false) &&
              edits(pid, cf, true));
    reap(pid);

    pid = spawn(file, image(b, true));
    ok = appc_patch::apply(msg);
    CHECK("already patched in the file (rebranded appconnect): taken as is",
          ok && msg.empty() && edits(pid, cf, true) && edits(pid, ip, true) && edits(pid, t1, true) &&
              edits(pid, t2, true) && edits(pid, sh, true) && edits(pid, ck, true));
    reap(pid);

    std::vector<char> mixed = image(b, false);
    put(mixed, kPatches[ip].edits[0].addr, kPatches[ip].edits[0].patched); // half-patched
    pid = spawn(file, mixed);
    ok = appc_patch::apply(msg);
    std::printf("      %s\n", msg.c_str());
    CHECK("half-patched code: left alone", !ok && has(msg, "neither stock nor patched") &&
                                           wordIn(pid, kPatches[ip].edits[1].addr) == kPatches[ip].edits[1].stock);
    reap(pid);

    // callkey's last run (the branch into its new code) written, the rest stock.
    mixed = image(b, false);
    const appc_patch::Edit& last = kPatches[ck].edits[kPatches[ck].nEdits - 1];
    put(mixed, last.addr, last.patched);
    pid = spawn(file, mixed);
    ok = appc_patch::apply(msg);
    std::printf("      %s\n", msg.c_str());
    CHECK("callkey with one run written: left alone, the others still made",
          !ok && has(msg, "callkey: pid") && has(msg, "neither stock nor patched") &&
              wordIn(pid, kPatches[ck].edits[0].addr) == kPatches[ck].edits[0].stock && edits(pid, sh, true));
    reap(pid);
#undef CHECK
}

int main() {
    const std::string file = "/tmp/appc_patch_test.text";
    std::string msg;
    bool names = kNumPatches == 6 * kNumBuilds;
    for (size_t p = 0; p < kNumPatches; ++p)
        names = names && !std::strcmp(kPatches[p].build, kBuilds[p % kNumBuilds]) &&
                !std::strcmp(kPatches[p].name, kPatches[p - p % kNumBuilds].name);
    check("six changes, each for the 0358, 0394 and 0317 appconnect", names);

    check("no appconnect process: reported, not patched",
          !appc_patch::apply(msg) && msg == "no appconnect process");

    for (const char* b : kBuilds) runBuild(file, b);

    int pid = spawn(file, std::vector<char>(kSize, 0));
    bool ok = appc_patch::apply(msg);
    std::printf("      %s\n", msg.c_str());
    check("an appconnect of neither build: every change left alone", !ok && has(msg, "callfix: pid") &&
                                                                       has(msg, "not a known appconnect build"));
    reap(pid);

    // With a record of the processes found patched (the shim's AppcWatch): a process is said
    // once, then not opened again while it lives, and forgotten when it is gone.
    appc_patch::Seen seen;
    pid = spawn(file, image("0358", true));
    ok = appc_patch::apply(msg, "appconnect", &seen);
    check("with a record: the patched file's process is said to have every change, once",
          ok && has(msg, "every change is in place already") && seen.on.count(pid) == 1 && seen.found == 1);
    ok = appc_patch::apply(msg, "appconnect", &seen);
    check("... then skipped (nothing said), still counted", ok && msg.empty() && seen.found == 1);
    reap(pid);
    ok = appc_patch::apply(msg, "appconnect", &seen);
    check("... and forgotten once it is gone", !ok && msg == "no appconnect process" && seen.on.empty());
    pid = spawn(file, image("0394", false));
    ok = appc_patch::apply(msg, "appconnect", &seen);
    check("with a record, stock code: every change made and the process recorded",
          ok && has(msg, "callfix: pid") && edits(pid, idx("iphone", "0394"), true) && seen.on.count(pid) == 1);
    check("its start time is read", appc_patch::startTime(pid) != 0 && appc_patch::startTime(pid) == seen.on[pid]);
    reap(pid);

    std::remove(file.c_str());
    std::printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
