#include "common.h"
#include <cmath>
#include <cstring>
#include <cwctype>
#include <memory>
#include <vector>

namespace cameravfx {

namespace {

constexpr uint32_t kRingBytes = 1u << 20;
constexpr size_t kStackMessageBytes = 1024;
constexpr size_t kMaxMessageBytes = 60000;
constexpr uint8_t kPaddingLevel = 0xFF;
constexpr DWORD kWriterPeriodMs = 10;
constexpr double kFileFlushMs = 250.0;
constexpr ULONGLONG kFinalDrainWaitMs = 2000;
constexpr DWORD kShutdownWaitMs = 3000;
constexpr size_t kFileBufferBytes = 64 * 1024;
constexpr int kTimestampColumns = 9;
constexpr size_t kMarkerColumns = 3;
constexpr size_t kContinuationIndent = kTimestampColumns + kMarkerColumns;
constexpr DWORD kConsoleChunkChars = 16384;

struct RecordHeader {
    uint32_t recordBytes;
    uint16_t messageBytes;
    uint8_t level;
    uint8_t unused;
    double timeSec;
};
static_assert(sizeof(RecordHeader) == 16);
static_assert(kMaxMessageBytes <= UINT16_MAX);

uint32_t RecordBytesFor(size_t messageBytes) { return (uint32_t)((sizeof(RecordHeader) + messageBytes + 7) & ~(size_t)7); }

enum class WriterState { NotStarted, Starting, Running, Stopped };

alignas(64) char g_ring[kRingBytes];
SRWLOCK g_ringLock = SRWLOCK_INIT;
uint64_t g_head = 0, g_tail = 0;
uint64_t g_droppedPending = 0;
bool g_ringClosed = false;
bool g_wakeSent = false;

std::atomic<WriterState> g_state{WriterState::NotStarted};
std::atomic<HANDLE> g_wakeEvent{nullptr};
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_consoleOn{true};
std::atomic<uint64_t> g_flushRequestGen{0};
SRWLOCK g_lifeLock = SRWLOCK_INIT;
SRWLOCK g_finalDrainLock = SRWLOCK_INIT;

SRWLOCK g_doneLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_doneCv = CONDITION_VARIABLE_INIT;
uint64_t g_flushDoneGen = 0;

SRWLOCK g_fileLock = SRWLOCK_INIT;
FILE* g_logFile = nullptr;
bool g_fileDirty = false;
int64_t g_lastFileFlush = 0;

SRWLOCK g_conLock = SRWLOCK_INIT;

SRWLOCK g_sinkLock = SRWLOCK_INIT;
LogSinkFn g_sinkFn = nullptr;
void* g_sinkCtx = nullptr;
std::atomic<bool> g_sinkSet{false};
thread_local bool t_inSink = false;

const char kLevelTags[] = {'D', 'I', 'W', 'E'};

int64_t LogEpoch() { static const int64_t t0 = Qpc::Now(); return t0; }
double LogTimeSec() { return Qpc::ToSec(Qpc::Now() - LogEpoch()); }

bool EnvEquals(const wchar_t* name, const wchar_t* value) {
    wchar_t buf[64];
    const DWORD n = GetEnvironmentVariableW(name, buf, 64);
    return n > 0 && n < 64 && !_wcsicmp(buf, value);
}
bool EnvSet(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) > 0; }

enum Stream { kStdout = 0, kStderr = 1 };

struct ConsoleInfo {
    HANDLE handle[2] = {nullptr, nullptr};
    bool isConsole[2] = {false, false};
    bool vt[2] = {false, false};
    bool sameConsole = false;
    bool tty = false;
    bool truecolor = false;
};

ConsoleInfo DetectConsole() {
    ConsoleInfo c;
    const DWORD ids[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 2; i++) {
        HANDLE h = GetStdHandle(ids[i]);
        c.handle[i] = h;
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            c.isConsole[i] = true;
            c.vt[i] = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) || SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    c.sameConsole = c.isConsole[kStdout] && c.isConsole[kStderr];
    c.tty = c.isConsole[kStdout] && c.vt[kStdout];
    c.truecolor = EnvSet(L"WT_SESSION") || EnvEquals(L"COLORTERM", L"truecolor") || EnvEquals(L"COLORTERM", L"24bit");
    return c;
}

const ConsoleInfo& Console() { static const ConsoleInfo c = DetectConsole(); return c; }

struct OutBufs {
    struct Segment { Stream stream; size_t begin, end; };
    std::string con;
    std::vector<Segment> segments;
    std::string file;
    std::wstring wide;
    bool urgent = false;
    void Reset() { con.clear(); segments.clear(); file.clear(); urgent = false; }
};

constexpr const char* kSgrDim = "\x1b[90m";
constexpr const char* kSgrFgOff = "\x1b[39m";
constexpr const char* kSgrYellow = "\x1b[33m";
constexpr const char* kSgrRed = "\x1b[91m";
constexpr const char* kInfoMarker = " \xC2\xB7 ";
constexpr const char* kWarnMarker = "\xE2\x96\xB2";
constexpr const char* kErrorMarker = "\xC3\x97 ";

int FormatTimestamp(char* out, size_t cap, double t) {
    if (!(t >= 0.0 && t < 1e12)) {
        const int n = snprintf(out, cap, "%*.3f", kTimestampColumns, t);
        return n < 0 ? 0 : std::min(n, (int)cap - 1);
    }
    unsigned long long ms = (unsigned long long)(t * 1000.0 + 0.5);
    char reversed[32];
    int k = 0;
    for (int i = 0; i < 3; i++) { reversed[k++] = (char)('0' + ms % 10); ms /= 10; }
    reversed[k++] = '.';
    do { reversed[k++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    int n = 0;
    for (int pad = kTimestampColumns - k; pad > 0; pad--) out[n++] = ' ';
    while (k) out[n++] = reversed[--k];
    return n;
}

void AppendPlain(std::string& out, int level, const char* ts, int tsLen, const char* msg, size_t len) {
    out.push_back('[');
    out.append(ts, (size_t)tsLen);
    out.append("][");
    out.push_back(kLevelTags[level & 3]);
    out.append("] ");
    out.append(msg, len);
    out.push_back('\n');
}

void AppendStyled(std::string& con, int level, const char* ts, int tsLen, const char* msg, size_t len) {
    con.append(kSgrDim);
    con.append(ts, (size_t)tsLen);
    switch ((LogLevel)level) {
    case LogLevel::Debug: con.append(kInfoMarker); break;
    case LogLevel::Info: con.append(kInfoMarker); con.append(kSgrFgOff); break;
    case LogLevel::Warn:
        con.append(kSgrFgOff); con.append(" "); con.append(kSgrYellow); con.append(kWarnMarker); con.append(kSgrFgOff); con.append(" ");
        break;
    case LogLevel::Error: con.append(kSgrFgOff); con.append(" "); con.append(kSgrRed); con.append(kErrorMarker); break;
    }
    size_t b = 0;
    for (;;) {
        const void* nl = memchr(msg + b, '\n', len - b);
        if (!nl) { con.append(msg + b, len - b); break; }
        const size_t e = (size_t)((const char*)nl - msg);
        con.append(msg + b, e + 1 - b);
        b = e + 1;
        if (b >= len) break;
        con.append(kContinuationIndent, ' ');
    }
    if (level == (int)LogLevel::Debug || level == (int)LogLevel::Error) con.append(kSgrFgOff);
    con.push_back('\n');
}

void AppendRecord(OutBufs& o, int level, double t, const char* msg, size_t len, bool toConsole, bool toFile) {
    char ts[48];
    const int tsLen = FormatTimestamp(ts, sizeof(ts), t);
    if (level >= (int)LogLevel::Warn) o.urgent = true;
    if (toFile) AppendPlain(o.file, level, ts, tsLen, msg, len);
    if (!toConsole) return;
    const ConsoleInfo& c = Console();
    const Stream stream = (c.sameConsole || level < (int)LogLevel::Warn) ? kStdout : kStderr;
    const size_t b = o.con.size();
    if (c.vt[stream]) AppendStyled(o.con, level, ts, tsLen, msg, len);
    else AppendPlain(o.con, level, ts, tsLen, msg, len);
    if (!o.segments.empty() && o.segments.back().stream == stream) o.segments.back().end = o.con.size();
    else o.segments.push_back({stream, b, o.con.size()});
}

void WriteRawLocked(OutBufs& o, Stream stream, const char* p, size_t n) {
    const ConsoleInfo& c = Console();
    if (!c.isConsole[stream]) {
        FILE* f = stream == kStderr ? stderr : stdout;
        fwrite(p, 1, n, f);
        fflush(f);
        return;
    }
    const int wn = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
    if (wn <= 0) return;
    o.wide.resize((size_t)wn);
    MultiByteToWideChar(CP_UTF8, 0, p, (int)n, o.wide.data(), wn);
    const wchar_t* w = o.wide.data();
    size_t left = (size_t)wn;
    while (left > 0) {
        DWORD chunk = (DWORD)std::min<size_t>(left, kConsoleChunkChars), done = 0;
        if (chunk < left && IS_HIGH_SURROGATE(w[chunk - 1])) chunk--;
        if (!WriteConsoleW(c.handle[stream], w, chunk, &done, nullptr) || done == 0) break;
        w += done;
        left -= done;
    }
}

void WriteConsoleLocked(OutBufs& o) {
    for (const auto& s : o.segments) WriteRawLocked(o, s.stream, o.con.data() + s.begin, s.end - s.begin);
}

void CallSinkLocked(int level, double t, const char* msg, size_t len) {
    if (!g_sinkFn) return;
    t_inSink = true;
    g_sinkFn(g_sinkCtx, (LogLevel)level, t, msg, len);
    t_inSink = false;
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
    if (!o.segments.empty()) {
        AcquireSRWLockExclusive(&g_conLock);
        if (g_consoleOn.load()) WriteConsoleLocked(o);
        ReleaseSRWLockExclusive(&g_conLock);
    }
    AcquireSRWLockExclusive(&g_fileLock);
    WriteFileLocked(o, forceFlush);
    ReleaseSRWLockExclusive(&g_fileLock);
}

bool LogFileOpen() {
    AcquireSRWLockShared(&g_fileLock);
    const bool open = g_logFile != nullptr;
    ReleaseSRWLockShared(&g_fileLock);
    return open;
}

void WriteSync(int level, double t, const char* msg, size_t len) {
    OutBufs o;
    AppendRecord(o, level, t, msg, len, g_consoleOn.load(std::memory_order_relaxed), LogFileOpen());
    if (g_sinkSet.load() && !t_inSink) {
        AcquireSRWLockExclusive(&g_sinkLock);
        CallSinkLocked(level, t, msg, len);
        ReleaseSRWLockExclusive(&g_sinkLock);
    }
    Emit(o, true);
}

void WaitFinalDrain() {
    const ULONGLONG deadline = GetTickCount64() + kFinalDrainWaitMs;
    while (!TryAcquireSRWLockShared(&g_finalDrainLock)) {
        if (GetTickCount64() >= deadline) return;
        Sleep(1);
    }
    ReleaseSRWLockShared(&g_finalDrainLock);
}

enum class EnqueueResult { Queued, Dropped, Closed };

EnqueueResult Enqueue(int level, double t, const char* msg, size_t len) {
    const uint32_t need = RecordBytesFor(len);
    bool wake = false;
    EnqueueResult r = EnqueueResult::Queued;
    AcquireSRWLockExclusive(&g_ringLock);
    if (g_ringClosed) {
        r = EnqueueResult::Closed;
    } else {
        const uint64_t used = g_head - g_tail;
        uint32_t off = (uint32_t)(g_head & (kRingBytes - 1));
        const uint32_t contiguous = kRingBytes - off;
        const uint32_t padding = contiguous < need ? contiguous : 0;
        if (used + padding + need > kRingBytes) {
            g_droppedPending++;
            r = EnqueueResult::Dropped;
            if (!g_wakeSent) { g_wakeSent = true; wake = true; }
        } else {
            if (padding) {
                RecordHeader* p = reinterpret_cast<RecordHeader*>(g_ring + off);
                p->recordBytes = padding; p->messageBytes = 0; p->level = kPaddingLevel;
                g_head += padding;
                off = 0;
            }
            RecordHeader* h = reinterpret_cast<RecordHeader*>(g_ring + off);
            h->recordBytes = need; h->messageBytes = (uint16_t)len; h->level = (uint8_t)level; h->unused = 0; h->timeSec = t;
            memcpy(h + 1, msg, len);
            g_head += need;
            if (!g_wakeSent && (level >= (int)LogLevel::Warn || used + padding + need > kRingBytes / 8)) {
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

void DrainOnce(OutBufs& o, bool forceFlush) {
    AcquireSRWLockExclusive(&g_ringLock);
    const uint64_t head = g_head, tail = g_tail, dropped = g_droppedPending;
    g_droppedPending = 0;
    g_wakeSent = false;
    ReleaseSRWLockExclusive(&g_ringLock);

    o.Reset();
    const bool toConsole = g_consoleOn.load(std::memory_order_relaxed);
    const bool toFile = LogFileOpen();
    const bool sink = (head != tail || dropped) && g_sinkSet.load();
    if (sink) AcquireSRWLockExclusive(&g_sinkLock);

    uint64_t released = tail;
    for (uint64_t pos = tail; pos < head;) {
        const RecordHeader* h = reinterpret_cast<const RecordHeader*>(g_ring + (pos & (kRingBytes - 1)));
        if (h->level != kPaddingLevel) {
            const char* msg = reinterpret_cast<const char*>(h + 1);
            AppendRecord(o, h->level, h->timeSec, msg, h->messageBytes, toConsole, toFile);
            if (sink) CallSinkLocked(h->level, h->timeSec, msg, h->messageBytes);
        }
        pos += h->recordBytes;
        if (pos - released >= kRingBytes / 8 && pos < head) {
            AcquireSRWLockExclusive(&g_ringLock);
            g_tail = pos;
            ReleaseSRWLockExclusive(&g_ringLock);
            released = pos;
        }
    }
    if (dropped) {
        char msg[96];
        const int len = std::max(snprintf(msg, sizeof(msg), "[log] %llu messages dropped", (unsigned long long)dropped), 0);
        const double t = LogTimeSec();
        AppendRecord(o, (int)LogLevel::Warn, t, msg, (size_t)len, toConsole, toFile);
        if (sink) CallSinkLocked((int)LogLevel::Warn, t, msg, (size_t)len);
    }
    if (sink) ReleaseSRWLockExclusive(&g_sinkLock);
    if (head != tail) {
        AcquireSRWLockExclusive(&g_ringLock);
        g_tail = head;
        ReleaseSRWLockExclusive(&g_ringLock);
    }
    Emit(o, forceFlush);
}

void PublishFlushDone(uint64_t gen) {
    AcquireSRWLockExclusive(&g_doneLock);
    if (gen > g_flushDoneGen) g_flushDoneGen = gen;
    ReleaseSRWLockExclusive(&g_doneLock);
    WakeAllConditionVariable(&g_doneCv);
}

void CloseRing() {
    AcquireSRWLockExclusive(&g_ringLock);
    g_ringClosed = true;
    ReleaseSRWLockExclusive(&g_ringLock);
}

void DrainFinal(OutBufs& o) {
    AcquireSRWLockExclusive(&g_finalDrainLock);
    CloseRing();
    DrainOnce(o, true);
    fflush(stdout);
    fflush(stderr);
    ReleaseSRWLockExclusive(&g_finalDrainLock);
}

DWORD WINAPI WriterMain(void*) {
    SetThreadDescription(GetCurrentThread(), L"CameraVFX log");
    OutBufs o;
    o.con.reserve(256 * 1024);
    o.file.reserve(256 * 1024);
    o.segments.reserve(64);
    uint64_t handledGen = 0;
    HANDLE ev = g_wakeEvent.load();
    for (;;) {
        WaitForSingleObject(ev, kWriterPeriodMs);
        if (g_stop.load()) {
            DrainFinal(o);
            PublishFlushDone(UINT64_MAX);
            return 0;
        }
        const uint64_t gen = g_flushRequestGen.load();
        const bool flushNow = gen != handledGen;
        DrainOnce(o, flushNow);
        if (flushNow) {
            fflush(stdout);
            fflush(stderr);
            handledGen = gen;
            PublishFlushDone(gen);
        }
    }
}

void AtExitShutdown() { LogShutdown(); }

void StartWriter() {
    AcquireSRWLockExclusive(&g_lifeLock);
    if (g_state.load() == WriterState::NotStarted) {
        g_state.store(WriterState::Starting);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_wakeEvent.store(ev, std::memory_order_release);
        g_thread = ev ? CreateThread(nullptr, 64 * 1024, WriterMain, nullptr, 0, nullptr) : nullptr;
        if (g_thread) {
            atexit(AtExitShutdown);
            g_state.store(WriterState::Running);
        } else {
            OutBufs o;
            DrainFinal(o);
            g_state.store(WriterState::Stopped);
        }
    }
    ReleaseSRWLockExclusive(&g_lifeLock);
}

void Write(LogLevel lvl, const char* fmt, va_list ap) {
    const double t = LogTimeSec();
    char stackBuf[kStackMessageBytes];
    char* msg = stackBuf;
    std::unique_ptr<char[]> big;
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    if (n < 0) { n = 0; stackBuf[0] = 0; }
    if ((size_t)n >= sizeof(stackBuf)) {
        const size_t cap = std::min<size_t>((size_t)n + 1, kMaxMessageBytes + 1);
        big.reset(new (std::nothrow) char[cap]);
        if (big) {
            vsnprintf(big.get(), cap, fmt, ap2);
            msg = big.get();
            n = (int)std::min<size_t>((size_t)n, kMaxMessageBytes);
        } else {
            n = (int)sizeof(stackBuf) - 1;
        }
    }
    va_end(ap2);

    if (g_state.load(std::memory_order_acquire) == WriterState::NotStarted) StartWriter();
    if (Enqueue((int)lvl, t, msg, (size_t)n) == EnqueueResult::Closed) {
        if (!t_inSink) WaitFinalDrain();
        WriteSync((int)lvl, t, msg, (size_t)n);
    }
}

}

namespace detail {
bool LogRateOk(std::atomic<int64_t>& last, double ms) {
    const int64_t now = Qpc::Now();
    int64_t prev = last.load(std::memory_order_relaxed);
    if (prev != 0 && now - prev < Qpc::FromMs(ms)) return false;
    return last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}
}

void SetLogLevel(LogLevel lvl) { detail::g_logLevel.store((int)lvl); }

void SetLogConsole(bool enabled) {
    AcquireSRWLockExclusive(&g_conLock);
    g_consoleOn.store(enabled);
    ReleaseSRWLockExclusive(&g_conLock);
}

void SetLogSink(LogSinkFn fn, void* ctx) {
    const bool ownsSinkLock = t_inSink;
    if (!ownsSinkLock) AcquireSRWLockExclusive(&g_sinkLock);
    g_sinkFn = fn;
    g_sinkCtx = ctx;
    g_sinkSet.store(fn != nullptr);
    if (!ownsSinkLock) ReleaseSRWLockExclusive(&g_sinkLock);
}

bool ConsoleIsInteractive() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) && Console().tty;
}

void SetLogFile(const std::string& utf8Path) {
    if (g_state.load() == WriterState::Running) LogFlush(1000);
    AcquireSRWLockExclusive(&g_fileLock);
    if (g_logFile) fclose(g_logFile);
    g_logFile = utf8Path.empty() ? nullptr : OpenUtf8(utf8Path, L"w");
    if (g_logFile) setvbuf(g_logFile, nullptr, _IOFBF, kFileBufferBytes);
    const bool failed = !utf8Path.empty() && !g_logFile;
    g_fileDirty = false;
    g_lastFileFlush = Qpc::Now();
    ReleaseSRWLockExclusive(&g_fileLock);
    if (failed) LOGE("cannot open the log file %s", utf8Path.c_str());
}

void Log(LogLevel lvl, const char* fmt, ...) {
    if (!LogEnabled(lvl)) return;
    va_list ap;
    va_start(ap, fmt);
    Write(lvl, fmt, ap);
    va_end(ap);
}

bool LogFlush(int timeoutMs) {
    if (g_state.load() != WriterState::Running) {
        AcquireSRWLockExclusive(&g_fileLock);
        if (g_logFile) fflush(g_logFile);
        g_fileDirty = false;
        ReleaseSRWLockExclusive(&g_fileLock);
        fflush(stdout);
        fflush(stderr);
        return true;
    }
    const uint64_t gen = g_flushRequestGen.fetch_add(1) + 1;
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
    const WriterState st = g_state.load();
    if (st == WriterState::Running) {
        g_stop.store(true);
        if (HANDLE e = g_wakeEvent.load()) SetEvent(e);
        if (WaitForSingleObject(g_thread, kShutdownWaitMs) == WAIT_OBJECT_0) CloseHandle(g_thread);
        g_thread = nullptr;
        CloseRing();
        g_state.store(WriterState::Stopped);
    } else if (st == WriterState::NotStarted) {
        CloseRing();
        g_state.store(WriterState::Stopped);
    }
    ReleaseSRWLockExclusive(&g_lifeLock);
    AcquireSRWLockExclusive(&g_fileLock);
    if (g_logFile) fflush(g_logFile);
    g_fileDirty = false;
    ReleaseSRWLockExclusive(&g_fileLock);
}

namespace style {
namespace {
std::string Wrap(const char* on, std::string_view s, const char* off) {
    if (!Console().tty) return std::string(s);
    std::string r;
    r.reserve(s.size() + 24);
    r.append(on);
    r.append(s);
    r.append(off);
    return r;
}
}
std::string Dim(std::string_view s) { return Wrap(kSgrDim, s, kSgrFgOff); }
std::string Bold(std::string_view s) { return Wrap("\x1b[1m", s, "\x1b[22m"); }
std::string Accent(std::string_view s) { return Wrap(Console().truecolor ? "\x1b[38;2;217;119;87m" : "\x1b[38;5;173m", s, kSgrFgOff); }
}

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
    if (!timer_) ThrowHr(HRESULT_FROM_WIN32(GetLastError()), "CreateWaitableTimerExW(HIGH_RESOLUTION)", __FILE__, __LINE__);
}
PreciseSleeper::~PreciseSleeper() { CloseHandle(timer_); }

void PreciseSleeper::SleepUntil(int64_t deadline, double spinMs) {
    const int64_t spin = Qpc::FromMs(spinMs);
    const int64_t wakeAt = deadline - spin;
    const int64_t now = Qpc::Now();
    lastWakeLateMs_ = 0;
    if (wakeAt > now) {
        LARGE_INTEGER relativeDue;
        relativeDue.QuadPart = -(LONGLONG)((double)(wakeAt - now) * kHnsPerSecond / (double)Qpc::Freq());
        if (SetWaitableTimerEx(timer_, &relativeDue, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer_, INFINITE);
            lastWakeLateMs_ = std::max(0.0, Qpc::ToMs(Qpc::Now() - wakeAt));
        }
    }
    while (Qpc::Now() < deadline) YieldProcessor();
}

FrameSize FitAspect(FrameSize inner, FrameSize outer) {
    const double aspect = (double)inner.width / inner.height;
    if ((double)outer.width / outer.height > aspect) return {(unsigned)std::lround(outer.height * aspect), outer.height};
    return {outer.width, (unsigned)std::lround(outer.width / aspect)};
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

std::optional<bool> ParseBool(std::string_view s) {
    const std::string v = Lower(s);
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return std::nullopt;
}

bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part) {
    auto upper = [](std::wstring_view s) {
        std::wstring out(s);
        for (wchar_t& c : out) c = (wchar_t)std::towupper(c);
        return out;
    };
    return !part.empty() && upper(text).find(upper(part)) != std::wstring::npos;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

FILE* OpenUtf8(const std::string& utf8Path, const wchar_t* mode) { return _wfopen(Utf8ToWide(utf8Path.c_str()).c_str(), mode); }

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    const std::wstring exe(path, GetModuleFileNameW(nullptr, path, MAX_PATH));
    return exe.substr(0, exe.find_last_of(L"\\/"));
}

}
