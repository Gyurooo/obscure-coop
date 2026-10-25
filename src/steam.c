// obscoop Steam transport: P2P packets through the game's own steam_api.dll (2014, app 254460).
// Included by net.c. That DLL has no flat API, only C++ interfaces, so calls go through vtables:
//   SteamUser017        slot 2  GetSteamID() -> CSteamID           (returned through a hidden pointer)
//   SteamFriends013     slot 0  GetPersonaName(), slot 3 GetFriendCount(int flags),
//                       slot 4  GetFriendByIndex(int, int flags) -> CSteamID (hidden pointer),
//                       slot 5  GetFriendRelationship(CSteamID), slot 8 GetFriendGamePlayed(CSteamID, FriendGameInfo_t*)
//   SteamNetworking005  slot 0  SendP2PPacket(CSteamID, const void*, uint32, EP2PSend, int channel)
//                       slot 1  IsP2PPacketAvailable(uint32*, int), slot 2 ReadP2PPacket(void*, uint32, uint32*, CSteamID*, int)
//                       slot 3  AcceptP2PSessionWithUser(CSteamID), slot 7 AllowP2PPacketRelay(bool)
// MSVC thiscall: this in ECX, callee pops; a CSteamID argument is 8 bytes on the stack; a CSteamID
// result is written through a hidden pointer passed first; bool comes back in AL. A __fastcall
// function with an unused EDX argument has the same layout.
// Callbacks: SteamAPI_RegisterCallback(CCallbackBase*, id). CCallbackBase is { vtable, uint8 flags,
// int id }; MSVC puts the two Run overloads in reverse order, so the vtable is
// { Run(void*, bool, SteamAPICall_t), Run(void*), GetCallbackSizeBytes() } (as Steamworks.NET does on
// Windows). P2PSessionRequest_t = 1202 { CSteamID remote }.

typedef unsigned long long sid_t;
typedef void *(__cdecl *steam_getter_t)(void);
typedef void(__cdecl *register_cb_t)(void *cb, int id);
typedef sid_t *(__fastcall *get_sid_t)(void *self, void *edx, sid_t *ret);
typedef sid_t *(__fastcall *friend_by_index_t)(void *self, void *edx, sid_t *ret, int i, int flags);
typedef int(__fastcall *friend_count_t)(void *self, void *edx, int flags);
typedef int(__fastcall *friend_rel_t)(void *self, void *edx, sid_t id);
typedef unsigned char(__fastcall *friend_game_t)(void *self, void *edx, sid_t id, void *info);
typedef const char *(__fastcall *persona_name_t)(void *self, void *edx);
typedef unsigned char(__fastcall *p2p_send_t)(void *self, void *edx, sid_t id, const void *data, DWORD len, int type, int ch);
typedef unsigned char(__fastcall *p2p_avail_t)(void *self, void *edx, DWORD *size, int ch);
typedef unsigned char(__fastcall *p2p_read_t)(void *self, void *edx, void *dst, DWORD cap, DWORD *size, sid_t *from, int ch);
typedef unsigned char(__fastcall *p2p_accept_t)(void *self, void *edx, sid_t id);
typedef unsigned char(__fastcall *p2p_relay_t)(void *self, void *edx, unsigned char allow);

#pragma pack(push, 4)
typedef struct { sid_t game_id; DWORD ip; WORD port, query_port; sid_t lobby; } friend_game_info;   // 24 bytes
#pragma pack(pop)

static void *g_suser, *g_sfriends, *g_snet;
static sid_t g_self_sid;
static sid_t g_host_sid_cfg;          // guest: host Steam id from OBSCOOP_HOST_STEAMID (optional)
static volatile LONG g_steam_ok, g_steam_session_requests, g_steam_accepted, g_steam_refused;
static volatile LONG g_steam_tx_fail;

static void **vt_of(void *iface) { return *(void ***)iface; }

static int steam_is_friend(sid_t id)
{
	return g_sfriends && ((friend_rel_t)vt_of(g_sfriends)[5])(g_sfriends, NULL, id) == 3;   // k_EFriendRelationshipFriend
}

static void steam_accept(sid_t id)
{
	if (((p2p_accept_t)vt_of(g_snet)[3])(g_snet, NULL, id))
		InterlockedIncrement(&g_steam_accepted);
}

// Fake CCallbackBase for P2PSessionRequest_t (1202). Runs on the main thread inside SteamAPI_RunCallbacks.
static void steam_session_request(sid_t from);
static void __fastcall cb_run_result(void *self, void *edx, void *param, DWORD io_fail, sid_t call)
{
	(void)self; (void)edx; (void)io_fail; (void)call;
	if (param)
		steam_session_request(*(sid_t *)param);
}
static void __fastcall cb_run(void *self, void *edx, void *param)
{
	(void)self; (void)edx;
	if (param)
		steam_session_request(*(sid_t *)param);
}
static int __fastcall cb_size(void *self, void *edx)
{
	(void)self; (void)edx;
	return 8;
}
static void *g_cb_vtable[3] = { (void *)cb_run_result, (void *)cb_run, (void *)cb_size };
static struct { void **vt; BYTE flags; BYTE pad[3]; int id; } g_cb_session = { g_cb_vtable, 0, { 0 }, 1202 };

static int steam_start(void)
{
	HMODULE m = GetModuleHandleA("steam_api.dll");
	if (!m) {
		logf("steam: steam_api.dll not loaded");
		return 0;
	}
	steam_getter_t user = (steam_getter_t)GetProcAddress(m, "SteamUser");
	steam_getter_t friends = (steam_getter_t)GetProcAddress(m, "SteamFriends");
	steam_getter_t net = (steam_getter_t)GetProcAddress(m, "SteamNetworking");
	register_cb_t reg = (register_cb_t)GetProcAddress(m, "SteamAPI_RegisterCallback");
	if (!user || !friends || !net || !reg) {
		logf("steam: missing exports");
		return 0;
	}
	g_suser = user();
	g_sfriends = friends();
	g_snet = net();
	if (!g_suser || !g_sfriends || !g_snet) {
		logf("steam: interfaces not available yet (user %p friends %p net %p)", g_suser, g_sfriends, g_snet);
		return 0;
	}
	((get_sid_t)vt_of(g_suser)[2])(g_suser, NULL, &g_self_sid);
	const char *name = ((persona_name_t)vt_of(g_sfriends)[0])(g_sfriends, NULL);
	int nfriends = ((friend_count_t)vt_of(g_sfriends)[3])(g_sfriends, NULL, 4);
	((p2p_relay_t)vt_of(g_snet)[7])(g_snet, NULL, 1);
	reg(&g_cb_session, 1202);
	// Sanity: a real individual account id has universe 1, type 1 (0x011000010...) in the top bits.
	int id_ok = (g_self_sid >> 52) == 0x011;
	logf("steam: ready; own id %s, persona name %s (%d chars), %d friends, session-request callback registered",
		id_ok ? "valid" : "INVALID", name ? "readable" : "missing", name ? (int)strlen(name) : 0, nfriends);
	return id_ok;
}

// Guest: candidate hosts = the configured id, else friends playing Obscure (app 254460) right now.
static int steam_find_hosts(sid_t *out, int max)
{
	if (g_host_sid_cfg) {
		out[0] = g_host_sid_cfg;
		return 1;
	}
	int n = 0, in_game = 0, count = ((friend_count_t)vt_of(g_sfriends)[3])(g_sfriends, NULL, 4);
	for (int i = 0; i < count && n < max; i++) {
		sid_t id = 0;
		((friend_by_index_t)vt_of(g_sfriends)[4])(g_sfriends, NULL, &id, i, 4);
		friend_game_info gi;
		memset(&gi, 0, sizeof gi);
		if (!id || !((friend_game_t)vt_of(g_sfriends)[8])(g_sfriends, NULL, id, &gi))
			continue;
		in_game++;
		if ((gi.game_id & 0xffffff) == 254460)
			out[n++] = id;
	}
	static int logged;
	if (!logged++)
		logf("steam: friends scan: %d of %d friends in a game, %d in Obscure", in_game, count, n);
	return n;
}

static int steam_send(sid_t to, const void *data, int len)
{
	// k_EP2PSendUnreliable (0): the protocol handles loss itself; every packet stays under 1200 bytes.
	if (!((p2p_send_t)vt_of(g_snet)[0])(g_snet, NULL, to, data, (DWORD)len, 0, 0)) {
		InterlockedIncrement(&g_steam_tx_fail);
		return 0;
	}
	return 1;
}

static void net_handle(const char *buf, int n, sid_t from);

static void steam_poll(void)
{
	DWORD size;
	static char buf[4096];
	while (((p2p_avail_t)vt_of(g_snet)[1])(g_snet, NULL, &size, 0)) {
		sid_t from = 0;
		DWORD got = 0;
		if (!((p2p_read_t)vt_of(g_snet)[2])(g_snet, NULL, buf, sizeof buf, &got, &from, 0))
			break;
		if (got <= sizeof buf)
			net_handle(buf, (int)got, from);
	}
}
