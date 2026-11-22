#pragma once
#include <cstdarg>
#include <cstdio>

namespace check {

inline int g_failures = 0;

inline const char* Verdict(bool ok) {
    if (!ok) g_failures++;
    return ok ? "PASS" : "FAIL";
}

inline bool Expect(bool ok, const char* format, ...) {
    printf("  %s  ", Verdict(ok));
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    return ok;
}

inline int Summary() {
    printf("\n%s\n", g_failures ? "SOME TESTS FAILED" : "ALL PASS");
    return g_failures ? 1 : 0;
}

}
