// obscoop in-game supersampling (1.2). Included by obscoop.c after sync.c.
//
// The game (Direct3D 9, Direct3DCreate9 imported from d3d9.dll) renders into a back buffer SSAA times
// wider and taller than its chosen resolution; each Present shrinks it into a window-sized output swap
// chain with averaging (2:1 bilinear = the 2x2 box filter; 4 = two 2:1 steps), i.e. SSAA 2 = 4 samples
// per pixel. A bigger back buffer than the screen needs a window, so exclusive fullscreen becomes a
// borderless window covering the monitor. Setting SSAA (obscoop.ini or OBSCOOP_SSAA): 1 or missing = off, 2 = 4 samples per pixel, 4 = 16.
// Menu and message text is drawn in fixed pixel sizes, so with SSAA it shows smaller (half at 2).
#include <d3d9.h>

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE *d3d_create_device_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
	D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(STDMETHODCALLTYPE *dev_reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *dev_present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);

static Direct3DCreate9_t real_Direct3DCreate9;
typedef HRESULT(STDMETHODCALLTYPE *dev_set_sampler_t)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
static dev_set_sampler_t real_dev_SetSamplerState;
static int g_aniso = -1;
static d3d_create_device_t real_d3d_CreateDevice;
static dev_reset_t real_dev_Reset;
static dev_present_t real_dev_Present;
static int g_ssaa = -1;
static IDirect3DSwapChain9 *g_ss_out;   // window-sized output
static IDirect3DSurface9 *g_ss_half;    // SSAA 4: the intermediate 2x surface
static HWND g_ss_wnd;
static UINT g_ss_out_w, g_ss_out_h;
static volatile LONG g_ss_presents, g_ss_fails;
static volatile LONG g_ss_active;   // the game's GetClientRect answers SSAA times bigger
static void ss_windowed_path(void);
static void ss_install_text_scale(void);
static int ssaa_factor(void)
{
	if (g_ssaa < 0) {
		char v[16];
		g_ssaa = GetEnvironmentVariableA("OBSCOOP_SSAA", v, sizeof v) ? atoi(v) : 1;
		if (g_ssaa != 1 && g_ssaa != 2 && g_ssaa != 4)
			g_ssaa = 1;
	}
	return g_ssaa;
}

static int aniso_level(void)
{
	if (g_aniso < 0) {
		char v[16];
		g_aniso = GetEnvironmentVariableA("OBSCOOP_ANISO", v, sizeof v) ? atoi(v) : 0;
		if (g_aniso < 2)
			g_aniso = 0;
		if (g_aniso > 16)
			g_aniso = 16;
	}
	return g_aniso;
}

// The game sets the mip filter to NONE (sampler type 7 = 0, observed 2026-10-25): it always samples the
// top mip level, so a mip LOD bias would change nothing. Anisotropic filtering still helps at an angle
// (about +4% detail on the far courtyard ground with SSAA 2).
static int filters_on(void)
{
	return aniso_level();
}

static HRESULT STDMETHODCALLTYPE hk_dev_SetSamplerState(IDirect3DDevice9 *dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
	if (sampler < 16 && type == D3DSAMP_MINFILTER && value == D3DTEXF_LINEAR) {
		real_dev_SetSamplerState(dev, sampler, D3DSAMP_MAXANISOTROPY, (DWORD)g_aniso);
		value = D3DTEXF_ANISOTROPIC;
	} else if (sampler < 16 && type == D3DSAMP_MAXANISOTROPY) {
		value = (DWORD)g_aniso;
	}
	return real_dev_SetSamplerState(dev, sampler, type, value);
}

// Before CreateDevice / Reset: window instead of exclusive fullscreen, back buffer SSAA times bigger.
// The game's own struct is changed, so whatever it reads back matches the buffer it renders into.
static void ss_adjust(D3DPRESENT_PARAMETERS *pp, HWND focus)
{
	HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
	g_ss_wnd = w;
	UINT bw = pp->BackBufferWidth, bh = pp->BackBufferHeight;
	if (!pp->Windowed) {   // borderless window over the monitor the game is on
		MONITORINFO mi;
		memset(&mi, 0, sizeof mi);
		mi.cbSize = sizeof mi;
		GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
		SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		pp->Windowed = TRUE;
		pp->FullScreen_RefreshRateInHz = 0;
		logf("ssaa: fullscreen %ux%u -> borderless window %ldx%ld", bw, bh, mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top);
	}
	if (!bw || !bh) {   // windowed with 0 = the client area
		RECT r;
		GetClientRect(w, &r);
		bw = (UINT)(r.right - r.left);
		bh = (UINT)(r.bottom - r.top);
	}
	pp->BackBufferWidth = bw * (UINT)g_ssaa;
	pp->BackBufferHeight = bh * (UINT)g_ssaa;
	pp->MultiSampleType = D3DMULTISAMPLE_NONE;
	pp->MultiSampleQuality = 0;
	logf("ssaa: game renders %ux%u (x%d) for %ux%u", pp->BackBufferWidth, pp->BackBufferHeight, g_ssaa, bw, bh);
}

static void ss_release(void)
{
	if (g_ss_half)
		IDirect3DSurface9_Release(g_ss_half);
	if (g_ss_out)
		IDirect3DSwapChain9_Release(g_ss_out);
	g_ss_half = NULL;
	g_ss_out = NULL;
}

static void ss_create(IDirect3DDevice9 *dev, const D3DPRESENT_PARAMETERS *pp)
{
	RECT r;
	GetClientRect(g_ss_wnd, &r);
	g_ss_out_w = (UINT)(r.right - r.left);
	g_ss_out_h = (UINT)(r.bottom - r.top);
	if (!g_ss_out_w || !g_ss_out_h) {
		g_ss_out_w = pp->BackBufferWidth / (UINT)g_ssaa;
		g_ss_out_h = pp->BackBufferHeight / (UINT)g_ssaa;
	}
	D3DPRESENT_PARAMETERS sp;
	memset(&sp, 0, sizeof sp);
	sp.BackBufferWidth = g_ss_out_w;
	sp.BackBufferHeight = g_ss_out_h;
	sp.BackBufferFormat = pp->BackBufferFormat;
	sp.BackBufferCount = 1;
	sp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	sp.hDeviceWindow = g_ss_wnd;
	sp.Windowed = TRUE;
	sp.PresentationInterval = pp->PresentationInterval;
	HRESULT hr = IDirect3DDevice9_CreateAdditionalSwapChain(dev, &sp, &g_ss_out);
	if (FAILED(hr)) {
		g_ss_out = NULL;
		logf("ssaa: output swap chain %ux%u failed (0x%08lx); presenting without supersampling", g_ss_out_w, g_ss_out_h, hr);
		return;
	}
	if (g_ssaa == 4) {
		hr = IDirect3DDevice9_CreateRenderTarget(dev, g_ss_out_w * 2, g_ss_out_h * 2, pp->BackBufferFormat,
			D3DMULTISAMPLE_NONE, 0, FALSE, &g_ss_half, NULL);
		if (FAILED(hr))
			g_ss_half = NULL;
	}
	logf("ssaa: output %ux%u ready", g_ss_out_w, g_ss_out_h);
}

static HRESULT STDMETHODCALLTYPE hk_dev_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
	if (!g_ss_out)
		return real_dev_Present(dev, src, dst, wnd, dirty);
	IDirect3DSurface9 *bb = NULL, *out = NULL;
	HRESULT hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
	if (SUCCEEDED(hr))
		hr = IDirect3DSwapChain9_GetBackBuffer(g_ss_out, 0, D3DBACKBUFFER_TYPE_MONO, &out);
	if (SUCCEEDED(hr)) {
		if (g_ss_half) {
			hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, g_ss_half, NULL, D3DTEXF_LINEAR);
			if (SUCCEEDED(hr))
				hr = IDirect3DDevice9_StretchRect(dev, g_ss_half, NULL, out, NULL, D3DTEXF_LINEAR);
		} else {
			hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, out, NULL, D3DTEXF_LINEAR);
		}
	}
	if (bb)
		IDirect3DSurface9_Release(bb);
	if (out)
		IDirect3DSurface9_Release(out);
	if (FAILED(hr)) {
		if (InterlockedIncrement(&g_ss_fails) <= 3)
			logf("ssaa: downscale failed (0x%08lx); showing the plain frame", hr);
		return real_dev_Present(dev, src, dst, wnd, dirty);
	}
	InterlockedIncrement(&g_ss_presents);
	return IDirect3DSwapChain9_Present(g_ss_out, NULL, NULL, NULL, NULL, 0);
}

static HRESULT STDMETHODCALLTYPE hk_dev_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
	ss_release();
	if (pp)
		ss_adjust(pp, g_ss_wnd);
	HRESULT hr = real_dev_Reset(dev, pp);
	if (SUCCEEDED(hr) && pp)
		ss_create(dev, pp);
	else
		logf("ssaa: Reset failed (0x%08lx)", hr);
	return hr;
}

static HRESULT STDMETHODCALLTYPE hk_d3d_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
	DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
	if (ssaa_factor() == 1 || !pp) {
		HRESULT r = real_d3d_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
		if (SUCCEEDED(r) && filters_on()) {
			patch_slot(&(*(void ***)*out)[69], (void *)hk_dev_SetSamplerState, (void **)&real_dev_SetSamplerState);
			logf("ssaa: off; anisotropic filtering x%d", g_aniso);
		}
		return r;
	}
	D3DPRESENT_PARAMETERS orig = *pp;
	ss_adjust(pp, focus);
	HRESULT hr = real_d3d_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
	if (FAILED(hr)) {   // too big for this card, or anything else: the game's own settings
		logf("ssaa: CreateDevice with %ux%u failed (0x%08lx); supersampling off", pp->BackBufferWidth, pp->BackBufferHeight, hr);
		*pp = orig;
		g_ssaa = 1;
		return real_d3d_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
	}
	void **vt = *(void ***)*out;
	if (filters_on()) {
		patch_slot(&vt[69], (void *)hk_dev_SetSamplerState, (void **)&real_dev_SetSamplerState);
		logf("ssaa: anisotropic filtering x%d", g_aniso);
	}
	patch_slot(&vt[16], (void *)hk_dev_Reset, (void **)&real_dev_Reset);
	patch_slot(&vt[17], (void *)hk_dev_Present, (void **)&real_dev_Present);
	ss_create(*out, pp);
	g_ss_active = 1;
	ss_windowed_path();
	ss_install_text_scale();
	return hr;
}

// The game lays out its viewports, HUD and menus from its renderer's screen rect (renderer
// *(0x6ab890): +0x188/+0x18c, FUN_005ab5a0 area): GetClientRect of its window when windowed (+0x124 == 0),
// the mode size (+0x118/+0x11c) in fullscreen. First try (2026-10-25): the game drew into the top-left
// quarter of the bigger buffer. So the game's GetClientRect of its D3D window answers SSAA times bigger,
// and a fullscreen game is put on the windowed path (+0x124 = 0) after the switch to a borderless window.
typedef BOOL(WINAPI *GetClientRect_t)(HWND, LPRECT);
static GetClientRect_t real_GetClientRect;

static BOOL WINAPI hk_GetClientRect(HWND w, LPRECT r)
{
	BOOL ok = real_GetClientRect(w, r);
	if (ok && r && g_ss_active && w == g_ss_wnd && g_ssaa > 1) {
		r->right = r->left + (r->right - r->left) * g_ssaa;
		r->bottom = r->top + (r->bottom - r->top) * g_ssaa;
	}
	return ok;
}

static void ss_windowed_path(void)
{
	BYTE *rd = *(BYTE **)0x6ab890;
	if (!rd || *(HWND *)(rd + 0x1f0) != g_ss_wnd) {
		logf("ssaa: renderer %p not found with window %p; layout may be off", rd, g_ss_wnd);
		return;
	}
	if (*(DWORD *)(rd + 0x124)) {
		*(DWORD *)(rd + 0x124) = 0;
		logf("ssaa: renderer put on its windowed path");
	}
}

// ---- text at its normal size with SSAA (1.4) ----------------------------------------------------
// Menu, message and HUD text are bitmap fonts drawn 1:1 in pixels (font manager *(0x6ab8ec), e.g.
// font_holstein16 = 24 px lines), so in an SSAA x2 frame they came out half size. FUN_0045ba60 (draw
// string: thiscall on the font manager; x, y, string, colour, box, box colour, centred, height, flags;
// ret 0x24) draws each glyph through the renderer *(0x6ab890) vtable +0xb8/+0xc0 (x0, y0, x1, y1, u0,
// v0, u1, v1, colour, texture) from 0x45beb4, and its background box through FUN_0045d440 (0x45d440,
// 775 bytes, same renderer call). While a string is drawn, those rectangles are scaled by TEXT_SCALE
// about the string's anchor: its left edge (or the screen centre when centred) and the middle of its
// first line, so layout and centring stay where the game put them. TEXT_SCALE defaults to SSAA.
typedef DWORD(__fastcall *draw_text_t)(void *, void *, int, int, void *, DWORD, int, DWORD, int, int, int);
typedef DWORD(__fastcall *draw_quad_t)(void *, void *, int, int, int, int, float, float, float, float, DWORD, DWORD);
static draw_text_t g_draw_text_tramp;
static draw_quad_t real_quad_b8, real_quad_c0;
static float g_text_scale;
static volatile LONG g_txt_on, g_txt_strings, g_txt_quads;
static float g_txt_ox, g_txt_oy;

static DWORD __fastcall hk_draw_text(void *fm, void *edx, int x, int y, void *str, DWORD color, int box, DWORD box_color,
	int centred, int height, int flags)
{
	BYTE *rd = *(BYTE **)0x6ab890;
	if (g_text_scale <= 1.0f || g_txt_on || !rd || GetCurrentThreadId() != g_main_tid)
		return g_draw_text_tramp(fm, edx, x, y, str, color, box, box_color, centred, height, flags);
	DWORD line_h = *(DWORD *)((BYTE *)fm + 0x1cc);   // the current font's; the game picks the font first
	g_txt_ox = centred ? (float)(*(int *)(rd + 0x188) / 2) : (float)x;
	g_txt_oy = (float)y + (float)(line_h < 200 ? line_h : 0) * 0.5f;
	g_txt_on = 1;
	DWORD r = g_draw_text_tramp(fm, edx, x, y, str, color, box, box_color, centred, height, flags);
	g_txt_on = 0;
	g_txt_strings++;
	return r;
}

static int txt_quad(void *ret)
{
	DWORD a = (DWORD)ret;
	return g_txt_on && (a == 0x45beb6 || (a > 0x45d440 && a < 0x45d440 + 775));
}

#define TXT_X(v) ((int)(g_txt_ox + ((float)(v) - g_txt_ox) * g_text_scale))
#define TXT_Y(v) ((int)(g_txt_oy + ((float)(v) - g_txt_oy) * g_text_scale))

static DWORD __fastcall hk_quad_b8(void *rd, void *edx, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
	DWORD color, DWORD tex)
{
	if (txt_quad(__builtin_return_address(0))) {
		g_txt_quads++;
		return real_quad_b8(rd, edx, TXT_X(x0), TXT_Y(y0), TXT_X(x1), TXT_Y(y1), u0, v0, u1, v1, color, tex);
	}
	return real_quad_b8(rd, edx, x0, y0, x1, y1, u0, v0, u1, v1, color, tex);
}

static DWORD __fastcall hk_quad_c0(void *rd, void *edx, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
	DWORD color, DWORD tex)
{
	if (txt_quad(__builtin_return_address(0))) {
		g_txt_quads++;
		return real_quad_c0(rd, edx, TXT_X(x0), TXT_Y(y0), TXT_X(x1), TXT_Y(y1), u0, v0, u1, v1, color, tex);
	}
	return real_quad_c0(rd, edx, x0, y0, x1, y1, u0, v0, u1, v1, color, tex);
}

static void ss_install_text_scale(void)
{
	char v[16];
	g_text_scale = GetEnvironmentVariableA("OBSCOOP_TEXT_SCALE", v, sizeof v) ? (float)atof(v) : (float)g_ssaa;
	if (g_text_scale <= 1.0f || g_text_scale > 4.0f || g_draw_text_tramp)
		return;
	BYTE *rd = *(BYTE **)0x6ab890;
	BYTE *f = (BYTE *)0x45ba60;
	static const BYTE entry[5] = { 0x55, 0x8b, 0xec, 0x6a, 0xff };   // push ebp; mov ebp, esp; push -1
	if (!rd || memcmp(f, entry, 5) || ((BYTE *)0x45beb4)[0] != 0xff || ((BYTE *)0x45beb4)[1] != 0xd0) {
		logf("ssaa: text scaling off (renderer %p or unexpected bytes at 0x45ba60 / 0x45beb4)", rd);
		return;
	}
	BYTE *t = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t)
		return;
	memcpy(t, entry, 5);
	t[5] = 0xe9;
	*(LONG *)(t + 6) = (LONG)(f + 5 - (t + 10));
	g_draw_text_tramp = (draw_text_t)t;
	void **vt = *(void ***)rd;
	patch_slot(&vt[0xb8 / 4], (void *)hk_quad_b8, (void **)&real_quad_b8);
	patch_slot(&vt[0xc0 / 4], (void *)hk_quad_c0, (void **)&real_quad_c0);
	DWORD old;
	VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old);
	f[0] = 0xe9;
	*(LONG *)(f + 1) = (LONG)((BYTE *)hk_draw_text - (f + 5));
	VirtualProtect(f, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), f, 5);
	logf("ssaa: text drawn x%.2f", g_text_scale);
}

static IDirect3D9 *WINAPI hk_Direct3DCreate9(UINT sdk)
{
	IDirect3D9 *d3d = real_Direct3DCreate9(sdk);
	ssaa_factor();
	filters_on();
	if (d3d && (g_ssaa != 1 || g_aniso)) {
		patch_slot(&(*(void ***)d3d)[16], (void *)hk_d3d_CreateDevice, (void **)&real_d3d_CreateDevice);
		logf("ssaa: Direct3DCreate9 hooked, supersampling x%d, anisotropic x%d", g_ssaa, g_aniso);
	} else if (d3d) {
		logf("ssaa: off (SSAA=1, ANISO=0)");
	}
	return d3d;
}
