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
    bool tty = false;                               // stdout is a console with VT processing
    bool truecolor = false;                         // 24-bit SGR (Windows Terminal / COLORTERM)
    bool ambWide = false;                           // ambiguous-width symbols may render 2 columns wide
};
ConsoleInfo g_con;                                  // under g_conLock (immutable once init)
std::atomic<int> g_ttyCache{-1};                    // -1 unknown, else g_con.tty

// Pinned status line: what is on screen. Under g_conLock.
struct StatusScreen {
    std::string want;                               // adopted text (sanitised, untruncated); empty = none
    bool shown = false;                             // a status line is on screen, cursor at its end
    int shownCols = 0;                              // its visible width
    int width = 0;                                  // console width when it was drawn
    bool dirty = false;                             // needs a redraw (new text / width / console back on)
};
StatusScreen g_scr;

// Requested status text (SetStatusLine -> writer).
SRWLOCK g_statusLock = SRWLOCK_INIT;
std::string g_statusText;                           // under g_statusLock
std::atomic<uint64_t> g_statusGen{0};
std::atomic<uint64_t> g_statusDraws{0};
constexpr double kStatusMinMs = 100.0;              // new status text at most ~10x per second

// Sink. g_sinkLock is held exclusively while the sink runs (calls are serialised) and by SetLogSink.
SRWLOCK g_sinkLock = SRWLOCK_INIT;
LogSinkFn g_sinkFn = nullptr;                       // under g_sinkLock
void* g_sinkCtx = nullptr;                          // under g_sinkLock
std::atomic<bool> g_sinkSet{false};
thread_local bool t_inSink = false;

const char kTags[] = {'D', 'I', 'W', 'E'};

int64_t LogEpoch() { static const int64_t t0 = Qpc::Now(); return t0; }
const int64_t g_epochInit = LogEpoch();             // epoch = static-init time (or first log, if earlier)

bool EnvSet(const wchar_t* name, const wchar_t* value = nullptr) {
    wchar_t buf[64];
    const DWORD n = GetEnvironmentVariableW(name, buf, 64);
    if (n == 0 || n >= 64) return n >= 64 && !value;
    return !value || !_wcsicmp(buf, value);
}

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
    g_con.tty = g_con.isCon[0] && g_con.vt[0];
    const bool wt = EnvSet(L"WT_SESSION");
    g_con.truecolor = wt || EnvSet(L"COLORTERM", L"truecolor") || EnvSet(L"COLORTERM", L"24bit");
    const UINT cp = GetConsoleOutputCP();
    g_con.ambWide = !wt && (cp == 932 || cp == 936 || cp == 949 || cp == 950);   // CJK conhost fonts
    g_ttyCache.store(g_con.tty ? 1 : 0, std::memory_order_release);
}

// Output staging buffers (one set owned by the writer thread, temporary ones for the sync path).
struct OutBufs {
    struct Seg { int stream; size_t b, e; };
    std::string con;
    std::vector<Seg> segs;
    std::string file;
    std::string tmp;       // console text with the status line erase / redraw around it
    std::wstring wide;
    bool urgent = false;   // contains Warn/Error -> flush the file now
    void Reset() { con.clear(); segs.clear(); file.clear(); urgent = false; }
};

// ---- console styling (only used for VT consoles)
constexpr const char* kSgrDim = "\x1b[90m";         // bright black: rendered by conhost and terminals alike
constexpr const char* kSgrFgOff = "\x1b[39m";
constexpr const char* kSgrYellow = "\x1b[33m";
constexpr const char* kSgrRed = "\x1b[91m";
constexpr size_t kIndent = 12;                      // "%9.3f" + ' ' + marker + ' '

// Decodes one UTF-8 code point at s[i] (advancing i); invalid bytes yield U+FFFD.
uint32_t NextCp(std::string_view s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!n) return 0xFFFD;
    uint32_t cp = c & (0x3F >> n);
    for (; n > 0; n--) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

int CpWidth(uint32_t c, bool ambWide) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) ||
        (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F680 && c <= 0x1F6FF) || (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD) ||
        c == 0x231A || c == 0x231B || c == 0x23F3 || c == 0x2705 || c == 0x274C || c == 0x2B50)
        return 2;
    if (ambWide && ((c >= 0xA1 && c <= 0xFF && c != 0xC0 && c != 0xE0) || (c >= 0x0391 && c <= 0x03C9) ||
                    (c >= 0x0401 && c <= 0x0451) || (c >= 0x2010 && c <= 0x203E) || (c >= 0x2100 && c <= 0x22FF) ||
                    (c >= 0x2460 && c <= 0x27BF) || (c >= 0xE000 && c <= 0xF8FF) || c == 0xFFFD))
        return 2;
    return 1;
}

// Length of the escape sequence starting at s[i] (s[i] == ESC).
size_t EscLen(std::string_view s, size_t i) {
    size_t j = i + 1;
    if (j >= s.size()) return 1;
    if (s[j] == '[') {                                       // CSI: params, final byte 0x40..0x7E
        for (j++; j < s.size(); j++) if ((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7E) return j + 1 - i;
        return s.size() - i;
    }
    if (s[j] == ']') {                                       // OSC: up to BEL or ESC '\'
        for (j++; j < s.size(); j++) {
            if (s[j] == '\a') return j + 1 - i;
            if (s[j] == '\x1b' && j + 1 < s.size() && s[j + 1] == '\\') return j + 2 - i;
        }
        return s.size() - i;
    }
    return 2;
}

int TextWidth(std::string_view s, bool ambWide) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') { i += EscLen(s, i); continue; }
        w += CpWidth(NextCp(s, i), ambWide);
    }
    return w;
}

// Appends `s` cut to at most maxCols visible columns ("…" marks a cut), keeping escape
// sequences, then a full SGR reset. Returns the visible width written.
int AppendFitted(std::string& out, std::string_view s, int maxCols, bool ambWide) {
    if (maxCols < 1) maxCols = 1;
    const bool fits = TextWidth(s, ambWide) <= maxCols;
    const int ell = ambWide ? 2 : 1;
    const int limit = fits ? maxCols : std::max(maxCols - ell, 0);
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') { const size_t n = EscLen(s, i); out.append(s.data() + i, n); i += n; continue; }
        const size_t b = i;
        const int cw = CpWidth(NextCp(s, i), ambWide);
        if (w + cw > limit) break;
        out.append(s.data() + b, i - b);
        w += cw;
    }
    if (!fits && w + ell <= maxCols) { out.append("\xE2\x80\xA6"); w += ell; }
    out.append("\x1b[0m");
    return w;
}

// "[    1.234][I] msg" -> dim time, small coloured level marker, message (indented continuation lines).
void AppendStyled(std::string& con, int level, const char* pre, int pn, const char* msg, size_t len) {
    const size_t tsLen = pn >= 6 ? (size_t)pn - 6 : 0;      // pre = '[' ts "][X] "
    con.append(kSgrDim);
    con.append(pre + 1, tsLen);
    if (level <= (int)LogLevel::Info) {
        con.append(" \xC2\xB7 ");                           // " · " (Debug: whole line stays dim)
        if (level == (int)LogLevel::Info) con.append(kSgrFgOff);
    } else {
        con.append(kSgrFgOff);
        if (level == (int)LogLevel::Warn) { con.append(" "); con.append(kSgrYellow); con.append("\xE2\x96\xB2"); con.append(kSgrFgOff); con.append(" "); }
        else { con.append(" "); con.append(kSgrRed); con.append("\xC3\x97 "); }   // "× " and the message in red
    }
    size_t b = 0;
    for (;;) {
        const void* nl = memchr(msg + b, '\n', len - b);
        if (!nl) { con.append(msg + b, len - b); break; }
        const size_t e = (size_t)((const char*)nl - msg);
        con.append(msg + b, e + 1 - b);
        b = e + 1;
        if (b >= len) break;                                // trailing newline: no indent
        con.append(kIndent, ' ');
    }
    if (level != (int)LogLevel::Info && level != (int)LogLevel::Warn) con.append(kSgrFgOff);
    con.push_back('\n');
}

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
        const size_t b = o.con.size();
        if (g_con.vt[stream]) {
            AppendStyled(o.con, level, pre, pn, msg, len);
        } else {
            o.con.append(pre, (size_t)pn);
            o.con.append(msg, len);
            o.con.push_back('\n');
        }
        if (!o.segs.empty() && o.segs.back().stream == stream) o.segs.back().e = o.con.size();
        else o.segs.push_back({stream, b, o.con.size()});
    }
}

void WriteRawLocked(OutBufs& o, int stream, const char* p, size_t n) {
    if (g_con.isCon[stream]) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
        if (wn <= 0) return;
        o.wide.resize((size_t)wn);
        MultiByteToWideChar(CP_UTF8, 0, p, (int)n, o.wide.data(), wn);
        const wchar_t* w = o.wide.data();
        size_t left = (size_t)wn;
        while (left > 0) {
            DWORD chunk = (DWORD)std::min<size_t>(left, 16384), done = 0;
            if (chunk < left && IS_HIGH_SURROGATE(w[chunk - 1])) chunk--;   // never split a surrogate pair
            if (!WriteConsoleW(g_con.h[stream], w, chunk, &done, nullptr) || done == 0) break;
            w += done; left -= done;
        }
    } else {
        FILE* f = stream ? stderr : stdout;
        fwrite(p, 1, n, f);
        fflush(f);
    }
}

int ConsoleWidthLocked() {
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (GetConsoleScreenBufferInfo(g_con.h[0], &bi) && bi.dwSize.X > 0) return bi.dwSize.X;
    return 80;
}

// Status line helpers (tty only, under g_conLock). The status line never ends with a
// newline: the cursor stays at its end, so erasing is "CR + erase line" (plus the rows
// it wraps onto if the console was made narrower and reflowed it).
void AppendStatusErase(std::string& out, int width) {
    if (!g_scr.shown) return;
    const int rows = g_scr.shownCols > 0 ? (g_scr.shownCols - 1) / std::max(width, 1) + 1 : 1;
    out.append("\r\x1b[2K");
    for (int r = 1; r < rows && r < 8; r++) out.append("\x1b[A\x1b[2K");
    g_scr.shown = false;
}
void AppendStatusDraw(std::string& out, int width) {
    g_scr.dirty = false;
    if (g_scr.want.empty()) return;
    g_scr.shownCols = AppendFitted(out, g_scr.want, width - 1, g_con.ambWide);
    g_scr.width = width;
    g_scr.shown = true;
}

// Writes staged console text: one write per run of same-stream records (normally one per
// batch). On a tty the status line is erased before and redrawn after, in the same write.
void WriteConsoleLocked(OutBufs& o) {
    const bool status = g_con.tty && (g_scr.shown || !g_scr.want.empty());
    size_t last0 = SIZE_MAX;
    if (status)
        for (size_t i = 0; i < o.segs.size(); i++) if (o.segs[i].stream == 0) last0 = i;
    const int width = last0 != SIZE_MAX ? ConsoleWidthLocked() : 0;
    for (size_t i = 0; i < o.segs.size(); i++) {
        const auto& s = o.segs[i];
        const char* p = o.con.data() + s.b;
        size_t n = s.e - s.b;
        if (status && s.stream == 0) {
            o.tmp.clear();
            AppendStatusErase(o.tmp, width);
            o.tmp.append(p, n);
            if (i == last0) { AppendStatusDraw(o.tmp, width); if (g_scr.shown) g_statusDraws.fetch_add(1, std::memory_order_relaxed); }
            p = o.tmp.data();
            n = o.tmp.size();
        }
        WriteRawLocked(o, s.stream, p, n);
    }
}

// Writer thread, no log batch: redraws the status line when its text / the width changed.
void StatusRedraw(OutBufs& o, bool checkWidth) {
    AcquireSRWLockExclusive(&g_conLock);
    if (g_con.tty && g_consoleOn.load()) {
        int width = 0;
        if (checkWidth && g_scr.shown) {
            width = ConsoleWidthLocked();
            if (width != g_scr.width) g_scr.dirty = true;
        }
        if (g_scr.dirty) {
            if (!width) width = ConsoleWidthLocked();
            o.tmp.clear();
            AppendStatusErase(o.tmp, width);
            AppendStatusDraw(o.tmp, width);
            if (!o.tmp.empty()) WriteRawLocked(o, 0, o.tmp.data(), o.tmp.size());
            if (g_scr.shown) g_statusDraws.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockExclusive(&g_conLock);
}

// Erases the status line now (SetLogConsole(false), shutdown). Under g_conLock.
void StatusEraseLocked() {
    if (!g_scr.shown) return;
    std::string s;
    AppendStatusErase(s, ConsoleWidthLocked());
    OutBufs o;
    WriteRawLocked(o, 0, s.data(), s.size());
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
    if (!o.segs.empty()) {
        AcquireSRWLockExclusive(&g_conLock);
        // Re-checked under the lock: after SetLogConsole(false) returns nothing reaches the
        // console any more (a dashboard may own the screen); the file and the sink still get it.
        if (g_consoleOn.load()) WriteConsoleLocked(o);
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
    if (g_sinkSet.load() && !t_inSink) {
        AcquireSRWLockExclusive(&g_sinkLock);
        CallSinkLocked(level, t, msg, len);
        ReleaseSRWLockExclusive(&g_sinkLock);
    }
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

    // The sink lock is held for the whole batch: SetLogSink(nullptr) waits for it to finish.
    const bool sink = (head != tail || dropped) && g_sinkSet.load();
    if (sink) AcquireSRWLockExclusive(&g_sinkLock);

    uint64_t n = 0;
    uint64_t released = tail;
    for (uint64_t pos = tail; pos < head;) {
        const RecHdr* h = reinterpret_cast<const RecHdr*>(g_ring + (pos & (kRingBytes - 1)));
        if (h->level != kPadLevel) {
            AppendRecord(o, h->level, h->t, reinterpret_cast<const char*>(h + 1), h->len, toConsole, toFile);
            if (sink) CallSinkLocked(h->level, h->t, reinterpret_cast<const char*>(h + 1), h->len);
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
        const double t = Qpc::ToSec(Qpc::Now() - LogEpoch());
        AppendRecord(o, (int)LogLevel::Warn, t, msg, (size_t)std::max(len, 0), toConsole, toFile);
        if (sink) CallSinkLocked((int)LogLevel::Warn, t, msg, (size_t)std::max(len, 0));
    }
    if (sink) ReleaseSRWLockExclusive(&g_sinkLock);
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
    // Status line: latest requested text, adopted at most every kStatusMinMs.
    const bool tty = g_ttyCache.load() == 1;
    std::string pending;
    pending.reserve(1024);
    uint64_t statusGen = 0;
    bool pendingNew = false;
    int64_t lastAdopt = 0, lastWidthCheck = 0;
    for (;;) {
        WaitForSingleObject(ev, kWriterPeriodMs);
        const bool stop = g_stop.load();
        if (g_paused.load() && !stop) continue;
        if (tty && !stop) {
            const uint64_t sg = g_statusGen.load();
            if (sg != statusGen) {
                statusGen = sg;
                AcquireSRWLockShared(&g_statusLock);
                pending.assign(g_statusText);
                ReleaseSRWLockShared(&g_statusLock);
                // Control characters (newline, CR, tab, backspace, bell, ...) would move the cursor
                // off the status line: spaces. ESC is kept for the style:: colours.
                for (char& c : pending)
                    if (((unsigned char)c < 0x20 && c != '\x1b') || c == '\x7f') c = ' ';
                pendingNew = true;
            }
            const int64_t now = Qpc::Now();
            if (pendingNew && (pending.empty() || Qpc::ToMs(now - lastAdopt) >= kStatusMinMs)) {
                AcquireSRWLockExclusive(&g_conLock);
                if (g_scr.want != pending) { g_scr.want = pending; g_scr.dirty = true; }
                ReleaseSRWLockExclusive(&g_conLock);
                pendingNew = false;
                lastAdopt = now;
            }
        }
        const uint64_t gen = g_flushReqGen.load();   // read before the ring snapshot (see LogFlush)
        const bool flushNow = gen != handledGen;
        DrainOnce(o, flushNow);
        if (tty && !stop) {
            // Redraw for new text / console resize when no log batch did it (width polled ~4x/s).
            const int64_t now = Qpc::Now();
            const bool checkWidth = Qpc::ToMs(now - lastWidthCheck) >= 250.0;
            if (checkWidth) lastWidthCheck = now;
            AcquireSRWLockShared(&g_conLock);
            const bool need = g_scr.dirty || (checkWidth && g_scr.shown);
            ReleaseSRWLockShared(&g_conLock);
            if (need) StatusRedraw(o, checkWidth);
        }
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
            // Remove the status line: later (synchronous) output starts at column 0.
            AcquireSRWLockExclusive(&g_conLock);
            if (g_consoleOn.load()) StatusEraseLocked();
            g_scr.shown = false;
            g_scr.want.clear();
            g_scr.dirty = false;
            ReleaseSRWLockExclusive(&g_conLock);
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

void SetLogConsole(bool enabled) {
    ConsoleReady();
    AcquireSRWLockExclusive(&g_conLock);   // waits for a console write in progress
    const bool was = g_consoleOn.exchange(enabled);
    if (was && !enabled) StatusEraseLocked();
    if (!was && enabled && !g_scr.want.empty()) g_scr.dirty = true;
    ReleaseSRWLockExclusive(&g_conLock);
    if (enabled && !was) if (HANDLE e = g_wakeEvent.load()) SetEvent(e);
}

void SetLogSink(LogSinkFn fn, void* ctx) {
    if (t_inSink) {   // called from inside the sink: this thread already holds g_sinkLock
        g_sinkFn = fn; g_sinkCtx = ctx;
        g_sinkSet.store(fn != nullptr);
        return;
    }
    AcquireSRWLockExclusive(&g_sinkLock);
    g_sinkFn = fn; g_sinkCtx = ctx;
    g_sinkSet.store(fn != nullptr);
    ReleaseSRWLockExclusive(&g_sinkLock);
}

bool LogConsoleIsTty() {
    int v = g_ttyCache.load(std::memory_order_acquire);
    if (v < 0) { ConsoleReady(); v = g_ttyCache.load(std::memory_order_acquire); }
    return v == 1;
}

void SetStatusLine(std::string_view utf8) {
    if (!LogConsoleIsTty()) return;
    if (g_state.load(std::memory_order_acquire) == kNotStarted) StartWriter();
    if (g_state.load() != kRunning) return;
    AcquireSRWLockExclusive(&g_statusLock);
    const bool same = std::string_view(g_statusText) == utf8;
    if (!same) g_statusText.assign(utf8.data(), utf8.size());
    ReleaseSRWLockExclusive(&g_statusLock);
    if (same) return;
    // No SetEvent: the writer polls every kWriterPeriodMs and the text is rate-limited to
    // kStatusMinMs anyway, so the caller (the app's render thread) saves a kernel call and
    // never makes the writer runnable / preempt it.
    g_statusGen.fetch_add(1, std::memory_order_release);
}

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
        if (!t_inSink) WaitFinalDrain();   // keep this thread's order (the writer itself must not wait for itself)
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
    c.statusDraws = g_statusDraws.load();
    return c;
}

// ============================================================== console styling

namespace style {
bool Enabled() { return LogConsoleIsTty(); }
static std::string Wrap(const char* on, std::string_view s, const char* off) {
    if (!Enabled()) return std::string(s);
    std::string r;
    r.reserve(s.size() + 24);
    r.append(on); r.append(s); r.append(off);
    return r;
}
std::string Dim(std::string_view s) { return Wrap(kSgrDim, s, kSgrFgOff); }
std::string Bold(std::string_view s) { return Wrap("\x1b[1m", s, "\x1b[22m"); }
std::string Accent(std::string_view s) {
    if (!Enabled()) return std::string(s);
    return Wrap(g_con.truecolor ? "\x1b[38;2;217;119;87m" : "\x1b[38;5;173m", s, kSgrFgOff);   // g_con is immutable once Enabled() ran
}
std::string Ok(std::string_view s) { return Wrap("\x1b[32m", s, kSgrFgOff); }
std::string Warn(std::string_view s) { return Wrap(kSgrYellow, s, kSgrFgOff); }
std::string Err(std::string_view s) { return Wrap(kSgrRed, s, kSgrFgOff); }
int Width(std::string_view s, bool ambiguousWide) { return TextWidth(s, ambiguousWide); }
} // namespace style

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
