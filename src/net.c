// obscoop networking: host/guest link. Included by obscoop.c (shares its statics).
//
// Roles (OBSCOOP_ROLE):
//   host  - receives the guest's pad and feeds it into virtual pad OBSCOOP_REMOTE_PAD (default 0),
//           which the game's input config gives to player 2 ("game controller 1" = Joystick 0).
//   guest - sends its local pad OBSCOOP_LOCAL_PAD (default 0) to the host at 125 Hz.
// Transport (OBSCOOP_NET):
//   udp   - OBSCOOP_UDP = host port (default 27960), OBSCOOP_PEER = host address for the guest
//           (default 127.0.0.1). The host listens on loopback only. Used by the one-PC tests.
//   steam - Steam P2P (steam.c). The host accepts sessions from Steam friends; the guest says hello to
//           OBSCOOP_HOST_STEAMID if set, else to every friend playing Obscure, and keeps the one that
//           answers. All Steam calls run on the game's main thread (net_frame from the frame hook).
//
// Packet: u32 magic 'OBCP', u8 type, u8 version, u16 payload length, u32 seq, payload.

#include <winsock2.h>
#include <ws2tcpip.h>

#define NET_MAGIC 0x5043424Fu   // "OBCP" little-endian
#define NET_VERSION 3   // 2 (1.1): state packets carry the host's send time; 3 (1.7): enemy packets too
enum { PKT_HELLO = 1, PKT_WELCOME = 2, PKT_INPUT = 3, PKT_PING = 4, PKT_PONG = 5, PKT_BYE = 6 };

#pragma pack(push, 1)
typedef struct { DWORD magic; BYTE type, version; WORD len; DWORD seq; } pkt_hdr;
typedef struct { pkt_hdr h; DWORD tick; xstate pad; } pkt_input;
typedef struct { pkt_hdr h; DWORD t_ms; } pkt_ping;
#pragma pack(pop)

static void guest_on_state(const char *buf, int n);
static void guest_on_level(const char *buf, int n);
static void guest_on_flags(const char *buf, int n);
static void guest_on_pnj(const char *buf, int n);
static void guest_on_items(const char *buf, int n);
static void guest_on_inv(const char *buf, int n);
static void guest_on_ui(const char *buf, int n);
static void guest_on_view(const char *buf, int n);
static void guest_on_msg(const char *buf, int n);
static void guest_on_save(const char *buf, int n);
static void host_on_save_ack(const char *buf, int n);
static void guest_on_load(const char *buf, int n);
static void host_on_moveref(const char *buf, int n);
static void guest_on_props(const char *buf, int n);
static void guest_send_moveref(void);

#include "steam.c"

static int g_use_steam;
static int g_last_candidates;
static int g_host_self_test;
static int g_remote_pad, g_local_pad;
static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_udp_peer;   // udp: where packets to the peer go
static sid_t g_peer;                    // peer key: steam id, or (ip << 16 | port) for udp
static volatile LONG g_peer_ok;         // host: guest accepted; guest: welcome received
static volatile LONG g_net_rx, g_net_tx, g_net_bad, g_net_rtt_ms = -1;
static volatile LONG g_last_input_seq;
static volatile DWORD g_last_rx_tick;
static DWORD g_tx_seq;

static sid_t udp_key(const struct sockaddr_in *a)
{
	return ((sid_t)ntohl(a->sin_addr.s_addr) << 16) | ntohs(a->sin_port);
}

static void net_send_to(sid_t to, BYTE type, void *pkt, WORD payload)
{
	pkt_hdr *h = (pkt_hdr *)pkt;
	h->magic = NET_MAGIC;
	h->type = type;
	h->version = NET_VERSION;
	h->len = payload;
	h->seq = ++g_tx_seq;
	int len = (int)(sizeof(pkt_hdr) + payload);
	int ok;
	if (g_use_steam) {
		ok = steam_send(to, pkt, len);
	} else {
		(void)to;
		ok = sendto(g_sock, (const char *)pkt, len, 0, (struct sockaddr *)&g_udp_peer, sizeof g_udp_peer) > 0;
	}
	if (ok)
		InterlockedIncrement(&g_net_tx);
}

static void net_send(BYTE type, void *pkt, WORD payload)
{
	net_send_to(g_peer, type, pkt, payload);
}

// The local pad as a player holds it (virtual pad if set, else the real one unless hidden).
static int local_pad(int user, xstate *out)
{
	memset(out, 0, sizeof *out);
	load_xinput();
	return pad_read((DWORD)user, out) == ERROR_SUCCESS;
}

static void host_drop_guest(const char *why)
{
	if (!g_peer_ok)
		return;
	g_peer_ok = 0;
	vpad_set(g_remote_pad, 0, 0, 0, 0, 0, 0, 0, 0);
	logf("net: guest dropped (%s); virtual pad %d disconnected", why, g_remote_pad);
}

// Steam: a new peer wants a P2P session (callback 1202, main thread).
static void steam_session_request(sid_t from)
{
	InterlockedIncrement(&g_steam_session_requests);
	int ok = (g_host_self_test || getenv("OBSCOOP_ALLOW_SELF")) && from == g_self_sid ? 1 :   // one-PC test only
		g_role == 1 ? (g_peer_ok ? from == g_peer : steam_is_friend(from))   // host: friends only, one guest
		: (g_host_sid_cfg ? from == g_host_sid_cfg : steam_is_friend(from));     // guest: the host it called
	if (ok) {
		steam_accept(from);
		logf("steam: accepted a P2P session from a friend");
	} else {
		InterlockedIncrement(&g_steam_refused);
		logf("steam: refused a P2P session (not a friend, or not our peer)");
	}
}

static void net_handle(const char *buf, int n, sid_t from)
{
	const pkt_hdr *h = (const pkt_hdr *)buf;
	if (n < (int)sizeof *h || h->magic != NET_MAGIC || h->version != NET_VERSION ||
		(int)(sizeof *h + h->len) != n) {
		InterlockedIncrement(&g_net_bad);
		return;
	}
	if (g_role == 1) {
		if (!g_peer_ok) {
			if (h->type != PKT_HELLO)
				return;
			g_peer = from;
			g_peer_ok = 1;
			g_last_input_seq = 0;
			logf("net: guest joined (%s)", g_use_steam ? "steam" : "udp");
		} else if (from != g_peer) {
			return;   // only one guest
		}
		g_last_rx_tick = GetTickCount();
		InterlockedIncrement(&g_net_rx);
		switch (h->type) {
		case 19 /* PKT_MOVEREF */:
			host_on_moveref(buf, n);
			break;
		case PKT_HELLO: {
			pkt_hdr w;
			net_send(PKT_WELCOME, &w, 0);
			break;
		}
		case PKT_INPUT: {
			const pkt_input *in = (const pkt_input *)buf;
			if (n != sizeof *in || (LONG)(h->seq - (DWORD)g_last_input_seq) <= 0)
				break;   // wrong size or older than what we applied
			g_last_input_seq = (LONG)h->seq;
			vpad_set(g_remote_pad, 1, in->pad.buttons, in->pad.lx, in->pad.ly, in->pad.rx, in->pad.ry,
				in->pad.lt, in->pad.rt);
			break;
		}
		case PKT_PING: {
			pkt_ping pong = *(const pkt_ping *)buf;
			if (n == sizeof pong)
				net_send(PKT_PONG, &pong, sizeof pong - sizeof(pkt_hdr));
			break;
		}
		case PKT_BYE:
			host_drop_guest("bye");
			break;
		case 13 /* PKT_SAVE_ACK */:
			host_on_save_ack(buf, n);
			break;
		}
	} else if (g_role == 2) {
		if (!g_peer_ok) {
			if (h->type != PKT_WELCOME)
				return;
			g_peer = from;   // steam: the friend that answered is the host
			g_peer_ok = 1;
			logf("net: host welcomed us (%s)", g_use_steam ? "steam" : "udp");
		} else if (from != g_peer) {
			return;
		}
		g_last_rx_tick = GetTickCount();
		InterlockedIncrement(&g_net_rx);
		if (h->type == 7 /* PKT_STATE */) {
			guest_on_state(buf, n);
		} else if (h->type == 8 /* PKT_FLAGS */) {
			guest_on_flags(buf, n);
		} else if (h->type == 9 /* PKT_LEVEL */) {
			guest_on_level(buf, n);
		} else if (h->type == 10 /* PKT_PNJ */) {
			guest_on_pnj(buf, n);
		} else if (h->type == 11 /* PKT_ITEMS */) {
			guest_on_items(buf, n);
		} else if (h->type == 17 /* PKT_MSG */) {
			guest_on_msg(buf, n);
		} else if (h->type == 16 /* PKT_VIEW */) {
			guest_on_view(buf, n);
		} else if (h->type == 15 /* PKT_UI */) {
			guest_on_ui(buf, n);
		} else if (h->type == 14 /* PKT_INV */) {
			guest_on_inv(buf, n);
		} else if (h->type == 12 /* PKT_SAVE */) {
			guest_on_save(buf, n);
		} else if (h->type == 20 /* PKT_PROPS */) {
			guest_on_props(buf, n);
		} else if (h->type == 18 /* PKT_LOAD */) {
			guest_on_load(buf, n);
		} else if (h->type == PKT_PONG && n == sizeof(pkt_ping)) {
			g_net_rtt_ms = (LONG)(GetTickCount() - ((const pkt_ping *)buf)->t_ms);
		}
	}
}

// Periodic work shared by both transports: timeouts, hello, the guest's input and pings.
static void net_tick(void)
{
	static DWORD last_hello, last_ping, last_input, last_moveref, tick;
	DWORD now = GetTickCount();
	if (g_role == 1) {
		if (g_peer_ok && now - g_last_rx_tick > (g_use_steam ? 10000u : 3000u))
			host_drop_guest(g_use_steam ? "timeout 10 s" : "timeout 3 s");
		// Steam: also accept sessions from every friend in Obscure now, in case the session-request
		// callback (1202) does not reach us (it could not be tested with one account).
		static DWORD last_accept;
		if (g_use_steam && !g_peer_ok && now - last_accept >= 2000) {
			last_accept = now;
			sid_t ids[16];
			sid_t saved = g_host_sid_cfg;
			g_host_sid_cfg = 0;   // scan friends, not the guest's configured host
			int k = steam_find_hosts(ids, 16);
			g_host_sid_cfg = saved;
			g_last_candidates = k;
			for (int i = 0; i < k; i++)
				steam_accept(ids[i]);
		}
		return;
	}
	if (!g_peer_ok && now - last_hello >= 1000) {
		pkt_hdr hello;
		if (g_use_steam) {
			sid_t hosts[16];
			int n = steam_find_hosts(hosts, 16);
			g_last_candidates = n;
			static int scan_only = -1;
			if (scan_only < 0)
				scan_only = getenv("OBSCOOP_SCAN_ONLY") != NULL;   // test: count candidates, contact nobody
			for (int i = 0; i < n && !scan_only; i++)
				net_send_to(hosts[i], PKT_HELLO, &hello, 0);
			static int logged;
			if (!logged++)
				logf("net: looking for the host among %d candidate(s)", n);
		} else {
			net_send(PKT_HELLO, &hello, 0);
		}
		last_hello = now;
	}
	if (g_peer_ok && now - g_last_rx_tick > (g_use_steam ? 10000u : 3000u)) {
		g_peer_ok = 0;
		logf("net: host silent; saying hello again");
	}
	if (g_peer_ok && now - last_input >= 8) {
		pkt_input in;
		local_pad(g_local_pad, &in.pad);
		in.tick = ++tick;
		net_send(PKT_INPUT, &in, sizeof in - sizeof(pkt_hdr));
		last_input = now;
	}
	if (g_peer_ok && g_role == 2 && now - last_moveref >= 50) {
		guest_send_moveref();
		last_moveref = now;
	}
	if (g_peer_ok && now - last_ping >= 500) {
		pkt_ping p;
		p.t_ms = now;
		net_send(PKT_PING, &p, sizeof p - sizeof(pkt_hdr));
		last_ping = now;
	}
}

static DWORD WINAPI udp_thread(LPVOID arg)
{
	(void)arg;
	char buf[4096];
	for (;;) {
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(g_sock, &rd);
		struct timeval tv = { 0, 2000 };   // 2 ms
		if (select(0, &rd, NULL, NULL, &tv) > 0) {
			struct sockaddr_in from;
			int fl = sizeof from;
			int n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
			if (n > 0) {
				if (g_role == 1 && !g_peer_ok)
					g_udp_peer = from;   // the guest's address, for replies
				net_handle(buf, n, udp_key(&from));
			}
		}
		net_tick();
	}
}

// One-line status for the window title (title.c-style helper in sync.c shows it).
static void net_status_line(char *out, size_t sz)
{
	if (!g_role) {
		_snprintf(out, sz, "co-op off (no role set)");
	} else if (g_use_steam && !g_steam_ok) {
		_snprintf(out, sz, "co-op %s: waiting for Steam", g_role == 1 ? "host" : "guest");
	} else if (!g_peer_ok) {
		if (g_role == 1)
			_snprintf(out, sz, "co-op host: waiting for a guest (%s)", g_use_steam ? "Steam" : "UDP");
		else
			_snprintf(out, sz, "co-op guest: looking for the host (%d candidate%s)", g_last_candidates,
				g_last_candidates == 1 ? "" : "s");
	} else {
		if (g_net_rtt_ms >= 0)
			_snprintf(out, sz, "co-op %s: connected, %ld ms", g_role == 1 ? "host" : "guest", g_net_rtt_ms);
		else
			_snprintf(out, sz, "co-op %s: connected", g_role == 1 ? "host" : "guest");
	}
	out[sz - 1] = 0;
}

// Steam: called once per frame on the main thread (frame hook).
static void net_frame(void)
{
	if (!g_use_steam || !g_role)
		return;
	if (!g_steam_ok) {
		static DWORD last_try;
		if (GetTickCount() - last_try < 1000)
			return;
		last_try = GetTickCount();
		if (!steam_start())
			return;
		g_steam_ok = 1;
		if (g_host_self_test)
			g_host_sid_cfg = g_self_sid;
	}
	steam_poll();
	net_tick();
}

static int env_int(const char *name, int def)
{
	char v[32];
	return GetEnvironmentVariableA(name, v, sizeof v) ? atoi(v) : def;
}

static void net_start(void)
{
	char role[16] = "", net[16] = "udp", peer[64] = "127.0.0.1", hostid[32] = "";
	GetEnvironmentVariableA("OBSCOOP_ROLE", role, sizeof role);
	GetEnvironmentVariableA("OBSCOOP_NET", net, sizeof net);
	GetEnvironmentVariableA("OBSCOOP_PEER", peer, sizeof peer);
	GetEnvironmentVariableA("OBSCOOP_HOST_STEAMID", hostid, sizeof hostid);
	g_role = !_stricmp(role, "host") ? 1 : !_stricmp(role, "guest") ? 2 : 0;
	if (!g_role)
		return;
	g_remote_pad = env_int("OBSCOOP_REMOTE_PAD", 0) & 3;
	g_local_pad = env_int("OBSCOOP_LOCAL_PAD", 0) & 3;
	if (!_stricmp(net, "steam")) {
		g_use_steam = 1;
		g_host_sid_cfg = _strtoui64(hostid, NULL, 10);
		g_host_self_test = !_stricmp(hostid, "self");   // one-PC test: call our own account
		logf("net: %s over steam (starts after SteamAPI_Init), remote pad %d, local pad %d, host id %s",
			g_role == 1 ? "host" : "guest", g_remote_pad, g_local_pad, g_host_sid_cfg ? "configured" : "from friends list");
		return;
	}
	if (_stricmp(net, "udp")) {
		logf("net: transport '%s' unknown", net);
		g_role = 0;
		return;
	}
	int port = env_int("OBSCOOP_UDP", 27960);
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
	g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in me = { 0 };
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only: udp is the one-PC test transport
	if (g_role == 1) {
		me.sin_port = htons((u_short)port);
	} else {
		me.sin_port = 0;
		g_udp_peer.sin_family = AF_INET;
		g_udp_peer.sin_port = htons((u_short)port);
		inet_pton(AF_INET, peer, &g_udp_peer.sin_addr);
		g_peer = udp_key(&g_udp_peer);
	}
	if (bind(g_sock, (struct sockaddr *)&me, sizeof me)) {
		logf("net: bind failed (%d)", WSAGetLastError());
		return;
	}
	logf("net: %s over udp, %s port %d, remote pad %d, local pad %d", g_role == 1 ? "host" : "guest",
		g_role == 1 ? "listening on 127.0.0.1" : peer, port, g_remote_pad, g_local_pad);
	CloseHandle(CreateThread(NULL, 0, udp_thread, NULL, 0, NULL));
}

static void net_status(char *reply, size_t replysz)
{
	_snprintf(reply, replysz, "net role %s via %s peer_ok %ld rx %ld tx %ld bad %ld last_input_seq %ld rtt_ms %ld "
		"steam_ok %ld session_requests %ld accepted %ld refused %ld tx_fail %ld",
		g_role == 1 ? "host" : g_role == 2 ? "guest" : "off", g_use_steam ? "steam" : "udp", g_peer_ok, g_net_rx,
		g_net_tx, g_net_bad, g_last_input_seq, g_net_rtt_ms, g_steam_ok, g_steam_session_requests, g_steam_accepted,
		g_steam_refused, g_steam_tx_fail);
}

// ---- single-instance lock -------------------------------------------------------
// The game refuses to start while the mutex "obscure_lock" exists. With OBSCOOP_MULTI=1 this copy
// uses a per-process name, so a host and a guest copy can run on one PC for tests.
static HANDLE(WINAPI *real_CreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static int g_multi;

static HANDLE WINAPI hk_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name)
{
	if (g_multi && name && !strcmp(name, "obscure_lock")) {
		char n[64];
		_snprintf(n, sizeof n, "obscure_lock_%lu", GetCurrentProcessId());
		logf("CreateMutexA %s -> %s", name, n);
		return real_CreateMutexA(sa, own, n);
	}
	return real_CreateMutexA(sa, own, name);
}
