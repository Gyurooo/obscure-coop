// obscoop: proxy xinput1_3.dll for Obscure (2004, Steam build 215286).
//
// Stage 0 (isolation + logging, no networking yet):
//  - forwards XInput to the system DLL and logs which user indices the game polls;
//  - keeps the test copy away from the owner's files:
//      * file paths inside "...\steamapps\common\Obscure\" outside our folder -> our folder
//        (video_cfg.dat comes from SteamApps()->GetAppInstallDir());
//      * SHGetFolderPathA(CSIDL_PERSONAL) -> <exe dir>\coop_profile\Documents;
//      * ISteamRemoteStorage (v011) save slots -> files in <exe dir>\coop_profile\remote;
//  - logs every file the game opens for writing and every unexpected remote-storage slot.
// Everything is patched in the exe's import table of this process only.

#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define EXPECTED_EXE_SIZE 3662336u
#define EXPECTED_PE_TIMESTAMP 1395162021u

static char g_dir[MAX_PATH];        // exe folder, no trailing slash
static char g_data[MAX_PATH];       // where the log and co-op saves go (see choose_data_dir)
static char g_profile[MAX_PATH];    // <data>\coop_profile
static char g_remote[MAX_PATH];     // <dir>\coop_profile\remote
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static LONG g_redirects, g_blocked;

static void logf(const char *fmt, ...)
{
	char buf[1024];
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = _snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond,
		t.wMilliseconds, GetCurrentThreadId());
	va_list ap;
	va_start(ap, fmt);
	int m = _vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
	va_end(ap);
	if (m < 0)
		m = (int)strlen(buf + n);
	n += m;
	buf[n++] = '\r';
	buf[n++] = '\n';
	EnterCriticalSection(&g_log_cs);
	if (g_log != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(g_log, buf, n, &w, NULL);
	}
	LeaveCriticalSection(&g_log_cs);
}

// ---- path redirection ------------------------------------------------------

static int starts_with_ci(const char *s, const char *prefix)
{
	return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

// Returns 1 and fills out if path points into a Steam "Obscure" install that is not ours.
static int map_path(const char *path, char *out, size_t outsz)
{
	static const char marker[] = "\\steamapps\\common\\obscure\\";
	if (!path || starts_with_ci(path, g_dir))
		return 0;
	char low[MAX_PATH * 2];
	size_t n = strlen(path);
	if (n >= sizeof low)
		return 0;
	for (size_t i = 0; i <= n; i++) {
		char c = path[i];
		low[i] = (c == '/') ? '\\' : (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
	}
	char *hit = strstr(low, marker);
	if (!hit)
		return 0;
	const char *rest = path + (hit - low) + sizeof marker - 1;
	_snprintf(out, outsz, "%s\\%s", g_dir, rest);
	out[outsz - 1] = 0;
	return 1;
}

static HANDLE(WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *real_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL(WINAPI *real_CreateDirectoryA)(LPCSTR, LPSECURITY_ATTRIBUTES);
static HANDLE(WINAPI *real_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static HANDLE(WINAPI *real_FindFirstFileExA)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
static HRESULT(WINAPI *real_SHGetFolderPathA)(HWND, int, HANDLE, DWORD, LPSTR);

static const char *redirect(const char *path, char *buf, size_t bufsz, const char *api)
{
	if (map_path(path, buf, bufsz)) {
		InterlockedIncrement(&g_redirects);
		logf("redirect %s: %s -> %s", api, path, buf);
		return buf;
	}
	return path;
}

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
	DWORD disp, DWORD flags, HANDLE tmpl)
{
	char buf[MAX_PATH * 2];
	const char *p = redirect(name, buf, sizeof buf, "CreateFileA");
	HANDLE h = real_CreateFileA(p, access, share, sa, disp, flags, tmpl);
	if (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA))
		logf("open-for-write %s disp=%lu -> %s", p ? p : "(null)", disp, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
	return h;
}

static HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
	DWORD disp, DWORD flags, HANDLE tmpl)
{
	char a[MAX_PATH * 2], b[MAX_PATH * 2];
	if (name && WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, NULL, NULL) && map_path(a, b, sizeof b)) {
		WCHAR w[MAX_PATH * 2];
		MultiByteToWideChar(CP_ACP, 0, b, -1, w, MAX_PATH * 2);
		InterlockedIncrement(&g_redirects);
		logf("redirect CreateFileW: %s -> %s", a, b);
		return real_CreateFileW(w, access, share, sa, disp, flags, tmpl);
	}
	if (name && (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)))
		logf("open-for-write (W) %s", WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, NULL, NULL) ? a : "?");
	return real_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI hk_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
{
	char buf[MAX_PATH * 2];
	const char *p = redirect(name, buf, sizeof buf, "CreateDirectoryA");
	logf("CreateDirectoryA %s", p ? p : "(null)");
	return real_CreateDirectoryA(p, sa);
}

static HANDLE WINAPI hk_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd)
{
	char buf[MAX_PATH * 2];
	return real_FindFirstFileA(redirect(name, buf, sizeof buf, "FindFirstFileA"), fd);
}

static HANDLE WINAPI hk_FindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS lvl, LPVOID fd, FINDEX_SEARCH_OPS op,
	LPVOID filter, DWORD flags)
{
	char buf[MAX_PATH * 2];
	return real_FindFirstFileExA(redirect(name, buf, sizeof buf, "FindFirstFileExA"), lvl, fd, op, filter, flags);
}

static HRESULT WINAPI hk_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR out)
{
	if ((csidl & 0xff) == CSIDL_PERSONAL) {
		_snprintf(out, MAX_PATH, "%s\\Documents", g_profile);
		out[MAX_PATH - 1] = 0;
		real_CreateDirectoryA(g_profile, NULL);
		real_CreateDirectoryA(out, NULL);
		InterlockedIncrement(&g_redirects);
		logf("redirect SHGetFolderPathA(csidl=%#x) -> %s", csidl, out);
		return S_OK;
	}
	logf("SHGetFolderPathA(csidl=%#x) passed through", csidl);
	return real_SHGetFolderPathA(hwnd, csidl, token, flags, out);
}

// ---- ISteamRemoteStorage (STEAMREMOTESTORAGE_INTERFACE_VERSION011) ---------
// Steam interfaces are thiscall on 32-bit MSVC: this in ecx, callee pops. A __fastcall function
// with an unused edx argument has the same stack layout.

#define RS_SLOTS 64
typedef void *(*SteamRemoteStorage_t)(void);
static SteamRemoteStorage_t real_SteamRemoteStorage;
static void *g_rs_real;
static void **g_rs_real_vt;
static void *g_rs_vt[RS_SLOTS];
static struct { void **vt; } g_rs_fake = { g_rs_vt };
static unsigned char *g_stubs;
static LONG g_slot_seen[RS_SLOTS];

static void remote_path(const char *name, char *out, size_t outsz)
{
	char safe[MAX_PATH];
	size_t i = 0;
	for (; name[i] && i < sizeof safe - 1; i++) {
		char c = name[i];
		safe[i] = (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
			c == '>' || c == '|') ? '_' : c;
	}
	safe[i] = 0;
	_snprintf(out, outsz, "%s\\%s", g_remote, safe);
	out[outsz - 1] = 0;
}

static int read_whole(const char *path, void *buf, int size)
{
	HANDLE h = real_CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return -1;
	DWORD got = 0;
	if (buf && size > 0)
		ReadFile(h, buf, (DWORD)size, &got, NULL);
	else
		got = GetFileSize(h, NULL);
	CloseHandle(h);
	return (int)got;
}

static bool __fastcall rs_FileWrite(void *self, void *edx, const char *name, const void *data, int size)
{
	char p[MAX_PATH * 2];
	(void)self; (void)edx;
	remote_path(name, p, sizeof p);
	real_CreateDirectoryA(g_profile, NULL);
	real_CreateDirectoryA(g_remote, NULL);
	HANDLE h = real_CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD w = 0;
	BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data, (DWORD)size, &w, NULL) && w == (DWORD)size;
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	logf("remote FileWrite %s (%d bytes) -> local %s", name, size, ok ? "ok" : "FAIL");
	return ok;
}

// The save the game loaded last (host): sent to a joining guest (net.c / sync.c).
static BYTE g_last_save[65536];
static volatile LONG g_last_save_len, g_last_save_ver;
static char g_last_save_name[32];

static int __fastcall rs_FileRead(void *self, void *edx, const char *name, void *buf, int size)
{
	char p[MAX_PATH * 2];
	(void)self; (void)edx;
	remote_path(name, p, sizeof p);
	int got = read_whole(p, buf, size);
	if (got > 0 && got <= (int)sizeof g_last_save && (!_strnicmp(name, "game", 4) || !_stricmp(name, "recover.sav")) &&
		strstr(name, ".sav")) {
		memcpy(g_last_save, buf, got);
		strncpy(g_last_save_name, name, sizeof g_last_save_name - 1);
		g_last_save_len = got;
		InterlockedIncrement(&g_last_save_ver);
	}
	logf("remote FileRead %s (%d) -> %d", name, size, got);
	return got < 0 ? 0 : got;
}

static bool __fastcall rs_FileDelete(void *self, void *edx, const char *name)
{
	char p[MAX_PATH * 2];
	(void)self; (void)edx;
	remote_path(name, p, sizeof p);
	BOOL ok = DeleteFileA(p);
	logf("remote FileDelete %s -> %d", name, ok);
	return ok;
}

static bool __fastcall rs_FileExists(void *self, void *edx, const char *name)
{
	char p[MAX_PATH * 2];
	(void)self; (void)edx;
	remote_path(name, p, sizeof p);
	DWORD a = GetFileAttributesA(p);
	bool ok = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	logf("remote FileExists %s -> %d", name, ok);
	return ok;
}

static int __fastcall rs_GetFileSize(void *self, void *edx, const char *name)
{
	char p[MAX_PATH * 2];
	(void)self; (void)edx;
	remote_path(name, p, sizeof p);
	int n = read_whole(p, NULL, 0);
	logf("remote GetFileSize %s -> %d", name, n);
	return n < 0 ? 0 : n;
}

static int64_t __fastcall rs_GetFileTimestamp(void *self, void *edx, const char *name)
{
	char p[MAX_PATH * 2];
	WIN32_FILE_ATTRIBUTE_DATA fa;
	(void)self; (void)edx;
	remote_path(name, p, sizeof p);
	if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa))
		return 0;
	uint64_t ft = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
	return (int64_t)((ft - 116444736000000000ULL) / 10000000ULL);   // unix seconds
}

static void __cdecl rs_note_slot(int slot)
{
	if (InterlockedIncrement(&g_slot_seen[slot]) == 1)
		logf("remote-storage slot %d forwarded to Steam (first call)", slot);
}

// Forwarding stub per unhandled slot: log the slot once, then jump to the real method with
// ecx = real interface. Layout: pushad; push imm8; call rel32; add esp,4; popad;
// mov ecx,imm32; jmp [imm32]  = 1+2+5+3+1+5+6 = 23 bytes, padded to 32.
static void build_stubs(void)
{
	g_stubs = VirtualAlloc(NULL, RS_SLOTS * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	for (int i = 0; i < RS_SLOTS; i++) {
		unsigned char *s = g_stubs + i * 32;
		s[0] = 0x60;
		s[1] = 0x6a; s[2] = (unsigned char)i;
		s[3] = 0xe8; *(int32_t *)(s + 4) = (int32_t)((uintptr_t)rs_note_slot - (uintptr_t)(s + 8));
		s[8] = 0x83; s[9] = 0xc4; s[10] = 0x04;
		s[11] = 0x61;
		s[12] = 0xb9; *(void **)(s + 13) = g_rs_real;
		s[17] = 0xff; s[18] = 0x25; *(void ***)(s + 19) = &g_rs_real_vt[i];
		g_rs_vt[i] = s;
	}
	g_rs_vt[0] = (void *)rs_FileWrite;
	g_rs_vt[1] = (void *)rs_FileRead;
	g_rs_vt[3] = (void *)rs_FileDelete;
	g_rs_vt[10] = (void *)rs_FileExists;
	g_rs_vt[12] = (void *)rs_GetFileSize;
	g_rs_vt[13] = (void *)rs_GetFileTimestamp;
	// Write-type slots 2 and 4..9 are refused in hk_SteamRemoteStorage. Everything from 14 on is
	// forwarded with a one-time log line; the game was not seen calling any of them.
}

static bool __fastcall rs_refuse1(void *self, void *edx, const char *name)
{
	(void)self; (void)edx;
	InterlockedIncrement(&g_blocked);
	logf("remote-storage write-type call refused: %s", name ? name : "?");
	return false;
}

// Refusers for write-type slots, with each slot's v011 argument list so the stack is popped right.
static uint64_t __fastcall rs_refuse_share(void *self, void *edx, const char *name)
{
	(void)self; (void)edx;
	InterlockedIncrement(&g_blocked);
	logf("remote-storage FileShare refused: %s", name ? name : "?");
	return 0;
}

static bool __fastcall rs_refuse_sync(void *self, void *edx, const char *name, int platforms)
{
	(void)self; (void)edx; (void)platforms;
	InterlockedIncrement(&g_blocked);
	logf("remote-storage SetSyncPlatforms refused: %s", name ? name : "?");
	return false;
}

static uint64_t __fastcall rs_refuse_stream_open(void *self, void *edx, const char *name)
{
	(void)self; (void)edx;
	InterlockedIncrement(&g_blocked);
	logf("remote-storage FileWriteStreamOpen refused: %s", name ? name : "?");
	return (uint64_t)-1;   // k_UGCFileStreamHandleInvalid
}

static bool __fastcall rs_refuse_stream_chunk(void *self, void *edx, uint64_t h, const void *data, int size)
{
	(void)self; (void)edx; (void)h; (void)data; (void)size;
	InterlockedIncrement(&g_blocked);
	logf("remote-storage FileWriteStreamWriteChunk refused");
	return false;
}

static bool __fastcall rs_refuse_stream_end(void *self, void *edx, uint64_t h)
{
	(void)self; (void)edx; (void)h;
	InterlockedIncrement(&g_blocked);
	logf("remote-storage FileWriteStream close/cancel refused");
	return false;
}

static void *hk_SteamRemoteStorage(void)
{
	if (!g_rs_real) {
		g_rs_real = real_SteamRemoteStorage();
		if (!g_rs_real) {
			logf("real SteamRemoteStorage() returned NULL");
			return NULL;
		}
		g_rs_real_vt = *(void ***)g_rs_real;
		build_stubs();
		g_rs_vt[2] = (void *)rs_refuse1;               // FileForget(const char*)
		g_rs_vt[4] = (void *)rs_refuse_share;          // FileShare(const char*)
		g_rs_vt[5] = (void *)rs_refuse_sync;           // SetSyncPlatforms(const char*, int)
		g_rs_vt[6] = (void *)rs_refuse_stream_open;    // FileWriteStreamOpen(const char*)
		g_rs_vt[7] = (void *)rs_refuse_stream_chunk;   // FileWriteStreamWriteChunk(handle, void*, int)
		g_rs_vt[8] = (void *)rs_refuse_stream_end;     // FileWriteStreamClose(handle)
		g_rs_vt[9] = (void *)rs_refuse_stream_end;     // FileWriteStreamCancel(handle)
		logf("SteamRemoteStorage proxied: real %p vtable %p, saves -> %s", g_rs_real, g_rs_real_vt, g_remote);
	}
	return &g_rs_fake;
}

// ---- XInput ----------------------------------------------------------------

typedef DWORD(WINAPI *XInputGetState_t)(DWORD, void *);
typedef DWORD(WINAPI *XInputSetState_t)(DWORD, void *);
static HMODULE g_xinput;
static XInputGetState_t real_XInputGetState;
static XInputSetState_t real_XInputSetState;
static LONG g_poll_seen[8];
static volatile LONG g_polls[4], g_virtual_reads[4];

static void load_xinput(void)
{
	if (g_xinput)
		return;
	char p[MAX_PATH];
	GetSystemDirectoryA(p, MAX_PATH);   // SysWOW64 for this 32-bit process
	strcat(p, "\\xinput1_3.dll");
	g_xinput = LoadLibraryA(p);
	if (g_xinput) {
		real_XInputGetState = (XInputGetState_t)GetProcAddress(g_xinput, (LPCSTR)2);
		real_XInputSetState = (XInputSetState_t)GetProcAddress(g_xinput, (LPCSTR)3);
	}
	logf("system xinput1_3 %s (GetState %p SetState %p)", g_xinput ? "loaded" : "MISSING", real_XInputGetState,
		real_XInputSetState);
}

// Virtual pads: set from the command pipe, override the real device for that user index.
// XINPUT_STATE layout: DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry.
#pragma pack(push, 1)
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xstate;
#pragma pack(pop)
static volatile LONG g_vpad_on[4];
static int g_hide_real_pads;   // OBSCOOP_HIDE_REAL_PADS=1: real controllers are never shown to this copy
static xstate g_vpad[4];
static CRITICAL_SECTION g_vpad_cs;

static int g_role;   // 0 off, 1 host, 2 guest (net.c)

// The pad as a player holds it: the virtual pad if set, else the real device unless hidden.
static DWORD pad_read(DWORD user, void *state)
{
	if (user < 4 && g_vpad_on[user]) {
		EnterCriticalSection(&g_vpad_cs);
		memcpy(state, &g_vpad[user], sizeof(xstate));
		LeaveCriticalSection(&g_vpad_cs);
		return ERROR_SUCCESS;
	}
	return (real_XInputGetState && !g_hide_real_pads) ? real_XInputGetState(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetState(DWORD user, void *state)
{
	load_xinput();
	if (g_role == 2)   // guest: the pad belongs to the host's player 2, not to this local game
		return ERROR_DEVICE_NOT_CONNECTED;
	if (user < 4)
		InterlockedIncrement(&g_polls[user]);
	if (user < 4 && g_vpad_on[user]) {
		InterlockedIncrement(&g_virtual_reads[user]);
		EnterCriticalSection(&g_vpad_cs);
		memcpy(state, &g_vpad[user], sizeof(xstate));
		LeaveCriticalSection(&g_vpad_cs);
		return ERROR_SUCCESS;
	}
	DWORD r = (real_XInputGetState && !g_hide_real_pads) ? real_XInputGetState(user, state) : ERROR_DEVICE_NOT_CONNECTED;
	if (user < 8 && InterlockedIncrement(&g_poll_seen[user]) == 1)
		logf("game polls XInput user %lu (first result %lu)", user, r);
	return r;
}

DWORD WINAPI XInputSetState(DWORD user, void *vib)
{
	load_xinput();
	if (g_hide_real_pads || (user < 4 && g_vpad_on[user]))
		return user < 4 && g_vpad_on[user] ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
	return real_XInputSetState ? real_XInputSetState(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

// ---- DirectInput keyboard ------------------------------------------------------
// Virtual keys from the command pipe are OR-ed into the game's own keyboard reads
// (IDirectInputDevice8A::GetDeviceState on the system keyboard), so bots never touch
// the real desktop input. COM methods are __stdcall with this as the first argument.

static const GUID GUID_SysKeyboard_ = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT(WINAPI *CreateDevice_t)(void *, REFGUID, void **, LPUNKNOWN);
typedef HRESULT(WINAPI *GetDeviceState_t)(void *, DWORD, LPVOID);
static DirectInput8Create_t real_DirectInput8Create;
static CreateDevice_t real_CreateDevice;
static GetDeviceState_t real_GetDeviceState;
static const GUID GUID_SysMouse_ = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static void *volatile g_kbd;            // newest system keyboard device
static void *volatile g_kbds[16];       // every system keyboard device the game created (it rebuilds them)
static int is_kbd(void *dev)
{
	for (int i = 0; i < 16; i++)
		if (g_kbds[i] == dev)
			return 1;
	return 0;
}
static void *volatile g_mouse;
static volatile LONG g_mdx, g_mdy;          // pending virtual mouse movement, consumed by the next read
static volatile BYTE g_mbuttons;            // bit i = button i down
static volatile LONG g_mouse_reads;
static volatile BYTE g_vkeys[256];
static int g_background;

static void patch_slot(void **slot, void *hook, void **orig)
{
	DWORD old;
	if (*slot == hook)
		return;
	if (orig && !*orig)
		*orig = *slot;
	VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
	*slot = hook;
	VirtualProtect(slot, sizeof(void *), old, &old);
}

static HRESULT WINAPI hk_GetDeviceState(void *self, DWORD cb, LPVOID data)
{
	HRESULT r = real_GetDeviceState(self, cb, data);
	if (self == g_mouse && data && cb >= 16) {
		// DIMOUSESTATE / DIMOUSESTATE2: LONG lX, lY, lZ; BYTE rgbButtons[4 or 8]
		InterlockedIncrement(&g_mouse_reads);
		LONG dx = InterlockedExchange(&g_mdx, 0), dy = InterlockedExchange(&g_mdy, 0);
		BYTE mb = g_mbuttons;
		if (!dx && !dy && !mb && !(FAILED(r) && g_background))
			return r;
		if (FAILED(r))
			memset(data, 0, cb);
		((LONG *)data)[0] += dx;
		((LONG *)data)[1] += dy;
		for (DWORD i = 0; i < 4 && 12 + i < cb; i++)
			if (mb & (1 << i))
				((BYTE *)data)[12 + i] = 0x80;
		return S_OK;
	}
	if (!is_kbd(self) || cb < 256 || !data)
		return r;
	int any = 0;
	for (int i = 0; i < 256; i++)
		any |= g_vkeys[i];
	// In the background the real read fails (device not acquired). Report an empty, successful
	// read there too, so the game sees each virtual key go up again and the next press registers.
	if (!any && !(FAILED(r) && g_background))
		return r;
	if (FAILED(r))
		memset(data, 0, cb);
	for (int i = 0; i < 256; i++)
		if (g_vkeys[i])
			((BYTE *)data)[i] = 0x80;
	return S_OK;
}

// Buffered keyboard input (GetDeviceData, slot 10): virtual key changes are queued as
// DIDEVICEOBJECTDATA records (dwOfs = DIK code, dwData = 0x80 down / 0 up) and appended to what
// the real device returns. Screens that wait for "press a key" read this instead of the snapshot.
typedef HRESULT(WINAPI *GetDeviceData_t)(void *, DWORD, void *, DWORD *, DWORD);
static GetDeviceData_t real_GetDeviceData;
#define KEYQ 64
static struct { BYTE dik, down; } g_keyq[KEYQ];
static volatile LONG g_keyq_head, g_keyq_tail;   // head = next to read, tail = next to write
static CRITICAL_SECTION g_keyq_cs;
static volatile LONG g_kbd_data_reads;

static void keyq_push(int dik, int down)
{
	EnterCriticalSection(&g_keyq_cs);
	if (g_keyq_tail - g_keyq_head < KEYQ) {
		g_keyq[g_keyq_tail % KEYQ].dik = (BYTE)dik;
		g_keyq[g_keyq_tail % KEYQ].down = (BYTE)down;
		g_keyq_tail++;
	}
	LeaveCriticalSection(&g_keyq_cs);
}

static HRESULT WINAPI hk_GetDeviceData(void *self, DWORD cb, void *rgdod, DWORD *inout, DWORD flags)
{
	if (!is_kbd(self))
		return real_GetDeviceData(self, cb, rgdod, inout, flags);
	InterlockedIncrement(&g_kbd_data_reads);
	DWORD cap = inout ? *inout : 0;
	HRESULT r = real_GetDeviceData(self, cb, rgdod, inout, flags);
	if (g_keyq_head == g_keyq_tail || !inout || cb < 16)
		return r;
	DWORD have = SUCCEEDED(r) ? *inout : 0;
	EnterCriticalSection(&g_keyq_cs);
	LONG h = g_keyq_head;
	while (h != g_keyq_tail && (!rgdod || have < cap)) {
		if (rgdod) {
			BYTE *rec = (BYTE *)rgdod + have * cb;
			memset(rec, 0, cb);
			((DWORD *)rec)[0] = g_keyq[h % KEYQ].dik;
			((DWORD *)rec)[1] = g_keyq[h % KEYQ].down ? 0x80 : 0;
			((DWORD *)rec)[2] = GetTickCount();
			((DWORD *)rec)[3] = 0x7fff0000u + (DWORD)h;   // sequence numbers after the real ones
		}
		have++;
		h++;
	}
	if (!(flags & 1))   // DIGDD_PEEK leaves the queue as it is
		g_keyq_head = h;
	LeaveCriticalSection(&g_keyq_cs);
	*inout = have;
	return S_OK;
}

static HRESULT WINAPI hk_CreateDevice(void *self, REFGUID guid, void **dev, LPUNKNOWN outer)
{
	HRESULT r = real_CreateDevice(self, guid, dev, outer);
	if (SUCCEEDED(r) && dev && *dev && IsEqualGUID(guid, &GUID_SysKeyboard_)) {
		g_kbd = *dev;
		for (int i = 0; i < 16; i++)
			if (!g_kbds[i]) {
				g_kbds[i] = *dev;
				break;
			}
		patch_slot(&(*(void ***)*dev)[9], (void *)hk_GetDeviceState, (void **)&real_GetDeviceState);
		patch_slot(&(*(void ***)*dev)[10], (void *)hk_GetDeviceData, (void **)&real_GetDeviceData);
		logf("DirectInput keyboard device %p hooked (GetDeviceState, GetDeviceData)", *dev);
	} else if (SUCCEEDED(r) && dev && *dev && IsEqualGUID(guid, &GUID_SysMouse_)) {
		g_mouse = *dev;
		patch_slot(&(*(void ***)*dev)[9], (void *)hk_GetDeviceState, (void **)&real_GetDeviceState);
		logf("DirectInput mouse device %p hooked (GetDeviceState)", *dev);
	}
	return r;
}

static HRESULT WINAPI hk_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID *out, LPUNKNOWN outer)
{
	HRESULT r = real_DirectInput8Create(inst, ver, iid, out, outer);
	if (SUCCEEDED(r) && out && *out) {
		patch_slot(&(*(void ***)*out)[3], (void *)hk_CreateDevice, (void **)&real_CreateDevice);
		logf("DirectInput8Create ok, CreateDevice hooked");
	}
	return r;
}

// ---- background running ---------------------------------------------------------
// The game's window procedure (FUN_00546660) stores WM_ACTIVATE's state in DAT_0069b870 and the
// main loop only reads input while it is set. With OBSCOOP_BACKGROUND=1 the window procedure is
// told every WM_ACTIVATE is an activation, so a test instance keeps running unfocused.

static ATOM(WINAPI *real_RegisterClassA)(const WNDCLASSA *);
static WNDPROC g_game_wndproc;

static LRESULT CALLBACK hk_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_ACTIVATE && g_background && LOWORD(wp) == WA_INACTIVE)
		wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
	return CallWindowProcA(g_game_wndproc, h, msg, wp, lp);
}

static ATOM WINAPI hk_RegisterClassA(const WNDCLASSA *wc)
{
	if (wc && g_background && !g_game_wndproc) {
		WNDCLASSA copy = *wc;
		g_game_wndproc = wc->lpfnWndProc;
		copy.lpfnWndProc = hk_wndproc;
		logf("RegisterClassA %s: window procedure wrapped for background running", wc->lpszClassName ? wc->lpszClassName : "?");
		return real_RegisterClassA(&copy);
	}
	return real_RegisterClassA(wc);
}

static void vpad_set(int user, int on, unsigned buttons, int lx, int ly, int rx, int ry, int lt, int rt);
#include "net.c"
#include "sync.c"
#include "render.c"

// ---- write watch (test tool) --------------------------------------------------------
// "watch <hex address> <ms>": PAGE_GUARD on the page; a vectored handler logs the code address of
// every write that hits [address, address+4), then single-steps to re-arm the guard. In-process,
// no debugger attached. Turns itself off after <ms>.
static volatile LONG g_watch_on;
static DWORD g_watch_addr, g_watch_until;
static DWORD g_watch_eips[32];
static LONG g_watch_counts[32], g_watch_n;

static void watch_arm(void)
{
	DWORD old;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery((void *)g_watch_addr, &mbi, sizeof mbi))
		VirtualProtect((void *)(g_watch_addr & ~0xfffu), 0x1000, mbi.Protect | PAGE_GUARD, &old);
}

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *e)
{
	DWORD code = e->ExceptionRecord->ExceptionCode;
	if (code == STATUS_GUARD_PAGE_VIOLATION && g_watch_on) {
		ULONG_PTR kind = e->ExceptionRecord->ExceptionInformation[0];
		ULONG_PTR addr = e->ExceptionRecord->ExceptionInformation[1];
		if (kind == 1 && addr >= g_watch_addr && addr < g_watch_addr + 4) {
			DWORD eip = e->ContextRecord->Eip;
			LONG i;
			for (i = 0; i < g_watch_n && g_watch_eips[i] != eip; i++)
				;
			if (i == g_watch_n && g_watch_n < 32)
				g_watch_eips[g_watch_n++] = eip;
			if (i < 32)
				g_watch_counts[i]++;
		}
		e->ContextRecord->EFlags |= 0x100;   // single step, then re-arm
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (code == STATUS_SINGLE_STEP && g_watch_on) {
		if (GetTickCount() < g_watch_until)
			watch_arm();
		else
			g_watch_on = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static void watch_start(DWORD addr, DWORD ms)
{
	static PVOID handler;
	if (!handler)
		handler = AddVectoredExceptionHandler(1, watch_handler);
	g_watch_n = 0;
	memset(g_watch_counts, 0, sizeof g_watch_counts);
	g_watch_addr = addr;
	g_watch_until = GetTickCount() + ms;
	g_watch_on = 1;
	watch_arm();
}

// ---- command pipe ----------------------------------------------------------
// \\.\pipe\obscoop_<pid>, one text command per line, one reply line each:
//   pad <user> <buttons hex> [lx ly rx ry lt rt]   virtual pad on for user 0..3
//   unpad <user>                                    give the user index back to the real device
//   key <DIK hex> <0|1>                             virtual keyboard key up/down
//   mouse <dx> <dy> <buttons hex>                   add mouse movement, set button bits
//   ping                                            -> pong <pid>

static void vpad_set(int user, int on, unsigned buttons, int lx, int ly, int rx, int ry, int lt, int rt)
{
	EnterCriticalSection(&g_vpad_cs);
	xstate *v = &g_vpad[user];
	v->packet++;
	v->buttons = (WORD)buttons;
	v->lx = (SHORT)lx; v->ly = (SHORT)ly; v->rx = (SHORT)rx; v->ry = (SHORT)ry;
	v->lt = (BYTE)lt; v->rt = (BYTE)rt;
	g_vpad_on[user] = on;
	LeaveCriticalSection(&g_vpad_cs);
}

static void handle_command(char *line, char *reply, size_t replysz)
{
	int user, lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
	unsigned buttons;
	if (sscanf(line, "pad %d %x %d %d %d %d %d %d", &user, &buttons, &lx, &ly, &rx, &ry, &lt, &rt) >= 2 &&
		user >= 0 && user < 4) {
		vpad_set(user, 1, buttons, lx, ly, rx, ry, lt, rt);
		_snprintf(reply, replysz, "ok");
	} else if (sscanf(line, "unpad %d", &user) == 1 && user >= 0 && user < 4) {
		vpad_set(user, 0, 0, 0, 0, 0, 0, 0, 0);
		_snprintf(reply, replysz, "ok");
	} else if (sscanf(line, "key %x %d", &buttons, &user) == 2 && buttons < 256) {
		if (g_vkeys[buttons] != (user ? 1 : 0))
			keyq_push((int)buttons, user ? 1 : 0);
		g_vkeys[buttons] = user ? 1 : 0;
		_snprintf(reply, replysz, g_kbd ? "ok" : "ok (no keyboard device yet)");
	} else if (sscanf(line, "mouse %d %d %x", &lx, &ly, &buttons) == 3) {
		InterlockedExchangeAdd(&g_mdx, lx);
		InterlockedExchangeAdd(&g_mdy, ly);
		g_mbuttons = (BYTE)buttons;
		_snprintf(reply, replysz, "ok reads %ld%s", g_mouse_reads, g_mouse ? "" : " (no mouse device)");
	} else if (!strncmp(line, "stats", 5)) {
		HWND fg = GetForegroundWindow();
		DWORD fgpid = 0;
		GetWindowThreadProcessId(fg, &fgpid);
		_snprintf(reply, replysz, "polls %ld %ld %ld %ld virtual %ld %ld %ld %ld kbd_data_reads %ld foreground_is_game %d",
			g_polls[0], g_polls[1], g_polls[2], g_polls[3], g_virtual_reads[0], g_virtual_reads[1],
			g_virtual_reads[2], g_virtual_reads[3], g_kbd_data_reads, fgpid == GetCurrentProcessId());
	} else if (!strncmp(line, "setref ", 7)) {
		float rx, ry, rz;
		_snprintf(reply, replysz, sscanf(line + 7, "%f %f %f", &rx, &ry, &rz) == 3 && test_set_ref(rx, ry, rz) ? "ok" : "error setref x y z");
	} else if (!strcmp(line, "camyaw")) {
		_snprintf(reply, replysz, "yaw_mdeg %ld view_sets %ld", cam_yaw_mdeg(), cam_view_sets());
	} else if (!strncmp(line, "shot ", 5) && strlen(line + 5) < 250) {
		_snprintf(reply, replysz, request_shot(line + 5) ? "ok shot requested" : "no shot hook (set OBSCOOP_TESTSHOT=1)");
	} else if (sscanf(line, "level %d %d", &user, &lx) == 2 && user >= 0 && user < 256) {
		g_level_cmd_entry = lx;
		g_level_cmd_id = user;   // picked up by the next frame on the main thread
		_snprintf(reply, replysz, "ok level %s", level_name(user) ? level_name(user) : "(unknown id)");
	} else if (!strncmp(line, "hurtp ", 6)) {
		int who;
		float d;
		if (sscanf(line + 6, "%d %f", &who, &d) == 2 && (who == 0 || who == 1)) {
			g_hurtp_delta = d;
			g_hurtp_player = who;
			_snprintf(reply, replysz, "ok");
		} else {
			_snprintf(reply, replysz, "error hurtp player delta");
		}
	} else if (!strncmp(line, "hurt ", 5)) {
		int idx;
		float d;
		if (sscanf(line + 5, "%d %f", &idx, &d) == 2 && idx >= 0) {
			g_hurt_delta = d;
			g_hurt_index = idx;
			_snprintf(reply, replysz, "ok");
		} else {
			_snprintf(reply, replysz, "error hurt index delta");
		}
	} else if (sscanf(line, "switch %d %d", &user, &lx) == 2 && (user == 0 || user == 1)) {
		g_sw_char = lx;
		g_sw_player = user;
		_snprintf(reply, replysz, "ok");
	} else if (sscanf(line, "watch %x %d", &buttons, &lx) == 2) {
		watch_start(buttons, (DWORD)lx);
		_snprintf(reply, replysz, "ok watching %#x for %d ms", buttons, lx);
	} else if (!strncmp(line, "watchresult", 11)) {
		int k = _snprintf(reply, replysz, "watch on %ld writes:", g_watch_on);
		for (LONG i = 0; i < g_watch_n && k > 0 && (size_t)k < replysz - 32; i++)
			k += _snprintf(reply + k, replysz - k, " %#lx x%ld", g_watch_eips[i], g_watch_counts[i]);
	} else if (sscanf(line, "give %d %d", &buttons, &lx) == 2 && buttons >= 0 && buttons <= 1 && lx >= 0 && lx <= 1) {
		g_give_to = lx;
		g_give_from = buttons;
		_snprintf(reply, replysz, "ok");
	} else if (sscanf(line, "stall %d", &lx) == 1 && lx > 0 && lx <= 60000) {
		g_stall_ms = lx;
		_snprintf(reply, replysz, "ok");
	} else if (!strncmp(line, "view ", 5) && strlen(line + 5) < 60) {
		strcpy(g_view_cmd, line + 5);
		g_view_cmd_set = 1;
		_snprintf(reply, replysz, "ok");
	} else if (sscanf(line, "msg %x %d", &buttons, &lx) == 2) {
		g_msg_cmd_id = buttons;
		g_msg_cmd_ms = lx;
		_snprintf(reply, replysz, "ok");
	} else if (sscanf(line, "loadslot %d", &lx) == 1 && lx >= 0) {
		g_load_cmd = lx;
		_snprintf(reply, replysz, "ok");
	} else if (!strcmp(line, "hostpause")) {
		g_pause_cmd = 1;
		_snprintf(reply, replysz, "ok");
	} else if (!strcmp(line, "camtoggle")) {
		g_cam_toggle_cmd = 1;
		_snprintf(reply, replysz, "ok");
	} else if (!strcmp(line, "baguse")) {
		g_bag_use = 1;
		_snprintf(reply, replysz, "ok");
	} else if (!strncmp(line, "tp ", 3)) {
		int who;
		float x, y, z;
		float fx = 0, fy = 0;
		if (sscanf(line + 3, "%d %f %f %f %f %f", &who, &x, &y, &z, &fx, &fy) >= 4 && (who == 0 || who == 1)) {
			g_tp_pos[0] = x; g_tp_pos[1] = y; g_tp_pos[2] = z;
			g_tp_dir[0] = fx; g_tp_dir[1] = fy; g_tp_dir[2] = 0;   // optional facing
			g_tp_player = who;
			g_tp_pending = 1;
			_snprintf(reply, replysz, "ok");
		} else {
			_snprintf(reply, replysz, "error tp player x y z");
		}
	} else if (!strncmp(line, "teleport ", 9)) {
		float x, y, z, fx = 0, fy = 0;
		if (sscanf(line + 9, "%f %f %f %f %f", &x, &y, &z, &fx, &fy) >= 3) {
			g_tp_pos[0] = x; g_tp_pos[1] = y; g_tp_pos[2] = z;
			g_tp_dir[0] = fx; g_tp_dir[1] = fy; g_tp_dir[2] = 0;   // optional facing; 0 0 keeps it
			g_tp_player = 0;
			g_tp_pending = 1;
			_snprintf(reply, replysz, "ok");
		} else {
			_snprintf(reply, replysz, "error teleport x y z");
		}
	} else if (!strncmp(line, "sync", 4)) {
		sync_status(reply, replysz);
	} else if (!strncmp(line, "net", 3)) {
		net_status(reply, replysz);
	} else if (!strncmp(line, "ping", 4)) {
		_snprintf(reply, replysz, "pong %lu", GetCurrentProcessId());
	} else {
		_snprintf(reply, replysz, "error unknown command");
	}
	reply[replysz - 1] = 0;
}

static DWORD WINAPI pipe_server(LPVOID arg)
{
	char name[64];
	(void)arg;
	_snprintf(name, sizeof name, "\\\\.\\pipe\\obscoop_%lu", GetCurrentProcessId());
	logf("command pipe %s", name);
	for (;;) {
		HANDLE p = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
		if (p == INVALID_HANDLE_VALUE) {
			logf("CreateNamedPipe failed %lu", GetLastError());
			return 1;
		}
		if (ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
			char buf[512];
			size_t len = 0;
			DWORD got;
			while (ReadFile(p, buf + len, (DWORD)(sizeof buf - 1 - len), &got, NULL) && got) {
				len += got;
				buf[len] = 0;
				char *nl;
				while ((nl = strchr(buf, '\n'))) {
					*nl = 0;
					if (nl > buf && nl[-1] == '\r')
						nl[-1] = 0;
					char reply[2048];
					handle_command(buf, reply, sizeof reply);
					strcat(reply, "\n");
					DWORD w;
					WriteFile(p, reply, (DWORD)strlen(reply), &w, NULL);
					len -= (size_t)(nl + 1 - buf);
					memmove(buf, nl + 1, len + 1);
				}
				if (len >= sizeof buf - 1)
					len = 0;
			}
		}
		DisconnectNamedPipe(p);
		CloseHandle(p);
	}
}

// ---- import table patching -------------------------------------------------

static int patch_import(HMODULE mod, const char *dll, const char *func, void *hook, void **orig)
{
	BYTE *base = (BYTE *)mod;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		if (_stricmp((char *)(base + d->Name), dll))
			continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
		IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, iat++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((char *)ibn->Name, func))
				continue;
			DWORD old;
			if (orig && !*orig)
				*orig = (void *)iat->u1.Function;
			VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			iat->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
			return 1;
		}
	}
	return 0;
}

static struct hook { const char *dll, *func; void *hook; void **orig; } g_hooks[] = {
	{ "KERNEL32.dll", "CreateFileA", (void *)hk_CreateFileA, (void **)&real_CreateFileA },
	{ "KERNEL32.dll", "CreateFileW", (void *)hk_CreateFileW, (void **)&real_CreateFileW },
	{ "KERNEL32.dll", "CreateDirectoryA", (void *)hk_CreateDirectoryA, (void **)&real_CreateDirectoryA },
	{ "KERNEL32.dll", "FindFirstFileA", (void *)hk_FindFirstFileA, (void **)&real_FindFirstFileA },
	{ "KERNEL32.dll", "FindFirstFileExA", (void *)hk_FindFirstFileExA, (void **)&real_FindFirstFileExA },
	{ "SHELL32.dll", "SHGetFolderPathA", (void *)hk_SHGetFolderPathA, (void **)&real_SHGetFolderPathA },
	{ "KERNEL32.dll", "CreateMutexA", (void *)hk_CreateMutexA, (void **)&real_CreateMutexA },
	{ "USER32.dll", "RegisterClassA", (void *)hk_RegisterClassA, (void **)&real_RegisterClassA },
	{ "USER32.dll", "GetClientRect", (void *)hk_GetClientRect, (void **)&real_GetClientRect },
	{ "d3d9.dll", "Direct3DCreate9", (void *)hk_Direct3DCreate9, (void **)&real_Direct3DCreate9 },
	{ "DINPUT8.dll", "DirectInput8Create", (void *)hk_DirectInput8Create, (void **)&real_DirectInput8Create },
	{ "steam_api.dll", "SteamAPI_RunCallbacks", (void *)hk_SteamAPI_RunCallbacks, (void **)&real_SteamAPI_RunCallbacks },
	{ "steam_api.dll", "SteamRemoteStorage", (void *)hk_SteamRemoteStorage, (void **)&real_SteamRemoteStorage },
};
#define NHOOKS (sizeof g_hooks / sizeof g_hooks[0])

// Called from DllMain and again from a short-lived watcher thread, in case SteamStub
// rewrites the import table after the loader filled it.
static int apply_hooks(int verbose)
{
	HMODULE exe = GetModuleHandleA(NULL);
	BYTE *base = (BYTE *)exe;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	(void)nt;
	int n = 0;
	for (size_t i = 0; i < NHOOKS; i++) {
		int ok = patch_import(exe, g_hooks[i].dll, g_hooks[i].func, g_hooks[i].hook, g_hooks[i].orig);
		n += ok;
		if (verbose)
			logf("hook %s!%s %s", g_hooks[i].dll, g_hooks[i].func, ok ? "installed" : "NOT FOUND");
	}
	return n;
}

static int hooks_intact(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	BYTE *base = (BYTE *)exe;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	int found = 0;
	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; iat->u1.Function; iat++)
			for (size_t i = 0; i < NHOOKS; i++)
				if (iat->u1.Function == (ULONG_PTR)g_hooks[i].hook)
					found++;
	}
	return found == (int)NHOOKS;
}

static DWORD WINAPI watcher(LPVOID arg)
{
	(void)arg;
	for (int i = 0; i < 2000; i++) {   // first ~20 s
		if (!hooks_intact()) {
			logf("import table changed after load; re-applying hooks");
			apply_hooks(1);
		}
		Sleep(10);
	}
	logf("watcher done; redirects so far %ld, refused %ld", g_redirects, g_blocked);
	return 0;
}

static void ws_install(void);
static int request_shot(const char *path);
static long cam_yaw_mdeg(void);
static int test_set_ref(float x, float y, float z);
static void move_hook_install(void);
static long cam_view_sets(void);
static int check_build(void)
{
	char exe[MAX_PATH];
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	WIN32_FILE_ATTRIBUTE_DATA fa;
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	if (!GetFileAttributesExA(exe, GetFileExInfoStandard, &fa))
		return 0;
	logf("exe %s size %lu timestamp %lu", exe, fa.nFileSizeLow, nt->FileHeader.TimeDateStamp);
	return fa.nFileSizeLow == EXPECTED_EXE_SIZE && nt->FileHeader.TimeDateStamp == EXPECTED_PE_TIMESTAMP;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(inst);
	InitializeCriticalSection(&g_log_cs);
	InitializeCriticalSection(&g_vpad_cs);
	InitializeCriticalSection(&g_keyq_cs);
	InitializeCriticalSection(&g_state_cs);
	GetModuleFileNameA(NULL, g_dir, MAX_PATH);
	char *slash = strrchr(g_dir, '\\');
	if (slash)
		*slash = 0;
	// Under Program Files, Windows silently redirects an old game's writes to a hidden VirtualStore
	// folder (seen on a friend's PC: no obscoop.log where expected). Use %LOCALAPPDATA%\obscoop there.
	{
		char low[MAX_PATH];
		strncpy(low, g_dir, MAX_PATH - 1);
		low[MAX_PATH - 1] = 0;
		_strlwr(low);
		strcpy(g_data, g_dir);
		if (strstr(low, "\\program files")) {
			char la[MAX_PATH];
			if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la))) {
				_snprintf(g_data, MAX_PATH, "%s\\obscoop", la);
				CreateDirectoryA(g_data, NULL);
			}
		}
	}
	_snprintf(g_profile, MAX_PATH, "%s\\coop_profile", g_data);
	_snprintf(g_remote, MAX_PATH, "%s\\remote", g_profile);
	char logpath[MAX_PATH];
	_snprintf(logpath, MAX_PATH, "%s\\obscoop.log", g_data);
	g_log = CreateFileA(logpath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
	logf("obscoop 0.2 loaded, pid %lu, game dir %s, data dir %s", GetCurrentProcessId(), g_dir, g_data);
	// Settings: obscoop.ini next to the exe, section [obscoop], key = name without OBSCOOP_. An
	// environment variable with the full name wins (the one-PC tests use those).
	{
		static const char *keys[] = { "ROLE", "NET", "HOST_STEAMID", "REMOTE_PAD", "LOCAL_PAD", "HIDE_REAL_PADS",
			"BACKGROUND", "VPADS", "MULTI", "UDP", "PEER", "SSAA", "ANISO", "TEXT_SCALE", "SMOOTH_MS", "WIDESCREEN" };
		char ini[MAX_PATH], v[64], name[64];
		_snprintf(ini, MAX_PATH, "%s\\obscoop.ini", g_dir);
		if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
			char alt[MAX_PATH];
			_snprintf(alt, MAX_PATH, "%s\\obscoop.ini.txt", g_dir);   // Explorer hides the .txt a renamed copy keeps
			if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) {
				strcpy(ini, alt);
				logf("reading settings from obscoop.ini.txt (the file still ends in .txt)");
			} else {
				logf("no obscoop.ini next to Obscure.exe");
			}
		}
		for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
			_snprintf(name, sizeof name, "OBSCOOP_%s", keys[i]);
			if (GetEnvironmentVariableA(name, v, sizeof v))
				continue;
			if (GetPrivateProfileStringA("obscoop", keys[i], "", v, sizeof v, ini) > 0) {
				SetEnvironmentVariableA(name, v);
				logf("setting %s from obscoop.ini", keys[i]);
			}
		}
	}
	if (!check_build()) {
		// Not the build these hooks were written for: refuse to start rather than guess.
		logf("unexpected Obscure.exe build (expected Steam build 215286); exiting");
		TerminateProcess(GetCurrentProcess(), 3);
	}
	ws_install();
	move_hook_install();
	char bg[8];
	g_background = GetEnvironmentVariableA("OBSCOOP_BACKGROUND", bg, sizeof bg) && bg[0] == '1';
	logf("background running %s", g_background ? "on" : "off");
	g_hide_real_pads = GetEnvironmentVariableA("OBSCOOP_HIDE_REAL_PADS", bg, sizeof bg) && bg[0] == '1';
	logf("real controllers %s", g_hide_real_pads ? "hidden" : "visible");
	apply_hooks(1);
	// OBSCOOP_VPADS=0,1 : report those virtual pads as connected (idle) from the first poll.
	char vp[32];
	if (GetEnvironmentVariableA("OBSCOOP_VPADS", vp, sizeof vp)) {
		for (char *c = vp; *c; c++)
			if (*c >= '0' && *c <= '3') {
				vpad_set(*c - '0', 1, 0, 0, 0, 0, 0, 0, 0);
				logf("virtual pad %c connected at start", *c);
			}
	}
	CloseHandle(CreateThread(NULL, 0, watcher, NULL, 0, NULL));
	CloseHandle(CreateThread(NULL, 0, pipe_server, NULL, 0, NULL));
	CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
	g_multi = GetEnvironmentVariableA("OBSCOOP_MULTI", bg, sizeof bg) && bg[0] == '1';
	net_start();
	if (g_role == 1)
		sync_install_update();
	if (g_role == 1)
		sync_install_view_hook();
	if (g_role == 1)
		sync_install_msg_hook();
	if (g_role == 2)
		sync_install();
	return TRUE;
}
