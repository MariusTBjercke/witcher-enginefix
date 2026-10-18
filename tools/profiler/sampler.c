// Minimal sampling profiler for 32-bit witcher.exe.
// Every N ms it briefly suspends each busy thread, records EIP plus an EBP-chain call stack, and resumes it.
// Usage: sampler.exe <pid> <seconds> <out.csv> [interval_ms]
// Output: one line per sample: ms,tid,eip,ret1,ret2,...  (hex, absolute addresses)
// Build as 32-bit (vcvars32) so the CONTEXT layout matches the target.

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#define MAX_THREADS 256
#define MAX_FRAMES  16

typedef struct { DWORD tid; HANDLE h; ULONGLONG lastCpu; DWORD lastActive; } Thr;

static Thr g_thr[MAX_THREADS];
static int g_nthr;

static ULONGLONG CpuTime(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

static void RefreshThreads(DWORD pid)
{
    THREADENTRY32 te = { sizeof te };
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    BOOL ok;
    for (ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) {
        int i, known = 0;
        if (te.th32OwnerProcessID != pid) continue;
        for (i = 0; i < g_nthr; i++) if (g_thr[i].tid == te.th32ThreadID) known = 1;
        if (!known && g_nthr < MAX_THREADS) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (h) { g_thr[g_nthr].tid = te.th32ThreadID; g_thr[g_nthr].h = h; g_thr[g_nthr].lastCpu = CpuTime(h); g_thr[g_nthr].lastActive = timeGetTime(); g_nthr++; }
        }
    }
    CloseHandle(s);
}

int main(int argc, char **argv)
{
    DWORD pid, start, now, interval;
    int seconds, i, f, samples = 0, refresh = 0;
    HANDLE proc;
    FILE *out;

    if (argc < 4) { fprintf(stderr, "usage: sampler <pid> <seconds> <out.csv> [interval_ms]\n"); return 1; }
    pid = atoi(argv[1]); seconds = atoi(argv[2]); interval = argc > 4 ? atoi(argv[4]) : 2;
    proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { fprintf(stderr, "OpenProcess failed (%lu)\n", GetLastError()); return 1; }
    if (fopen_s(&out, argv[3], "w") != 0) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }

    timeBeginPeriod(1);
    RefreshThreads(pid);
    start = timeGetTime();
    while ((now = timeGetTime()) - start < (DWORD)seconds * 1000) {
        if (++refresh % 500 == 0) RefreshThreads(pid);
        for (i = 0; i < g_nthr; i++) {
            CONTEXT ctx;
            DWORD stack[MAX_FRAMES], ebp;
            int n = 0;
            ULONGLONG cpu = CpuTime(g_thr[i].h);
            // only sample threads that have used CPU recently (thread CPU time only advances per clock tick)
            if (cpu != g_thr[i].lastCpu) { g_thr[i].lastCpu = cpu; g_thr[i].lastActive = now; }
            if (now - g_thr[i].lastActive > 100) continue;
            if (SuspendThread(g_thr[i].h) == (DWORD)-1) continue;
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(g_thr[i].h, &ctx)) {
                stack[n++] = ctx.Eip;
                ebp = ctx.Ebp;
                for (f = 1; f < MAX_FRAMES && ebp; f++) {
                    DWORD frame[2]; SIZE_T got;
                    if (!ReadProcessMemory(proc, (LPCVOID)ebp, frame, sizeof frame, &got) || got != sizeof frame) break;
                    if (frame[0] <= ebp) break; // stack grows down; frames must move up
                    stack[n++] = frame[1];
                    ebp = frame[0];
                }
            }
            ResumeThread(g_thr[i].h);
            if (n) {
                fprintf(out, "%lu,%lu", now - start, g_thr[i].tid);
                for (f = 0; f < n; f++) fprintf(out, ",%08lx", stack[f]);
                fputc('\n', out);
                samples++;
            }
        }
        Sleep(interval);
    }
    timeEndPeriod(1);

    // module map so non-witcher.exe samples can be attributed: "#mod,base,size,name"
    {
        MODULEENTRY32 me = { sizeof me };
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        BOOL ok;
        for (ok = Module32First(s, &me); ok; ok = Module32Next(s, &me))
            fprintf(out, "#mod,%08lx,%08lx,%s\n", (DWORD)(DWORD_PTR)me.modBaseAddr, me.modBaseSize, me.szModule);
        CloseHandle(s);
    }
    fclose(out);
    printf("%d samples from %d threads in %d s\n", samples, g_nthr, seconds);
    return 0;
}
