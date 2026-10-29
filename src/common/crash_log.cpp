// Logs fatal-looking exceptions (access violation, stack overflow, illegal instruction ...) with the faulting module +
// RVA and a short stack in module RVAs. A vectored handler only observes: it never handles anything, so the game's and
// Unity's own handlers keep working. At most a few reports per run.
#include "crash_log.h"
#include "log.h"
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace uxr {

static std::atomic<int> g_reports{0};
static PVOID g_handle = nullptr;

static void Describe(void* addr, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        snprintf(out, n, "%s+0x%llx", base ? base + 1 : name, (unsigned long long)((char*)addr - (char*)mod));
    } else {
        snprintf(out, n, "%p", addr);
    }
}

static LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
                 code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ||
                 code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == 0xC0000409 /* fast fail / GS */ ||
                 code == EXCEPTION_IN_PAGE_ERROR;
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
    int n = g_reports.fetch_add(1);
    if (n >= 6) return EXCEPTION_CONTINUE_SEARCH;
    char where[300];
    Describe(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    unsigned long long info0 = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    unsigned long long info1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    LOGE("EXCEPTION 0x%08lx at %s (thread %lu) info %llu 0x%llx  [may be handled by the game; report %d]", code, where,
         GetCurrentThreadId(), info0, info1, n + 1);
    // Walk the faulting thread's stack from the exception context.
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 24 && ctx.Rip; i++) {
        char fr[300];
        Describe((void*)ctx.Rip, fr, sizeof(fr));
        LOGE("    #%02d %s", i, fr);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {   // leaf function: return address is at RSP
            if (IsBadReadPtr((void*)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- watchdog: logs the stacks of stalled threads (main, render) once per stall
static DWORD g_tid[2] = {};
static std::atomic<DWORD> g_beat[2] = {};
static const char* kThreadName[2] = {"main", "render"};

static void LogThreadStack(DWORD tid, const char* name) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { LOGE("watchdog: cannot open %s thread %lu", name, tid); return; }
    if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); return; }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    char lines[24][300];
    int n = 0;
    if (GetThreadContext(h, &ctx)) {
        for (; n < 24 && ctx.Rip; n++) {
            Describe((void*)ctx.Rip, lines[n], sizeof(lines[n]));
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                if (IsBadReadPtr((void*)ctx.Rsp, 8)) { n++; break; }
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
                continue;
            }
            void* hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
        }
    }
    ResumeThread(h);   // resume before logging (the logger may take a lock the thread holds)
    CloseHandle(h);
    LOGE("watchdog: %s thread %lu stack:", name, tid);
    for (int i = 0; i < n; i++) LOGE("    #%02d %s", i, lines[i]);
    LogFlush();
}

static DWORD WINAPI WatchdogThread(void*) {
    bool reported[2] = {};
    for (;;) {
        Sleep(1000);
        DWORD now = GetTickCount();
        DWORD mb = g_beat[0].load();
        if (!g_tid[0] || !mb) continue;
        if (now - mb > 4000) {
            if (!reported[0]) {
                reported[0] = true;
                LOGE("watchdog: main thread has not ticked for %lu ms", now - mb);
                LogThreadStack(g_tid[0], kThreadName[0]);
                if (g_tid[1]) LogThreadStack(g_tid[1], kThreadName[1]);
            }
        } else if (reported[0]) {
            LOGI("watchdog: main thread ticking again");
            reported[0] = false;
        }
    }
}

void WatchdogSetThread(int which, DWORD tid) { if (which >= 0 && which < 2 && g_tid[which] != tid) g_tid[which] = tid; }
void WatchdogBeat(int which) { if (which >= 0 && which < 2) g_beat[which] = GetTickCount(); }

void InstallCrashLog() {
    static bool wd = false;
    if (!wd) { wd = true; CloseHandle(CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr)); }
    if (!g_handle) g_handle = AddVectoredExceptionHandler(0 /* last */, Handler);
}

} // namespace uxr
