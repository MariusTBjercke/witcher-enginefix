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
// Each fix can be disabled in witcher_enginefix.ini next to the DLL ([fixes] fix2=0 etc.).

#include <windows.h>
#include <stdio.h>
#include <math.h>

static HMODULE g_self;
static HMODULE g_realDinput;
static DWORD g_mainThreadId;
static volatile LONG g_patchState; // 0 = pending, 1 = done, 2 = gave up

static void Log(const char *fmt, ...)
{
    char path[MAX_PATH], *slash;
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    GetModuleFileNameA(g_self, path, MAX_PATH);
    slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? (slash + 1 - path) : 0), "witcher_enginefix.log");
    if (fopen_s(&f, path, "a") != 0) return;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
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

static int IniFlag(const char *section, const char *key, int def)
{
    char path[MAX_PATH], *slash;
    GetModuleFileNameA(g_self, path, MAX_PATH);
    slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? (slash + 1 - path) : 0), "witcher_enginefix.ini");
    return GetPrivateProfileIntA(section, key, def, path) != 0;
}

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

    g_danglyLog = IniFlag("debug", "dangly_log", 1) ? 5 : 0;
    *(DWORD *)(patch + 1) = (DWORD)stub - (DANGLY_SITE + 5);
    return WriteCode(DANGLY_SITE, patch, sizeof patch) ? "applied" : "FAILED (VirtualProtect)";
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
    if (id == T_LIST) { HookCreateFileOnce(); g_inList = 1; }
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
        int fast = FixEnabled("fix5"), timing = IniFlag("debug", "crc_timing", 1);
        Log("fix 5 (faster crc32 for saves): %s", fast || timing ? ApplyCrcFix(fast, timing) : "disabled in ini");
    }
    Log("fix 6 (frame-rate dependent hair damping): %s", FixEnabled("fix6") ? ApplyDanglyFix() : "disabled in ini");
    if (IniFlag("debug", "savelist_timing", 1)) Log("debug: save-list timing %s", ApplySaveListTiming());
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
        Log("enginefix loaded");
        t = CreateThread(NULL, 0, PatchThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
