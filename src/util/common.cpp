#include "common.h"
#include <comdef.h>
#include <vector>
#include <psapi.h>
#include <cstring>
#include <memory>

namespace camss {

// ============================================================== logging
//
// Producers (any thread) format into a stack buffer and append one variable-size
// record to a 1 MB byte ring under an SRW lock (critical section = bounds check +
// memcpy). The writer thread snapshots [tail, head) under the lock, formats the
// records into its own buffers *outside* the lock, releases the ring space and
// then performs the (possibly slow) console / file I/O. A single lock keeps the
// ring FIFO, so messages from one thread stay in order.

namespace {

constexpr uint32_t kRingBytes = 1u << 20;           // power of two
constexpr size_t kStackMsg = 1024;                  // typical message fits here
constexpr size_t kMaxMsg = 60000;                   // hard cap (record len is uint16)
constexpr uint8_t kPadLevel = 0xFF;
constexpr DWORD kWriterPeriodMs = 10;               // writer wake-up period
constexpr double kFileFlushMs = 250.0;

struct RecHdr {
    uint32_t size;     // total record size incl. header, multiple of 8
    uint16_t len;      // message bytes
    uint8_t level;     // LogLevel or kPadLevel
    uint8_t reserved;
    double t;          // seconds since log epoch
};
static_assert(sizeof(RecHdr) == 16, "record header layout");

enum WriterState : int { kNotStarted = 0, kStarting = 1, kRunning = 2, kStopped = 3 };

alignas(64) char g_ring[kRingBytes];                // zero-initialised, usable before main
SRWLOCK g_ringLock = SRWLOCK_INIT;
uint64_t g_head = 0, g_tail = 0;                    // byte positions (monotonic), under g_ringLock
uint64_t g_enqueued = 0;                            // under g_ringLock
uint64_t g_droppedPending = 0, g_droppedTotal = 0;  // under g_ringLock
bool g_closed = false;                              // under g_ringLock: async path closed -> write synchronously
bool g_wakeSent = false;                            // under g_ringLock: writer already signalled for this batch

std::atomic<int> g_state{kNotStarted};
std::atomic<HANDLE> g_wakeEvent{nullptr};
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_consoleOn{true};
std::atomic<uint64_t> g_written{0}, g_batches{0};
std::atomic<uint64_t> g_flushReqGen{0};
SRWLOCK g_lifeLock = SRWLOCK_INIT;
// Held exclusively from closing the async path until the ring's last records are written.
// A producer that finds the path closed waits on it (shared) before writing synchronously,
// so its earlier queued messages are never overtaken by its own later synchronous ones.
SRWLOCK g_finalLock = SRWLOCK_INIT;

// Flush completion (LogFlush waits on this).
SRWLOCK g_doneLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_doneCv = CONDITION_VARIABLE_INIT;
uint64_t g_flushDoneGen = 0;                        // under g_doneLock

// Output sinks.
SRWLOCK g_fileLock = SRWLOCK_INIT;
FILE* g_logFile = nullptr;                          // under g_fileLock
bool g_fileDirty = false;                           // under g_fileLock
int64_t g_lastFileFlush = 0;                        // under g_fileLock

SRWLOCK g_conLock = SRWLOCK_INIT;
struct ConsoleInfo {
    bool init = false;
    HANDLE h[2] = {nullptr, nullptr};
    bool isCon[2] = {false, false};
    bool vt[2] = {false, false};
    bool same = false;                              // stdout and stderr are both the (one) process console
};
ConsoleInfo g_con;                                  // under g_conLock

const char kTags[] = {'D', 'I', 'W', 'E'};

int64_t LogEpoch() { static const int64_t t0 = Qpc::Now(); return t0; }
const int64_t g_epochInit = LogEpoch();             // epoch = static-init time (or first log, if earlier)

void InitConsoleLocked() {
    if (g_con.init) return;
    g_con.init = true;
    const DWORD ids[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 2; i++) {
        HANDLE h = GetStdHandle(ids[i]);
        g_con.h[i] = h;
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            g_con.isCon[i] = true;
            g_con.vt[i] = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
                          SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    g_con.same = g_con.isCon[0] && g_con.isCon[1];
}

// Output staging buffers (one set owned by the writer thread, temporary ones for the sync path).
struct OutBufs {
    struct Seg { int stream; size_t b, e; };
    std::string con;
    std::vector<Seg> segs;
    std::string file;
    std::wstring wide;
    bool urgent = false;   // contains Warn/Error -> flush the file now
    void Reset() { con.clear(); segs.clear(); file.clear(); urgent = false; }
};

// "[%9.3f][X] " without printf (the writer's per-line cost is dominated by this otherwise).
int FormatPrefix(char* out, size_t cap, double t, int level) {
    const char tag = kTags[level & 3];
    if (!(t >= 0.0 && t < 1e12)) {
        int n = snprintf(out, cap, "[%9.3f][%c] ", t, tag);
        return n < 0 ? 0 : n;
    }
    unsigned long long ms = (unsigned long long)(t * 1000.0 + 0.5);
    char rev[32];
    int k = 0;
    for (int i = 0; i < 3; i++) { rev[k++] = (char)('0' + ms % 10); ms /= 10; }
    rev[k++] = '.';
    do { rev[k++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    int n = 0;
    out[n++] = '[';
    for (int pad = 9 - k; pad > 0; pad--) out[n++] = ' ';
    while (k) out[n++] = rev[--k];
    out[n++] = ']'; out[n++] = '['; out[n++] = tag; out[n++] = ']'; out[n++] = ' ';
    return n;
}

void AppendRecord(OutBufs& o, int level, double t, const char* msg, size_t len, bool toConsole, bool toFile) {
    char pre[64];
    const int pn = FormatPrefix(pre, sizeof(pre), t, level);
    if (level >= (int)LogLevel::Warn) o.urgent = true;
    if (toFile) {
        o.file.append(pre, (size_t)pn);
        o.file.append(msg, len);
        o.file.push_back('\n');
    }
    if (toConsole) {
        const int stream = (g_con.same || level < (int)LogLevel::Warn) ? 0 : 1;
        const bool color = g_con.vt[stream] && level >= (int)LogLevel::Warn;
        const size_t b = o.con.size();
        if (color) o.con.append(level >= (int)LogLevel::Error ? "\x1b[91m" : "\x1b[93m");
        o.con.append(pre, (size_t)pn);
        o.con.append(msg, len);
        if (color) o.con.append("\x1b[0m");
        o.con.push_back('\n');
        if (!o.segs.empty() && o.segs.back().stream == stream) o.segs.back().e = o.con.size();
        else o.segs.push_back({stream, b, o.con.size()});
    }
}

// Writes staged console text: one write per run of same-stream records (normally one per batch).
void WriteConsoleLocked(OutBufs& o) {
    for (const auto& s : o.segs) {
        const char* p = o.con.data() + s.b;
        const size_t n = s.e - s.b;
        if (g_con.isCon[s.stream]) {
            int wn = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
            if (wn <= 0) continue;
            o.wide.resize((size_t)wn);
            MultiByteToWideChar(CP_UTF8, 0, p, (int)n, o.wide.data(), wn);
            const wchar_t* w = o.wide.data();
            size_t left = (size_t)wn;
            while (left > 0) {
                DWORD chunk = (DWORD)std::min<size_t>(left, 16384), done = 0;
                if (chunk < left && IS_HIGH_SURROGATE(w[chunk - 1])) chunk--;   // never split a surrogate pair
                if (!WriteConsoleW(g_con.h[s.stream], w, chunk, &done, nullptr) || done == 0) break;
                w += done; left -= done;
            }
        } else {
            FILE* f = s.stream ? stderr : stdout;
            fwrite(p, 1, n, f);
            fflush(f);
        }
    }
}

void WriteFileLocked(OutBufs& o, bool forceFlush) {
    if (!g_logFile) return;
    if (!o.file.empty()) { fwrite(o.file.data(), 1, o.file.size(), g_logFile); g_fileDirty = true; }
    const int64_t now = Qpc::Now();
    if (g_fileDirty && (forceFlush || o.urgent || Qpc::ToMs(now - g_lastFileFlush) >= kFileFlushMs)) {
        fflush(g_logFile);
        g_fileDirty = false;
        g_lastFileFlush = now;
    }
}

void Emit(OutBufs& o, bool forceFlush) {
    if (!o.segs.empty()) {
        AcquireSRWLockExclusive(&g_conLock);
        WriteConsoleLocked(o);
        ReleaseSRWLockExclusive(&g_conLock);
    }
    AcquireSRWLockExclusive(&g_fileLock);
    WriteFileLocked(o, forceFlush);
    ReleaseSRWLockExclusive(&g_fileLock);
}

bool ConsoleReady() {
    // Console detection is done once, lazily, by whoever writes first.
    AcquireSRWLockShared(&g_conLock);
    bool init = g_con.init;
    ReleaseSRWLockShared(&g_conLock);
    if (!init) {
        AcquireSRWLockExclusive(&g_conLock);
        InitConsoleLocked();
        ReleaseSRWLockExclusive(&g_conLock);
    }
    return true;
}

void WriteSync(int level, double t, const char* msg, size_t len) {
    ConsoleReady();
    OutBufs o;
    AcquireSRWLockShared(&g_fileLock);
    const bool toFile = g_logFile != nullptr;
    ReleaseSRWLockShared(&g_fileLock);
    AppendRecord(o, level, t, msg, len, g_consoleOn.load(std::memory_order_relaxed), toFile);
    Emit(o, true);
}

// Waits (bounded) until the writer has finished writing the ring after closing it. Only
// reached by messages logged during/after shutdown, never on the normal async path.
void WaitFinalDrain() {
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (!TryAcquireSRWLockShared(&g_finalLock)) {
        if (GetTickCount64() >= deadline) return;   // writer stuck (e.g. console in QuickEdit): don't hang
        Sleep(1);
    }
    ReleaseSRWLockShared(&g_finalLock);
}

enum class EnqResult { Ok, Dropped, Closed };

EnqResult Enqueue(int level, double t, const char* msg, size_t len) {
    const uint32_t need = (uint32_t)((sizeof(RecHdr) + len + 7) & ~(size_t)7);
    bool wake = false;
    EnqResult r = EnqResult::Ok;
    AcquireSRWLockExclusive(&g_ringLock);
    if (g_closed) {
        r = EnqResult::Closed;
    } else {
        const uint64_t used = g_head - g_tail;
        uint32_t off = (uint32_t)(g_head & (kRingBytes - 1));
        const uint32_t contig = kRingBytes - off;
        const uint32_t pad = contig < need ? contig : 0;
        if (used + pad + need > kRingBytes) {
            g_droppedPending++;
            g_droppedTotal++;
            r = EnqResult::Dropped;
            if (!g_wakeSent) { g_wakeSent = true; wake = true; }
        } else {
            if (pad) {
                RecHdr* p = reinterpret_cast<RecHdr*>(g_ring + off);   // >= 8 bytes available: size/len/level fit
                p->size = pad; p->len = 0; p->level = kPadLevel;
                g_head += pad;
                off = 0;
            }
            RecHdr* h = reinterpret_cast<RecHdr*>(g_ring + off);
            h->size = need; h->len = (uint16_t)len; h->level = (uint8_t)level; h->reserved = 0; h->t = t;
            memcpy(h + 1, msg, len);
            g_head += need;
            g_enqueued++;
            if (!g_wakeSent && (level >= (int)LogLevel::Warn || used + pad + need > kRingBytes / 8)) {
                g_wakeSent = true;
                wake = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_ringLock);
    if (wake) {
        HANDLE e = g_wakeEvent.load(std::memory_order_acquire);
        if (e) SetEvent(e);
    }
    return r;
}

// Drains everything currently in the ring into `o` and performs the I/O.
// Returns the number of messages written.
uint64_t DrainOnce(OutBufs& o, bool forceFlush) {
    uint64_t head, tail, dropped;
    AcquireSRWLockExclusive(&g_ringLock);
    head = g_head; tail = g_tail; dropped = g_droppedPending; g_droppedPending = 0; g_wakeSent = false;
    ReleaseSRWLockExclusive(&g_ringLock);

    o.Reset();
    const bool toConsole = g_consoleOn.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&g_fileLock);
    const bool toFile = g_logFile != nullptr;
    ReleaseSRWLockShared(&g_fileLock);

    uint64_t n = 0;
    uint64_t released = tail;
    for (uint64_t pos = tail; pos < head;) {
        const RecHdr* h = reinterpret_cast<const RecHdr*>(g_ring + (pos & (kRingBytes - 1)));
        if (h->level != kPadLevel) {
            AppendRecord(o, h->level, h->t, reinterpret_cast<const char*>(h + 1), h->len, toConsole, toFile);
            n++;
        }
        pos += h->size;
        if (pos - released >= kRingBytes / 8 && pos < head) {   // give producers space early during big batches
            AcquireSRWLockExclusive(&g_ringLock);
            g_tail = pos;
            ReleaseSRWLockExclusive(&g_ringLock);
            released = pos;
        }
    }
    if (dropped) {
        char msg[96];
        int len = snprintf(msg, sizeof(msg), "[log] %llu messages dropped", (unsigned long long)dropped);
        AppendRecord(o, (int)LogLevel::Warn, Qpc::ToSec(Qpc::Now() - LogEpoch()), msg, (size_t)std::max(len, 0), toConsole, toFile);
    }
    if (head != tail) {   // release ring space before the slow I/O
        AcquireSRWLockExclusive(&g_ringLock);
        g_tail = head;
        ReleaseSRWLockExclusive(&g_ringLock);
    }
    if (n || dropped || forceFlush) Emit(o, forceFlush);
    else {
        // Periodic file flush even when idle.
        AcquireSRWLockExclusive(&g_fileLock);
        WriteFileLocked(o, false);
        ReleaseSRWLockExclusive(&g_fileLock);
    }
    if (n || dropped) g_batches.fetch_add(1, std::memory_order_relaxed);
    g_written.fetch_add(n, std::memory_order_relaxed);
    return n;
}

void PublishFlushDone(uint64_t gen) {
    AcquireSRWLockExclusive(&g_doneLock);
    if (gen > g_flushDoneGen) g_flushDoneGen = gen;
    ReleaseSRWLockExclusive(&g_doneLock);
    WakeAllConditionVariable(&g_doneCv);
}

DWORD WINAPI WriterMain(void*) {
    SetThreadDescription(GetCurrentThread(), L"camss-log");
    ConsoleReady();
    OutBufs o;
    o.con.reserve(256 * 1024);
    o.file.reserve(256 * 1024);
    o.segs.reserve(64);
    uint64_t handledGen = 0;
    HANDLE ev = g_wakeEvent.load();
    for (;;) {
        WaitForSingleObject(ev, kWriterPeriodMs);
        const bool stop = g_stop.load();
        if (g_paused.load() && !stop) continue;
        const uint64_t gen = g_flushReqGen.load();   // read before the ring snapshot (see LogFlush)
        const bool flushNow = gen != handledGen;
        DrainOnce(o, flushNow);
        if (flushNow) {
            fflush(stdout); fflush(stderr);
            handledGen = gen;
            PublishFlushDone(gen);
        }
        if (stop) {
            // Close the async path, then write whatever slipped in before the close.
            AcquireSRWLockExclusive(&g_finalLock);
            AcquireSRWLockExclusive(&g_ringLock);
            g_closed = true;
            ReleaseSRWLockExclusive(&g_ringLock);
            DrainOnce(o, true);
            fflush(stdout); fflush(stderr);
            ReleaseSRWLockExclusive(&g_finalLock);
            PublishFlushDone(UINT64_MAX);
            break;
        }
    }
    return 0;
}

void AtExitShutdown() { LogShutdown(); }

void StartWriter() {
    AcquireSRWLockExclusive(&g_lifeLock);
    if (g_state.load() == kNotStarted) {
        g_state.store(kStarting);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_wakeEvent.store(ev, std::memory_order_release);
        g_thread = ev ? CreateThread(nullptr, 64 * 1024, WriterMain, nullptr, 0, nullptr) : nullptr;
        if (g_thread) {
            atexit(AtExitShutdown);
            g_state.store(kRunning);
        } else {
            // No writer: close the async path and write anything already queued synchronously.
            AcquireSRWLockExclusive(&g_finalLock);
            AcquireSRWLockExclusive(&g_ringLock);
            g_closed = true;
            ReleaseSRWLockExclusive(&g_ringLock);
            OutBufs o;
            ConsoleReady();
            DrainOnce(o, true);
            ReleaseSRWLockExclusive(&g_finalLock);
            g_state.store(kStopped);
        }
    }
    ReleaseSRWLockExclusive(&g_lifeLock);
}

} // namespace

namespace detail {
bool LogRateOk(std::atomic<int64_t>& last, double ms) {
    const int64_t now = Qpc::Now();
    int64_t prev = last.load(std::memory_order_relaxed);
    if (prev != 0 && now - prev < Qpc::FromMs(ms)) return false;
    return last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}
void LogTestPauseWriter(bool paused) {
    g_paused.store(paused);
    if (!paused) { HANDLE e = g_wakeEvent.load(); if (e) SetEvent(e); }
}
} // namespace detail

void SetLogLevel(LogLevel lvl) { detail::g_logLevel.store((int)lvl); }

LogLevel ParseLogLevel(const char* s, LogLevel fallback) {
    if (!s) return fallback;
    if (!_stricmp(s, "debug") || !_stricmp(s, "d")) return LogLevel::Debug;
    if (!_stricmp(s, "info") || !_stricmp(s, "i")) return LogLevel::Info;
    if (!_stricmp(s, "warn") || !_stricmp(s, "warning") || !_stricmp(s, "w")) return LogLevel::Warn;
    if (!_stricmp(s, "error") || !_stricmp(s, "e")) return LogLevel::Error;
    return fallback;
}

void SetLogConsole(bool enabled) { g_consoleOn.store(enabled); }

void SetLogFile(const char* path) {
    if (g_state.load() == kRunning) LogFlush(1000);   // earlier messages go to the previous file (if any)
    AcquireSRWLockExclusive(&g_fileLock);
    if (g_logFile) fclose(g_logFile);
    g_logFile = path ? fopen(path, "w") : nullptr;
    if (g_logFile) setvbuf(g_logFile, nullptr, _IOFBF, 64 * 1024);
    g_fileDirty = false;
    g_lastFileFlush = Qpc::Now();
    ReleaseSRWLockExclusive(&g_fileLock);
}

void LogV(LogLevel lvl, const char* fmt, va_list ap) {
    if (!LogEnabled(lvl)) return;
    const double t = Qpc::ToSec(Qpc::Now() - LogEpoch());
    char stackBuf[kStackMsg];
    char* msg = stackBuf;
    std::unique_ptr<char[]> big;
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    if (n < 0) { n = 0; stackBuf[0] = 0; }
    if ((size_t)n >= sizeof(stackBuf)) {
        // Rare (e.g. the periodic statistics table): format into a heap buffer once.
        // nothrow: logging must never throw (LOGx is used in destructors / noexcept code).
        const size_t cap = std::min<size_t>((size_t)n + 1, kMaxMsg + 1);
        big.reset(new (std::nothrow) char[cap]);
        if (big) {
            vsnprintf(big.get(), cap, fmt, ap2);
            msg = big.get();
            n = (int)std::min<size_t>((size_t)n, kMaxMsg);
        } else {
            n = (int)sizeof(stackBuf) - 1;   // out of memory: keep the truncated stack copy
        }
    }
    va_end(ap2);

    if (g_state.load(std::memory_order_acquire) == kNotStarted) StartWriter();
    if (Enqueue((int)lvl, t, msg, (size_t)n) == EnqResult::Closed) {
        WaitFinalDrain();   // keep this thread's order: its queued messages are written first
        WriteSync((int)lvl, t, msg, (size_t)n);
    }
}

bool LogFlush(int timeoutMs) {
    if (g_state.load() != kRunning) {
        AcquireSRWLockExclusive(&g_fileLock);
        if (g_logFile) fflush(g_logFile);
        g_fileDirty = false;
        ReleaseSRWLockExclusive(&g_fileLock);
        fflush(stdout); fflush(stderr);
        return true;
    }
    const uint64_t gen = g_flushReqGen.fetch_add(1) + 1;
    if (HANDLE e = g_wakeEvent.load()) SetEvent(e);
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)std::max(timeoutMs, 0);
    AcquireSRWLockExclusive(&g_doneLock);
    while (g_flushDoneGen < gen) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        SleepConditionVariableSRW(&g_doneCv, &g_doneLock, (DWORD)(deadline - now), 0);
    }
    const bool ok = g_flushDoneGen >= gen;
    ReleaseSRWLockExclusive(&g_doneLock);
    return ok;
}

void LogShutdown() {
    AcquireSRWLockExclusive(&g_lifeLock);
    const int st = g_state.load();
    if (st == kRunning) {
        g_stop.store(true);
        g_paused.store(false);
        if (HANDLE e = g_wakeEvent.load()) SetEvent(e);
        // Bounded: a console stuck in QuickEdit selection must not hang process exit.
        if (WaitForSingleObject(g_thread, 3000) == WAIT_OBJECT_0) CloseHandle(g_thread);
        g_thread = nullptr;
        AcquireSRWLockExclusive(&g_ringLock);
        g_closed = true;
        ReleaseSRWLockExclusive(&g_ringLock);
        g_state.store(kStopped);
    } else if (st == kNotStarted) {
        AcquireSRWLockExclusive(&g_ringLock);
        g_closed = true;
        ReleaseSRWLockExclusive(&g_ringLock);
        g_state.store(kStopped);
    }
    ReleaseSRWLockExclusive(&g_lifeLock);
    AcquireSRWLockExclusive(&g_fileLock);
    if (g_logFile) fflush(g_logFile);
    g_fileDirty = false;
    ReleaseSRWLockExclusive(&g_fileLock);
}

LogCounters GetLogCounters() {
    LogCounters c;
    AcquireSRWLockShared(&g_ringLock);
    c.enqueued = g_enqueued;
    c.dropped = g_droppedTotal;
    ReleaseSRWLockShared(&g_ringLock);
    c.written = g_written.load();
    c.batches = g_batches.load();
    return c;
}

// ============================================================== errors

std::string HrToString(HRESULT hr) {
    char buf[512] = {};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), buf, sizeof(buf), nullptr);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    char out[640];
    snprintf(out, sizeof(out), "0x%08lX %s", (unsigned long)hr, n ? buf : "");
    return out;
}

void ThrowHr(HRESULT hr, const char* expr, const char* file, int line) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s failed: %s (%s:%d)", expr, HrToString(hr).c_str(), file, line);
    LOGE("%s", buf);
    throw std::runtime_error(buf);
}

PreciseSleeper::PreciseSleeper() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
}
PreciseSleeper::~PreciseSleeper() { if (timer_) CloseHandle(timer_); }

void PreciseSleeper::SleepUntil(int64_t deadline, double spinMs) {
    int64_t now = Qpc::Now();
    int64_t spin = Qpc::FromMs(spinMs);
    if (deadline - now > spin && timer_) {
        int64_t waitTicks = deadline - now - spin;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((double)waitTicks * 1e7 / (double)Qpc::Freq()); // relative, 100ns
        if (due.QuadPart < 0 && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer_, INFINITE);
    }
    while (Qpc::Now() < deadline) YieldProcessor();
}

void DumpModules(const wchar_t* filter, const char* when) {
    HMODULE mods[2048]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        wchar_t m[MAX_PATH]; GetModuleFileNameW(mods[i], m, MAX_PATH);
        if (wcsstr(m, filter)) LOGI("[%s] module %s", when, WideToUtf8(m).c_str());
    }
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

} // namespace camss
