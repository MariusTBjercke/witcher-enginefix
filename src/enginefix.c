// Witcher EE engine fix - loaded as a dinput8.dll proxy from the game's System folder.
// Forwards DirectInput8Create to the real system dinput8.dll and patches the engine in memory.
// witcher.exe on disk is never touched. Delete System\dinput8.dll to uninstall.
// Every fix checks the original bytes first and is skipped (and logged) if they do not match.
//
// Fix 1: death-effect NULL creator (crash at witcher.exe+0x579DF8, 0x979DF8).
//   FUN_00979d50 ("Applying Death Effect to %x") looks up the effect's creator by object ID.
//   If the creator no longer exists (killed/despawned) the lookup returns NULL and the code reads
//   [NULL+0x938]. We add `test ebx,ebx` and take the existing "deleted death" exit (0x979E12),
//   which is the same path the game takes when the creator does not match.
//
// Fix 2: main thread pinned to CPU 0.
//   The engine timer (FUN_007fdc80) calls SetThreadAffinityMask(GetCurrentThread(), 1) on the main
//   thread because its clock uses RDTSC, which was not synchronised across cores on 2007-era CPUs.
//   Modern CPUs have an invariant, synchronised TSC, so the pin only forces the main thread to share
//   CPU 0 with interrupt/DPC work. We drop the call and restore the main thread's affinity.
//
// Fix 3 (experimental, off by default in the shipped ini): shader effect cache keep-alive.
//   Materials share D3DX effects through a name-keyed cache (FUN_00525ae0 get, FUN_00524db0 release),
//   which destroys an effect when its last user releases it. This keeps released effects alive
//   (refcount 0) and lets the lookup reuse them. It was written on a misread profile: the steady
//   d3dx9 time is ID3DXEffect::BeginPass/CommitChanges (FUN_0051fc00/0051fc50), not effect
//   recreation, so there is no measured benefit yet.
//
// Fix 4: save list rewrites every thumbnail.
//   Building the load/save list extracts each save's screenshot to temp\saves\screens\<id>.tga every
//   time (FUN_008a8f40), then the GUI opens that freshly written file twice. Opening a file that was
//   just modified costs ~1.5 ms (it gets scanned again), ~4.5 ms per save. We compare the existing
//   file with the thumbnail inside the save and skip the write when they are byte-identical, so the
//   file stays unchanged. Saves sharing a thumbnail name (autosaves, 000003.tga) still get rewritten.
//   The game wipes temp\ between sessions, so thumbnails are also kept in The Witcher\enginefix_thumbs
//   and hard-linked back into temp on the first list open of a session instead of being rewritten.
//
// Fix 5: saves compute zlib 1.1.4's byte-at-a-time crc32 over the whole save. Replaced with an
//   identical-result slicing-by-8 crc32.
//
// Fix 6: dangly nodes (hair and other spring-hung parts, 0x552B70) damp their velocity once per frame,
//   so above 60 FPS hair is damped more per second than at 60. The damping factor is raised to the
//   power dt*60, which gives the 60 FPS behaviour at any frame rate.
//
// Fix 7: the first load/save list after a restart opened every save cold (the antivirus scans each file
//   again on its first open), 3.5-6 s instead of ~1 s with ~575 saves. A low-priority thread opens and
//   reads each save (trailer, TOC, thumbnail) and its cached thumbnail once at startup. It only reads.
//
// Fix 8: D3DX builds the mip levels of DDS textures that have none (UI, fog of war) on the CPU, at every
//   load (~1.5 s of a ~2.8 s area transition). The levels it built are cached on disk and handed back
//   with the texture next time, so D3DX copies them instead of filtering (same bytes).
//
// Fix 9: after loads the main thread waits for the texture loader in Sleep(250) rounds (242 ms slept on an
//   already empty queue in one transition); it now polls with Sleep(1).
//
// Fix 10: during mouse look the hidden cursor is parked at half the GUI size (1365x768 at 4K) taken as window
//   pixels, so it reappears in the top-left area (active pause, Shift); it is now parked at the window's middle.
//
// Fix 11: while the window is inactive the main loop sleeps 1 s per frame (the game pauses in the background), so
//   after coming back it reacted up to 1 s late; the sleep now ends as soon as the window is in front again.
//
// Fix 12: with the over-the-shoulder camera chosen, Shift in the main menu worked as in a level: releasing it hid
//   the cursor and let the mouse turn a camera that is not there (the cursor stayed gone until Shift was held).
//   The Shift handler now leaves the cursor alone while the main menu is open.
//
// QoL 1 ([qol] ots_pause_mmb_camera): in the over-the-shoulder camera, active pause shows the cursor and
//   leaves only edge scrolling; holding the middle mouse button now turns the camera, as it does in the other cameras.
//
// Graphics detail ([graphics] detail=1 or 2, off by default): multiplies the engine's LOD, grass, shadow and light
//   distances (g_cAuroraSettings) on top of what the game and its options menu set.
//
// Each fix can be disabled in witcher_enginefix.ini next to the DLL ([fixes] fix2=0 etc.).
// QoL 3 ([qol] borderless, off by default): windowed mode gets a borderless window (instant alt-tab) that otherwise
//   behaves like fullscreen (cursor clip, edge camera turn, pause while in the background).
//
// QoL 4 ([qol] quick_loot, quick_loot_key): a hotkey (Space) presses "Take all" in the loot window and keeps Space's
//   pause elsewhere. All in Scripts\enginefix_hook.lua (bindkey + Data\Override\EngineFix\ef_lootall.ncs); the DLL only
//   logs its state. Steps aside when the Witcher Quick Loot mod is installed.
//
// QoL changes ([qol]) can be turned off the same way. Diagnostics ([debug]) are off unless the ini turns them on.

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <intrin.h>
#include <shlobj.h>

#pragma intrinsic(_ReturnAddress, _AddressOfReturnAddress)
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

#define ENGINEFIX_VERSION "0.6.3"

static HMODULE g_self;
static HMODULE g_realDinput;
static DWORD g_mainThreadId;
static volatile LONG g_patchState; // 0 = pending, 1 = done, 2 = gave up

// One WriteFile per line on a handle opened for appending: lines from several threads never get lost or
// mixed (fopen_s denies shared writes, so a second thread's line used to be dropped), and there is no
// lock that a thread killed at exit could leave held.
static void Log(const char *fmt, ...)
{
    char path[MAX_PATH], line[2048], *slash;
    va_list ap;
    SYSTEMTIME st;
    HANDLE h;
    DWORD written;
    int n, m;

    GetModuleFileNameA(g_self, path, MAX_PATH);
    slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? (slash + 1 - path) : 0), "witcher_enginefix.log");
    GetLocalTime(&st);
    n = sprintf_s(line, sizeof line, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    m = _vsnprintf_s(line + n, sizeof line - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n += m < 0 ? (int)strlen(line + n) : m;
    line[n++] = '\r';
    line[n++] = '\n';
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, line, n, &written, NULL);
    CloseHandle(h);
}

static BOOL BytesMatch(DWORD addr, const BYTE *bytes, size_t len)
{
    __try {
        return memcmp((void *)addr, bytes, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

static BOOL WriteCode(DWORD addr, const BYTE *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy((void *)addr, bytes, len);
    VirtualProtect((void *)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
    return TRUE;
}

static void IniPath(char *path) // MAX_PATH chars
{
    char *slash;
    GetModuleFileNameA(g_self, path, MAX_PATH);
    slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? (slash + 1 - path) : 0), "witcher_enginefix.ini");
}

static int IniInt(const char *section, const char *key, int def)
{
    char path[MAX_PATH];
    IniPath(path);
    return (int)GetPrivateProfileIntA(section, key, def, path);
}

static int IniFlag(const char *section, const char *key, int def) { return IniInt(section, key, def) != 0; }

static int FixEnabled(const char *key) { return IniFlag("fixes", key, 1); }

static void PutRel32(BYTE *at, DWORD target) // writes rel32 for an instruction ending at at+4
{
    *(DWORD *)at = target - (DWORD)(at + 4);
}

// ---- Fix 1: death-effect NULL creator ---------------------------------------

#define DEATH_SITE      0x00979DF8 // mov eax,[ebx+0x938]  (6 bytes)
#define DEATH_CONTINUE  0x00979DFE // cmp eax,[edi+4]
#define DEATH_ABORT     0x00979E12 // push "deleted death"; ... jmp 0x97A60F

static const BYTE kDeathOrig[] = { 0x8B, 0x83, 0x38, 0x09, 0x00, 0x00, 0x3B, 0x47, 0x04 };

static const char *ApplyDeathFix(void)
{
    BYTE *tramp, *p, patch[6];

    if (!BytesMatch(DEATH_SITE, kDeathOrig, sizeof kDeathOrig)) return "skipped (bytes differ)";
    tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return "FAILED (no memory)";

    p = tramp;
    *p++ = 0x85; *p++ = 0xDB;                                   // test ebx,ebx
    *p++ = 0x0F; *p++ = 0x84; PutRel32(p, DEATH_ABORT); p += 4; // jz  DEATH_ABORT
    memcpy(p, kDeathOrig, 6); p += 6;                           // mov eax,[ebx+0x938]
    *p++ = 0xE9; PutRel32(p, DEATH_CONTINUE); p += 4;           // jmp DEATH_CONTINUE
    FlushInstructionCache(GetCurrentProcess(), tramp, p - tramp);

    patch[0] = 0xE9;                                            // jmp tramp
    *(DWORD *)(patch + 1) = (DWORD)tramp - (DEATH_SITE + 5);
    patch[5] = 0x90;                                            // nop
    return WriteCode(DEATH_SITE, patch, 6) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Fix 2: main thread CPU 0 pin -------------------------------------------

#define AFFINITY_SITE 0x007FDC92 // call [SetThreadAffinityMask]  (args: GetCurrentThread(), 1)

// push 1 / mov [esi+2c],edi / call [GetCurrentThread] / push eax / call [SetThreadAffinityMask]
static const BYTE kAffinityOrig[] = { 0x6A, 0x01, 0x89, 0x7E, 0x2C, 0xFF, 0x15, 0x5C, 0xF1, 0xB3, 0x00,
                                      0x50, 0xFF, 0x15, 0x88, 0xF2, 0xB3, 0x00 };
static const BYTE kAffinityPatch[] = { 0x83, 0xC4, 0x08, 0x90, 0x90, 0x90 }; // add esp,8 (drop the 2 args)

static void UnpinMainThread(void)
{
    DWORD_PTR procMask, sysMask;
    HANDLE t;
    if (!g_mainThreadId || !GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) return;
    t = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, g_mainThreadId);
    if (!t) return;
    SetThreadAffinityMask(t, procMask);
    CloseHandle(t);
}

static const char *ApplyAffinityFix(void)
{
    const char *r;
    if (!BytesMatch(AFFINITY_SITE - 12, kAffinityOrig, sizeof kAffinityOrig)) r = "skipped (bytes differ)";
    else r = WriteCode(AFFINITY_SITE, kAffinityPatch, sizeof kAffinityPatch) ? "applied" : "FAILED (VirtualProtect)";
    UnpinMainThread(); // in case the timer was already created before we patched
    return r;
}

// ---- Fix 3: keep released effects in the cache --------------------------------

#define FXREL_SITE 0x00524DD5 // release: cmp [eax+24],0 / jnz keep  -> always jmp keep
#define FXGET_SITE 0x00525C12 // lookup:  cmp [eax+24],0 / jz  load  -> reuse even at refcount 0

static const BYTE kFxRelOrig[]  = { 0x8B, 0x06, 0x83, 0x78, 0x24, 0x00, 0x0F, 0x85, 0xB6, 0x00, 0x00, 0x00 };
static const BYTE kFxRelPatch[] = { 0xE9, 0xB7, 0x00, 0x00, 0x00, 0x90 }; // jmp 0x524E91
static const BYTE kFxGetOrig[]  = { 0x83, 0x78, 0x24, 0x00, 0x74, 0x22, 0x83, 0x3D, 0x50, 0x01, 0xC5, 0x00 };
static const BYTE kFxGetPatch[] = { 0x90, 0x90 };

static const char *ApplyEffectCacheFix(void)
{
    // patch both or neither: keeping effects alive without the lookup change would leak them
    if (!BytesMatch(FXREL_SITE - 6, kFxRelOrig, sizeof kFxRelOrig) ||
        !BytesMatch(FXGET_SITE - 4, kFxGetOrig, sizeof kFxGetOrig)) return "skipped (bytes differ)";
    if (!WriteCode(FXGET_SITE, kFxGetPatch, sizeof kFxGetPatch)) return "FAILED (VirtualProtect)";
    if (!WriteCode(FXREL_SITE, kFxRelPatch, sizeof kFxRelPatch)) return "FAILED (VirtualProtect)";
    return "applied";
}

// ---- Fix 4: don't rewrite unchanged save-list thumbnails --------------------------

#define THUMB_SITE   0x008AACCA // call FUN_008a8f40(FILE *save, TocEntry *entry, "screens")
#define THUMB_TARGET 0x008A8F40
#define MAX_THUMB    (256 * 1024)

typedef struct { DWORD size, offset; BYTE name[0x1C]; } TocEntry; // name is an msvcp80 std::string
typedef void (__cdecl *ThumbWrite_t)(void *, TocEntry *, const char *);
typedef const char *(__fastcall *StrCStr_t)(void *self, void *edx); // thiscall, no stack args
typedef int (__cdecl *FileNo_t)(void *);
typedef intptr_t (__cdecl *OsfHandle_t)(int);
static StrCStr_t g_strCStr;
static FileNo_t g_fileno;
static OsfHandle_t g_getOsfHandle;
static wchar_t g_tempSaves[MAX_PATH];  // %LOCALAPPDATA%\The Witcher\temp\saves (wiped by the game between sessions)
static wchar_t g_thumbCache[MAX_PATH]; // %LOCALAPPDATA%\The Witcher\enginefix_thumbs (kept)
static int g_thumbSkipped, g_thumbLinked, g_thumbWritten;
static volatile LONG g_listStarted; // a save list is being built: fix 7's warm-up stops

static BOOL ReadAt(HANDLE h, DWORD offset, void *buf, DWORD size)
{
    OVERLAPPED ov = { 0 };
    DWORD got = 0;
    ov.Offset = offset;
    return ReadFile(h, buf, size, &got, &ov) && got == size;
}

// Reads the thumbnail bytes out of the save; restores the OS file pointer so the CRT's FILE state is untouched.
static BOOL ReadThumbFromSave(void *save, TocEntry *entry, BYTE *buf)
{
    HANDLE h = (HANDLE)g_getOsfHandle(g_fileno(save));
    LARGE_INTEGER pos, zero = { 0 };
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE || !SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) return FALSE;
    ok = ReadAt(h, entry->offset, buf, entry->size);
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    return ok;
}

// TRUE when the file at path holds exactly size bytes equal to data.
static BOOL FileEquals(const wchar_t *path, const BYTE *data, DWORD size)
{
    static BYTE onDisk[MAX_THUMB];
    LARGE_INTEGER fileSize;
    BOOL same = FALSE;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSizeEx(h, &fileSize) && fileSize.QuadPart == size && ReadAt(h, 0, onDisk, size))
        same = memcmp(onDisk, data, size) == 0;
    CloseHandle(h);
    return same;
}

static void CreateDirs(const wchar_t *path) // creates every directory in path
{
    wchar_t tmp[MAX_PATH], *p;
    wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
    for (p = tmp + 3; *p; p++)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(tmp, NULL); *p = L'\\'; }
    CreateDirectoryW(tmp, NULL);
}

static void __cdecl ThumbWriteCached(void *save, TocEntry *entry, const char *sub)
{
    static BYTE fromSave[MAX_THUMB];
    wchar_t dest[MAX_PATH], cache[MAX_PATH], cacheDir[MAX_PATH];
    const char *name, *base;

    g_listStarted = 1;
    cache[0] = cacheDir[0] = 0;
    name = (entry->size && entry->size <= MAX_THUMB && sub) ? g_strCStr(entry->name, NULL) : NULL;
    if (!name || !ReadThumbFromSave(save, entry, fromSave)) goto write;
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    if (swprintf_s(dest, MAX_PATH, L"%ls\\%hs\\%hs", g_tempSaves, sub, base) < 0 ||
        swprintf_s(cacheDir, MAX_PATH, L"%ls\\%hs", g_thumbCache, sub) < 0 ||
        swprintf_s(cache, MAX_PATH, L"%ls\\%hs", cacheDir, base) < 0) goto write;

    // 1) temp copy already identical (second and later list opens in a session)
    if (FileEquals(dest, fromSave, entry->size)) {
        g_thumbSkipped++;
        if (!FileEquals(cache, fromSave, entry->size)) { // seed the cache from the temp copy
            CreateDirs(cacheDir);
            DeleteFileW(cache);
            CreateHardLinkW(cache, dest, NULL);
        }
        return;
    }

    // 2) first open in a session: the game wiped temp, but our cache has the same bytes -> hard link it
    if (FileEquals(cache, fromSave, entry->size)) {
        DeleteFileW(dest);
        if (CreateHardLinkW(dest, cache, NULL)) { g_thumbLinked++; return; }
    }

write:
    g_thumbWritten++;
    ((ThumbWrite_t)THUMB_TARGET)(save, entry, sub);
    // refresh the cache by hard-linking it to the file the game just wrote (no extra copy)
    if (cache[0] && g_thumbCache[0]) {
        CreateDirs(cacheDir);
        DeleteFileW(cache);
        CreateHardLinkW(cache, dest, NULL);
    }
}

static const char *ApplyThumbFix(void)
{
    HMODULE crt = GetModuleHandleA("msvcr80.dll"), cpp = GetModuleHandleA("msvcp80.dll");
    wchar_t local[MAX_PATH];
    BYTE orig[5] = { 0xE8 }, call[5] = { 0xE8 };
    *(DWORD *)(orig + 1) = (DWORD)(THUMB_TARGET - (THUMB_SITE + 5));
    if (!BytesMatch(THUMB_SITE, orig, 5)) return "skipped (bytes differ)";
    if (!crt || !cpp) return "skipped (CRT not loaded)";
    g_fileno = (FileNo_t)GetProcAddress(crt, "_fileno");
    g_getOsfHandle = (OsfHandle_t)GetProcAddress(crt, "_get_osfhandle");
    g_strCStr = (StrCStr_t)GetProcAddress(cpp, "?c_str@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEPBDXZ");
    if (!g_fileno || !g_getOsfHandle || !g_strCStr) return "skipped (CRT exports missing)";
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return "skipped (no LOCALAPPDATA)";
    swprintf_s(g_tempSaves, MAX_PATH, L"%ls\\The Witcher\\temp\\saves", local);
    swprintf_s(g_thumbCache, MAX_PATH, L"%ls\\The Witcher\\enginefix_thumbs", local);
    *(DWORD *)(call + 1) = (DWORD)ThumbWriteCached - (THUMB_SITE + 5);
    return WriteCode(THUMB_SITE, call, 5) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Fix 5: faster CRC32 for saves ------------------------------------------------
// The engine links zlib 1.1.4, whose crc32() (FUN_00800010, table at 0x00BC6610) processes one byte per
// table lookup. Saving (autosaves on area transitions) runs it over the whole save. We jump from its
// entry to a slicing-by-8 version with the identical result (same polynomial, same pre/post inversion,
// returns 0 for a NULL buffer). [debug] crc_timing logs time and bytes per burst of calls.

#define CRC_SITE  0x00800010 // uLong __cdecl crc32(uLong crc, const Bytef *buf, uInt len)
#define CRC_RESUME 0x00800016 // after the first two instructions (mov ecx,[esp+8]; test ecx,ecx)

static const BYTE kCrcOrig[] = { 0x8B, 0x4C, 0x24, 0x08, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3 };
typedef DWORD (__cdecl *Crc32_t)(DWORD, const BYTE *, DWORD);
static DWORD g_crcTable[8][256];
static Crc32_t g_crcOriginal; // trampoline into the game's own crc32
static int g_crcFast, g_crcTiming;
static volatile LONG g_crcCalls, g_crcBytes;
static volatile LONGLONG g_crcTicks;
static volatile LONG g_crcLastTick;

static void CrcInitTables(void)
{
    DWORD n, c, k;
    for (n = 0; n < 256; n++) {
        for (c = n, k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        g_crcTable[0][n] = c;
    }
    for (n = 0; n < 256; n++)
        for (k = 1; k < 8; k++)
            g_crcTable[k][n] = (g_crcTable[k - 1][n] >> 8) ^ g_crcTable[0][g_crcTable[k - 1][n] & 0xFF];
}

static DWORD CrcSlicing8(DWORD crc, const BYTE *buf, DWORD len)
{
    crc = ~crc;
    while (len && ((DWORD_PTR)buf & 3)) { crc = g_crcTable[0][(crc ^ *buf++) & 0xFF] ^ (crc >> 8); len--; }
    while (len >= 8) {
        DWORD one = *(const DWORD *)buf ^ crc, two = *(const DWORD *)(buf + 4);
        crc = g_crcTable[7][one & 0xFF] ^ g_crcTable[6][(one >> 8) & 0xFF] ^
              g_crcTable[5][(one >> 16) & 0xFF] ^ g_crcTable[4][one >> 24] ^
              g_crcTable[3][two & 0xFF] ^ g_crcTable[2][(two >> 8) & 0xFF] ^
              g_crcTable[1][(two >> 16) & 0xFF] ^ g_crcTable[0][two >> 24];
        buf += 8;
        len -= 8;
    }
    while (len--) crc = g_crcTable[0][(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static DWORD __cdecl Crc32Hook(DWORD crc, const BYTE *buf, DWORD len)
{
    LARGE_INTEGER a, b;
    DWORD r;
    if (!buf) return 0;
    if (!g_crcTiming) return g_crcFast ? CrcSlicing8(crc, buf, len) : g_crcOriginal(crc, buf, len);
    QueryPerformanceCounter(&a);
    r = g_crcFast ? CrcSlicing8(crc, buf, len) : g_crcOriginal(crc, buf, len);
    QueryPerformanceCounter(&b);
    InterlockedIncrement(&g_crcCalls);
    InterlockedExchangeAdd(&g_crcBytes, (LONG)len);
    InterlockedExchangeAdd64(&g_crcTicks, b.QuadPart - a.QuadPart);
    InterlockedExchange(&g_crcLastTick, (LONG)GetTickCount());
    return r;
}

// Logs one line per burst of crc32 calls (a save), once the calls have stopped for a second.
static DWORD WINAPI CrcTimingThread(LPVOID unused)
{
    LARGE_INTEGER f;
    (void)unused;
    QueryPerformanceFrequency(&f);
    for (;;) {
        Sleep(500);
        if (g_crcCalls && GetTickCount() - (DWORD)g_crcLastTick > 1000) {
            LONG calls = InterlockedExchange(&g_crcCalls, 0), bytes = InterlockedExchange(&g_crcBytes, 0);
            LONGLONG ticks = InterlockedExchange64(&g_crcTicks, 0);
            Log("crc32 (%s): %ld calls, %.1f MB, %.1f ms", g_crcFast ? "slicing-by-8" : "original",
                calls, bytes / 1048576.0, ticks * 1000.0 / f.QuadPart);
        }
    }
}

static const char *ApplyCrcFix(int fast, int timing)
{
    BYTE *tramp, jmp[5] = { 0xE9 };
    if (!BytesMatch(CRC_SITE, kCrcOrig, sizeof kCrcOrig)) return "skipped (bytes differ)";
    // trampoline for the original: its first 6 bytes, then jump back into it
    tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return "FAILED (no memory)";
    memcpy(tramp, kCrcOrig, 6);
    tramp[6] = 0xE9;
    PutRel32(tramp + 7, CRC_RESUME);
    FlushInstructionCache(GetCurrentProcess(), tramp, 11);
    g_crcOriginal = (Crc32_t)tramp;
    CrcInitTables();
    g_crcFast = fast;
    g_crcTiming = timing;
    if (timing) {
        HANDLE t = CreateThread(NULL, 0, CrcTimingThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    *(DWORD *)(jmp + 1) = (DWORD)Crc32Hook - (CRC_SITE + 5);
    return WriteCode(CRC_SITE, jmp, 5) ? (fast ? "applied" : "timing only (original crc32)") : "FAILED (VirtualProtect)";
}

// ---- Fix 6: frame-rate dependent dangly-node damping ---------------------------
// The dangly-node update (0x552B70, hair and other spring-hung parts) integrates with dt but damps
// the velocity once per frame: v *= damping (damping at [node data+0xC8]). Above 60 FPS the hair is
// damped more per second than at 60, below it less. We replace the load of the damping factor with
// a call that returns damping^(dt*60), the per-frame factor a 60 FPS frame of length dt would get.
// At 60 FPS the result is the same as the original.

#define DANGLY_SITE 0x005534FE // movss xmm0,[edi+0xC8]  (8 bytes); dt is in xmm1 here

static const BYTE kDanglyOrig[] = { 0xF3, 0x0F, 0x10, 0x87, 0xC8, 0x00, 0x00, 0x00,   // movss xmm0,[edi+0xC8]
                                    0xF3, 0x0F, 0x10, 0x96, 0x04, 0x01, 0x00, 0x00 }; // movss xmm2,[esi+0x104]
static int g_danglyLog; // log the first few calls, to confirm the hook runs in game

static float __cdecl DanglyDamping(float damping, float dt)
{
    float f = damping;
    if (damping > 0.0f && damping < 1.0f && dt >= 0.0f) f = (float)pow(damping, dt * 60.0f);
    if (g_danglyLog > 0) {
        g_danglyLog--;
        Log("fix 6: damping %.4f, dt %.2f ms -> %.4f", damping, dt * 1000.0f, f);
    }
    return f;
}

static const char *ApplyDanglyFix(void)
{
    static const BYTE save[] = { // sub esp,0x70 / movups [esp+i*16],xmm1..xmm7 / push eax,ecx,edx
        0x83, 0xEC, 0x70,
        0x0F, 0x11, 0x0C, 0x24,       0x0F, 0x11, 0x54, 0x24, 0x10, 0x0F, 0x11, 0x5C, 0x24, 0x20,
        0x0F, 0x11, 0x64, 0x24, 0x30, 0x0F, 0x11, 0x6C, 0x24, 0x40, 0x0F, 0x11, 0x74, 0x24, 0x50,
        0x0F, 0x11, 0x7C, 0x24, 0x60,
        0x50, 0x51, 0x52,
        0x83, 0xEC, 0x08,                               // sub esp,8
        0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x04,             // movss [esp+4],xmm1        (dt)
        0xF3, 0x0F, 0x10, 0x87, 0xC8, 0x00, 0x00, 0x00, // movss xmm0,[edi+0xC8]     (damping)
        0xF3, 0x0F, 0x11, 0x04, 0x24,                   // movss [esp],xmm0
    };
    static const BYTE restore[] = {
        0xD9, 0x1C, 0x24,                               // fstp dword [esp]
        0xF3, 0x0F, 0x10, 0x04, 0x24,                   // movss xmm0,[esp]
        0x83, 0xC4, 0x08,                               // add esp,8
        0x5A, 0x59, 0x58,                               // pop edx,ecx,eax
        0x0F, 0x10, 0x0C, 0x24,       0x0F, 0x10, 0x54, 0x24, 0x10, 0x0F, 0x10, 0x5C, 0x24, 0x20,
        0x0F, 0x10, 0x64, 0x24, 0x30, 0x0F, 0x10, 0x6C, 0x24, 0x40, 0x0F, 0x10, 0x74, 0x24, 0x50,
        0x0F, 0x10, 0x7C, 0x24, 0x60,
        0x83, 0xC4, 0x70,                               // add esp,0x70
        0xC3,                                           // ret
    };
    BYTE *stub, *p, patch[8] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90 }; // call stub / 3x nop

    if (!BytesMatch(DANGLY_SITE, kDanglyOrig, sizeof kDanglyOrig)) return "skipped (bytes differ)";
    stub = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return "FAILED (no memory)";
    p = stub;
    memcpy(p, save, sizeof save); p += sizeof save;
    *p++ = 0xE8; PutRel32(p, (DWORD)DanglyDamping); p += 4; // call; result in st0, args popped by add esp,8
    memcpy(p, restore, sizeof restore); p += sizeof restore;
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

    g_danglyLog = IniFlag("debug", "dangly_log", 0) ? 5 : 0;
    *(DWORD *)(patch + 1) = (DWORD)stub - (DANGLY_SITE + 5);
    return WriteCode(DANGLY_SITE, patch, sizeof patch) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Fix 7: warm the save files before the first save-list open ------------------
// Building the list opens every save (FUN_008aaa40: trailer, TOC, thumbnail) and fix 4 reads the cached
// thumbnail. After a restart each first open costs ~4-8 ms on NVMe, ~7 ms per save in all (Defender scans
// the file again; later opens are ~0.5 ms). This thread does the same opens and reads at startup, at
// below-normal priority, so the list finds them warm. Read only; it stops as soon as a list build starts.

#define MAX_TOC_BYTES (1024 * 1024)

static BOOL WarmSave(const wchar_t *path, BYTE *buf, LONGLONG *bytes, int *thumbs)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    LARGE_INTEGER size;
    DWORD trailer[3], toc, tocLen, p, n, i, entrySize, entryOffset;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    // trailer (last 12 bytes): {?, toc_offset, entry_count}; tools/savetoc.py also accepts the first field
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 12 || size.QuadPart > 0x7FFFFFFF ||
        !ReadAt(h, (DWORD)size.QuadPart - 12, trailer, 12)) goto done;
    *bytes += 12;
    toc = trailer[1] < (DWORD)size.QuadPart - 12 ? trailer[1] : trailer[0];
    if (toc >= (DWORD)size.QuadPart - 12) goto done;
    tocLen = (DWORD)size.QuadPart - 12 - toc;
    if (tocLen > MAX_TOC_BYTES || !ReadAt(h, toc, buf, tocLen)) goto done;
    *bytes += tocLen;
    for (i = 0, p = 0; i < trailer[2] && p + 4 <= tocLen; i++, p += 12 + n) { // u32 name_len, name, u32 size, u32 offset
        n = *(DWORD *)(buf + p);
        if (p + 12 + n > tocLen) break;
        entrySize = *(DWORD *)(buf + p + 4 + n);
        entryOffset = *(DWORD *)(buf + p + 8 + n);
        if (n >= 4 && _strnicmp((char *)buf + p + 4 + n - 4, ".tga", 4) == 0 && entrySize && entrySize <= MAX_THUMB &&
            entryOffset < (DWORD)size.QuadPart) {
            char base[MAX_PATH];
            const char *b = (char *)buf + p + 4, *e = b + n, *s;
            wchar_t cache[MAX_PATH];
            HANDLE c;
            for (s = e; s > b && s[-1] != '\\'; s--) {}
            if (e - s >= MAX_PATH) break;
            memcpy(base, s, e - s);
            base[e - s] = 0;
            if (ReadAt(h, entryOffset, buf + MAX_TOC_BYTES, entrySize)) *bytes += entrySize;
            if (g_thumbCache[0] && swprintf_s(cache, MAX_PATH, L"%ls\\screens\\%hs", g_thumbCache, base) > 0 &&
                (c = CreateFileW(cache, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL))
                    != INVALID_HANDLE_VALUE) {
                if (ReadAt(c, 0, buf + MAX_TOC_BYTES, entrySize)) { *bytes += entrySize; (*thumbs)++; }
                CloseHandle(c);
            }
            break;
        }
    }
done:
    CloseHandle(h);
    return TRUE;
}

static DWORD WINAPI WarmSavesThread(LPVOID unused)
{
    wchar_t docs[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    BYTE *buf;
    LARGE_INTEGER f, a, b, o1, o2;
    LONGLONG bytes = 0, slowest = 0;
    int saves = 0, thumbs = 0;
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs))) return 0;
    swprintf_s(pattern, MAX_PATH, L"%ls\\The Witcher\\saves\\*.TheWitcherSave", docs);
    buf = VirtualAlloc(NULL, MAX_TOC_BYTES + MAX_THUMB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (g_listStarted || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            swprintf_s(path, MAX_PATH, L"%ls\\The Witcher\\saves\\%ls", docs, fd.cFileName);
            QueryPerformanceCounter(&o1);
            if (WarmSave(path, buf, &bytes, &thumbs)) saves++;
            QueryPerformanceCounter(&o2);
            if (o2.QuadPart - o1.QuadPart > slowest) slowest = o2.QuadPart - o1.QuadPart;
        } while (!g_listStarted && FindNextFileW(find, &fd));
        FindClose(find);
    }
    QueryPerformanceCounter(&b);
    VirtualFree(buf, 0, MEM_RELEASE);
    Log("fix 7: warmed %d saves and %d cached thumbnails (%.1f MB read) in %.0f ms, slowest %.1f ms%s", saves, thumbs,
        bytes / 1048576.0, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart, slowest * 1000.0 / f.QuadPart,
        g_listStarted ? " (stopped: a save list was opened)" : "");
    return 0;
}

static const char *ApplySaveWarmup(void)
{
    HANDLE t;
    if (!g_thumbCache[0]) { // fix 4 off or not applied: still warm the saves, just not the thumbnail cache
        wchar_t local[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
            swprintf_s(g_thumbCache, MAX_PATH, L"%ls\\The Witcher\\enginefix_thumbs", local);
    }
    t = CreateThread(NULL, 0, WarmSavesThread, NULL, 0, NULL);
    if (!t) return "FAILED (no thread)";
    CloseHandle(t);
    return "started";
}

// ---- Diagnostics: save-list timing --------------------------------------------
// Times the parts of building the load/save list (Lua_GetSavedGameList) by redirecting individual
// CALL rel32 instructions to generated stubs: Enter(id) -> original -> Leave(id). Logs one summary
// line per list build. Enabled with [debug] savelist_timing=1 (default on while we investigate).

enum { T_LIST, T_ENUM, T_VALIDATE, T_TOC, T_THUMB, T_ADDLUA, T_COUNT };
static const char *kTimingNames[T_COUNT] = { "total", "enumerate", "open+validate", "read TOC", "write thumbnail", "Lua AddSaveGame" };
static const struct { DWORD site, target; } kTimingSites[T_COUNT] = {
    { 0x0063EFC6, 0x004C41A0 }, // Lua wrapper -> CLoadSaveSystem::GetSavedGameList
    { 0x004C41CA, 0x00432990 }, // enumerate save folders
    { 0x004C42AA, 0x007824A0 }, // per save: open file, check TOC, extract thumbnail
    { 0x008AAC2F, 0x008AA1E0 }, //   read TOC entries
    { 0x008AACCA, 0x008A8F40 }, //   write screens\<id>.tga
    { 0x004C42FD, 0x0051CE10 }, // per save: call Lua AddSaveGame
};
static LARGE_INTEGER g_tStart[T_COUNT], g_qpf;
static LONGLONG g_tTotal[T_COUNT], g_tMax[T_COUNT];
static int g_tCount[T_COUNT];
static DWORD g_tRet[T_COUNT];
static BYTE *g_tStub[T_COUNT];
static DWORD g_tTarget[T_COUNT];

// While a list is being built, also log which files are opened and how long that takes, grouped by
// path (top entries only). Hooks CreateFileW in msvcr80.dll's import table, so every CRT open
// (fopen, _wfopen_s, _wsopen_s, ...) is seen regardless of which CRT function the game called.
#define MAX_OPEN_PATHS 256
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t g_realCreateFileW;
static volatile int g_inList;
static struct { wchar_t path[200]; int count; LONGLONG ticks; } g_opens[MAX_OPEN_PATHS];
static int g_nOpens, g_openOverflow;
static LONGLONG g_openTicks;
static int g_openCalls, g_openFailed;

static HANDLE WINAPI TimedCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disposition, DWORD flags, HANDLE templ)
{
    LARGE_INTEGER a, b;
    HANDLE r;
    int i;
    if (!g_inList || GetCurrentThreadId() != g_mainThreadId)
        return g_realCreateFileW(path, access, share, sa, disposition, flags, templ);
    QueryPerformanceCounter(&a);
    r = g_realCreateFileW(path, access, share, sa, disposition, flags, templ);
    QueryPerformanceCounter(&b);
    g_openTicks += b.QuadPart - a.QuadPart;
    g_openCalls++;
    if (r == INVALID_HANDLE_VALUE) g_openFailed++;
    for (i = 0; i < g_nOpens && wcsncmp(g_opens[i].path, path, 199) != 0; i++) {}
    if (i == g_nOpens) {
        if (g_nOpens == MAX_OPEN_PATHS) { g_openOverflow++; return r; }
        wcsncpy_s(g_opens[i].path, 200, path, _TRUNCATE);
        g_opens[i].count = 0;
        g_opens[i].ticks = 0;
        g_nOpens++;
    }
    g_opens[i].count++;
    g_opens[i].ticks += b.QuadPart - a.QuadPart;
    return r;
}

// Finds the IAT slot for dll!func in module mod and swaps in hook; returns the original pointer.
static void *HookImport(HMODULE mod, const char *dll, const char *func, void *hook)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    void *target = GetProcAddress(GetModuleHandleA(dll), func);
    for (; dir.VirtualAddress && imp->Name; imp++) {
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; iat->u1.Function; iat++) {
            if ((void *)iat->u1.Function == target) {
                DWORD old;
                void *orig = (void *)iat->u1.Function;
                if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return NULL;
                iat->u1.Function = (DWORD)hook;
                VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                return orig;
            }
        }
    }
    return NULL;
}

static void HookCreateFileOnce(void)
{
    HMODULE crt;
    if (g_realCreateFileW || !(crt = GetModuleHandleA("msvcr80.dll"))) return;
    g_realCreateFileW = (CreateFileW_t)HookImport(crt, "kernel32.dll", "CreateFileW", TimedCreateFileW);
    if (!g_realCreateFileW) // some kernel32 builds forward CreateFileW to kernelbase
        g_realCreateFileW = (CreateFileW_t)HookImport(crt, "kernelbase.dll", "CreateFileW", TimedCreateFileW);
    if (!g_realCreateFileW) {
        g_realCreateFileW = (CreateFileW_t)-1; // don't retry
        Log("debug: could not hook msvcr80 CreateFileW import");
    }
}

static void LogOpens(void)
{
    int i, j, best;
    Log("  CreateFileW (via CRT) during list: %d calls (%d failed), %.1f ms, %d distinct paths%s", g_openCalls,
        g_openFailed, g_openTicks * 1000.0 / g_qpf.QuadPart, g_nOpens, g_openOverflow ? " (+ more not tracked)" : "");
    for (j = 0; j < 12 && j < g_nOpens; j++) { // top 12 by time
        best = -1;
        for (i = 0; i < g_nOpens; i++)
            if (g_opens[i].count >= 0 && (best < 0 || g_opens[i].ticks > g_opens[best].ticks)) best = i;
        if (best < 0) break;
        Log("    %5d x %8.1f ms  %ls", g_opens[best].count, g_opens[best].ticks * 1000.0 / g_qpf.QuadPart, g_opens[best].path);
        g_opens[best].count = -1;
    }
    g_nOpens = g_openOverflow = g_openCalls = g_openFailed = 0;
    g_openTicks = 0;
}

static void __cdecl TimingEnter(int id)
{
    if (id == T_LIST) { HookCreateFileOnce(); g_inList = 1; g_listStarted = 1; }
    QueryPerformanceCounter(&g_tStart[id]);
}

static void __cdecl TimingLeave(int id)
{
    LARGE_INTEGER now;
    LONGLONG d;
    int i;
    QueryPerformanceCounter(&now);
    d = now.QuadPart - g_tStart[id].QuadPart;
    g_tTotal[id] += d;
    g_tCount[id]++;
    if (d > g_tMax[id]) g_tMax[id] = d;
    if (id != T_LIST) return;
    {
        char line[512];
        int n = 0;
        for (i = 0; i < T_COUNT; i++) {
            n += sprintf_s(line + n, sizeof line - n, "%s%s %.1f ms (n=%d, max %.2f)", i ? " | " : "", kTimingNames[i],
                           g_tTotal[i] * 1000.0 / g_qpf.QuadPart, g_tCount[i], g_tMax[i] * 1000.0 / g_qpf.QuadPart);
            g_tTotal[i] = g_tMax[i] = 0;
            g_tCount[i] = 0;
        }
        Log("save list: %s", line);
        if (g_thumbSkipped || g_thumbLinked || g_thumbWritten)
            Log("  thumbnails: %d unchanged (skipped), %d linked from cache, %d written", g_thumbSkipped, g_thumbLinked, g_thumbWritten);
        g_thumbSkipped = g_thumbLinked = g_thumbWritten = 0;
        g_inList = 0;
        if (g_realCreateFileW && g_realCreateFileW != (CreateFileW_t)-1) LogOpens();
    }
}

static BYTE *EmitCall(BYTE *p, DWORD target) { *p++ = 0xE8; PutRel32(p, target); return p + 4; }

static const char *ApplySaveListTiming(void)
{
    BYTE *mem, *p;
    int i;
    QueryPerformanceFrequency(&g_qpf);
    for (i = 0; i < T_COUNT; i++) {
        BYTE orig[5] = { 0xE8 }, ours[5] = { 0xE8 };
        *(DWORD *)(orig + 1) = kTimingSites[i].target - (kTimingSites[i].site + 5);
        *(DWORD *)(ours + 1) = (DWORD)ThumbWriteCached - (kTimingSites[i].site + 5);
        if (BytesMatch(kTimingSites[i].site, orig, 5)) g_tTarget[i] = kTimingSites[i].target;
        else if (BytesMatch(kTimingSites[i].site, ours, 5)) g_tTarget[i] = (DWORD)ThumbWriteCached; // fix 4 is in place
        else return "skipped (bytes differ)";
    }
    mem = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return "FAILED (no memory)";
    p = mem;
    for (i = 0; i < T_COUNT; i++) {
        BYTE *stub = p;
        g_tStub[i] = stub;
        *p++ = 0x60; *p++ = 0x6A; *p++ = (BYTE)i;                      // pushad; push id
        p = EmitCall(p, (DWORD)TimingEnter);                            // call TimingEnter
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04; *p++ = 0x61;             // add esp,4; popad
        *p++ = 0x8F; *p++ = 0x05; *(DWORD *)p = (DWORD)&g_tRet[i]; p += 4; // pop [ret slot]
        p = EmitCall(p, g_tTarget[i]);                                  // call original (same stack args)
        *p++ = 0xFF; *p++ = 0x35; *(DWORD *)p = (DWORD)&g_tRet[i]; p += 4; // push [ret slot]
        *p++ = 0x60; *p++ = 0x6A; *p++ = (BYTE)i;                      // pushad; push id
        p = EmitCall(p, (DWORD)TimingLeave);                            // call TimingLeave
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04; *p++ = 0x61;             // add esp,4; popad
        *p++ = 0xC3;                                                    // ret
    }
    FlushInstructionCache(GetCurrentProcess(), mem, p - mem);
    for (i = 0; i < T_COUNT; i++) { // redirect the call sites only once every stub is in place
        BYTE call[5] = { 0xE8 };
        *(DWORD *)(call + 1) = (DWORD)g_tStub[i] - (kTimingSites[i].site + 5);
        if (!WriteCode(kTimingSites[i].site, call, 5)) return "FAILED (VirtualProtect)";
    }
    return "applied";
}

// ---- Diagnostics: wait timing --------------------------------------------------
// The engine waits for other threads with Sleep polling loops. [debug] wait_timing=1 swaps the exe's
// Sleep import for a hook that sums the time per caller (return address) and logs one line per burst.
// It also wraps the texture wait that AurWaitForAllTexturesToLoad (module.luc OnLoaded) triggers:
// FUN_00588460 -> FUN_006e8170 polls the texture loader's pending queue with Sleep(250). The hook sleeps
// those 250 ms in 1 ms slices to see when the queue actually emptied; the game still waits the full time.
// The hook keeps no locks (Interlocked only): at exit other threads die wherever they are.

#define SLEEP_IAT         0x00B3F22C // witcher.exe import slot for kernel32!Sleep
#define TEXWAIT_SITE      0x005884A2 // call FUN_006e8170 (wait until the texture loader's queue is empty)
#define TEXWAIT_FUNC      0x006E8170
#define TEXWAIT_SLEEP     0x006E81DB // push 250; call ebp (Sleep) inside FUN_006e8170
#define TEXWAIT_SLEEP_RET 0x006E81E2
#define TEXLOADER_PTR     0x00C58C3C // texture loader; pending queue (0x94-byte items) begin/end at +0x28/+0x2C
#define MAX_SLEEP_SITES   64

static const BYTE kTexWaitSleepOrig[] = { 0x68, 0xFA, 0x00, 0x00, 0x00, 0xFF, 0xD5 };
static const BYTE kTexWaitSleepFix9[] = { 0x68, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xD5 }; // after fix 9
static const struct { DWORD ret; const char *name; } kSleepNames[] = {
    { TEXWAIT_SLEEP_RET, "texture wait" },
    { 0x0081A8A4, "resource wait, Sleep(5)" },
    { 0x00456064, "spinlock, Sleep(1)" },
    { 0x007536C4, "main loop, window inactive, Sleep(1000)" },
    { 0x007536B1, "main loop, configured sleep" },
    { 0x00753694, "main loop, requested sleep" },
};
typedef VOID (WINAPI *Sleep_t)(DWORD);
typedef void (__fastcall *TexWait_t)(void *loader, void *edx); // thiscall, no stack arguments
typedef struct {
    volatile LONG key; // return address, | 0x80000000 for the main thread; 0 = free slot
    volatile LONG calls, first, last;
    volatile LONGLONG askedMs, ticks, maxTicks;
} SleepSite;
static SleepSite g_sleepSites[MAX_SLEEP_SITES];
static Sleep_t g_realSleep;
static volatile LONG g_waitOff; // set at process detach: just sleep
static int g_texWaiting, g_texSleeps;
static LONGLONG g_texEmptyAt;

static SleepSite *SleepSiteFor(LONG key, int *isNew)
{
    int i;
    *isNew = 0;
    for (i = 0; i < MAX_SLEEP_SITES; i++) {
        LONG k = g_sleepSites[i].key;
        if (k == key) return &g_sleepSites[i];
        if (k == 0) {
            k = InterlockedCompareExchange(&g_sleepSites[i].key, key, 0);
            if (k == 0) { *isNew = 1; return &g_sleepSites[i]; }
            if (k == key) return &g_sleepSites[i];
        }
    }
    return NULL;
}

static const char *SleepSiteName(DWORD ret)
{
    int i;
    for (i = 0; i < (int)(sizeof kSleepNames / sizeof kSleepNames[0]); i++)
        if (kSleepNames[i].ret == ret) return kSleepNames[i].name;
    return NULL;
}

static int TexPendingEmpty(void)
{
    __try {
        BYTE *loader = *(BYTE **)TEXLOADER_PTR;
        DWORD begin = *(DWORD *)(loader + 0x28), end = *(DWORD *)(loader + 0x2C);
        return begin == 0 || begin == end;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int TexPendingCount(void *loader)
{
    __try {
        DWORD begin = *(DWORD *)((BYTE *)loader + 0x28), end = *(DWORD *)((BYTE *)loader + 0x2C);
        return begin ? (int)((end - begin) / 0x94) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static VOID WINAPI WaitSleepHook(DWORD ms)
{
    DWORD ret = (DWORD)_ReturnAddress();
    LARGE_INTEGER a, b;
    LONGLONG d, m;
    SleepSite *s;
    int isMain, isNew;

    if (g_waitOff) { g_realSleep(ms); return; }
    QueryPerformanceCounter(&a);
    if (ret == TEXWAIT_SLEEP_RET && g_texWaiting && GetCurrentThreadId() == g_mainThreadId) {
        // same total sleep as the game's, in 1 ms slices, noting when the pending queue went empty
        LONGLONG end = a.QuadPart + (LONGLONG)ms * g_qpf.QuadPart / 1000;
        g_texSleeps++;
        for (;;) {
            QueryPerformanceCounter(&b);
            if (!g_texEmptyAt && TexPendingEmpty()) g_texEmptyAt = b.QuadPart;
            if (b.QuadPart >= end) break;
            g_realSleep(1);
        }
    } else {
        g_realSleep(ms);
        QueryPerformanceCounter(&b);
    }
    d = b.QuadPart - a.QuadPart;
    isMain = GetCurrentThreadId() == g_mainThreadId;
    s = SleepSiteFor((LONG)(ret | (isMain ? 0x80000000 : 0)), &isNew);
    if (!s) return;
    if (InterlockedIncrement(&s->calls) == 1) InterlockedExchange(&s->first, (LONG)GetTickCount());
    InterlockedExchange(&s->last, (LONG)GetTickCount());
    InterlockedExchangeAdd64(&s->askedMs, ms);
    InterlockedExchangeAdd64(&s->ticks, d);
    while ((m = s->maxTicks) < d && InterlockedCompareExchange64(&s->maxTicks, d, m) != m) {}
    if (isNew) {
        const char *name = SleepSiteName(ret);
        Log("wait: first Sleep(%lu) from 0x%08lX%s%s%s (%s thread %lu)", ms, ret, name ? " (" : "", name ? name : "",
            name ? ")" : "", isMain ? "main" : "other", GetCurrentThreadId());
    }
}

// Logs a site once it has been quiet for 2 s, or every 60 s while it keeps sleeping.
static DWORD WINAPI WaitTimingThread(LPVOID unused)
{
    int i;
    (void)unused;
    for (;;) {
        Sleep(500);
        if (g_waitOff) return 0;
        for (i = 0; i < MAX_SLEEP_SITES && g_sleepSites[i].key; i++) {
            SleepSite *s = &g_sleepSites[i];
            DWORD now = GetTickCount(), ret = (DWORD)s->key & 0x7FFFFFFF;
            const char *name;
            LONG calls;
            LONGLONG asked, ticks, maxTicks;
            DWORD span;
            if (!s->calls || (now - (DWORD)s->last < 2000 && now - (DWORD)s->first < 60000)) continue;
            span = (DWORD)s->last - (DWORD)s->first;
            calls = InterlockedExchange(&s->calls, 0);
            asked = InterlockedExchange64(&s->askedMs, 0);
            ticks = InterlockedExchange64(&s->ticks, 0);
            maxTicks = InterlockedExchange64(&s->maxTicks, 0);
            name = SleepSiteName(ret);
            Log("wait: %s%s0x%08lX [%s thread]: %ld x Sleep, asked %I64d ms, slept %.1f ms (max %.1f ms) over %.1f s",
                name ? name : "", name ? " " : "", ret, s->key & 0x80000000 ? "main" : "other", calls, asked,
                ticks * 1000.0 / g_qpf.QuadPart, maxTicks * 1000.0 / g_qpf.QuadPart, span / 1000.0);
        }
    }
}

static void __fastcall TexWaitHook(void *loader, void *edx)
{
    LARGE_INTEGER a, b;
    int pending = TexPendingCount(loader);
    QueryPerformanceCounter(&a);
    g_texSleeps = 0;
    g_texEmptyAt = 0;
    g_texWaiting = 1;
    ((TexWait_t)TEXWAIT_FUNC)(loader, edx);
    g_texWaiting = 0;
    QueryPerformanceCounter(&b);
    if (!g_texSleeps)
        Log("texture wait (AurWaitForAllTexturesToLoad): %.1f ms, no sleep (%d pending at start)",
            (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart, pending);
    else
        Log("texture wait (AurWaitForAllTexturesToLoad): %.1f ms, %d x Sleep, %d pending at start, "
            "queue empty after %.1f ms (%.1f ms waited on an empty queue)",
            (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart, g_texSleeps, pending,
            ((g_texEmptyAt ? g_texEmptyAt : b.QuadPart) - a.QuadPart) * 1000.0 / g_qpf.QuadPart,
            (b.QuadPart - (g_texEmptyAt ? g_texEmptyAt : b.QuadPart)) * 1000.0 / g_qpf.QuadPart);
}

static const char *ApplyWaitTiming(void)
{
    static char result[128];
    BYTE call[5] = { 0xE8 };
    DWORD cur = 0, old;
    HMODULE mod;
    char modPath[MAX_PATH], *modName;
    HANDLE t;

    *(DWORD *)(call + 1) = TEXWAIT_FUNC - (TEXWAIT_SITE + 5);
    if (!BytesMatch(TEXWAIT_SITE, call, 5) || (!BytesMatch(TEXWAIT_SLEEP, kTexWaitSleepOrig, sizeof kTexWaitSleepOrig) &&
                                               !BytesMatch(TEXWAIT_SLEEP, kTexWaitSleepFix9, sizeof kTexWaitSleepFix9)))
        return "skipped (bytes differ)";
    __try { cur = *(DWORD *)SLEEP_IAT; } __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
    if (!cur || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)cur, &mod))
        return "skipped (Sleep import slot does not point into a module)";
    GetModuleFileNameA(mod, modPath, MAX_PATH);
    modName = strrchr(modPath, '\\');
    modName = modName ? modName + 1 : modPath;
    QueryPerformanceFrequency(&g_qpf);
    g_realSleep = (Sleep_t)cur;
    if (!VirtualProtect((void *)SLEEP_IAT, 4, PAGE_READWRITE, &old)) return "FAILED (VirtualProtect)";
    *(DWORD *)SLEEP_IAT = (DWORD)WaitSleepHook;
    VirtualProtect((void *)SLEEP_IAT, 4, old, &old);
    *(DWORD *)(call + 1) = (DWORD)TexWaitHook - (TEXWAIT_SITE + 5);
    if (!WriteCode(TEXWAIT_SITE, call, 5)) return "FAILED (VirtualProtect)";
    t = CreateThread(NULL, 0, WaitTimingThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    sprintf_s(result, sizeof result, "applied (Sleep import was %s+0x%lX)", modName, cur - (DWORD)mod);
    return result;
}

// ---- Fix 8 and diagnostics: texture creation ---------------------------------------
// Area loads spend 3-5 s in d3dx9 (public PDB): D3DXCreateTextureFromFileInMemoryEx -> D3DXFilterTexture ->
// box filter with DXT decode/encode, i.e. mipmaps built on the CPU. FUN_0053f910 passes the resource's DDS
// with MipLevels = D3DX_DEFAULT (full chain), Format = D3DFMT_FROM_FILE, MANAGED. Many DDS files (UI, fog of
// war) have no mips, and the game recreates them on every load (~70 ms for a 2048x1024 DXT5).
//
// Fix 8: for a DXT DDS with only the top level, the levels D3DX built are read back once (LockRect, read
// only) and kept in %LOCALAPPDATA%\The Witcher\enginefix_mips\<d3dx9 build>\, keyed by two checksums of the
// source (CRC32C and CRC32) and its size. Next time the texture is created from the same DDS with those
// levels appended, so D3DX copies them instead of filtering: the same bytes it produced before. Files on
// disk, not in memory: Windows' file cache keeps repeats fast without using the game's address space.
//
// [debug] texture_timing=1 logs, per burst of loads, how many textures came with all their levels, how many
// had levels generated, how many came from fix 8's cache, and the slowest textures.

#define TEXCREATE_SITE  0x0053FAC3 // call D3DXCreateTextureFromFileInMemoryEx (thunk) in FUN_0053f910
#define TEXCREATE_THUNK 0x00A750FE
#define TEX_TOP 12
#define MIP_MAGIC MAKEFOURCC('E', 'F', 'M', 'P')
#define MIP_VERSION 3

typedef HRESULT (WINAPI *TexCreate_t)(void *dev, const BYTE *data, UINT size, UINT w, UINT h, UINT mips, DWORD usage,
                                      DWORD fmt, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey,
                                      void *info, void *palette, void **tex);
typedef struct { double ms; UINT w, h, fileMips, dataLevels, levels, outW, outH; DWORD fourcc, bits, outFmt; int cached; char name[48]; } TexRec;
typedef struct { DWORD magic, version, w, h, fourcc, levels, bytes, crc32c, crc32, size, levelsCrc; } MipFileHeader;
static struct {
    int n, nGen, nResized, nOther, nCached, nStored;
    LONGLONG ticks, ticksGen, ticksOther, ticksCached;
    TexRec top[TEX_TOP];
    int nTop;
    DWORD last;
} g_tex;
static CRITICAL_SECTION g_texLock;
static int g_texTiming, g_mipFix;
static wchar_t g_mipDir[MAX_PATH];
static volatile LONG g_mipHits, g_mipStores, g_mipFailures;

static UINT DdsLevelBytes(UINT w, UINT h, DWORD fourcc, UINT bits)
{
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '1') || fourcc == MAKEFOURCC('A', 'T', 'I', '1'))
        return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (fourcc) return ((w + 3) / 4) * ((h + 3) / 4) * 16; // DXT2-5, ATI2
    return w * h * (bits / 8);
}

static BOOL IsDxt(DWORD fourcc)
{
    return fourcc == MAKEFOURCC('D', 'X', 'T', '1') || fourcc == MAKEFOURCC('D', 'X', 'T', '3') ||
           fourcc == MAKEFOURCC('D', 'X', 'T', '5');
}

static void FormatName(char *out, size_t len, DWORD fmt, UINT bits)
{
    if (fmt > 0xFF) sprintf_s(out, len, "%c%c%c%c", (char)fmt, (char)(fmt >> 8), (char)(fmt >> 16), (char)(fmt >> 24));
    else if (bits) sprintf_s(out, len, "RGB%u", bits);
    else sprintf_s(out, len, "fmt%lu", fmt);
}

static DWORD Crc32c(const BYTE *p, DWORD len) // SSE4.2 crc32 instruction (checked in ApplyMipCache)
{
    DWORD c = 0xFFFFFFFF;
    for (; len >= 4; p += 4, len -= 4) c = _mm_crc32_u32(c, *(const DWORD *)p);
    for (; len; p++, len--) c = _mm_crc32_u8(c, *p);
    return ~c;
}

static UINT FullChain(UINT w, UINT h)
{
    UINT n = 1, m = w > h ? w : h;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

// Levels kept in the cache: down to the last one with both sides >= 4. D3DX re-processes the partial
// DXT blocks of smaller levels when it loads them, so those it builds itself (from the last cached level,
// as it did the first time), which keeps the result byte-identical.
static UINT CachedLevels(UINT w, UINT h)
{
    UINT n = 1;
    while ((w >> n) >= 4 && (h >> n) >= 4) n++;
    return n;
}

static void MipPath(wchar_t *out, DWORD c32c, DWORD c32, UINT size)
{
    swprintf_s(out, MAX_PATH, L"%ls\\%08lx%08lx_%x.mip", g_mipDir, c32c, c32, size);
}

// Returns a malloc'ed DDS = source + cached levels 1..n-1 (header updated), or NULL.
static BYTE *MipLoad(const BYTE *data, UINT size, const TexRec *r, DWORD c32c, DWORD c32, UINT keep, UINT *outSize)
{
    wchar_t path[MAX_PATH];
    MipFileHeader fh;
    HANDLE h;
    BYTE *buf = NULL;
    DWORD got;
    MipPath(path, c32c, c32, size);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (ReadFile(h, &fh, sizeof fh, &got, NULL) && got == sizeof fh && fh.magic == MIP_MAGIC && fh.version == MIP_VERSION &&
        fh.w == r->w && fh.h == r->h && fh.fourcc == r->fourcc && fh.levels == keep && fh.crc32c == c32c &&
        fh.crc32 == c32 && fh.size == size && fh.bytes < 64 * 1024 * 1024 && (buf = malloc(size + fh.bytes)) != NULL) {
        if (ReadFile(h, buf + size, fh.bytes, &got, NULL) && got == fh.bytes && Crc32c(buf + size, fh.bytes) == fh.levelsCrc) {
            memcpy(buf, data, size);
            *(DWORD *)(buf + 0x08) |= 0x20000;    // DDSD_MIPMAPCOUNT
            *(DWORD *)(buf + 0x1C) = keep;        // dwMipMapCount
            *(DWORD *)(buf + 0x6C) |= 0x400008;   // DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
            *outSize = size + fh.bytes;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    CloseHandle(h);
    return buf;
}

// Reads levels 1..n-1 back from the texture D3DX just built and writes them to the cache.
static BOOL MipStore(void *tex, const TexRec *r, DWORD c32c, DWORD c32, UINT size, UINT keep)
{
    void **vt = *(void ***)tex;
    UINT level, bytes = 0, blockSize = r->fourcc == MAKEFOURCC('D', 'X', 'T', '1') ? 8 : 16;
    BYTE *blob, *p;
    MipFileHeader fh;
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    HANDLE h;
    DWORD put;
    BOOL ok = TRUE;
    for (level = 1; level < keep; level++)
        bytes += DdsLevelBytes(max(1, r->w >> level), max(1, r->h >> level), r->fourcc, 0);
    if (!(blob = malloc(bytes))) return FALSE;
    for (p = blob, level = 1; ok && level < keep; level++) {
        UINT bw = max(1, ((max(1, r->w >> level)) + 3) / 4), bh = max(1, ((max(1, r->h >> level)) + 3) / 4), row;
        struct { INT pitch; BYTE *bits; } lr;
        if (((HRESULT (WINAPI *)(void *, UINT, void *, const RECT *, DWORD))vt[19])(tex, level, &lr, NULL, 0x10) != 0) {
            ok = FALSE; // LockRect(D3DLOCK_READONLY) failed
            break;
        }
        for (row = 0; row < bh; row++, p += bw * blockSize) memcpy(p, lr.bits + row * lr.pitch, bw * blockSize);
        ((HRESULT (WINAPI *)(void *, UINT))vt[20])(tex, level); // UnlockRect
    }
    if (ok) {
        fh.magic = MIP_MAGIC; fh.version = MIP_VERSION; fh.w = r->w; fh.h = r->h; fh.fourcc = r->fourcc;
        fh.levels = keep; fh.bytes = bytes; fh.crc32c = c32c; fh.crc32 = c32; fh.size = size;
        fh.levelsCrc = Crc32c(blob, bytes);
        MipPath(path, c32c, c32, size);
        swprintf_s(tmp, MAX_PATH, L"%ls.%lu.tmp", path, GetCurrentThreadId());
        h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) {
            ok = WriteFile(h, &fh, sizeof fh, &put, NULL) && put == sizeof fh && WriteFile(h, blob, bytes, &put, NULL) && put == bytes;
            CloseHandle(h);
            ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING); // never leave a half-written cache file
            if (!ok) DeleteFileW(tmp);
        }
    }
    free(blob);
    return ok;
}

static HRESULT WINAPI TexCreateHook(void *dev, const BYTE *data, UINT size, UINT w, UINT h, UINT mips, DWORD usage,
                                    DWORD fmt, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey,
                                    void *info, void *palette, void **tex)
{
    void *retSlot = _AddressOfReturnAddress(); // our 15 arguments follow it
    LARGE_INTEGER a, b;
    HRESULT hr = E_FAIL;
    TexRec r = { 0 };
    UINT full = 0, keep = 0, bigSize = 0;
    DWORD c32c = 0, c32 = 0;
    BYTE *big = NULL;
    int eligible = 0, called = 0;
    QueryPerformanceCounter(&a);
    __try {
        if (size >= 0x80 && *(DWORD *)data == MAKEFOURCC('D', 'D', 'S', ' ')) {
            UINT lw, lh, left = size - 0x80, lb;
            r.h = *(DWORD *)(data + 0x0C);
            r.w = *(DWORD *)(data + 0x10);
            r.fileMips = *(DWORD *)(data + 0x1C);
            r.fourcc = (*(DWORD *)(data + 0x50) & 4) ? *(DWORD *)(data + 0x54) : 0; // DDPF_FOURCC
            r.bits = *(DWORD *)(data + 0x58);
            for (lw = r.w, lh = r.h; r.dataLevels < 16 && (lb = DdsLevelBytes(lw, lh, r.fourcc, r.bits)) && left >= lb; r.dataLevels++) {
                left -= lb;
                if (lw == 1 && lh == 1) { r.dataLevels++; break; }
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
            // fix 8 handles exactly this: a DXT DDS holding only its top level, full chain requested, no resize
            full = r.w && r.h ? FullChain(r.w, r.h) : 0;
            keep = r.w && r.h ? CachedLevels(r.w, r.h) : 0;
            eligible = g_mipFix && IsDxt(r.fourcc) && r.dataLevels == 1 && r.fileMips <= 1 && keep > 1 &&
                       size == 0x80 + DdsLevelBytes(r.w, r.h, r.fourcc, 0) && mips == (UINT)-1 && w == (UINT)-1 &&
                       h == (UINT)-1 && fmt == (DWORD)-3 && !(r.w & (r.w - 1)) && !(r.h & (r.h - 1));
        }
        if (eligible) {
            c32c = Crc32c(data, size);
            c32 = CrcSlicing8(0, data, size);
            big = MipLoad(data, size, &r, c32c, c32, keep, &bigSize);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        eligible = 0;
        big = NULL;
    }
    if (big) {
        hr = ((TexCreate_t)TEXCREATE_THUNK)(dev, big, bigSize, w, h, mips, usage, fmt, pool, filter, mipFilter,
                                            colorKey, info, palette, tex);
        free(big);
        if (SUCCEEDED(hr)) { r.cached = 1; called = 1; InterlockedIncrement(&g_mipHits); }
        else InterlockedIncrement(&g_mipFailures); // retry the normal way below
    }
    if (!called)
        hr = ((TexCreate_t)TEXCREATE_THUNK)(dev, data, size, w, h, mips, usage, fmt, pool, filter, mipFilter, colorKey,
                                            info, palette, tex);
    __try {
        if (SUCCEEDED(hr) && tex && *tex) {
            void **vt = *(void ***)*tex;
            DWORD desc[8] = { 0 }; // D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, Quality, Width, Height
            r.levels = ((DWORD (WINAPI *)(void *))vt[13])(*tex);                         // GetLevelCount
            if (((HRESULT (WINAPI *)(void *, UINT, DWORD *))vt[17])(*tex, 0, desc) == 0) { // GetLevelDesc
                r.outFmt = desc[0];
                r.outW = desc[6];
                r.outH = desc[7];
            }
            if (eligible && !r.cached && r.levels == full && r.outFmt == r.fourcc && r.outW == r.w && r.outH == r.h) {
                if (MipStore(*tex, &r, c32c, c32, size, keep)) InterlockedIncrement(&g_mipStores);
                else InterlockedIncrement(&g_mipFailures);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    QueryPerformanceCounter(&b);
    if (!g_texTiming) return hr;
    r.ms = (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart;
    __try { // the caller sprintf'ed "<resource name>:<index>" into its frame at [esp+0x5C] before pushing our 15 args
        strncpy_s(r.name, sizeof r.name, (const char *)retSlot + 4 + 15 * 4 + 0x5C, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!TryEnterCriticalSection(&g_texLock)) return hr; // never block the game for a statistic
    g_tex.n++;
    g_tex.ticks += b.QuadPart - a.QuadPart;
    if (r.cached) { g_tex.nCached++; g_tex.ticksCached += b.QuadPart - a.QuadPart; }
    else if (r.w && r.levels > r.dataLevels) { g_tex.nGen++; g_tex.ticksGen += b.QuadPart - a.QuadPart; }
    else if (!r.w) { g_tex.nOther++; g_tex.ticksOther += b.QuadPart - a.QuadPart; }
    if (r.w && r.outW && r.outW != r.w) g_tex.nResized++;
    if (g_tex.nTop < TEX_TOP || r.ms > g_tex.top[g_tex.nTop - 1].ms) {
        int i = g_tex.nTop < TEX_TOP ? g_tex.nTop++ : TEX_TOP - 1;
        for (; i > 0 && g_tex.top[i - 1].ms < r.ms; i--) g_tex.top[i] = g_tex.top[i - 1];
        g_tex.top[i] = r;
    }
    g_tex.last = GetTickCount();
    LeaveCriticalSection(&g_texLock);
    return hr;
}

static DWORD WINAPI TexTimingThread(LPVOID unused)
{
    int i;
    (void)unused;
    for (;;) {
        Sleep(500);
        if (g_waitOff) return 0;
        EnterCriticalSection(&g_texLock);
        if (g_tex.n && GetTickCount() - g_tex.last > 2000) {
            double f = 1000.0 / g_qpf.QuadPart;
            LONG stores = InterlockedExchange(&g_mipStores, 0), fails = InterlockedExchange(&g_mipFailures, 0);
            InterlockedExchange(&g_mipHits, 0);
            Log("textures: %d created in %.1f ms | %d with levels generated: %.1f ms | %d levels from fix 8's cache: %.1f ms | "
                "%d with all levels in the data: %.1f ms | %d not DDS: %.1f ms | %d resized | fix 8 stored %ld, failed %ld",
                g_tex.n, g_tex.ticks * f, g_tex.nGen, g_tex.ticksGen * f, g_tex.nCached, g_tex.ticksCached * f,
                g_tex.n - g_tex.nGen - g_tex.nOther - g_tex.nCached,
                (g_tex.ticks - g_tex.ticksGen - g_tex.ticksOther - g_tex.ticksCached) * f, g_tex.nOther,
                g_tex.ticksOther * f, g_tex.nResized, stores, fails);
            for (i = 0; i < g_tex.nTop; i++) {
                TexRec *r = &g_tex.top[i];
                char inFmt[16], outFmt[16];
                FormatName(inFmt, sizeof inFmt, r->fourcc, r->bits);
                FormatName(outFmt, sizeof outFmt, r->outFmt, 0);
                Log("  %7.1f ms  %ux%u %s, header mips %u, %u in data -> %u levels %ux%u %s%s  %s", r->ms, r->w, r->h,
                    inFmt, r->fileMips, r->dataLevels, r->levels, r->outW, r->outH, outFmt, r->cached ? " (fix 8 cache)" : "",
                    r->name);
            }
            memset(&g_tex, 0, sizeof g_tex);
        }
        LeaveCriticalSection(&g_texLock);
    }
}

// The cache folder is per d3dx9_35.dll build (PE timestamp), so a different D3DX never reads another's levels.
static const char *MipCacheDir(void)
{
    HMODULE dx = GetModuleHandleA("d3dx9_35.dll");
    wchar_t local[MAX_PATH], parent[MAX_PATH];
    IMAGE_NT_HEADERS *nt;
    int cpu[4];
    __cpuid(cpu, 1);
    if (!(cpu[2] & (1 << 20))) return "skipped (CPU without SSE4.2)";
    if (!dx) return "skipped (d3dx9_35.dll not loaded)";
    nt = (IMAGE_NT_HEADERS *)((BYTE *)dx + ((IMAGE_DOS_HEADER *)dx)->e_lfanew);
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return "skipped (no LOCALAPPDATA)";
    swprintf_s(parent, MAX_PATH, L"%ls\\The Witcher\\enginefix_mips", local);
    swprintf_s(g_mipDir, MAX_PATH, L"%ls\\%08lx", parent, nt->FileHeader.TimeDateStamp);
    CreateDirs(g_mipDir);
    return NULL;
}

static const char *ApplyTextureHook(int mipFix, int timing, const char **mipResult)
{
    BYTE call[5] = { 0xE8 };
    HANDLE t;
    *mipResult = "disabled in ini";
    *(DWORD *)(call + 1) = TEXCREATE_THUNK - (TEXCREATE_SITE + 5);
    if (!BytesMatch(TEXCREATE_SITE, call, 5)) { *mipResult = "skipped (bytes differ)"; return "skipped (bytes differ)"; }
    if (mipFix) {
        const char *err = MipCacheDir();
        if (err) { *mipResult = err; mipFix = 0; }
        else { *mipResult = "applied"; CrcInitTables(); }
    }
    if (!mipFix && !timing) return "disabled in ini";
    QueryPerformanceFrequency(&g_qpf);
    g_mipFix = mipFix;
    g_texTiming = timing;
    if (timing) {
        InitializeCriticalSection(&g_texLock);
        t = CreateThread(NULL, 0, TexTimingThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    *(DWORD *)(call + 1) = (DWORD)TexCreateHook - (TEXCREATE_SITE + 5);
    if (!WriteCode(TEXCREATE_SITE, call, 5)) { *mipResult = "FAILED (VirtualProtect)"; return "FAILED (VirtualProtect)"; }
    return "applied";
}

// ---- Fix 9: texture wait polls every 250 ms -----------------------------------------
// After AurWaitForAllTexturesToLoad (module.luc OnLoaded) FUN_006e8170 waits for the texture loader's pending
// queue with Sleep(250) rounds. Measured on a Temple Quarter -> Trade Quarter transition: 8 textures pending,
// queue empty after 9.5 ms, 242 ms slept on an empty queue. We make it Sleep(1); each round still processes
// the finished textures first (FUN_006e8050), so the result is the same, only sooner.

#define TEXWAIT_SLEEP_MS 0x006E81DC // imm32 of push 250 before call ebp (Sleep)

static const char *ApplyTexWaitFix(void)
{
    static const BYTE one[] = { 0x01, 0x00, 0x00, 0x00 };
    if (!BytesMatch(TEXWAIT_SLEEP_MS - 1, kTexWaitSleepOrig, sizeof kTexWaitSleepOrig)) return "skipped (bytes differ)";
    return WriteCode(TEXWAIT_SLEEP_MS, one, sizeof one) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Diagnostics: frame log ------------------------------------------------------------
// After an alt-tab back the game once stayed laggy until the next alt-tab, with no "window inactive"
// Sleep(1000) in the log. [debug] frame_log=1 counts frames on the main loop's per-frame path (the call
// to FUN_0079a600 at 0x007532A5, a singleton getter) and logs every stretch of 3+ s below 30 FPS: frame
// rate, worst frame, and how much of it the game was in the foreground, had an active caption, or
// considered itself inactive (0x00C2A324 = 0, the main loop then sleeps 1 s per frame).
// To find out what the alt-tab lag is (a steady ~24 FPS after coming back, every D3D call slow, cured by
// the next alt-tab) it also records:
//  - time spent in IDirect3DDevice9::Present (vtable hook on the game's device at [0x00C587BC]) and a
//    frame-time histogram;
//  - how long Sleep(1) really lasts in the game process (a probe thread, 10 times a second). Windows 11
//    stops honouring a process's timer resolution while its window is minimized or hidden; if it did not
//    come back after an alt-tab, every timed wait in DXVK would run on 15.6 ms ticks;
//  - a "focus back" line 10 s after each return to the game (time away, frames, Present, Sleep(1), window,
//    display and timer state, visible windows above the game), so fine and laggy returns can be compared;
//  - Reset calls with their parameters;
//  - after the game window gets the focus (EVENT_SYSTEM_FOREGROUND), the time to the next frame and Present;
//  - on the lag signature (4 s in a row at 10-30 FPS, in the foreground, no frame over 250 ms) the same
//    state, and if [debug] frame_sampler names sampler.exe, a 15 s profile next to the log.

#define FRAME_SITE   0x007532A5
#define FRAME_TARGET 0x0079A600
#define GAME_ACTIVE  0x00C2A324 // 0: main loop treats the window as inactive
#define GAME_WINSTAT 0x00EE4754 // WINDOWINFO.dwWindowStatus from the main loop's last GetWindowInfo
#define GAME_DEVICE  0x00C587BC // IDirect3DDevice9 *

typedef void *(__cdecl *FrameTarget_t)(void);
typedef HRESULT (WINAPI *Present_t)(void *, const RECT *, const RECT *, HWND, const void *);
typedef HRESULT (WINAPI *Reset_t)(void *, DWORD *);
typedef HRESULT (WINAPI *DwmGetWindowAttribute_t)(HWND, DWORD, void *, DWORD);
typedef LONG (NTAPI *NtQueryTimerResolution_t)(ULONG *, ULONG *, ULONG *);

typedef struct {
    double secs, sleepMs, sleepMax, fg, caption, inactive;
    LONGLONG worst, presTicks, presMax;
    int frames, presents, presentFails, sleeps, hist[6]; // frame times <10, <20, <30, <50, <100, 100+ ms
} FrameStats;

static FrameStats g_fsSec, g_fsSlow, g_fsBack; // this second, the current slow stretch, the 10 s after a return
static int g_detailStats;                        // [debug] detail_hotkey: log frame statistics per detail level
static void DetailFrameStats(const FrameStats *s);
static LONGLONG g_fLast, g_fSecStart, g_awayStart;
static DWORD g_frameTid, g_samplerTick;
static int g_slow, g_lagSecs, g_away, g_backOpen, g_devHooked;
static double g_backAway;
static HWND g_gameWnd;
// Hooked vtables with their original Present/Reset. Windows' own d3d9 keeps the vtable in heap memory and
// rewrites its Present slot after a while (DXVK does not), and could switch vtables; both are hooked again.
// PresentHook calls the original of the vtable the device has now.
#define MAX_HOOKED_VT 4
static struct { void **vt; Present_t present; Reset_t reset; } g_vts[MAX_HOOKED_VT];
static volatile LONG g_nVts;

static int VtIndex(void *dev)
{
    void **vt = *(void ***)dev;
    LONG i, n = g_nVts;
    for (i = 0; i < n; i++)
        if (g_vts[i].vt == vt) return i;
    return 0; // a copy of a hooked vtable (it points to us): the first original
}
static volatile LONG g_probeN, g_probeUs, g_probeMaxUs; // Sleep(1) probe: count, total and max in µs
static LONG g_probeNPrev, g_probeUsPrev;
// Return latency: when the game window gets the focus (WinEvent), the time to the next frame and the next Present.
static volatile LONG g_fgState;            // 0 idle, 1 got the focus, 2 frame seen, waiting for Present
static volatile LONG g_fgAtUs, g_fgFrameUs; // QPC in microseconds (32-bit, only differences are used)

static LONG NowUs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (LONG)(t.QuadPart * 1000000 / g_qpf.QuadPart);
}

// On-screen message (the graphics detail hotkey): drawn on the back buffer right before Present with D3DX's font
// (d3dx9_35.dll, which the game loads), through raw vtables: IDirect3DDevice9 GetBackBuffer 18, SetRenderTarget 37,
// GetRenderTarget 38, BeginScene 41, EndScene 42; IDirect3DSurface9 GetDesc 12; ID3DXFont DrawTextW 15,
// OnLostDevice 16, OnResetDevice 17. The font's own sprite saves and restores the render states.
typedef HRESULT (WINAPI *D3DXCreateFontW_t)(void *, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCWSTR, void **);
typedef HRESULT (WINAPI *ComCall_t)(void *);
typedef INT (WINAPI *FontDrawTextW_t)(void *, void *, LPCWSTR, INT, RECT *, DWORD, DWORD);

static WCHAR g_osdText[96];
static const WCHAR *volatile g_osdFace = L"Georgia"; // set by OsdShow (Georgia has no Chinese)
static volatile DWORD g_osdUntil; // GetTickCount when the message is gone; 0 = none
static void *g_osdFont, *g_osdFontDev;
static const WCHAR *g_osdFontFace;
static int g_osdFontH, g_osdFailed;

#define VT(obj, i) ((*(void ***)(obj))[i])

static void OsdShow(const WCHAR *text, const WCHAR *face, DWORD ms)
{
    wcscpy_s(g_osdText, sizeof g_osdText / sizeof g_osdText[0], text);
    g_osdFace = face;
    g_osdUntil = (GetTickCount() + ms) | 1;
}

static void OsdDraw(void *dev)
{
    void *bb = NULL, *oldRt = NULL;
    DWORD desc[8], left = g_osdUntil - GetTickCount(), alpha;
    RECT r;
    int h;
    if ((LONG)left <= 0) { g_osdUntil = 0; return; }
    if (((HRESULT (WINAPI *)(void *, UINT, UINT, DWORD, void **))VT(dev, 18))(dev, 0, 0, 0, &bb) != 0 || !bb) return;
    if (((HRESULT (WINAPI *)(void *, DWORD *))VT(bb, 12))(bb, desc) != 0) goto done;
    h = (int)desc[7] / 30; // ~72 px at 2160
    if (h < 16) h = 16;
    if (g_osdFont && (g_osdFontDev != dev || g_osdFontH != h || g_osdFontFace != g_osdFace)) {
        if (g_osdFontDev == dev) ((ComCall_t)VT(g_osdFont, 2))(g_osdFont); // Release; a font of an older device is left alone
        g_osdFont = NULL;
    }
    if (!g_osdFont && !g_osdFailed) {
        D3DXCreateFontW_t create = NULL;
        HMODULE d3dx = GetModuleHandleA("d3dx9_35.dll");
        const WCHAR *face = g_osdFace;
        if (d3dx) create = (D3DXCreateFontW_t)GetProcAddress(d3dx, "D3DXCreateFontW");
        if (!create || create(dev, h, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH | FF_ROMAN, face, &g_osdFont) != 0 || !g_osdFont) {
            g_osdFont = NULL;
            g_osdFailed = 1;
            Log("on-screen message: no D3DX font (%s); the message is in the log only", d3dx ? "D3DXCreateFontW failed" : "d3dx9_35.dll not loaded");
            goto done;
        }
        g_osdFontDev = dev;
        g_osdFontH = h;
        g_osdFontFace = face;
        Log("on-screen message: D3DX font %S %d px", face, h);
    }
    if (!g_osdFont) goto done;
    alpha = left >= 500 ? 255 : left * 255 / 500; // fade out over the last 0.5 s
    ((HRESULT (WINAPI *)(void *, DWORD, void **))VT(dev, 38))(dev, 0, &oldRt); // draw on the back buffer, whatever is bound
    ((HRESULT (WINAPI *)(void *, DWORD, void *))VT(dev, 37))(dev, 0, bb);
    if (((ComCall_t)VT(dev, 41))(dev) == 0) { // BeginScene
        r.left = 0; r.right = (LONG)desc[6];
        r.top = (LONG)desc[7] / 12 + h / 15; r.bottom = r.top + h * 2;
        OffsetRect(&r, h / 15, 0);
        ((FontDrawTextW_t)VT(g_osdFont, 15))(g_osdFont, NULL, g_osdText, -1, &r, DT_CENTER | DT_TOP | DT_NOCLIP, alpha << 24);
        OffsetRect(&r, -h / 15, -h / 15);
        ((FontDrawTextW_t)VT(g_osdFont, 15))(g_osdFont, NULL, g_osdText, -1, &r, DT_CENTER | DT_TOP | DT_NOCLIP,
                                             (alpha << 24) | 0x00F0E6D2);
        ((ComCall_t)VT(dev, 42))(dev); // EndScene
    }
    if (oldRt) {
        ((HRESULT (WINAPI *)(void *, DWORD, void *))VT(dev, 37))(dev, 0, oldRt);
        ((ComCall_t)VT(oldRt, 2))(oldRt);
    }
done:
    ((ComCall_t)VT(bb, 2))(bb);
}

static HRESULT WINAPI PresentHook(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty)
{
    LARGE_INTEGER a, b;
    HRESULT hr;
    if (g_osdUntil) {
        __try { OsdDraw(dev); } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_osdUntil = 0;
            g_osdFailed = 1;
            Log("on-screen message: exception %08lx while drawing; turned off", GetExceptionCode());
        }
    }
    QueryPerformanceCounter(&a);
    hr = g_vts[VtIndex(dev)].present(dev, src, dst, wnd, dirty);
    QueryPerformanceCounter(&b);
    if (g_fgState == 2 && GetCurrentThreadId() == g_frameTid) {
        LONG pres = (LONG)(b.QuadPart * 1000000 / g_qpf.QuadPart) - g_fgAtUs;
        g_fgState = 0;
        Log("frame log: game window in front; next frame after %.1f ms, next Present done after %.1f ms (%s)",
            g_fgFrameUs / 1000.0, pres / 1000.0, *(DWORD *)GAME_ACTIVE ? "active" : "still treated as inactive");
    }
    if (GetCurrentThreadId() == g_frameTid) { // the statistics belong to the main loop
        LONGLONG t = b.QuadPart - a.QuadPart;
        g_fsSec.presents++;
        g_fsSec.presTicks += t;
        if (t > g_fsSec.presMax) g_fsSec.presMax = t;
        if (hr != 0) g_fsSec.presentFails++;
    }
    return hr;
}

static HRESULT WINAPI ResetHook(void *dev, DWORD *pp)
{
    LARGE_INTEGER a, b;
    HRESULT hr;
    if (g_osdFont && g_osdFontDev == dev) ((ComCall_t)VT(g_osdFont, 16))(g_osdFont); // OnLostDevice
    QueryPerformanceCounter(&a);
    hr = g_vts[VtIndex(dev)].reset(dev, pp);
    QueryPerformanceCounter(&b);
    if (g_osdFont && g_osdFontDev == dev && hr == 0) ((ComCall_t)VT(g_osdFont, 17))(g_osdFont); // OnResetDevice
    __try { // D3DPRESENT_PARAMETERS
        Log("frame log: Reset %.1f ms, hr %08lx: %lux%lu, %lu back buffers, swap effect %lu, windowed %lu, %lu Hz, interval %08lx",
            (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart, (DWORD)hr, pp[0], pp[1], pp[3], pp[6], pp[8], pp[12], pp[13]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return hr;
}

static void ModuleOf(const void *addr, char *name, size_t n)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *slash;
    strcpy_s(name, n, "?");
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) || !GetModuleFileNameA(m, path, MAX_PATH)) return;
    slash = strrchr(path, '\\');
    strcpy_s(name, n, slash ? slash + 1 : path);
}

// Hooks Present and Reset in the device's current vtable (up to MAX_HOOKED_VT vtables). A hooked vtable whose
// slot no longer points to us is hooked again only when the new function is in the same module as the one we
// saved (d3d9 rewrote its own slot); a function elsewhere is an overlay wrapping our hook, and hooking that again
// would make us call ourselves. Called from the main loop (frame log, once a second) and from the detail
// hotkey's thread (while the on-screen message shows); one at a time.
static int SameModule(const void *a, const void *b)
{
    HMODULE ma = NULL, mb = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &ma);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)b, &mb);
    return ma && ma == mb;
}

// The slots of a vtable we hooked were rewritten by d3d9 itself: take its new functions and hook again.
static void RehookSlots(LONG i)
{
    void **vt = g_vts[i].vt;
    DWORD old;
    int pres = vt[17] != (void *)PresentHook && SameModule(vt[17], (void *)g_vts[i].present);
    int reset = vt[16] != (void *)ResetHook && SameModule(vt[16], (void *)g_vts[i].reset);
    static int logs;
    if (!pres && !reset) return;
    if (!VirtualProtect(&vt[16], 2 * sizeof(void *), PAGE_READWRITE, &old)) return;
    if (pres) { g_vts[i].present = (Present_t)vt[17]; vt[17] = (void *)PresentHook; }
    if (reset) { g_vts[i].reset = (Reset_t)vt[16]; vt[16] = (void *)ResetHook; }
    VirtualProtect(&vt[16], 2 * sizeof(void *), old, &old);
    if (logs++ < 5) Log("frame log: d3d9 rewrote its %s slot; hooked again", pres && reset ? "Present and Reset" : pres ? "Present" : "Reset");
}

static void HookDevice(void)
{
    static volatile LONG busy;
    void *dev = NULL, **vt;
    DWORD old, cp[4] = { 0 }; // D3DDEVICE_CREATION_PARAMETERS: adapter, device type, focus window, flags
    char vtMod[64], presMod[64];
    LONG i, n;
    __try { dev = *(void **)GAME_DEVICE; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!dev) return;
    vt = *(void ***)dev;
    if (InterlockedCompareExchange(&busy, 1, 0) != 0) return;
    n = g_nVts;
    for (i = 0; i < n; i++)
        if (g_vts[i].vt == vt) {
            RehookSlots(i);
            busy = 0;
            return;
        }
    if (vt[17] == (void *)PresentHook || n >= MAX_HOOKED_VT) { busy = 0; return; }
    if (!VirtualProtect(&vt[16], 2 * sizeof(void *), PAGE_READWRITE, &old)) {
        Log("frame log: Present hook FAILED (VirtualProtect)");
        busy = 0;
        return;
    }
    g_vts[n].vt = vt;
    g_vts[n].reset = (Reset_t)vt[16];
    g_vts[n].present = (Present_t)vt[17];
    InterlockedIncrement(&g_nVts); // the entry is complete before our hook can be reached through this vtable
    vt[16] = (void *)ResetHook;
    vt[17] = (void *)PresentHook;
    VirtualProtect(&vt[16], 2 * sizeof(void *), old, &old);
    g_devHooked = 1;
    busy = 0;
    if (((HRESULT (WINAPI *)(void *, DWORD *))vt[9])(dev, cp) == 0 && cp[2]) g_gameWnd = (HWND)cp[2]; // GetCreationParameters
    ModuleOf(vt, vtMod, sizeof vtMod);
    ModuleOf((void *)g_vts[n].present, presMod, sizeof presMod);
    Log("frame log: Present and Reset hooked%s (device %p, vtable in %s, Present in %s, window %p)",
        n ? " in the device's new vtable" : "", dev, vtMod, presMod, g_gameWnd);
}

static DWORD WINAPI SleepProbeThread(LPVOID unused)
{
    LARGE_INTEGER a, b;
    LONG us, m;
    (void)unused;
    for (;;) {
        Sleep(100);
        if (g_waitOff) return 0;
        QueryPerformanceCounter(&a);
        Sleep(1);
        QueryPerformanceCounter(&b);
        us = (LONG)((b.QuadPart - a.QuadPart) * 1000000 / g_qpf.QuadPart);
        InterlockedExchangeAdd(&g_probeUs, us); // wraps after ~35 min of sleeping; readers take differences
        InterlockedIncrement(&g_probeN);
        do m = g_probeMaxUs; while (us > m && InterlockedCompareExchange(&g_probeMaxUs, us, m) != m);
    }
}

static void StatsAdd(FrameStats *d, const FrameStats *s)
{
    int i;
    d->secs += s->secs;
    d->sleepMs += s->sleepMs;
    d->fg += s->fg;
    d->caption += s->caption;
    d->inactive += s->inactive;
    d->presTicks += s->presTicks;
    d->frames += s->frames;
    d->presents += s->presents;
    d->presentFails += s->presentFails;
    d->sleeps += s->sleeps;
    if (s->sleepMax > d->sleepMax) d->sleepMax = s->sleepMax;
    if (s->worst > d->worst) d->worst = s->worst;
    if (s->presMax > d->presMax) d->presMax = s->presMax;
    for (i = 0; i < 6; i++) d->hist[i] += s->hist[i];
}

// "<fps> FPS (worst frame ...); Present ...; Sleep(1) ...; game in foreground ...". frame_log readers match
// "frames: <secs> s at <fps> FPS" and "inactive <n>%", so keep that order.
static void StatsText(char *buf, size_t n, const FrameStats *s)
{
    double ms = 1000.0 / g_qpf.QuadPart, secs = s->secs > 0 ? s->secs : 1;
    char pres[96], slp[64];
    if (s->presents)
        sprintf_s(pres, sizeof pres, "Present %d x, avg %.1f ms, max %.1f ms%s", s->presents, s->presTicks * ms / s->presents,
                  s->presMax * ms, s->presentFails ? " (some failed)" : "");
    else
        strcpy_s(pres, sizeof pres, g_devHooked ? "no Present seen" : "Present not hooked yet");
    if (s->sleeps) sprintf_s(slp, sizeof slp, "Sleep(1) lasts %.1f ms (max %.1f)", s->sleepMs / s->sleeps, s->sleepMax);
    else strcpy_s(slp, sizeof slp, "Sleep(1) not measured");
    sprintf_s(buf, n, "%.1f FPS (worst frame %.0f ms; frames <10/10-20/20-30/30-50/50-100/100+ ms: %d/%d/%d/%d/%d/%d); %s; %s; "
              "game in foreground %.0f%%, active caption %.0f%%, treated as inactive %.0f%%",
              s->frames / secs, s->worst * ms, s->hist[0], s->hist[1], s->hist[2], s->hist[3], s->hist[4], s->hist[5], pres, slp,
              100 * s->fg / secs, 100 * s->caption / secs, 100 * s->inactive / secs);
}

static void ExeOf(DWORD pid, char *name, size_t n)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    char path[MAX_PATH], *slash;
    DWORD len = MAX_PATH;
    strcpy_s(name, n, "?");
    if (!p) return;
    if (QueryFullProcessImageNameA(p, 0, path, &len)) {
        slash = strrchr(path, '\\');
        strcpy_s(name, n, slash ? slash + 1 : path);
    }
    CloseHandle(p);
}

// Window, display and timer state, plus the visible windows above the game on its monitor (an overlay or
// any window on top can make Windows compose the game's frames instead of flipping them).
static void LogWindowState(void)
{
    static DwmGetWindowAttribute_t dwmAttr;
    static NtQueryTimerResolution_t ntRes;
    static int looked;
    HWND w = g_gameWnd, h, fg = GetForegroundWindow();
    RECT wr = { 0 }, cr = { 0 };
    MONITORINFOEXA mi;
    DEVMODEA dm;
    ULONG resMax = 0, resMin = 0, resCur = 0;
    int shown = 0, walked = 0;
    if (!looked) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        looked = 1;
        if (dwm) dwmAttr = (DwmGetWindowAttribute_t)GetProcAddress(dwm, "DwmGetWindowAttribute");
        ntRes = (NtQueryTimerResolution_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    }
    if (!w || !IsWindow(w)) { Log("  window: not known yet"); return; }
    GetWindowRect(w, &wr);
    GetClientRect(w, &cr);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(mi.szDevice[0] ? mi.szDevice : NULL, ENUM_CURRENT_SETTINGS, &dm);
    if (ntRes) ntRes(&resMax, &resMin, &resCur);
    Log("  window %ld,%ld %ldx%ld (client %ldx%ld), style %08lx, ex style %08lx%s%s; monitor %s %ld,%ld %ldx%ld, mode %lux%lu %lu Hz; "
        "system timer %.2f ms; priority class %lx, main thread priority %d",
        wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom, (DWORD)GetWindowLongA(w, GWL_STYLE),
        (DWORD)GetWindowLongA(w, GWL_EXSTYLE), IsIconic(w) ? ", minimized" : "", fg == w ? ", foreground" : ", NOT foreground",
        mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, resCur / 10000.0,
        GetPriorityClass(GetCurrentProcess()), GetThreadPriority(GetCurrentThread()));
    for (h = GetWindow(w, GW_HWNDPREV); h && shown < 5 && walked < 2000; h = GetWindow(h, GW_HWNDPREV), walked++) {
        RECT r, x;
        DWORD pid = 0, cloaked = 0;
        char cls[64], exe[64];
        if (!IsWindowVisible(h) || !GetWindowRect(h, &r) || !IntersectRect(&x, &r, &mi.rcMonitor)) continue;
        if (dwmAttr && dwmAttr(h, 14, &cloaked, sizeof cloaked) == 0 && cloaked) continue; // DWMWA_CLOAKED
        GetClassNameA(h, cls, sizeof cls);
        GetWindowThreadProcessId(h, &pid);
        ExeOf(pid, exe, sizeof exe);
        Log("  above it: %s (%s) %ld,%ld %ldx%ld, ex style %08lx", cls, exe, r.left, r.top, r.right - r.left, r.bottom - r.top,
            (DWORD)GetWindowLongA(h, GWL_EXSTYLE));
        shown++;
    }
    if (!shown) Log("  above it: no visible window on that monitor");
}

static void StartSampler(void)
{
    char ini[MAX_PATH], exe[MAX_PATH], out[MAX_PATH], cmd[3 * MAX_PATH], *slash;
    SYSTEMTIME st;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    IniPath(ini);
    GetPrivateProfileStringA("debug", "frame_sampler", "", exe, MAX_PATH, ini);
    if (!exe[0]) return;
    if (g_samplerTick && GetTickCount() - g_samplerTick < 5 * 60 * 1000) return; // at most one profile per 5 min
    g_samplerTick = GetTickCount() | 1;
    GetModuleFileNameA(g_self, out, MAX_PATH);
    slash = strrchr(out, '\\');
    GetLocalTime(&st);
    sprintf_s(slash ? slash + 1 : out, MAX_PATH - (slash ? (slash + 1 - out) : 0), "enginefix_lag_%02d%02d%02d_main%lu.csv",
              st.wHour, st.wMinute, st.wSecond, g_frameTid);
    sprintf_s(cmd, sizeof cmd, "\"%s\" %lu 15 \"%s\" 1", exe, GetCurrentProcessId(), out);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log("frame log: profiling 15 s with %s -> %s", exe, out);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log("frame log: could not start %s (error %lu)", exe, GetLastError());
    }
}

static void FocusBackLog(const char *how)
{
    char s[640];
    StatsText(s, sizeof s, &g_fsBack);
    Log("focus back after %.1f s away, %s %.1f s: %s", g_backAway, how, g_fsBack.secs, s);
    LogWindowState();
    g_backOpen = 0;
}

static void FrameSecond(LONGLONG now)
{
    double secs = (double)(now - g_fSecStart) / g_qpf.QuadPart, fps = g_fsSec.frames / secs;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    LONG n = g_probeN, us = g_probeUs;
    int ours, caption, inactive;
    g_fsSec.secs = secs;
    g_fsSec.sleeps = n - g_probeNPrev;
    g_fsSec.sleepMs = ((DWORD)us - (DWORD)g_probeUsPrev) / 1000.0;
    g_fsSec.sleepMax = InterlockedExchange(&g_probeMaxUs, 0) / 1000.0;
    g_probeNPrev = n;
    g_probeUsPrev = us;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    ours = pid == GetCurrentProcessId();
    if (ours && !g_gameWnd) g_gameWnd = fg;
    caption = (*(DWORD *)GAME_WINSTAT & 1) != 0; // WS_ACTIVECAPTION
    inactive = *(DWORD *)GAME_ACTIVE == 0;
    g_fsSec.fg = ours ? secs : 0;
    g_fsSec.caption = caption ? secs : 0;
    g_fsSec.inactive = inactive ? secs : 0;
    HookDevice();
    if (fps < 30.0) {
        if (!g_slow) {
            g_slow = 1;
            memset(&g_fsSlow, 0, sizeof g_fsSlow);
        }
        StatsAdd(&g_fsSlow, &g_fsSec);
    } else if (g_slow) {
        g_slow = 0;
        if (g_fsSlow.secs >= 3.0) {
            char s[640];
            StatsText(s, sizeof s, &g_fsSlow);
            Log("frames: %.1f s at %s", g_fsSlow.secs, s);
        }
    }
    if (fps >= 10.0 && fps < 30.0 && ours && caption && !inactive && g_fsSec.worst * 1000.0 / g_qpf.QuadPart < 250.0) {
        if (++g_lagSecs == 4) {
            char s[640];
            StatsText(s, sizeof s, &g_fsSec);
            Log("frame log: lag signature (4 s in a row at 10-30 FPS in the foreground); last second: %s", s);
            LogWindowState();
            StartSampler();
        }
    } else {
        g_lagSecs = 0;
    }
    if (g_backOpen) {
        StatsAdd(&g_fsBack, &g_fsSec);
        if (g_fsBack.secs >= 10.0) FocusBackLog("first");
    }
    if (g_detailStats) DetailFrameStats(&g_fsSec);
    memset(&g_fsSec, 0, sizeof g_fsSec);
    g_fSecStart = now;
}

static void *__cdecl FrameHook(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_frameTid) g_frameTid = GetCurrentThreadId();
    if (g_fgState == 1) {
        g_fgFrameUs = (LONG)(now.QuadPart * 1000000 / g_qpf.QuadPart) - g_fgAtUs;
        g_fgState = 2;
    }
    if (*(DWORD *)GAME_ACTIVE == 0) {
        if (!g_away) {
            g_away = 1;
            g_awayStart = now.QuadPart;
            if (g_backOpen) FocusBackLog("left again after");
        }
    } else if (g_away) {
        g_away = 0;
        g_backOpen = 1;
        g_backAway = (double)(now.QuadPart - g_awayStart) / g_qpf.QuadPart;
        memset(&g_fsBack, 0, sizeof g_fsBack);
    }
    if (g_fLast) {
        LONGLONG dt = now.QuadPart - g_fLast;
        double ms = dt * 1000.0 / g_qpf.QuadPart;
        if (dt > g_fsSec.worst) g_fsSec.worst = dt;
        g_fsSec.frames++;
        g_fsSec.hist[ms < 10 ? 0 : ms < 20 ? 1 : ms < 30 ? 2 : ms < 50 ? 3 : ms < 100 ? 4 : 5]++;
        if (now.QuadPart - g_fSecStart >= g_qpf.QuadPart) FrameSecond(now.QuadPart);
    } else {
        g_fSecStart = now.QuadPart;
    }
    g_fLast = now.QuadPart;
    return ((FrameTarget_t)FRAME_TARGET)();
}

static void CALLBACK ForegroundEvent(HWINEVENTHOOK h, DWORD ev, HWND w, LONG obj, LONG child, DWORD tid, DWORD ms)
{
    (void)h; (void)ev; (void)obj; (void)child; (void)tid; (void)ms;
    if (w && w == *(HWND *)0x00EE4794 && g_fgState == 0) { // the game's window (FUN_00750150)
        g_fgAtUs = NowUs();
        g_fgState = 1;
    }
}

static DWORD WINAPI ForegroundThread(LPVOID unused)
{
    MSG m;
    (void)unused;
    if (!SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, ForegroundEvent, 0, 0,
                         WINEVENT_OUTOFCONTEXT)) return 0;
    while (GetMessageA(&m, NULL, 0, 0) > 0) DispatchMessageA(&m);
    return 0;
}

static const char *ApplyFrameLog(void)
{
    BYTE call[5] = { 0xE8 };
    HANDLE t;
    *(DWORD *)(call + 1) = FRAME_TARGET - (FRAME_SITE + 5);
    if (!BytesMatch(FRAME_SITE, call, 5)) return "skipped (bytes differ)";
    QueryPerformanceFrequency(&g_qpf);
    t = CreateThread(NULL, 0, SleepProbeThread, NULL, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL); // measure the timer, not CPU contention
        CloseHandle(t);
    }
    t = CreateThread(NULL, 0, ForegroundThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    *(DWORD *)(call + 1) = (DWORD)FrameHook - (FRAME_SITE + 5);
    return WriteCode(FRAME_SITE, call, 5) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Fix 10: cursor appears in the top-left area after mouse look ----------------------------------
// While the mouse turns the camera (OTS mouse look, middle-button turns), FUN_00419b50 parks the hidden
// Windows cursor every frame at "the middle" through FUN_00418900 -> FUN_004cc920 (ClientToScreen +
// SetCursorPos, stdcall x, y). It passes half the GUI size ([0x00EE47B4]+0xA0/+0xA4), which is 1365x768 at
// 3840x2160, as window pixels, so the cursor is parked at (682, 384) px, the top-left area, and shows up
// there when it becomes visible (active pause, Shift). At 1366x768 that was the middle. We park it at the
// middle of the window's client area instead.

#define PARK_CALL   0x00419BE0 // call FUN_00418900 (5 bytes)
#define PARK_TARGET 0x00418900
#define GAME_HWND   0x00EE4798 // the window FUN_004cc920 uses

static void __stdcall ParkCursor(int x, int y) // replaces a thiscall with the same stack arguments; ECX unused
{
    HWND w = *(HWND *)GAME_HWND;
    RECT r;
    POINT p;
    (void)x; (void)y;
    if (!w || !GetClientRect(w, &r)) return;
    p.x = (r.right - r.left) / 2;
    p.y = (r.bottom - r.top) / 2;
    ClientToScreen(w, &p);
    SetCursorPos(p.x, p.y);
}

static const char *ApplyCursorParkFix(void)
{
    BYTE call[5] = { 0xE8 };
    *(DWORD *)(call + 1) = (DWORD)(PARK_TARGET - (PARK_CALL + 5));
    if (!BytesMatch(PARK_CALL, call, 5)) return "skipped (bytes differ)";
    *(DWORD *)(call + 1) = (DWORD)ParkCursor - (PARK_CALL + 5);
    return WriteCode(PARK_CALL, call, 5) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Fix 11: up to 1 s late after coming back to the game ------------------------------------------
// While the window is inactive (fullscreen without the active caption, or minimized; flag 0x00C2A324 = 0) the
// main loop sleeps 1 s per frame (push 1000; call [Sleep] at 0x007536BE), which is what keeps the game paused
// in the background. Back in front, the game first finishes that Sleep, so it reacts up to 1 s late. The
// sleep now ends as soon as the game window is in front again; in the background it still lasts 1 s.

#define INACTIVE_SLEEP 0x007536BE // call dword ptr [0x00B3F22C] (Sleep), 6 bytes

static void __stdcall InactiveSleep(DWORD ms) // replaces Sleep(1000) (stdcall, one argument)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        HWND w = *(HWND *)GAME_HWND, fg = GetForegroundWindow();
        DWORD left, waited = GetTickCount() - t0;
        if (waited >= ms) return;
        if (w && fg == w && !IsIconic(w)) return;
        left = ms - waited;
        Sleep(left < 10 ? left : 10);
    }
}

static const char *ApplyInactiveSleepFix(void)
{
    static const BYTE push[5] = { 0x68, 0xE8, 0x03, 0x00, 0x00 }; // push 1000
    static const BYTE orig[6] = { 0xFF, 0x15, 0x2C, 0xF2, 0xB3, 0x00 };
    BYTE code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    if (!BytesMatch(INACTIVE_SLEEP - 5, push, sizeof push) || !BytesMatch(INACTIVE_SLEEP, orig, 6))
        return "skipped (bytes differ)";
    *(DWORD *)(code + 1) = (DWORD)InactiveSleep - (INACTIVE_SLEEP + 5);
    return WriteCode(INACTIVE_SLEEP, code, 6) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- Fix 12: Shift hides the cursor in the main menu ---------------------------------------------
// The input handler FUN_00423bd0 (ESI = the client object) handles Shift in the over-the-shoulder camera
// ([0x00C500F0] = 2): down (0x004245E1) frees the cursor (FUN_0043ee00(0) on [ESI+0x48], ShowCursor 1), up
// (0x00424650) turns mouse look back on (FUN_0043ee00(1), ShowCursor 0); FUN_00419b50 then parks the hidden
// cursor every frame. Neither path checks for the main menu, where [ESI+0x48] exists too, so a Shift tap there
// (also the one in Win+Shift+S) hid the cursor until Shift was held again. Each path loads [ESI+0x48] and skips
// everything when it is NULL (mov ecx,[esi+0x48]; test ecx,ecx; jz); a call to a stub now does that load and
// test and gives NULL while the main menu is open ([ESI+0x609], read by CClientExoApp::GetMainMenuPanelActive).

#define SHIFT_DOWN_LOAD 0x00424612 // mov ecx,[esi+0x48]; test ecx,ecx (5 bytes), then jz 0x0042462E
#define SHIFT_UP_LOAD   0x0042467D // the same, then jz 0x0042469A
#define CLIENT_MAINMENU 0x609      // byte: 1 while the main menu is open

static const BYTE kShiftLoadOrig[] = { 0x8B, 0x4E, 0x48, 0x85, 0xC9 };

static const char *ApplyMainMenuShiftFix(void)
{
    static const BYTE stub[] = {
        0x8B, 0x4E, 0x48,                                     // mov ecx,[esi+0x48]
        0x80, 0xBE, CLIENT_MAINMENU & 0xFF, CLIENT_MAINMENU >> 8, 0x00, 0x00, 0x00, // cmp byte ptr [esi+0x609],0
        0x74, 0x02,                                           // jz +2
        0x33, 0xC9,                                           // xor ecx,ecx (main menu: as if there were no camera)
        0x85, 0xC9,                                           // test ecx,ecx (the jz after the call reads it)
        0xC3                                                  // ret
    };
    BYTE *code, call[5] = { 0xE8 };
    if (!BytesMatch(SHIFT_DOWN_LOAD, kShiftLoadOrig, sizeof kShiftLoadOrig) ||
        !BytesMatch(SHIFT_UP_LOAD, kShiftLoadOrig, sizeof kShiftLoadOrig))
        return "skipped (bytes differ)";
    code = VirtualAlloc(NULL, sizeof stub, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return "FAILED (no memory)";
    memcpy(code, stub, sizeof stub);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof stub);
    *(DWORD *)(call + 1) = (DWORD)code - (SHIFT_DOWN_LOAD + 5);
    if (!WriteCode(SHIFT_DOWN_LOAD, call, 5)) return "FAILED (VirtualProtect)";
    *(DWORD *)(call + 1) = (DWORD)code - (SHIFT_UP_LOAD + 5);
    return WriteCode(SHIFT_UP_LOAD, call, 5) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- QoL 1: turn the OTS camera with the middle mouse button during active pause ---------------
// In the isometric and hybrid cameras, holding the middle mouse button turns the camera with the mouse, also
// in active pause (FUN_0041bb60 -> FUN_0041b830 hides the cursor and lets the mouse turn the camera, while
// g_cAuroraSettings.m_bMMBCameraTurn is on; release -> FUN_0041bbf0 -> FUN_0041b9b0 shows it again). In
// the over-the-shoulder camera (mode 2) the mouse always turns the camera, so the input handler FUN_00423bd0
// skips the middle button there (0x00424213: cmp [0x00C500F0],2 / jz). Active pause in OTS shows the cursor
// (FUN_00444cb0 -> FUN_0041b9b0) and leaves only edge scrolling. We take that jz: during active pause
// (bit 4 of the pause state at [handler+0x4F0], the value GetActivePauseState returns) the middle button
// starts and ends the same turn mode as in the other cameras. Unpausing while the button is held is left to
// the game, which switches OTS mouse look back on by itself.

#define OTSMMB_JZ        0x00424219 // jz 0x00425174 (6 bytes) after cmp [camera mode],2
#define OTSMMB_SKIP      0x00425174
#define MMB_DOWN         0x0041BB60 // thiscall (handler, int repeat), ret 4
#define CAMTURN_END      0x0041B9B0 // thiscall (handler, int unused), ret 4
#define HANDLER_TURNING  0x348      // byte: 1 while the mouse turns the camera (cursor hidden)
#define HANDLER_PAUSE    0x4F0      // byte: pause state bits, 4 = active pause
#define HANDLER_CURSOR_X 0x2A4      // cursor position saved when the turn started
#define HANDLER_CURSOR_Y 0x2A8

static const BYTE kOtsMmbOrig[] = { 0x0F, 0x84, 0x55, 0x0F, 0x00, 0x00 };
typedef void (__fastcall *HandlerCall_t)(void *handler, void *edx, int arg);
static int g_otsMmbTurning, g_otsMmbLogged;

static void __cdecl OtsMmb(BYTE *handler, int state) // state: 0 = released, otherwise pressed (-1 = repeat)
{
    __try {
        BYTE pause = handler[HANDLER_PAUSE];
        if (g_otsMmbLogged < 3) {
            g_otsMmbLogged++;
            Log("qol 1: middle button %s in OTS, pause state %u, camera turn %u", state ? "down" : "up", pause, handler[HANDLER_TURNING]);
        }
        if (state) {
            if ((pause & 4) && !handler[HANDLER_TURNING]) {
                ((HandlerCall_t)MMB_DOWN)(handler, NULL, state == -1);
                g_otsMmbTurning = handler[HANDLER_TURNING] != 0;
            }
        } else if (g_otsMmbTurning) {
            g_otsMmbTurning = 0;
            if (pause & 4) { // still paused: back to the cursor where it was, as the other cameras do
                ((HandlerCall_t)CAMTURN_END)(handler, NULL, 0);
                SetCursorPos(*(int *)(handler + HANDLER_CURSOR_X), *(int *)(handler + HANDLER_CURSOR_Y));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static const char *ApplyOtsMmb(void)
{
    BYTE *stub, *p, jz[6] = { 0x0F, 0x84 };
    if (!BytesMatch(OTSMMB_JZ, kOtsMmbOrig, sizeof kOtsMmbOrig)) return "skipped (bytes differ)";
    stub = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return "FAILED (no memory)";
    p = stub;
    *p++ = 0x9C; *p++ = 0x60;                                   // pushfd; pushad
    *p++ = 0x53; *p++ = 0x56;                                   // push ebx (button state); push esi (handler)
    p = EmitCall(p, (DWORD)OtsMmb);                             // call OtsMmb
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                      // add esp,8
    *p++ = 0x61; *p++ = 0x9D;                                   // popad; popfd
    *p++ = 0xE9; PutRel32(p, OTSMMB_SKIP); p += 4;              // jmp to where the game skipped to
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);
    *(DWORD *)(jz + 2) = (DWORD)stub - (OTSMMB_JZ + 6);
    return WriteCode(OTSMMB_JZ, jz, sizeof jz) ? "applied" : "FAILED (VirtualProtect)";
}

// ---- QoL 3: borderless window ------------------------------------------------------------------
// In windowed mode (options menu, FullScreen off) FUN_005932f0 (vfunc +8 of the video object [0x00EE47BC])
// gives the window a caption and border (style 0x02CA0000, "mov esi" at 0x0059334D) and sizes it so the client
// area is the game's resolution, at (0, 0). Fullscreen uses a popup (0x82000000); borderless uses that for
// windowed too: at the desktop resolution a window over the primary monitor without border, while Direct3D
// stays windowed (no exclusive mode, instant alt-tab).
// Some behaviour is tied to fullscreen through the video object's vfunc +4 (FUN_00592b80, [this+0xAC]); while
// borderless is on these sites treat it as fullscreen, so the game feels the same: the cursor is clipped to
// the screen (FUN_004e2100) and released (FUN_0043f6c0), the camera turns at the screen edge (FUN_004262f0),
// and the main loop sleeps while the window is inactive (0x007532E7), which pauses the game after an alt-tab.
// Device creation and the device-lost path keep asking the real question.
// The option (Options > Graphics, enginefix_hook.lua) takes effect at once: the patches are always in, and a
// thread reads the stored option (registry value EngineFixBorderless, written when the player presses OK)
// a few times a second. It switches the style FUN_005932f0 will use and, when the game is windowed at that
// moment, restyles the window itself. It also releases the cursor clip when another window gets the focus.

#define BL_STYLE      0x0059334D // mov esi,0x02CA0000 (windowed style), replaced by a call to BlStyleStub
#define GAME_SETTINGS 0x00EE47BC // the video object; vfunc +4 says "fullscreen"
#define BL_FRAMED     0x02CA0000
#define BL_POPUP      0x82000000

typedef struct { DWORD at; BYTE orig[16]; int len; } BlSite; // at: the vfunc load + call to replace

static const BlSite kBlSites[] = {
    { 0x004266B5, { 0x8B, 0x50, 0x04, 0xFF, 0xD2 }, 5 },                   // edge camera turn
    { 0x0043F7CF, { 0x8B, 0x42, 0x04, 0xFF, 0xD0 }, 5 },                   // release cursor clip
    { 0x004E2108, { 0x8B, 0x50, 0x04, 0x83, 0xEC, 0x10, 0xFF, 0xD2 }, 8 }, // clip cursor (sub esp,0x10 in between)
    { 0x007532EF, { 0x8B, 0x42, 0x04, 0xFF, 0xD0 }, 5 },                   // sleep while inactive
};

static volatile LONG g_blOn;                // borderless wanted now
static volatile DWORD g_blStyle = BL_FRAMED; // the windowed style FUN_005932f0 sets

typedef int (__fastcall *IsFullscreen_t)(void *, void *); // thiscall without stack arguments

static int RealFullscreen(void *obj)
{
    return ((IsFullscreen_t)(*(void ***)obj)[1])(obj, NULL);
}

// Called instead of the vfunc with ECX = the video object (thiscall, no stack arguments, so __fastcall fits).
static int __fastcall BorderlessFullscreen(void *obj)
{
    return g_blOn ? 1 : RealFullscreen(obj);
}

static int BorderlessWanted(const char **from);
static void BorderlessSet(int on);

// FUN_005932f0 is about to style the window for windowed mode: take the option as stored right now (the Lua
// part stores it just before "Full screen" is applied), so a switch to windowed never needs a restyle.
static void __cdecl BlStyleNow(void)
{
    const char *from;
    int on = BorderlessWanted(&from);
    if (on != g_blOn) {
        BorderlessSet(on);
        Log("qol 3: borderless window %s (%s), at the switch to windowed", on ? "on" : "off", from);
    }
}

// Replaces "mov esi,0x02CA0000" (5 bytes) with "call BlStyleStub" (5 bytes). Only ESI changes, as before.
static __declspec(naked) void BlStyleStub(void)
{
    __asm {
        pushfd
        pushad
        call BlStyleNow
        popad
        popfd
        mov esi, dword ptr [g_blStyle]
        ret
    }
}

// The options menu's "Borderless window" (enginefix_hook.lua) is stored with the game's settings; until it has
// been saved there, [qol] borderless decides. *from says which.
static int BorderlessWanted(const char **from)
{
    HKEY k;
    char v[32];
    DWORD type, n = sizeof v - 1;
    LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\CD Projekt RED\\Witcher\\Settings", 0, KEY_QUERY_VALUE, &k);
    if (r == ERROR_SUCCESS) {
        r = RegQueryValueExA(k, "EngineFixBorderless", NULL, &type, (BYTE *)v, &n);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_SZ) {
            v[n] = 0;
            *from = "options menu";
            return atof(v) > 0;
        }
    }
    *from = "ini";
    return IniFlag("qol", "borderless", 0);
}

static void BorderlessSet(int on)
{
    g_blStyle = on ? BL_POPUP : BL_FRAMED;
    InterlockedExchange(&g_blOn, on);
}

// The game is windowed right now: give its window the new style, sized as FUN_005932f0 does (client area =
// the current client size, window at (0, 0)). Asynchronous, so this thread never waits for the main thread.
static void BorderlessRestyle(int on)
{
    HWND w = *(HWND *)GAME_HWND;
    RECT c, r;
    DWORD style = (on ? BL_POPUP : BL_FRAMED) | WS_VISIBLE; // never hide it (the game's own styles lack WS_VISIBLE)
    if (!w || IsIconic(w) || !GetClientRect(w, &c) || c.right <= 0 || c.bottom <= 0) return;
    r = c;
    AdjustWindowRect(&r, style, FALSE);
    SetWindowLongA(w, GWL_STYLE, (LONG)style);
    SetWindowPos(w, HWND_TOP, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
    if (!on) ClipCursor(NULL);
}

static int GameWindowed(void)
{
    __try {
        void *video = *(void **)GAME_SETTINGS;
        return video && !RealFullscreen(video);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static DWORD WINAPI BorderlessThread(LPVOID unused)
{
    int wasOurs = 0, tick = 0;
    (void)unused;
    for (;;) {
        HWND fg;
        DWORD pid = 0;
        int ours;
        Sleep(100);
        fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &pid);
        ours = pid == GetCurrentProcessId();
        if (wasOurs && !ours) ClipCursor(NULL); // the clip is global; do not keep it after an alt-tab
        wasOurs = ours;
        if (++tick % 3 == 0) { // ~3 times a second
            const char *from;
            int on = BorderlessWanted(&from);
            if (on != g_blOn) {
                int windowed = GameWindowed();
                BorderlessSet(on);
                if (windowed) BorderlessRestyle(on);
                Log("qol 3: borderless window %s (%s)%s", on ? "on" : "off", from,
                    windowed ? ", window restyled" : ", takes effect when full screen is off");
            }
        }
    }
}

static const char *ApplyBorderless(int on)
{
    static const BYTE style[5] = { 0xBE, 0x00, 0x00, 0xCA, 0x02 };
    BYTE code[8], *p;
    size_t i;
    HANDLE t;
    if (!BytesMatch(BL_STYLE, style, sizeof style)) return "skipped (bytes differ)";
    for (i = 0; i < sizeof kBlSites / sizeof kBlSites[0]; i++)
        if (!BytesMatch(kBlSites[i].at, kBlSites[i].orig, kBlSites[i].len)) return "skipped (bytes differ)";
    BorderlessSet(on);
    code[0] = 0xE8;
    *(DWORD *)(code + 1) = (DWORD)BlStyleStub - (BL_STYLE + 5);
    if (!WriteCode(BL_STYLE, code, 5)) return "FAILED (VirtualProtect)";
    for (i = 0; i < sizeof kBlSites / sizeof kBlSites[0]; i++) {
        const BlSite *s = &kBlSites[i];
        p = code;
        if (s->len == 8) { *p++ = 0x83; *p++ = 0xEC; *p++ = 0x10; } // keep the sub esp,0x10
        *p++ = 0xE8;
        *(DWORD *)p = (DWORD)BorderlessFullscreen - (s->at + (DWORD)(p - code) + 4);
        if (!WriteCode(s->at, code, s->len)) return "FAILED (VirtualProtect)";
    }
    t = CreateThread(NULL, 0, BorderlessThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return on ? "applied, on (in windowed mode)" : "applied, off (the options menu turns it on)";
}

// ---- Graphics detail ([graphics] detail) --------------------------------------
// The engine's draw distances live in g_cAuroraSettings (constructor FUN_00401190, object at 0x00C500E8;
// each member is registered as a console variable by name, "push var; push name" at the addresses below).
// The options menu (hwdepsettings.luc) only sets some of them, and none of the LOD distances. [graphics]
// detail=1 (high), 2 (ultra) or 3 (extreme, about the ranges of the Higher LOD mod on Nexus, for fast PCs)
// multiplies a fixed set by a factor per level: LOD distances (5-45 m by
// default) for scenery, creatures and trees, grass range (set by the menu's grass slider, 60 m at most),
// shadow distance (30 m) and shadow-casting lights (4), how far lights stay on (7 x their radius), and the
// decal and weapon effect distances, and the water reflection texture (512 px; scenes create it when they
// load, CAurScene has RecreateReflectionRTs for later changes). The factor applies to whatever the game set: a thread checks the
// values every 50 ms, and a value that differs from what we wrote was set by the game (constructor,
// startup.luc, the grass slider), so it becomes the new base. Level 0 writes nothing. The game stores only
// the menu's slider positions in the registry, never these values, so nothing accumulates across runs.
// [graphics] hotkey=1 (default): Ctrl+F12 cycles the level in game (until the game exits) and shows it on screen
// for 2.5 s (OsdDraw; the device's Present is hooked on the first press). With [debug] frame_log=1 a line every
// 10 s gives the frame rate and the main thread's time per frame outside Present (the game's own work).

typedef struct {
    const char *name; // g_cAuroraSettings member
    DWORD var, nameStr, push; // the variable, "g_cAuroraSettings.<name>", the registration's "push var"
    int isInt;
    float scale[4];   // per level
    int ok, have;     // verified; base known
    DWORD base, written;
} DetailVar;

// m_fSceneLODDistancesMultiplier only matters for the six area models with LoD_dummy groups (Outskirts and a
// few others): terrain tiles, buildings, fences, and trees and bushes placed in the area. Each group shows its
// detailed nodes (LOD 0-2) near and coarse planes (LOD 3-5) far. The node loop FUN_005c0890 skips a node whose
// sector is not in the visible-sector bits, and the areas' sector data never expected detailed nodes that far
// away, so with a longer distance parts of hills went missing (0.6.0). While the level is above 0, a stub at that
// test lets nodes that belong to a LOD group (+0xCC != -1) through; all other nodes keep the sector test. The
// multiplier is only scaled when the stub is in place (docs/fixes/graphics-detail.md).
#define SCENE_SECTOR_JZ   0x005C093D // jz 0x005C0CCE: node's sector not visible
#define SCENE_SECTOR_PASS 0x005C0943 // where a node in a visible sector continues
#define SCENE_SECTOR_SKIP 0x005C0CCE // next node
static const BYTE kSceneSectorJz[6] = { 0x0F, 0x84, 0x8B, 0x03, 0x00, 0x00 };
static volatile LONG g_sceneSectorPass; // 1 while the detail level is above 0
static DetailVar g_detailVars[] = {
    { "m_fSceneLODDistancesMultiplier",    0x00C5058C, 0x00B5DF50, 0x00B20C32, 0, { 1, 2, 3, 10 } }, // keep first
    { "m_fCreatureLODDistancesMultiplier", 0x00C50588, 0x00B5DF1C, 0x00B20C02, 0, { 1, 2, 3, 4 } },
    { "m_fSpeedTreeLODDistancesMultiplier", 0x00C50590, 0x00B5DF84, 0x00B20C62, 0, { 1, 2, 3, 4 } },
    { "m_fGrassVisibilityRange",           0x00C50544, 0x00B5CD38, 0x00B1F972, 0, { 1, 1.5f, 2, 3 } },
    { "m_fGrassFadeRange",                 0x00C50548, 0x00B5CD64, 0x00B1F9A2, 0, { 1, 1.5f, 2, 3 } },
    { "m_fMaxShadowDistance",              0x00C50598, 0x00B5DAB0, 0x00B20752, 0, { 1, 2, 3, 6 } },
    { "m_nMaxShadowLights",                0x00C506C4, 0x00B5BBBC, 0x00B1E652, 1, { 1, 1.5f, 2, 2.5f } },
    { "m_fLightsFadeDistanceFactor",       0x00C505E8, 0x00B5FCE4, 0x00B22C72, 0, { 1, 1.5f, 2, 10 } },
    { "m_fMaxDecalsDist",                  0x00C508E8, 0x00B5EB6C, 0x00B219E2, 0, { 1, 1.5f, 2, 2.5f } },
    { "m_fMaxWeaponVisualsDist",           0x00C508E4, 0x00B5EB40, 0x00B219B2, 0, { 1, 1.5f, 2, 3 } },
    { "m_nReflectionRTSize",               0x00C5051C, 0x00B5BE68, 0x00B1E952, 1, { 1, 2, 4, 4 } }, // water reflections, 512 px
};
#define DETAIL_COUNT (sizeof g_detailVars / sizeof g_detailVars[0])

static const char *const kDetailNames[4] = { "off", "high", "ultra", "extreme" };
// The on-screen message in the game's text language (registry Settings\TextLanguage, ids from Data\languages.2da);
// English for any other.
static const struct { int lang; const WCHAR *title; const WCHAR *level[4]; } kDetailTexts[] = {
    {  3, L"Graphics detail:", { L"Off", L"High", L"Ultra", L"Extreme" } }, // English
    {  5, L"Detale grafiki:", { L"wy\u0142\u0105czone", L"wysokie", L"ultra", L"ekstremalne" } }, // Polish
    { 10, L"Grafikdetails:", { L"Aus", L"Hoch", L"Ultra", L"Extrem" } }, // German
    { 11, L"D\u00e9tails graphiques :", { L"d\u00e9sactiv\u00e9s", L"\u00e9lev\u00e9s", L"ultra", L"extr\u00eames" } }, // French
    { 12, L"Detalle gr\u00e1fico:", { L"desactivado", L"alto", L"ultra", L"extremo" } }, // Spanish
    { 13, L"Dettaglio grafico:", { L"disattivato", L"alto", L"ultra", L"estremo" } }, // Italian
    { 14, L"\u0414\u0435\u0442\u0430\u043b\u0438\u0437\u0430\u0446\u0438\u044f \u0433\u0440\u0430\u0444\u0438\u043a\u0438:", { L"\u0432\u044b\u043a\u043b.", L"\u0432\u044b\u0441\u043e\u043a\u0430\u044f", L"\u0443\u043b\u044c\u0442\u0440\u0430", L"\u044d\u043a\u0441\u0442\u0440\u0435\u043c\u0430\u043b\u044c\u043d\u0430\u044f" } }, // Russian
    { 15, L"Detaily grafiky:", { L"vypnuto", L"vysok\u00e9", L"ultra", L"extr\u00e9mn\u00ed" } }, // Czech
    { 16, L"Grafikai r\u00e9szletess\u00e9g:", { L"ki", L"magas", L"ultra", L"extr\u00e9m" } }, // Hungarian
    { 21, L"\u5716\u5f62\u7d30\u7bc0\uff1a", { L"\u95dc\u9589", L"\u9ad8", L"\u6975\u81f4", L"\u6975\u9650" } }, // Traditional Chinese
};

static int TextLanguage(void)
{
    HKEY k;
    char v[16];
    DWORD type, n = sizeof v - 1;
    int lang = 3;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\CD Projekt RED\\Witcher\\Settings", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "TextLanguage", NULL, &type, (BYTE *)v, &n) == ERROR_SUCCESS && type == REG_SZ) {
            v[n] = 0;
            lang = atoi(v);
        }
        RegCloseKey(k);
    }
    return lang;
}

static void DetailShowLevel(int level)
{
    WCHAR msg[96];
    int i, lang = TextLanguage(), t = 0;
    for (i = 0; i < (int)(sizeof kDetailTexts / sizeof kDetailTexts[0]); i++)
        if (kDetailTexts[i].lang == lang) t = i;
    swprintf_s(msg, sizeof msg / sizeof msg[0], lang == 21 ? L"%s%s   (Ctrl+F12)" : L"%s %s   (Ctrl+F12)", // a full-width colon has its own space
               kDetailTexts[t].title, kDetailTexts[t].level[level]);
    OsdShow(msg, lang == 21 ? L"Microsoft JhengHei" : L"Georgia", 2500);
}
static volatile LONG g_detailLevel;
static int g_detailHotkey, g_detailBaseLogs;
static FrameStats g_fsDetail;
static LONG g_fsDetailLevel = -1;

static void DetailText(char *buf, size_t n, const DetailVar *v, DWORD raw)
{
    if (v->isInt) sprintf_s(buf, n, "%d", (int)raw);
    else sprintf_s(buf, n, "%g", *(float *)&raw);
}

static DWORD DetailScaled(const DetailVar *v, int level)
{
    DWORD raw = v->base;
    if (v->isInt) {
        int i = (int)raw;
        if (i > 0) raw = (DWORD)(int)(i * v->scale[level] + 0.5f);
    } else {
        float f = *(float *)&raw;
        if (f > 0) { f *= v->scale[level]; raw = *(DWORD *)&f; } // negative = "not set" (grass before the menu applies)
    }
    return raw;
}

// One line per round in which the game set some of the values: "<name> <game's value> -> <ours>, ...".
// The sector pass for LOD-group nodes goes on before the scenery LOD distance grows and off after it shrinks.
static void DetailApply(int level)
{
    char s[1024];
    size_t i, n = 0;
    if (level) g_sceneSectorPass = 1;
    for (i = 0; i < DETAIL_COUNT; i++) {
        DetailVar *v = &g_detailVars[i];
        DWORD cur, want;
        int changed;
        if (!v->ok) continue;
        cur = *(volatile DWORD *)v->var;
        changed = !v->have || cur != v->written; // first look, or the game wrote it since
        if (changed) v->base = cur;
        want = DetailScaled(v, level);
        if (changed) {
            if ((v->have || cur) && n < sizeof s - 128) { // 0 = before the settings constructor ran
                char a[32], b[32];
                DetailText(a, sizeof a, v, cur);
                DetailText(b, sizeof b, v, want);
                n += sprintf_s(s + n, sizeof s - n, "%s%s %s -> %s", n ? ", " : "", v->name + 2, a, b);
            }
            v->have = 1;
        }
        if (want != cur) *(volatile DWORD *)v->var = want;
        v->written = want;
    }
    if (!level) g_sceneSectorPass = 0;
    if (n && g_detailBaseLogs < 40) {
        g_detailBaseLogs++;
        Log("graphics detail %d: the game set %s", level, s);
    }
}

static void DetailLogLevel(int level, const char *why)
{
    char s[1024];
    size_t i, n = 0;
    s[0] = 0;
    for (i = 0; i < DETAIL_COUNT; i++) {
        const DetailVar *v = &g_detailVars[i];
        char a[32], b[32];
        if (!v->ok) continue;
        DetailText(a, sizeof a, v, v->base);
        DetailText(b, sizeof b, v, DetailScaled(v, level));
        n += sprintf_s(s + n, sizeof s - n, "%s%s %s -> %s", n ? ", " : "", v->name + 2, a, b);
    }
    Log("graphics detail %d (%s)%s: %s", level, kDetailNames[level], why, s);
}

static DWORD WINAPI DetailThread(LPVOID unused)
{
    int level = g_detailLevel, down = 0;
    (void)unused;
    for (;;) {
        Sleep(50);
        if (g_detailHotkey) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            int now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (now && !down && pid == GetCurrentProcessId()) {
                level = (level + 1) % 4;
                InterlockedExchange(&g_detailLevel, level);
                DetailApply(level);
                DetailLogLevel(level, " (Ctrl+F12)");
                DetailShowLevel(level);
                HookDevice(); // draws the message
                down = now;
                continue;
            }
            down = now;
        }
        if (g_osdUntil) HookDevice(); // the device may have a new vtable (Windows' own d3d9)
        DetailApply(level);
    }
}

// Called once per second from the frame log with that second's statistics.
static void DetailFrameStats(const FrameStats *s)
{
    LONG level = g_detailLevel;
    int flush = g_fsDetail.secs >= 10.0 || (level != g_fsDetailLevel && g_fsDetail.secs >= 2.0);
    if (flush && g_fsDetailLevel >= 0 && g_fsDetail.frames) {
        char t[640];
        double work = (g_fsDetail.secs - (double)g_fsDetail.presTicks / g_qpf.QuadPart) * 1000.0 / g_fsDetail.frames;
        StatsText(t, sizeof t, &g_fsDetail);
        Log("graphics detail %ld (%s), %.1f s: main thread %.1f ms per frame outside Present; %s", g_fsDetailLevel,
            kDetailNames[g_fsDetailLevel], g_fsDetail.secs, work, t);
    }
    if (flush || level != g_fsDetailLevel) {
        memset(&g_fsDetail, 0, sizeof g_fsDetail);
        g_fsDetailLevel = level;
    }
    StatsAdd(&g_fsDetail, s);
}

// The stub the sector test's jz goes to (only reached for a node in a sector that is not visible):
//   cmp dword [g_sceneSectorPass],0 / je skip / cmp dword [esi+0xCC],-1 / je skip / jmp PASS / skip: jmp SKIP
static BOOL ApplySceneSectorPass(void)
{
    BYTE *stub, *p, jz[6] = { 0x0F, 0x84 };
    if (!BytesMatch(SCENE_SECTOR_JZ, kSceneSectorJz, sizeof kSceneSectorJz)) return FALSE;
    stub = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return FALSE;
    p = stub;
    *p++ = 0x83; *p++ = 0x3D; *(DWORD *)p = (DWORD)&g_sceneSectorPass; p += 4; *p++ = 0x00; // cmp [pass],0
    *p++ = 0x74; *p++ = 0x0E;                                                          // je skip
    *p++ = 0x83; *p++ = 0xBE; *(DWORD *)p = 0xCC; p += 4; *p++ = 0xFF;                 // cmp dword [esi+0xCC],-1
    *p++ = 0x74; *p++ = 0x05;                                                          // je skip
    *p++ = 0xE9; PutRel32(p, SCENE_SECTOR_PASS); p += 4;                               // jmp PASS
    *p++ = 0xE9; PutRel32(p, SCENE_SECTOR_SKIP); p += 4;                               // skip: jmp SKIP
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);
    *(DWORD *)(jz + 2) = (DWORD)stub - (SCENE_SECTOR_JZ + 6);
    return WriteCode(SCENE_SECTOR_JZ, jz, sizeof jz);
}

static const char *ApplyDetail(int level, int hotkey)
{
    static char result[96];
    size_t i;
    int ok = 0;
    HANDLE t;
    BYTE push[5] = { 0x68 };
    char name[64];
    for (i = 0; i < DETAIL_COUNT; i++) {
        DetailVar *v = &g_detailVars[i];
        sprintf_s(name, sizeof name, "g_cAuroraSettings.%s", v->name);
        *(DWORD *)(push + 1) = v->var;
        v->ok = BytesMatch(v->nameStr, (const BYTE *)name, strlen(name) + 1) && BytesMatch(v->push, push, 5);
        if (v->ok && i == 0 && !ApplySceneSectorPass()) {
            v->ok = 0; // without the sector pass a longer scenery LOD distance leaves holes
            Log("graphics detail: %s skipped (sector test bytes differ)", v->name);
        } else if (!v->ok) Log("graphics detail: %s skipped (bytes differ)", v->name);
        if (v->ok) ok++;
    }
    if (!ok) return "skipped (bytes differ)";
    g_detailLevel = level;
    g_detailHotkey = hotkey;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf); // Present hook statistics
    t = CreateThread(NULL, 0, DetailThread, NULL, 0, NULL);
    if (!t) return "FAILED (no thread)";
    CloseHandle(t);
    g_detailStats = hotkey && IniFlag("debug", "frame_log", 0); // set before the frame log is applied
    sprintf_s(result, sizeof result, "applied, level %d (%s), %d of %d settings%s", level, kDetailNames[level], ok,
              (int)DETAIL_COUNT, hotkey ? ", Ctrl+F12 cycles the level" : "");
    return result;
}

// ---- Debug: the engine's own messages ([debug] engine_log) ------------------------
// The engine reports through FUN_00546030(fmt, va_list) (wrappers FUN_005464c0, FUN_00546430), e.g.
// "***Dialog interrupted due to speaker's death" from AIActionDialogObject (FUN_008a15e0); retail writes
// these nowhere. We copy them into our log (repeats collapsed, at most ENGINELOG_PER_SEC lines a second).
// Also logged, for a state where Geralt can walk but not act after a conversation (docs/research.md):
// real changes of the dialog bit (8) in the creature state flags (creature+0x350: +0x1E0 flags, +0x1E4 locked;
// FUN_008b18f0), and the bits FUN_008aaf80 means to clear before a mode switch but cannot: it tests
// (flags & bit) == 1, which only holds for bit 1, so bits 2, 8, 0x1000 and 0x40000 stay set. Log only.

#define ENGINELOG_SITE    0x00546030 // void __cdecl FUN_00546030(const char *fmt, va_list args)
#define ENGINELOG_RESUME  0x00546036 // after mov eax,fs:[0]
#define STATEBITS_SITE    0x008B18F0 // void __thiscall FUN_008b18f0(creature, DWORD bits, int set)
#define STATECLEAR_SITE   0x008AAF80 // void __thiscall FUN_008aaf80(creature, DWORD mask)
#define STATE_RESUME      5          // both start with push ebx; mov ebx,[esp+8]
#define STATE_UNCLEARED   (2 | 8 | 0x1000 | 0x40000)
#define ENGINELOG_PER_SEC 200 // an area load writes ~60 lines in its busiest second

static const BYTE kEngineLogOrig[]  = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xFB, 0x43, 0xAF, 0x00 };
static const BYTE kStateBitsOrig[]  = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x50, 0x03, 0x00, 0x00 };
static const BYTE kStateClearOrig[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0xF6, 0xC3, 0x01, 0x56, 0x8B, 0xF1 };
typedef void (__cdecl *EngineLog_t)(const char *, va_list);
typedef int (__cdecl *VsnPrintf_t)(char *, size_t, const char *, va_list);
typedef void (__fastcall *StateBits_t)(void *, void *, DWORD, int);  // thiscall, dummy EDX
typedef void (__fastcall *StateClear_t)(void *, void *, DWORD);
static EngineLog_t g_engineLogOriginal;
static StateBits_t g_stateBitsOriginal;
static StateClear_t g_stateClearOriginal;
static VsnPrintf_t g_crtVsnprintf; // msvcr80's, the CRT the engine formats these with
static CRITICAL_SECTION g_engineLogLock;
static char g_engineLogLast[512];
static int g_engineLogRepeats, g_engineLogLines, g_engineLogDropped;
static DWORD g_engineLogSecond;

static void EngineNote(const char *text)
{
    DWORD now = GetTickCount();
    int repeats = 0, dropped = 0, write = 0;
    if (!TryEnterCriticalSection(&g_engineLogLock)) return; // diagnostics only: never wait
    if (strcmp(text, g_engineLogLast) == 0) {
        g_engineLogRepeats++;
    } else {
        repeats = g_engineLogRepeats;
        g_engineLogRepeats = 0;
        if (now - g_engineLogSecond >= 1000) {
            dropped = g_engineLogDropped;
            g_engineLogDropped = g_engineLogLines = 0;
            g_engineLogSecond = now;
        }
        write = g_engineLogLines < ENGINELOG_PER_SEC;
        if (write) g_engineLogLines++;
        else g_engineLogDropped++;
        // a dropped line is not the last written one, so its repeats are not counted against it
        strncpy_s(g_engineLogLast, sizeof g_engineLogLast, write ? text : "", _TRUNCATE);
    }
    LeaveCriticalSection(&g_engineLogLock);
    if (repeats) Log("engine: (the line above %d more times)", repeats);
    if (dropped) Log("engine: (%d lines dropped, more than %d a second)", dropped, ENGINELOG_PER_SEC);
    if (write) Log("engine: %s", text);
}

static void __cdecl EngineLogHook(const char *fmt, va_list args)
{
    char text[512], *s, *d, *end;
    text[0] = 0;
    if (fmt) {
        __try {
            g_crtVsnprintf(text, sizeof text - 1, fmt, args);
            text[sizeof text - 1] = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strcpy_s(text, sizeof text, "(message could not be formatted)");
        }
    }
    // drop the console colour tags (FUN_005464c0 wraps its text in <c???>...</c>) and line breaks
    for (s = d = text; *s; ) {
        if (s[0] == '<' && s[1] == 'c' && (end = strchr(s, '>')) != NULL && end - s <= 6) { s = end + 1; continue; }
        if (strncmp(s, "</c>", 4) == 0) { s += 4; continue; }
        *d++ = *s == '\r' || *s == '\n' || *s == '\t' ? ' ' : *s;
        s++;
    }
    while (d > text && d[-1] == ' ') d--;
    *d = 0;
    if (text[0]) EngineNote(text);
    g_engineLogOriginal(fmt, args);
}

static BOOL StateFlags(void *creature, DWORD *flags, DWORD *locked, DWORD *id)
{
    __try {
        BYTE *stats = *(BYTE **)((BYTE *)creature + 0x350);
        *flags = *(DWORD *)(stats + 0x1E0);
        *locked = *(DWORD *)(stats + 0x1E4);
        *id = *(DWORD *)((BYTE *)creature + 4);
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

static void __fastcall StateBitsHook(void *creature, void *edx, DWORD bits, int set)
{
    DWORD before = 0, after = 0, locked = 0, id = 0;
    BOOL dialog = (bits & 8) && StateFlags(creature, &before, &locked, &id);
    g_stateBitsOriginal(creature, edx, bits, set);
    // NPC AI clears the bit about once a second on everyone; only real changes are worth a line
    if (dialog && StateFlags(creature, &after, &locked, &id) && (before & 8) != (after & 8)) {
        char text[160];
        sprintf_s(text, sizeof text, "dialog bit %s on object %08X: flags %X -> %X (locked %X)", set == 1 ? "set" : "cleared",
                  id, before, after, locked);
        EngineNote(text);
    }
}

static void __fastcall StateClearHook(void *creature, void *edx, DWORD mask)
{
    DWORD flags = 0, locked = 0, id = 0, left;
    BOOL check = (mask & STATE_UNCLEARED) && StateFlags(creature, &flags, &locked, &id);
    g_stateClearOriginal(creature, edx, mask);
    left = check ? mask & STATE_UNCLEARED & flags & ~locked : 0;
    if (left) {
        char text[200];
        sprintf_s(text, sizeof text, "FUN_008aaf80 (mask %X) leaves bits %X set on object %08X (flags %X)%s", mask, left, id,
                  flags, left & 8 ? ": its dialog is not ended" : "");
        EngineNote(text);
    }
}

// A copy of the first len bytes of a function, then a jump back into it at resume.
static void *Trampoline(const BYTE *orig, size_t len, DWORD resume)
{
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, orig, len);
    t[len] = 0xE9;
    PutRel32(t + len + 1, resume);
    FlushInstructionCache(GetCurrentProcess(), t, len + 5);
    return t;
}

static BOOL JumpFrom(DWORD site, void *target)
{
    BYTE jmp[5] = { 0xE9 };
    *(DWORD *)(jmp + 1) = (DWORD)target - (site + 5);
    return WriteCode(site, jmp, 5);
}

static const char *ApplyEngineLog(void)
{
    if (!BytesMatch(ENGINELOG_SITE, kEngineLogOrig, sizeof kEngineLogOrig) ||
        !BytesMatch(STATEBITS_SITE, kStateBitsOrig, sizeof kStateBitsOrig) ||
        !BytesMatch(STATECLEAR_SITE, kStateClearOrig, sizeof kStateClearOrig))
        return "skipped (bytes differ)";
    g_crtVsnprintf = (VsnPrintf_t)GetProcAddress(GetModuleHandleA("msvcr80.dll"), "_vsnprintf");
    if (!g_crtVsnprintf) return "skipped (no msvcr80 _vsnprintf)";
    g_engineLogOriginal = (EngineLog_t)Trampoline(kEngineLogOrig, ENGINELOG_RESUME - ENGINELOG_SITE, ENGINELOG_RESUME);
    g_stateBitsOriginal = (StateBits_t)Trampoline(kStateBitsOrig, STATE_RESUME, STATEBITS_SITE + STATE_RESUME);
    g_stateClearOriginal = (StateClear_t)Trampoline(kStateClearOrig, STATE_RESUME, STATECLEAR_SITE + STATE_RESUME);
    if (!g_engineLogOriginal || !g_stateBitsOriginal || !g_stateClearOriginal) return "FAILED (no memory)";
    InitializeCriticalSection(&g_engineLogLock);
    if (!JumpFrom(ENGINELOG_SITE, EngineLogHook) || !JumpFrom(STATEBITS_SITE, StateBitsHook) ||
        !JumpFrom(STATECLEAR_SITE, StateClearHook))
        return "FAILED (VirtualProtect)";
    return "applied (engine messages, dialog bit changes, bits FUN_008aaf80 leaves set)";
}

// ---- Debug: who starts and ends mouse look ([debug] mouse_log) --------------------------------
// FUN_0041b830 (thiscall handler, char, char; ret 8) hides the cursor and lets the mouse turn the camera
// (handler+0x348 = 1, DirectInput mouse acquired exclusively); FUN_0041b9b0 (thiscall handler, int; ret 4)
// ends it. In the main menu the cursor was seen to vanish after Win+Shift+S, Shift showing it while held, as
// in the OTS camera. Each call is logged with its return address and the code addresses found on the stack
// above it (the exe has no frame pointers), as is every change of the exclusive mouse (FUN_004cc3a0), which
// hides the cursor too. Shift alone turned out to start it in the main menu without FUN_0041b830. Log only.

#define TURNSTART_SITE  0x0041B830 // push ebx; push esi; mov esi,ecx; call FUN_0044e0d0
#define TURNSTART_CALL  0x0044E0D0
#define TURNEND_SITE    0x0041B9B0 // push esi; mov esi,ecx; cmp dword ptr [esi+0x48],0
#define ACQUIRE_SITE    0x004CC3A0 // thiscall (mouse, int acquire) ret 4: (un)acquires the exclusive DirectInput mouse
#define CAMERA_MODE     0x00C500F0 // 2 = over the shoulder
#define MOUSELOG_LINES  200

static const BYTE kTurnStartOrig[] = { 0x53, 0x56, 0x8B, 0xF1, 0xE8, 0x97, 0x28, 0x03, 0x00 };
static const BYTE kTurnEndOrig[]   = { 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x48, 0x00 };
static const BYTE kAcquireOrig[]   = { 0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1 }; // push ecx/ebx/ebp/esi; mov esi,ecx
typedef void (__fastcall *TurnStart_t)(void *, void *, int, int);
typedef void (__fastcall *TurnEnd_t)(void *, void *, int);
static TurnStart_t g_turnStartOriginal;
static TurnEnd_t g_turnEndOriginal, g_acquireOriginal;
static volatile LONG g_mouseLogLines;

static void MouseLookNote(const char *what, BYTE *handler, DWORD *ret)
{
    char callers[160] = "";
    int n = 0, i, len = 0;
    if (InterlockedIncrement(&g_mouseLogLines) > MOUSELOG_LINES) return;
    __try {
        for (i = 1; i < 96 && n < 8; i++) { // dwords above the return address that look like exe code addresses
            DWORD v = ret[i];
            if (v >= 0x00401000 && v < 0x00AE0000) {
                len += sprintf_s(callers + len, sizeof callers - len, " %06X", v);
                n++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        Log("mouse look: %s from %06X (stack:%s), camera mode %d, handler+0x48 %p, turning %u, foreground %s", what, ret[0],
            callers, *(int *)CAMERA_MODE, *(void **)(handler + 0x48), handler[0x348],
            GetForegroundWindow() == *(HWND *)GAME_HWND ? "game" : "other");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("mouse look: %s from %06X (stack:%s)", what, ret[0], callers);
    }
}

static void __fastcall TurnStartHook(void *handler, void *edx, int a, int b)
{
    MouseLookNote("start", (BYTE *)handler, (DWORD *)_AddressOfReturnAddress());
    g_turnStartOriginal(handler, edx, a, b);
}

static void __fastcall TurnEndHook(void *handler, void *edx, int a)
{
    MouseLookNote("end", (BYTE *)handler, (DWORD *)_AddressOfReturnAddress());
    g_turnEndOriginal(handler, edx, a);
}

// Called every frame with the current state; only changes are logged ([mouse+4] = acquired).
static void __fastcall AcquireHook(void *mouse, void *edx, int acquire)
{
    int was = -1;
    __try { was = *(int *)((BYTE *)mouse + 4); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (was != acquire) {
        DWORD *ret = (DWORD *)_AddressOfReturnAddress();
        char callers[160] = "";
        int n = 0, i, len = 0;
        if (InterlockedIncrement(&g_mouseLogLines) <= MOUSELOG_LINES) {
            __try {
                for (i = 1; i < 96 && n < 8; i++) {
                    DWORD v = ret[i];
                    if (v >= 0x00401000 && v < 0x00AE0000) {
                        len += sprintf_s(callers + len, sizeof callers - len, " %06X", v);
                        n++;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            Log("mouse look: exclusive mouse %s from %06X (stack:%s), camera mode %d, foreground %s",
                acquire ? "acquire" : "release", ret[0], callers, *(int *)CAMERA_MODE,
                GetForegroundWindow() == *(HWND *)GAME_HWND ? "game" : "other");
        }
    }
    g_acquireOriginal(mouse, edx, acquire);
}

static const char *ApplyMouseLog(void)
{
    BYTE *t;
    if (!BytesMatch(TURNSTART_SITE, kTurnStartOrig, sizeof kTurnStartOrig) ||
        !BytesMatch(TURNEND_SITE, kTurnEndOrig, sizeof kTurnEndOrig) ||
        !BytesMatch(ACQUIRE_SITE, kAcquireOrig, sizeof kAcquireOrig))
        return "skipped (bytes differ)";
    // the start's first 9 bytes end in a relative call, so its trampoline re-targets that call
    t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return "FAILED (no memory)";
    memcpy(t, kTurnStartOrig, 4);
    EmitCall(t + 4, TURNSTART_CALL);
    t[9] = 0xE9;
    PutRel32(t + 10, TURNSTART_SITE + sizeof kTurnStartOrig);
    FlushInstructionCache(GetCurrentProcess(), t, 14);
    g_turnStartOriginal = (TurnStart_t)t;
    g_turnEndOriginal = (TurnEnd_t)Trampoline(kTurnEndOrig, sizeof kTurnEndOrig, TURNEND_SITE + sizeof kTurnEndOrig);
    g_acquireOriginal = (TurnEnd_t)Trampoline(kAcquireOrig, sizeof kAcquireOrig, ACQUIRE_SITE + sizeof kAcquireOrig);
    if (!g_turnEndOriginal || !g_acquireOriginal) return "FAILED (no memory)";
    if (!JumpFrom(TURNSTART_SITE, TurnStartHook) || !JumpFrom(TURNEND_SITE, TurnEndHook) || !JumpFrom(ACQUIRE_SITE, AcquireHook))
        return "FAILED (VirtualProtect)";
    return "applied (camera turn start/end and exclusive mouse changes with callers)";
}

// ---- Patch driver -----------------------------------------------------------

// The Steam exe is SteamStub-wrapped: our DLL loads before the stub decrypts the code,
// so we wait until fix 1's original bytes are visible (= code decrypted) before patching anything.
static void TryPatch(void)
{
    if (g_patchState != 0 || !BytesMatch(DEATH_SITE, kDeathOrig, sizeof kDeathOrig)) return;
    if (InterlockedCompareExchange(&g_patchState, 1, 0) != 0) return;
    Log("fix 1 (death-effect NULL creator): %s", FixEnabled("fix1") ? ApplyDeathFix() : "disabled in ini");
    Log("fix 2 (main thread CPU 0 pin): %s", FixEnabled("fix2") ? ApplyAffinityFix() : "disabled in ini");
    Log("fix 3 (shader effect cache churn): %s", FixEnabled("fix3") ? ApplyEffectCacheFix() : "disabled in ini");
    Log("fix 4 (save list thumbnail rewrite): %s", FixEnabled("fix4") ? ApplyThumbFix() : "disabled in ini");
    {
        int fast = FixEnabled("fix5"), timing = IniFlag("debug", "crc_timing", 0);
        Log("fix 5 (faster crc32 for saves): %s", fast || timing ? ApplyCrcFix(fast, timing) : "disabled in ini");
    }
    Log("fix 6 (frame-rate dependent hair damping): %s", FixEnabled("fix6") ? ApplyDanglyFix() : "disabled in ini");
    Log("fix 7 (warm save files for the first save list): %s", FixEnabled("fix7") ? ApplySaveWarmup() : "disabled in ini");
    {
        const char *mip, *hook = ApplyTextureHook(FixEnabled("fix8"), IniFlag("debug", "texture_timing", 0), &mip);
        Log("fix 8 (cache mip levels D3DX builds on the CPU): %s", mip);
        if (IniFlag("debug", "texture_timing", 0)) Log("debug: texture timing %s", hook);
    }
    Log("fix 9 (texture wait polls every 250 ms): %s", FixEnabled("fix9") ? ApplyTexWaitFix() : "disabled in ini");
    Log("fix 10 (cursor parked in the top-left area during mouse look): %s", FixEnabled("fix10") ? ApplyCursorParkFix() : "disabled in ini");
    Log("fix 11 (up to 1 s late after coming back to the game): %s", FixEnabled("fix11") ? ApplyInactiveSleepFix() : "disabled in ini");
    Log("fix 12 (Shift hides the cursor in the main menu): %s", FixEnabled("fix12") ? ApplyMainMenuShiftFix() : "disabled in ini");
    Log("qol 1 (OTS camera: middle mouse button turns it in active pause): %s",
        IniFlag("qol", "ots_pause_mmb_camera", 1) ? ApplyOtsMmb() : "disabled in ini");
    {
        const char *from;
        int on = BorderlessWanted(&from);
        Log("qol 3 (borderless window instead of a framed one in windowed mode): %s (set in the %s)",
            ApplyBorderless(on), from);
    }
    {
        int level = IniInt("graphics", "detail", 0);
        int hotkey = IniFlag("graphics", "hotkey", 1) || IniFlag("debug", "detail_hotkey", 0); // the old [debug] name still counts
        if (level < 0 || level > 3) level = 0;
        Log("graphics detail (LOD, grass, shadow and light distances): %s",
            level || hotkey ? ApplyDetail(level, hotkey) : "off (detail=0)");
    }
    if (IniFlag("debug", "savelist_timing", 0)) Log("debug: save-list timing %s", ApplySaveListTiming());
    if (IniFlag("debug", "wait_timing", 0)) Log("debug: wait timing %s", ApplyWaitTiming());
    if (IniFlag("debug", "frame_log", 0)) Log("debug: frame log %s", ApplyFrameLog());
    if (IniFlag("debug", "engine_log", 0)) Log("debug: engine log %s", ApplyEngineLog());
    if (IniFlag("debug", "mouse_log", 0)) Log("debug: mouse log %s", ApplyMouseLog());
}

static DWORD WINAPI PatchThread(LPVOID unused)
{
    int i;
    (void)unused;
    for (i = 0; i < 6000 && g_patchState == 0; i++) { // up to ~60 s
        TryPatch();
        Sleep(10);
    }
    if (g_patchState == 0 && InterlockedCompareExchange(&g_patchState, 2, 0) == 0)
        Log("code never matched - different game version? Nothing patched.");
    return 0;
}

// ---- Lua part: is the save-list hook loaded? ---------------------------------
// The save-list Lua fix (Scripts\enginefix_hook.lua) only runs if Scripts\testtools.lua, the shared
// startup loader, lists it. Another mod can replace testtools.lua with an older loader, so say so.

static void CheckLuaLoader(void)
{
    char dir[MAX_PATH], path[MAX_PATH], buf[4096], *slash;
    FILE *f;
    size_t n;
    int listed = 0;

    GetModuleFileNameA(g_self, dir, MAX_PATH);
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    sprintf_s(path, MAX_PATH, "%sScripts\\enginefix_hook.lua", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Log("Lua save-list fix: Scripts\\enginefix_hook.lua not found (inactive)");
        return;
    }
    sprintf_s(path, MAX_PATH, "%sScripts\\testtools.lua", dir);
    if (fopen_s(&f, path, "rb") == 0) {
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        listed = strstr(buf, "\"enginefix_hook\"") != NULL;
        fclose(f);
    }
    Log(listed ? "Lua save-list fix: loaded by Scripts\\testtools.lua"
               : "Lua save-list fix: INACTIVE, Scripts\\testtools.lua does not list enginefix_hook "
                 "(another mod may have replaced it; copy the one from this mod back)");

    // QoL 4 is bound by enginefix_hook.lua; this only says what it will do.
    sprintf_s(path, MAX_PATH, "%sScripts\\quickloot_hook.lua", dir);
    if (!listed) {
        Log("qol 4 (quick loot hotkey): inactive with the Lua part");
    } else if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        Log("qol 4 (quick loot hotkey): Witcher Quick Loot is installed (Scripts\\quickloot_hook.lua), its hotkey is used");
    } else if (!IniFlag("qol", "quick_loot", 1)) {
        Log("qol 4 (quick loot hotkey): disabled in ini");
    } else {
        char ini[MAX_PATH], key[32];
        IniPath(ini);
        GetPrivateProfileStringA("qol", "quick_loot_key", "", key, sizeof key, ini);
        key[strcspn(key, " \t;")] = 0;
        Log("qol 4 (quick loot hotkey): %s, bound by Scripts\\enginefix_hook.lua (Take all in the loot window%s)",
            key[0] ? key : "SPACE", !key[0] || _stricmp(key, "SPACE") == 0 ? ", pause elsewhere" : "");
    }

    // QoL 5 is done by enginefix_hook.lua (it filters the intro's playCutscene calls).
    Log("qol 5 (skip the intro movies at startup): %s", !IniFlag("qol", "skip_intro", 0) ? "off (ini)"
        : listed ? "on, by Scripts\\enginefix_hook.lua" : "inactive with the Lua part");
}

// ---- dinput8 proxy ----------------------------------------------------------

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    static DirectInput8Create_t real;
    TryPatch(); // code is certainly decrypted by the time input is initialised
    if (g_patchState == 1 && FixEnabled("fix2")) UnpinMainThread();
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, MAX_PATH, "\\dinput8.dll");
        g_realDinput = LoadLibraryA(path);
        if (g_realDinput) real = (DirectInput8Create_t)GetProcAddress(g_realDinput, "DirectInput8Create");
        if (!real) { Log("could not load real dinput8.dll from %s", path); return E_FAIL; }
    }
    return real(inst, ver, riid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;
        g_self = inst;
        g_mainThreadId = GetCurrentThreadId(); // static imports load on the process's initial thread
        DisableThreadLibraryCalls(inst);
        Log("enginefix %s loaded", ENGINEFIX_VERSION);
        CheckLuaLoader();
        t = CreateThread(NULL, 0, PatchThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_waitOff = 1; // our CRT is shutting down: the Sleep hook must not log any more
    }
    return TRUE;
}
