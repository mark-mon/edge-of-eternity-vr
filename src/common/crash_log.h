#pragma once
namespace uxr {
// Vectored exception logger (observes only): fatal exception codes are logged with module+RVA and a stack.
void InstallCrashLog();
// Watchdog (started by InstallCrashLog): which = 0 main thread, 1 render thread. If the main thread stops beating for
// 4 s, the stacks of both threads are logged once.
void WatchdogSetThread(int which, unsigned long tid);
void WatchdogBeat(int which);
}
