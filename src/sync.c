// obscoop world sync, stage 3: host-authoritative mirror of the human characters.
// Included by obscoop.c after net.c. Addresses are for Steam build 215286 (checked at load).
//
// Facts used (static analysis, notes "Phase 3" and "Run 14"):
//   game object       *(0x6ab8e4)
//   real players      game+0x4fc, game+0x500; real player +0xc = controlled character, +0x14 = game
//   human characters  list *(game+0x510)+0x1c: head at +0, nodes {item, next}
//   character         +0x20/+0x24/+0x28 position (x, y ground plane, z height), +0x2c/+0x30/+0x34
//                     facing, +0x54 health, +0x68 physics body, +0x6c body-disabled flag, +0x78 id,
//                     vtable slot 2 SetPosition(float*), slot 3 SetDirection(float*)
//                     (from TM_SetPlayerPos 0x4dc640 and TM_SetPlayerView 0x4dc6d0)
//   character +0x60   movement state (10 stand, 20 walk, 30 run; run 19), set with FUN_00436bd0(state)
//                     (thiscall on the character, as TM_SetPlayerState 0x4dddf0 calls it)
//   CPlayerHuman      vtable 0x6449e4, slot 1 = Update(dt) (FUN_0043f6e0). It reads the move intent
//                     +0x238..+0x254 (direction vectors, magnitude; written earlier in the frame by the
//                     player's controller or the AI), +0xd8 (moving) and +0x230 (requested state:
//                     FUN_0043bf60 moves from 10 to whatever is requested), then picks state and animation.
//   FUN_0044b7e0      real player method: switch to this character (sets real player +0xc; returns
//                     early while story byte 0x1c is 1..9). FUN_0044b3b0 only moves the +0x84 flag.

#define GAME_PTR ((BYTE **)0x6ab8e4)

// Milliseconds from the performance counter (GetTickCount steps in 15.6 ms, too coarse for smoothing).
static double clock_ms(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER c;
	if (!f.QuadPart)
		QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#define MAX_CHARS 16

#pragma pack(push, 1)
typedef struct char_state_s { DWORD id; float pos[3], dir[3], hp; DWORD flags, state, moving, wanted; BYTE intent[0x1c];
	DWORD ev_seq; WORD ev_val[4]; DWORD door_type, door_id; } char_state;   // ev_val[k] = requested state of event ev_seq - k   // flags bit0 = player 0's, bit1 = player 1's
typedef struct { pkt_hdr h; DWORD frame, t_ms; BYTE level, count; char_state c[MAX_CHARS]; } pkt_state;   // t_ms: host clock_ms() at send
typedef struct { pkt_hdr h; DWORD frame; BYTE level; BYTE flags[161]; } pkt_flags;
typedef struct { pkt_hdr h; DWORD seq, entry; char name[32]; } pkt_level;
#define MAX_PNJ 32   // 32 * 34 bytes + header stays under 1200 (Steam unreliable limit)
typedef struct { DWORD vtable; float pos[3], dir[3], hp; BYTE dead58, dead5c; } pnj_state;
typedef struct { pkt_hdr h; DWORD frame, t_ms; BYTE level, count; pnj_state e[MAX_PNJ]; } pkt_pnj;   // t_ms as in pkt_state
#define PNJ_HDR (2 * sizeof(DWORD) + 2)
static volatile LONG g_breaks_applied;
#define MAX_ITEMS 48
typedef struct { DWORD type, id, vtable; BYTE kind; DWORD a, b; } item_state;   // kind 1 door (a +0x88, b +0xfc), 2 collected, 3 broken (b = +0x30)
typedef struct { pkt_hdr h; DWORD frame; BYTE level, count; item_state e[MAX_ITEMS]; } pkt_items;
#define SAVE_CHUNK 1000
typedef struct { pkt_hdr h; DWORD xfer, total, offset; WORD len; BYTE data[SAVE_CHUNK]; } pkt_save;
typedef struct { pkt_hdr h; DWORD xfer, have; } pkt_save_ack;
#define MAX_INV 64   // 64 * 16 + 25 bytes stays under the 1200-byte unreliable limit
typedef struct { DWORD type, id, count, energy; } inv_item;
typedef struct { pkt_hdr h; DWORD frame; BYTE level, count; DWORD char_id, hand_type, hand_id; inv_item e[MAX_INV]; } pkt_inv;
typedef struct { DWORD char_id, b48[2], b4c[2], timer, mode, menu, action, act_type, act_id, sel[2], scroll[2], anim[2]; } ui_player;
typedef struct { pkt_hdr h; DWORD frame; BYTE level, count; ui_player p[2]; } pkt_ui;
typedef struct { pkt_hdr h; DWORD seq, active, a, b, c; char name[64]; } pkt_view;
#define MSG_RING 8
typedef struct { DWORD a[7]; } msg_args;
typedef struct { pkt_hdr h; DWORD seq; msg_args m[MSG_RING]; } pkt_msg;   // m[k] = message seq - k
#pragma pack(pop)
enum { PKT_UI = 15, PKT_VIEW = 16, PKT_MSG = 17 };
enum { PKT_STATE = 7, PKT_FLAGS = 8, PKT_LEVEL = 9, PKT_PNJ = 10, PKT_ITEMS = 11, PKT_SAVE = 12, PKT_SAVE_ACK = 13, PKT_INV = 14 };
#define STATE_HDR (2 * sizeof(DWORD) + 2)

typedef void(__fastcall *set_vec_t)(void *self, void *edx, float *v);
typedef void(__fastcall *body_update_t)(void *self, void *edx);
typedef void(__fastcall *control_char_t)(void *realplayer, void *edx, void *character);
typedef void(__fastcall *set_state_t)(void *character, void *edx, int state);
static LONG g_state_sets;   // puppet updates driven with host intent (guest)
static struct { BYTE *c; char_state st; } g_puppet[MAX_CHARS];   // main thread only
static int g_puppets;

static volatile LONG g_frames;
static DWORD g_sync_frame;
static CRITICAL_SECTION g_state_cs;
static pkt_state g_rx_state;          // guest: latest state from the host
static volatile LONG g_rx_state_new, g_applied, g_apply_missing;
static volatile LONG g_rx_state_ok, g_rx_state_bad, g_skip_nochars, g_skip_level, g_skip_change;
static DWORD g_guest_char_id = 0xffffffff;   // guest: id of the character we control on the host
static LONG g_guest_switch_tries;

static BYTE *game_obj(void)
{
	return *GAME_PTR;
}

// ---- requested-state events --------------------------------------------------------------
// One-frame requests (an attack swing: +0x230 = 103 for a single frame, then the state machine runs
// it) fall between snapshots. The host records every change of a character's requested state in its
// Update (before the game consumes it); the guest replays missed ones in order, one per Update.
#define EV_IDS 16
static struct { DWORD last, seq; WORD val[4]; } g_ev_host[EV_IDS];
static struct { DWORD applied; int primed; } g_ev_guest[EV_IDS];
static volatile LONG g_events_recorded, g_events_replayed;

static void host_record_wanted(DWORD id, DWORD wanted)
{
	if (id >= EV_IDS)
		return;
	if (g_ev_host[id].seq && g_ev_host[id].last == wanted)
		return;
	g_ev_host[id].last = wanted;
	g_ev_host[id].seq++;
	memmove(&g_ev_host[id].val[1], &g_ev_host[id].val[0], 3 * sizeof(WORD));
	g_ev_host[id].val[0] = (WORD)wanted;
	InterlockedIncrement(&g_events_recorded);
}

struct char_state_s;
static void host_events_for(DWORD id, struct char_state_s *st);

// Current level id: byte at *(game+0x510)+8 (FUN_00416a70 saves it as the previous level).
static int level_id(void)
{
	BYTE *g = *GAME_PTR;
	BYTE *mgr = g ? *(BYTE **)(g + 0x510) : NULL;
	return mgr ? mgr[8] : -1;
}

// Level name for an id, from the game's table (FUN_0042c150: 0x6f ids at 0x695df8, names at 0x695e68).
static const char *level_name(int id)
{
	for (int i = 0; i < 0x6f; i++)
		if (((BYTE *)0x695df8)[i] == (BYTE)id)
			return ((const char **)0x695e68)[i];
	return NULL;
}

// Request a level change exactly as TM_LoadLevel / TM_EventChangeLevel do. Main thread only.
static void request_level(const char *name, DWORD entry)
{
	BYTE *g = game_obj();
	if (!g || !name)
		return;
	strncpy((char *)g + 0x590, name, 31);
	((char *)g)[0x590 + 31] = 0;
	*(DWORD *)(g + 0x5d0) = entry;
	*(DWORD *)(g + 8) = 5;
}

static volatile LONG g_level_cmd_id = -1, g_level_cmd_entry;   // pipe "level" (host tests)
static volatile LONG g_tp_pending;                              // pipe "teleport" (tests)
static float g_tp_pos[3], g_tp_dir[3];
static int g_tp_player;   // 0 = real player 0 (default), 1 = real player 1
static volatile LONG g_sw_player = -1, g_sw_char;   // pipe "switch" (tests)
static volatile LONG g_hurt_index = -1;                         // pipe "hurt" (tests)
static volatile LONG g_hurtp_player = -1;                       // pipe "hurtp" (tests)
static float g_hurtp_delta;
static float g_hurt_delta;
static DWORD g_level_seq, g_level_sent_until;
static pkt_level g_level_pkt;
static volatile LONG g_rx_level_seq, g_rx_level_new, g_level_applied, g_level_blocked, g_flags_applied;
static pkt_level g_rx_level;
static pkt_flags g_rx_flags;
static volatile LONG g_rx_flags_new;

static BYTE *rd_ptr(BYTE *p, int off)
{
	return p ? *(BYTE **)(p + off) : NULL;
}

// Walks an entity list of the level manager *(game+0x510) without touching the game's own
// iteration cursor (+8): +0x1c humans (CPlayerHuman), +0x58 enemies (CPlayerPNJ*, run 24: six
// CPlayerPNJMord in C006, same order and positions on both copies after loading).
static int list_entities(int off, BYTE **out, int max)
{
	BYTE *g = game_obj();
	BYTE *mgr = rd_ptr(g, 0x510);
	if (!mgr)
		return 0;
	BYTE **node = *(BYTE ***)(mgr + off);
	int n = 0;
	while (node && n < max) {
		if (node[0])
			out[n++] = node[0];
		node = (BYTE **)node[1];
	}
	return n;
}

static int list_chars(BYTE **out, int max)
{
	return list_entities(0x1c, out, max);
}


// The guest's real player on the host. Usually slot 2 (+0x500), but when player 1's character dies
// with no free character left, FUN_0044c2b0 swaps the two real players (the survivor becomes player 1,
// the dead one player 2 with role 200). The objects keep their controllers, so the host follows the
// object that joined as player 2, not the slot (0.7 playtest: the guest was switched onto the dead
// character and crashed in FUN_0044b7e0).
static BYTE *g_remote_rp;
static void host_send_props(LONG tick);
static void guest_apply_props(void);
static void host_send_breaks(LONG tick);
static void guest_apply_breaks(void);
static float fabs_f(float v) { return v < 0 ? -v : v; }

static void host_send_state(void)
{
	BYTE *g = game_obj();
	BYTE *chars[MAX_CHARS];
	int n = list_chars(chars, MAX_CHARS);
	if (!g || !n)
		return;
	BYTE *r0 = rd_ptr(g, 0x4fc), *r1 = rd_ptr(g, 0x500);
	if (g_remote_rp != r0 && g_remote_rp != r1)
		g_remote_rp = r1;   // joined (or reloaded): the guest is player 2
	BYTE *local_rp = g_remote_rp == r0 ? r1 : r0;
	static BYTE *logged_slot;
	if (g_remote_rp && g_remote_rp == r0 && logged_slot != r0) {
		logf("sync: the game swapped the players (a death); the guest is now in player slot 1");
		logged_slot = r0;
	}
	BYTE *pc0 = rd_ptr(local_rp, 0xc), *pc1 = rd_ptr(g_remote_rp, 0xc);
	pkt_state s;
	s.frame = ++g_sync_frame;
	s.t_ms = (DWORD)clock_ms();
	s.level = (BYTE)level_id();
	s.count = (BYTE)n;
	for (int i = 0; i < n; i++) {
		BYTE *c = chars[i];
		s.c[i].id = *(DWORD *)(c + 0x78);
		memcpy(s.c[i].pos, c + 0x20, 12);
		memcpy(s.c[i].dir, c + 0x2c, 12);
		s.c[i].hp = *(float *)(c + 0x54);
		s.c[i].flags = (c == pc0 ? 1 : 0) | (c == pc1 ? 2 : 0);
		s.c[i].state = *(DWORD *)(c + 0x60);
		s.c[i].moving = *(DWORD *)(c + 0xd8);
		s.c[i].wanted = *(DWORD *)(c + 0x230);
		BYTE *door = rd_ptr(c, 0x224);   // the door being used (set with request 50, FUN_004f3eb0)
		s.c[i].door_type = door ? *(DWORD *)(door + 4) : 0;
		s.c[i].door_id = door ? *(DWORD *)(door + 8) : 0;
		host_events_for(*(DWORD *)(c + 0x78), &s.c[i]);
		memcpy(s.c[i].intent, c + 0x238, sizeof s.c[i].intent);
	}
	net_send(PKT_STATE, &s, (WORD)(STATE_HDR + n * sizeof(char_state)));
}

static void put_char(BYTE *c, const struct char_state_s *st);
// Host: enemies, in list order, with their vtable so the guest can check it has the same kind.
static void host_send_pnj(void)
{
	BYTE *e[MAX_PNJ];
	int n = list_entities(0x58, e, MAX_PNJ);
	pkt_pnj p;
	p.frame = g_sync_frame;
	p.t_ms = (DWORD)clock_ms();
	p.level = (BYTE)level_id();
	p.count = (BYTE)n;
	for (int i = 0; i < n; i++) {
		p.e[i].vtable = *(DWORD *)e[i];
		memcpy(p.e[i].pos, e[i] + 0x20, 12);
		memcpy(p.e[i].dir, e[i] + 0x2c, 12);
		p.e[i].hp = *(float *)(e[i] + 0x54);
		p.e[i].dead58 = *(DWORD *)(e[i] + 0x58) != 0;
		p.e[i].dead5c = *(DWORD *)(e[i] + 0x5c) != 0;
	}
	net_send(PKT_PNJ, &p, (WORD)(PNJ_HDR + n * sizeof(pnj_state)));
}

static volatile LONG g_pnj_applied, g_pnj_mismatch, g_life_changes, g_kills;
// Guest: the last PNJ_SNAPS enemy packets with the host's send time (smoothed like the characters,
// see guest_smoothed_state; 1.0 placed enemies only when a packet arrived and the guest's own AI
// moved them in between).
#define PNJ_SNAPS 32
static pkt_pnj g_pnj_snap[PNJ_SNAPS];
static double g_pnj_t[PNJ_SNAPS], g_pnj_last_host_t;
static int g_pnj_count, g_pnj_head;
static DWORD g_pnj_last_wire_t, g_pnj_last_frame;
static int guest_smoothed_pnj(pkt_pnj *out);

static void guest_on_pnj(const char *buf, int n)
{
	const pkt_pnj *p = (const pkt_pnj *)buf;
	if (n < (int)(sizeof(pkt_hdr) + PNJ_HDR) || p->count > MAX_PNJ || n != (int)(sizeof(pkt_hdr) + PNJ_HDR + p->count * sizeof(pnj_state)))
		return;
	EnterCriticalSection(&g_state_cs);
	if (!g_pnj_count || (LONG)(p->frame - g_pnj_last_frame) > 0) {
		double ht = g_pnj_count ? g_pnj_last_host_t + (double)(LONG)(p->t_ms - g_pnj_last_wire_t) : (double)p->t_ms;
		g_pnj_last_host_t = ht;
		g_pnj_last_wire_t = p->t_ms;
		g_pnj_last_frame = p->frame;
		memcpy(&g_pnj_snap[g_pnj_head], buf, n);
		g_pnj_t[g_pnj_head] = ht;
		g_pnj_head = (g_pnj_head + 1) % PNJ_SNAPS;
		if (g_pnj_count < PNJ_SNAPS)
			g_pnj_count++;
	}
	LeaveCriticalSection(&g_state_cs);
}

// Health is the host's: changes go through the game's own AddLife (vtable slot 6, as TM_SetLife
// uses it) so hit reactions and deaths play; a death the guest did not reach is completed with the
// flags TM_KillPlayer sets (+0x54 = 0, +0x58 = +0x5c = 1).
typedef void(__fastcall *add_life_t)(void *self, void *edx, float delta);

static void sync_life(BYTE *c, float hp, int dead58, int dead5c)
{
	float mine = *(float *)(c + 0x54);
	if (hp < mine - 0.01f || hp > mine + 0.01f) {
		((add_life_t)(*(void ***)c)[6])(c, NULL, hp - mine);
		InterlockedIncrement(&g_life_changes);
	}
	if ((dead58 || dead5c) && !*(DWORD *)(c + 0x58) && !*(DWORD *)(c + 0x5c)) {
		*(float *)(c + 0x54) = 0.0f;
		*(DWORD *)(c + 0x5c) = 1;
		*(DWORD *)(c + 0x58) = 1;
		InterlockedIncrement(&g_kills);
	}
}

static void guest_apply_pnj(void)
{
	static pkt_pnj p;   // every frame: the smoothed enemy state (the guest's own AI moves them in between)
	if (!guest_smoothed_pnj(&p))
		return;
	BYTE *g = game_obj();
	if (!g || p.level != (BYTE)level_id() || *(DWORD *)(g + 8) == 5)
		return;
	BYTE *e[MAX_PNJ];
	int n = list_entities(0x58, e, MAX_PNJ);
	for (int i = 0; i < p.count && i < n; i++) {
		if (*(DWORD *)e[i] != p.e[i].vtable) {
			InterlockedIncrement(&g_pnj_mismatch);
			continue;
		}
		char_state st;
		memset(&st, 0, sizeof st);
		memcpy(st.pos, p.e[i].pos, 12);
		memcpy(st.dir, p.e[i].dir, 12);
		put_char(e[i], &st);
		sync_life(e[i], p.e[i].hp, p.e[i].dead58, p.e[i].dead5c);
	}
	if (p.count != n)
		InterlockedIncrement(&g_pnj_mismatch);
	InterlockedIncrement(&g_pnj_applied);
}

// ---- items (level manager list +0x30; key = type +4, id +8) -------------------------------------
// CItDoor (type 1): +0x88 opening, +0xfc opening time in ms (FUN_004f34d0 animates up to 2000).
// Collectables (vtable slot 2 returns non-zero, as TM_ItIsCollected tests): +0xfc collected. The
// collect path (FUN_00448a00, the inventory's AddItem) sets +0xfc = 1, then calls slot 35 (0) and
// slot 18 SetVisible(0); the guest repeats those two calls to take the item out of its world.
#define VT_CITDOOR 0
typedef DWORD(__fastcall *item_query_t)(void *self, void *edx);
typedef void(__fastcall *item_set_t)(void *self, void *edx, DWORD v);
static DWORD g_door_vtable;   // learned from the first item of type 1 that RTTI calls CItDoor
static volatile LONG g_items_applied, g_items_collected_on_guest, g_doors_set;
static pkt_items g_rx_items;
static volatile LONG g_rx_items_new;

static int is_door(BYTE *it)
{
	if (!g_door_vtable) {
		// RTTI: vtable[-1] -> complete object locator, +12 -> type descriptor, +8 -> name
		DWORD vt = *(DWORD *)it;
		const char *name = (const char *)(*(DWORD *)(*(DWORD *)(vt - 4) + 12) + 8);
		if (!strcmp(name, ".?AVCItDoor@@"))
			g_door_vtable = vt;
	}
	return *(DWORD *)it == g_door_vtable && g_door_vtable;
}

static int is_collectable(BYTE *it)
{
	return ((item_query_t)(*(void ***)it)[2])(it, NULL) != 0;
}

// Breakable items (windows, door windows, walls): CItBreakable (vtable 0x64dc14), CItBreakableDoorWindow (0x64dcf4),
// CItBreakableWall (0x64dde4), CItBreakableWall2 (0x64de54), CItBreakableWindow (0x64e02c). Item +0x2c is 1 once broken
// and +0x30 counts the break animation up to 1000; vtable slot 0x60 breaks it with sound and effects (what
// TM_EventBreakIt calls), slot 0x64 breaks it at once without them. The pieces that fly are not synced.
typedef void(__fastcall *item_void_t)(void *self, void *edx);
static int is_breakable(BYTE *it)
{
	DWORD vt = *(DWORD *)it;
	return vt == 0x64dc14 || vt == 0x64dcf4 || vt == 0x64dde4 || vt == 0x64de54 || vt == 0x64e02c;
}

static void host_send_items(void)
{
	BYTE *it[256];
	int n = list_entities(0x30, it, 256);
	pkt_items p;
	int k = 0;
	p.frame = g_sync_frame;
	p.level = (BYTE)level_id();
	for (int i = 0; i < n; i++) {
		item_state *e = &p.e[k];
		if (is_door(it[i])) {
			e->kind = 1;
			e->a = *(DWORD *)(it[i] + 0x88);
			e->b = *(DWORD *)(it[i] + 0xfc);
		} else if (is_collectable(it[i]) && *(DWORD *)(it[i] + 0xfc)) {
			e->kind = 2;
			e->a = e->b = 1;
		} else if (is_breakable(it[i]) && *(DWORD *)(it[i] + 0x2c)) {
			e->kind = 3;
			e->a = 1;
			e->b = *(DWORD *)(it[i] + 0x30);
		} else {
			continue;
		}
		e->type = *(DWORD *)(it[i] + 4);
		e->id = *(DWORD *)(it[i] + 8);
		e->vtable = *(DWORD *)it[i];
		if (++k == MAX_ITEMS || i == n - 1) {
			p.count = (BYTE)k;
			net_send(PKT_ITEMS, &p, (WORD)(sizeof(DWORD) + 2 + k * sizeof(item_state)));
			k = 0;
		}
	}
	if (k) {
		p.count = (BYTE)k;
		net_send(PKT_ITEMS, &p, (WORD)(sizeof(DWORD) + 2 + k * sizeof(item_state)));
	}
}


// Test commands: "brk" lists the breakable items (type/id, vtable, broken flag, animation); "breakit type id [instant]"
// breaks one of this copy's the way TM_EventBreakIt does.
static void brk_status(char *reply, size_t replysz)
{
	BYTE *it[256];
	int n = list_entities(0x30, it, 256), k = _snprintf(reply, replysz, "breakables applied %ld:", g_breaks_applied), cnt = 0;
	for (int i = 0; i < n && k > 0 && (size_t)k < replysz - 60; i++) {
		if (!is_breakable(it[i]))
			continue;
		k += _snprintf(reply + k, replysz - k, " %lu/%lu v%lx b%lu t%lu", *(DWORD *)(it[i] + 4), *(DWORD *)(it[i] + 8), *(DWORD *)it[i], *(DWORD *)(it[i] + 0x2c), *(DWORD *)(it[i] + 0x30));
		cnt++;
	}
	_snprintf(reply + k, replysz - k, " (%d)", cnt);
}

static int brk_do(DWORD type, DWORD id, int instant)
{
	BYTE *it[256];
	int n = list_entities(0x30, it, 256);
	for (int i = 0; i < n; i++) {
		if (is_breakable(it[i]) && *(DWORD *)(it[i] + 4) == type && *(DWORD *)(it[i] + 8) == id) {
			((item_void_t)(*(void ***)it[i])[instant ? 0x64 / 4 : 0x60 / 4])(it[i], NULL);
			return 1;
		}
	}
	return 0;
}

static void guest_on_items(const char *buf, int n)
{
	const pkt_items *p = (const pkt_items *)buf;
	if (n < (int)(sizeof(pkt_hdr) + 6) || p->count > MAX_ITEMS || n != (int)(sizeof(pkt_hdr) + 6 + p->count * sizeof(item_state)))
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_items, buf, n);
	g_rx_items_new = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void guest_apply_items(void)
{
	pkt_items p;
	if (!g_rx_items_new)
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&p, &g_rx_items, sizeof p);
	g_rx_items_new = 0;
	LeaveCriticalSection(&g_state_cs);
	BYTE *g = game_obj();
	if (!g || p.level != (BYTE)level_id() || *(DWORD *)(g + 8) == 5)
		return;
	BYTE *it[256];
	int n = list_entities(0x30, it, 256);
	for (int i = 0; i < p.count; i++) {
		for (int j = 0; j < n; j++) {
			if (*(DWORD *)(it[j] + 4) != p.e[i].type || *(DWORD *)(it[j] + 8) != p.e[i].id || *(DWORD *)it[j] != p.e[i].vtable)
				continue;
			if (p.e[i].kind == 1) {
				if (*(DWORD *)(it[j] + 0x88) != p.e[i].a || *(DWORD *)(it[j] + 0xfc) != p.e[i].b) {
					*(DWORD *)(it[j] + 0x88) = p.e[i].a;
					*(DWORD *)(it[j] + 0xfc) = p.e[i].b;
					InterlockedIncrement(&g_doors_set);
				}
			} else if (p.e[i].kind == 3 && !*(DWORD *)(it[j] + 0x2c)) {
				// animated break (sound, effects) while the host's one is still going, at once if it finished long ago
				((item_void_t)(*(void ***)it[j])[p.e[i].b >= 1000 ? 0x64 / 4 : 0x60 / 4])(it[j], NULL);
				InterlockedIncrement(&g_breaks_applied);
				logf("sync: breakable type %lu id %lu was broken on the host; broken here (%s)", p.e[i].type, p.e[i].id, p.e[i].b >= 1000 ? "at once" : "animated");
			} else if (p.e[i].kind == 2 && !*(DWORD *)(it[j] + 0xfc)) {
				*(DWORD *)(it[j] + 0xfc) = 1;
				((item_set_t)(*(void ***)it[j])[35])(it[j], NULL, 0);
				((item_set_t)(*(void ***)it[j])[18])(it[j], NULL, 0);
				InterlockedIncrement(&g_items_collected_on_guest);
				logf("sync: item type %lu id %lu collected on the host; removed here", p.e[i].type, p.e[i].id);
			}
			break;
		}
	}
	InterlockedIncrement(&g_items_applied);
}

// ---- character inventories (weapons) ----------------------------------------------------
// Each character carries a CPlayerInventory at +0x128: 8 lists {head, tail, iter, count, flag} from
// +0x10 (nodes {item, next}; list k = category k of FUN_004eb1d0: 1 bare hands, 2..7 weapons), the
// weapon in hand at +0xd8. Items: +4 type, +8 id (unique per type), +0x3c count, +0x12c energy.
// The team's other items live elsewhere and are shared. The inventory's Give action (FUN_005111a0
// case 6, weapons only) moves the weapon: FUN_00449410(item, count) on the giver, FUN_00448a00(item,
// 0, 0) on the receiver. The guest never ran that, so the host sends every character's weapon lists
// and the guest repeats the same calls until its inventories match. Every collectable of the level
// is in the registry at game+0x648 (FUN_004ebdd0: lists from +0x18, stride 0x14, categories 1..17).
// Categories 8..17 (item +0x30) are the team's shared bag, *(game+0x7bc) + 0x14 * category; the
// game's add and remove (FUN_00448a00 / FUN_00449410, any character's inventory) route those there
// (0x448a19). The bag goes as pseudo character BAG_ID; for it hand_type 1 means "list cut short".
typedef DWORD(__fastcall *inv_remove_t)(void *inv, void *edx, void *item, DWORD count);
typedef DWORD(__fastcall *inv_add_t)(void *inv, void *edx, void *item, DWORD no_equip, DWORD refresh);
typedef void(__fastcall *inv_hold_t)(void *inv, void *edx, void *item);
#define INV_LISTS 8
#define BAG_ID 15
static volatile LONG g_inv_added, g_inv_removed, g_inv_held, g_inv_missing;
static pkt_inv g_rx_inv[EV_IDS];
static volatile LONG g_rx_inv_new[EV_IDS];
static volatile LONG g_give_from = -1, g_give_to;   // pipe "give" (tests)
static volatile LONG g_bag_use;                       // pipe "baguse" (tests)

// A character's own lists, or the team bag when c is NULL.
static int inv_items(BYTE *c, BYTE **out, int max)
{
	int n = 0;
	BYTE *bag = c ? NULL : rd_ptr(game_obj(), 0x7bc);
	if (!c && !bag)
		return 0;
	for (int k = c ? 0 : INV_LISTS; k < (c ? INV_LISTS : 0x12); k++)
		for (BYTE **node = *(BYTE ***)(c ? c + 0x128 + 0x10 + 0x14 * k : bag + 0x14 * k); node && n < max; node = (BYTE **)node[1])
			if (node[0])
				out[n++] = node[0];
	return n;
}

static BYTE *registry_item(DWORD type, DWORD id)
{
	BYTE *reg = game_obj() + 0x648;
	for (int k = 1; k < 0x12; k++)
		for (BYTE **node = *(BYTE ***)(reg + 4 + 0x14 * k); node; node = (BYTE **)node[1])
			if (node[0] && *(DWORD *)(node[0] + 4) == type && *(DWORD *)(node[0] + 8) == id)
				return node[0];
	return NULL;
}

static void host_send_inventories(void)
{
	BYTE *chars[MAX_CHARS + 1];
	int n = list_chars(chars, MAX_CHARS);
	chars[n++] = NULL;   // the team bag
	for (int i = 0; i < n; i++) {
		pkt_inv p;
		BYTE *it[MAX_INV + 1];
		int k = inv_items(chars[i], it, MAX_INV + 1);
		BYTE *hand = chars[i] ? rd_ptr(chars[i], 0x128 + 0xd8) : NULL;
		p.frame = g_sync_frame;
		p.level = (BYTE)level_id();
		p.char_id = chars[i] ? *(DWORD *)(chars[i] + 0x78) : BAG_ID;
		p.hand_type = hand ? *(DWORD *)(hand + 4) : (!chars[i] && k > MAX_INV);
		p.hand_id = hand ? *(DWORD *)(hand + 8) : 0;
		if (k > MAX_INV)
			k = MAX_INV;
		p.count = (BYTE)k;
		for (int j = 0; j < k; j++) {
			p.e[j].type = *(DWORD *)(it[j] + 4);
			p.e[j].id = *(DWORD *)(it[j] + 8);
			p.e[j].count = *(DWORD *)(it[j] + 0x3c);
			p.e[j].energy = chars[i] ? *(DWORD *)(it[j] + 0x12c) : 0;
		}
		net_send(PKT_INV, &p, (WORD)(offsetof(pkt_inv, e) - sizeof(pkt_hdr) + k * sizeof(inv_item)));
	}
}

static void guest_on_inv(const char *buf, int n)
{
	const pkt_inv *p = (const pkt_inv *)buf;
	if (n < (int)offsetof(pkt_inv, e) || p->count > MAX_INV || n != (int)(offsetof(pkt_inv, e) + p->count * sizeof(inv_item))
		|| p->char_id >= EV_IDS)
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_inv[p->char_id], buf, n);
	g_rx_inv_new[p->char_id] = 1;
	LeaveCriticalSection(&g_state_cs);
}

static int inv_has(const pkt_inv *p, BYTE *item)
{
	for (int j = 0; j < p->count; j++)
		if (p->e[j].type == *(DWORD *)(item + 4) && p->e[j].id == *(DWORD *)(item + 8))
			return 1;
	return 0;
}

// Takes the item out of whichever character's inventory holds it (the giver, after a Give).
static void inv_detach(BYTE *item, BYTE **chars, int n)
{
	for (int i = 0; i < n; i++) {
		BYTE *it[MAX_INV];
		int k = inv_items(chars[i], it, MAX_INV);
		for (int j = 0; j < k; j++)
			if (it[j] == item) {
				((inv_remove_t)0x449410)(chars[i] + 0x128, NULL, item, *(DWORD *)(item + 0x3c));
				InterlockedIncrement(&g_inv_removed);
				return;
			}
	}
}

// c is the character, or NULL for the team bag (then inv is any character's inventory).
static void guest_apply_inv_one(const pkt_inv *p, BYTE *c, BYTE *inv, BYTE **chars, int n)
{
	BYTE *it[MAX_INV];
	int k = inv_items(c, it, MAX_INV);
	for (int j = 0; j < k && !(!c && p->hand_type); j++)   // items the host no longer has (not on a cut list)
		if (!inv_has(p, it[j])) {
			logf("inv: character %lu loses item type %lu id %lu", p->char_id, *(DWORD *)(it[j] + 4), *(DWORD *)(it[j] + 8));
			((inv_remove_t)0x449410)(inv, NULL, it[j], *(DWORD *)(it[j] + 0x3c));
			InterlockedIncrement(&g_inv_removed);
		}
	for (int j = 0; j < p->count; j++) {
		BYTE *item = registry_item(p->e[j].type, p->e[j].id);
		if (!item) {
			if (InterlockedIncrement(&g_inv_missing) < 8)
				logf("inv: item type %lu id %lu of character %lu not in this level's registry", p->e[j].type, p->e[j].id, p->char_id);
			continue;
		}
		k = inv_items(c, it, MAX_INV);
		int have = 0;
		for (int i = 0; i < k; i++)
			have |= it[i] == item;
		if (!have) {
			if (c)
				inv_detach(item, chars, n);
			*(DWORD *)(item + 0x3c) = p->e[j].count;
			((inv_add_t)0x448a00)(inv, NULL, item, 0, 0);   // as the Give action does
			InterlockedIncrement(&g_inv_added);
			logf("inv: character %lu gets item type %lu id %lu", p->char_id, p->e[j].type, p->e[j].id);
		}
		if (*(DWORD *)(item + 0x3c) != p->e[j].count)
			*(DWORD *)(item + 0x3c) = p->e[j].count;
		// +0x12c (energy) exists on weapons only; team-bag items are smaller objects (run 70: writing it
		// there crashed the guest)
		if (c && *(DWORD *)(item + 0x12c) != p->e[j].energy)
			*(DWORD *)(item + 0x12c) = p->e[j].energy;
	}
	BYTE *hand = rd_ptr(inv, 0xd8);
	if (c && p->hand_id && (!hand || *(DWORD *)(hand + 4) != p->hand_type || *(DWORD *)(hand + 8) != p->hand_id)) {
		k = inv_items(c, it, MAX_INV);
		for (int i = 0; i < k; i++)
			if (*(DWORD *)(it[i] + 4) == p->hand_type && *(DWORD *)(it[i] + 8) == p->hand_id) {
				((inv_hold_t)0x448500)(inv, NULL, it[i]);   // FUN_00448500: put away the old one, take this one
				InterlockedIncrement(&g_inv_held);
				break;
			}
	}
}

static void guest_apply_inventories(void)
{
	BYTE *g = game_obj();
	if (!g || *(DWORD *)(g + 4) != 2 || *(DWORD *)(g + 8) == 5)
		return;
	BYTE *chars[MAX_CHARS];
	int n = list_chars(chars, MAX_CHARS);
	for (int i = 0; i < n; i++) {
		DWORD id = *(DWORD *)(chars[i] + 0x78);
		if (id >= EV_IDS || !g_rx_inv_new[id])
			continue;
		pkt_inv p;
		EnterCriticalSection(&g_state_cs);
		memcpy(&p, &g_rx_inv[id], sizeof p);
		g_rx_inv_new[id] = 0;
		LeaveCriticalSection(&g_state_cs);
		if (p.level == (BYTE)level_id())
			guest_apply_inv_one(&p, chars[i], chars[i] + 0x128, chars, n);
	}
	BYTE *rp0 = rd_ptr(rd_ptr(g, 0x4fc), 0xc);
	if (g_rx_inv_new[BAG_ID] && (rp0 || n)) {
		pkt_inv p;
		EnterCriticalSection(&g_state_cs);
		memcpy(&p, &g_rx_inv[BAG_ID], sizeof p);
		g_rx_inv_new[BAG_ID] = 0;
		LeaveCriticalSection(&g_state_cs);
		if (p.level == (BYTE)level_id())
			guest_apply_inv_one(&p, NULL, (rp0 ? rp0 : chars[0]) + 0x128, chars, n);
	}
}

// Test command: real player <from> gives the weapon in hand to real player <to>, with the same two
// calls as the inventory's Give action (no distance check).
static void host_test_give(void)
{
	BYTE *g = game_obj();
	BYTE *a = rd_ptr(rd_ptr(g, g_give_from ? 0x500 : 0x4fc), 0xc);
	BYTE *b = rd_ptr(rd_ptr(g, g_give_to ? 0x500 : 0x4fc), 0xc);
	BYTE *item = rd_ptr(a, 0x128 + 0xd8);
	if (a && b && item && *(DWORD *)(item + 4) != 0x75) {
		((inv_remove_t)0x449410)(a + 0x128, NULL, item, *(DWORD *)(item + 0x3c));
		((inv_add_t)0x448a00)(b + 0x128, NULL, item, 0, 0);
		logf("inv: test give of item type %lu id %lu from character %lu to %lu", *(DWORD *)(item + 4), *(DWORD *)(item + 8),
			*(DWORD *)(a + 0x78), *(DWORD *)(b + 0x78));
	} else {
		logf("inv: test give: nothing to give");
	}
	g_give_from = -1;
}

// ---- inventory menu on the guest's screen ------------------------------------------------
// The guest's own player never gets input (its pad goes to the host), so its inventory menu never
// opened there. CInventory (real player +0x10) is updated by FUN_00420020, called per frame from
// FUN_0044c2b0 at 0x44c6a7. It opens the items/weapons section while the real player's hold button
// objects (+0x48 items, +0x4c weapons; +0x10c held, +0x110 pressed) are held, and fades its bar with
// the timer +0x10. The selected item is the character's active item (+0x204, the section's +0x198
// points at it); each section re-derives it every frame from its scroll offset (+0xe8, 75 per item,
// +0xec animation, index +0x19c), so those are sent too. The action menu (embedded at +0x28) opens with +0x34 = 1 and FUN_00510b30(item, 1)
// (0x4208a0); +0x3c is the highlighted action.
// The host sends these per real player; the guest's call at 0x44c6a7 goes through hk_inv_update,
// which feeds the remote player's hold buttons and timer into its own player's update and mirrors the
// active item and the action menu. The action button is never fed, so no action runs on the guest.
typedef void(__fastcall *inv_update_t)(void *inv, void *edx, DWORD dt);
typedef void(__fastcall *action_open_t)(void *menu, void *edx, void *item, DWORD on);
static pkt_ui g_rx_ui;
static volatile LONG g_rx_ui_ok, g_ui_applied, g_ui_menu_opened;

static void host_send_ui(void)
{
	pkt_ui p;
	BYTE *g = game_obj();
	memset(&p, 0, sizeof p);
	p.frame = g_sync_frame;
	p.level = (BYTE)level_id();
	for (int i = 0; i < 2; i++) {
		BYTE *rp = rd_ptr(g, 0x4fc + 4 * i);
		BYTE *c = rd_ptr(rp, 0xc), *inv = rd_ptr(rp, 0x10);
		if (!c || !inv)
			continue;
		ui_player *u = &p.p[p.count++];
		u->char_id = *(DWORD *)(c + 0x78);
		BYTE *b48 = rd_ptr(rp, 0x48), *b4c = rd_ptr(rp, 0x4c);
		if (b48)
			memcpy(u->b48, b48 + 0x10c, 8);
		if (b4c)
			memcpy(u->b4c, b4c + 0x10c, 8);
		u->timer = *(DWORD *)(inv + 0x10);
		u->mode = *(DWORD *)(inv + 0x1c);
		u->menu = *(DWORD *)(inv + 0x34);
		u->action = *(DWORD *)(inv + 0x3c);
		BYTE *act = rd_ptr(c, 0x204);
		u->act_type = act ? *(DWORD *)(act + 4) : 0;
		u->act_id = act ? *(DWORD *)(act + 8) : 0;
		for (int k = 0; k < 2; k++) {
			BYTE *sec = rd_ptr(inv, 0x20 + 4 * k);
			u->sel[k] = sec ? *(DWORD *)(sec + 0x19c) : 0;
			u->scroll[k] = sec ? *(DWORD *)(sec + 0xe8) : 0;
			u->anim[k] = sec ? *(DWORD *)(sec + 0xec) : 0;
		}
	}
	net_send(PKT_UI, &p, (WORD)(offsetof(pkt_ui, p) - sizeof(pkt_hdr) + p.count * sizeof(ui_player)));
}

static void guest_on_ui(const char *buf, int n)
{
	const pkt_ui *p = (const pkt_ui *)buf;
	if (n < (int)offsetof(pkt_ui, p) || p->count > 2 || n != (int)(offsetof(pkt_ui, p) + p->count * sizeof(ui_player)))
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_ui, buf, n);
	g_rx_ui.count = p->count;
	g_rx_ui_ok = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void __fastcall hk_inv_update(BYTE *inv, void *edx, DWORD dt)
{
	BYTE *rp = rd_ptr(inv, 8), *c = rd_ptr(rp, 0xc);
	ui_player u;
	int have = 0;
	if (g_role == 2 && g_rx_ui_ok && c && rp == rd_ptr(game_obj(), 0x4fc)) {
		EnterCriticalSection(&g_state_cs);
		for (int i = 0; i < g_rx_ui.count; i++)
			if (g_rx_ui.p[i].char_id == *(DWORD *)(c + 0x78) && g_rx_ui.level == (BYTE)level_id()) {
				u = g_rx_ui.p[i];
				have = 1;
			}
		LeaveCriticalSection(&g_state_cs);
	}
	if (!have) {
		((inv_update_t)0x420020)(inv, edx, dt);
		return;
	}
	BYTE *b48 = rd_ptr(rp, 0x48), *b4c = rd_ptr(rp, 0x4c);
	DWORD s48[2], s4c[2];
	if (b48) {
		memcpy(s48, b48 + 0x10c, 8);
		memcpy(b48 + 0x10c, u.b48, 8);
	}
	if (b4c) {
		memcpy(s4c, b4c + 0x10c, 8);
		memcpy(b4c + 0x10c, u.b4c, 8);
	}
	if (u.timer < *(DWORD *)(inv + 0x10))
		*(DWORD *)(inv + 0x10) = u.timer;   // the bar shows while the host's shows
	for (int k = 0; k < 2; k++) {
		BYTE *sec = rd_ptr(inv, 0x20 + 4 * k);
		if (sec) {
			*(DWORD *)(sec + 0x19c) = u.sel[k];
			*(DWORD *)(sec + 0xe8) = u.scroll[k];
			*(DWORD *)(sec + 0xec) = u.anim[k];
		}
	}
	BYTE *act = rd_ptr(c, 0x204);
	if (u.act_id && (!act || *(DWORD *)(act + 4) != u.act_type || *(DWORD *)(act + 8) != u.act_id)) {
		BYTE *item = registry_item(u.act_type, u.act_id);
		if (item)
			*(BYTE **)(c + 0x204) = item;
	}
	((inv_update_t)0x420020)(inv, edx, dt);
	if (b48)
		memcpy(b48 + 0x10c, s48, 8);
	if (b4c)
		memcpy(b4c + 0x10c, s4c, 8);
	static DWORD last_sel, last_act, last_menu;
	if (u.sel[1] != last_sel || u.act_id != last_act || u.menu != last_menu) {
		BYTE *sec = rd_ptr(inv, 0x24), *now = rd_ptr(c, 0x204);
		logf("ui: host sel %lu item %lu/%lu menu %lu mode %lu -> here sel %lu item %lu mode %lu menu %lu", u.sel[1], u.act_type, u.act_id,
			u.menu, u.mode, sec ? *(DWORD *)(sec + 0x19c) : 99, now ? *(DWORD *)(now + 4) : 0, *(DWORD *)(inv + 0x1c), *(DWORD *)(inv + 0x34));
		last_sel = u.sel[1];
		last_act = u.act_id;
		last_menu = u.menu;
	}
	DWORD mode = *(DWORD *)(inv + 0x1c);
	if (u.menu && !*(DWORD *)(inv + 0x34) && mode == u.mode && mode < 2) {
		BYTE *sec = rd_ptr(inv, 0x20 + 4 * mode);
		BYTE **slot = sec ? *(BYTE ***)(sec + 0x198) : NULL;
		if (slot && *slot) {
			*(DWORD *)(inv + 0x34) = 1;
			((action_open_t)0x510b30)(inv + 0x28, NULL, *slot, 1);
			InterlockedIncrement(&g_ui_menu_opened);
		}
	} else if (!u.menu && *(DWORD *)(inv + 0x34)) {
		*(DWORD *)(inv + 0x34) = 0;
	}
	if (*(DWORD *)(inv + 0x34))
		*(DWORD *)(inv + 0x3c) = u.action;
	InterlockedIncrement(&g_ui_applied);
}

static void sync_install_inv_ui(void)
{
	BYTE *site = (BYTE *)0x44c6a7;
	if (site[0] != 0xe8 || *(LONG *)(site + 1) != (LONG)(0x420020 - (0x44c6a7 + 5))) {
		logf("ui: call at 0x44c6a7 is not call 0x420020; inventory menu mirror off");
		return;
	}
	DWORD old;
	VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
	*(LONG *)(site + 1) = (LONG)((BYTE *)hk_inv_update - (site + 5));
	VirtualProtect(site, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 5);
	logf("ui: inventory menu mirror installed (guest)");
}

// ---- full-screen pictures (item pickups, documents) on the guest's screen ------------------
// SFLB_10 (0x430350, thiscall on the viewer *(0x6ab8f0)) shows DATA\_common\photos_pc\<name>: the
// blue item picture after a pickup (FUN_0043bf60, "vflash", "vcompass", ...) and the background of a
// readable document (CItWrittenDocument use, FUN_004f0be0). viewer+4 = showing; it pauses the game
// (game+0xc) and closes on a player's A/B (FUN_00430270 -> FUN_004301b0). The guest never runs those
// pickups or reads, so the host hooks SFLB_10's entry (prologue 55 8b ec 81 ec 04 04 00 00 copied to a
// trampoline), sends what was shown, and the guest shows the same picture and closes it with the host.
typedef void(__fastcall *view_show_t)(void *viewer, void *edx, const char *name, DWORD a, DWORD b, DWORD c);
typedef void(__fastcall *view_close_t)(void *viewer, void *edx);
static BYTE *g_view_tramp;
static volatile LONG g_view_seq, g_views_shown, g_views_closed;
static char g_view_name[64];
static DWORD g_view_args[3];
static pkt_view g_rx_view;
static volatile LONG g_rx_view_ok;

static void __fastcall hk_view_show(void *viewer, void *edx, const char *name, DWORD a, DWORD b, DWORD c)
{
	if (name && g_role == 1) {
		strncpy(g_view_name, name, sizeof g_view_name - 1);
		g_view_args[0] = a;
		g_view_args[1] = b;
		g_view_args[2] = c;
		InterlockedIncrement(&g_view_seq);
		logf("view: host shows \"%s\" (%lu %lu %lu)", g_view_name, a, b, c);
	}
	((view_show_t)g_view_tramp)(viewer, edx, name, a, b, c);
}

static void sync_install_view_hook(void)
{
	static const BYTE prologue[9] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x04, 0x04, 0x00, 0x00 };
	BYTE *f = (BYTE *)0x430350;
	if (memcmp(f, prologue, 9)) {
		logf("view: SFLB_10 prologue differs; picture mirror off");
		return;
	}
	g_view_tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_view_tramp)
		return;
	memcpy(g_view_tramp, f, 9);
	g_view_tramp[9] = 0xe9;
	*(LONG *)(g_view_tramp + 10) = (LONG)((f + 9) - (g_view_tramp + 14));
	DWORD old;
	VirtualProtect(f, 9, PAGE_EXECUTE_READWRITE, &old);
	f[0] = 0xe9;
	*(LONG *)(f + 1) = (LONG)((BYTE *)hk_view_show - (f + 5));
	memset(f + 5, 0x90, 4);
	VirtualProtect(f, 9, old, &old);
	FlushInstructionCache(GetCurrentProcess(), f, 9);
	logf("view: picture hook installed (host)");
}

static void host_send_view(void)
{
	pkt_view p;
	BYTE *viewer = *(BYTE **)0x6ab8f0;
	memset(&p, 0, sizeof p);
	p.seq = (DWORD)g_view_seq;
	p.active = viewer ? *(DWORD *)(viewer + 4) : 0;
	p.a = g_view_args[0];
	p.b = g_view_args[1];
	p.c = g_view_args[2];
	memcpy(p.name, g_view_name, sizeof p.name);
	net_send(PKT_VIEW, &p, (WORD)(sizeof p - sizeof(pkt_hdr)));
}

static void guest_on_view(const char *buf, int n)
{
	if (n != (int)sizeof(pkt_view))
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_view, buf, n);
	g_rx_view.name[63] = 0;
	g_rx_view_ok = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void guest_apply_view(void)
{
	static DWORD shown_seq;
	static int primed;
	if (!g_rx_view_ok)
		return;
	pkt_view p;
	EnterCriticalSection(&g_state_cs);
	memcpy(&p, &g_rx_view, sizeof p);
	LeaveCriticalSection(&g_state_cs);
	BYTE *viewer = *(BYTE **)0x6ab8f0, *g = game_obj();
	if (!viewer || !g || *(DWORD *)(g + 4) != 2)
		return;
	if (!primed) {   // joining while a picture is up: don't replay old ones
		shown_seq = p.seq;
		primed = 1;
	}
	if (p.active && p.seq != shown_seq && p.name[0]) {
		shown_seq = p.seq;
		((view_show_t)0x430350)(viewer, NULL, p.name, p.a, p.b, p.c);
		InterlockedIncrement(&g_views_shown);
		logf("view: guest shows \"%s\" (seq %lu)", p.name, p.seq);
	} else if (!p.active && *(DWORD *)(viewer + 4)) {
		((view_close_t)0x4301b0)(viewer, NULL);
		InterlockedIncrement(&g_views_closed);
		logf("view: guest closes the picture with the host");
	}
}

// ---- on-screen messages (pickup text, hints) on the guest's screen --------------------------
// FUN_0045bf40 (thiscall on the text box *(0x6ab8ec); text id, ms, 3 x display args, colour, 1 more)
// shows a line of text, e.g. the picked-up item's name (0x43d159). Same hook as the pictures
// (prologue 55 8b ec 6a ff 68 1b 71 62 00); the host keeps the last 8 calls with a sequence number and
// the guest replays the ones it missed, in order.
typedef DWORD(__fastcall *msg_show_t)(void *box, void *edx, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7);
static BYTE *g_msg_tramp;
static msg_args g_msg_ring[MSG_RING];
static volatile LONG g_msg_seq, g_msgs_replayed;
static pkt_msg g_rx_msg;
static volatile LONG g_rx_msg_ok;

static DWORD __fastcall hk_msg_show(void *box, void *edx, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7)
{
	if (g_role == 1) {
		memmove(&g_msg_ring[1], &g_msg_ring[0], (MSG_RING - 1) * sizeof(msg_args));
		DWORD v[7] = { a1, a2, a3, a4, a5, a6, a7 };
		memcpy(g_msg_ring[0].a, v, sizeof v);
		InterlockedIncrement(&g_msg_seq);
	}
	return ((msg_show_t)g_msg_tramp)(box, edx, a1, a2, a3, a4, a5, a6, a7);
}

// Jumps from the function's entry to hook, after copying its first len bytes to a trampoline.
static BYTE *hook_entry(BYTE *f, const BYTE *prologue, int len, void *hook, const char *what)
{
	if (memcmp(f, prologue, len)) {
		logf("hook: %s prologue differs; not hooked", what);
		return NULL;
	}
	BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t)
		return NULL;
	memcpy(t, f, len);
	t[len] = 0xe9;
	*(LONG *)(t + len + 1) = (LONG)((f + len) - (t + len + 5));
	DWORD old;
	VirtualProtect(f, len, PAGE_EXECUTE_READWRITE, &old);
	f[0] = 0xe9;
	*(LONG *)(f + 1) = (LONG)((BYTE *)hook - (f + 5));
	memset(f + 5, 0x90, len - 5);
	VirtualProtect(f, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), f, len);
	logf("hook: %s hooked", what);
	return t;
}

static void sync_install_msg_hook(void)
{
	static const BYTE prologue[10] = { 0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x1b, 0x71, 0x62, 0x00 };
	g_msg_tramp = hook_entry((BYTE *)0x45bf40, prologue, 10, (void *)hk_msg_show, "on-screen messages (host)");
}

static void host_send_msgs(void)
{
	pkt_msg p;
	p.seq = (DWORD)g_msg_seq;
	memcpy(p.m, g_msg_ring, sizeof p.m);
	net_send(PKT_MSG, &p, (WORD)(sizeof p - sizeof(pkt_hdr)));
}

static void guest_on_msg(const char *buf, int n)
{
	if (n != (int)sizeof(pkt_msg))
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_msg, buf, n);
	g_rx_msg_ok = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void guest_apply_msgs(void)
{
	static DWORD applied;
	static int primed;
	BYTE *box = *(BYTE **)0x6ab8ec, *g = game_obj();
	if (!g_rx_msg_ok || !box || !g || *(DWORD *)(g + 4) != 2)
		return;
	pkt_msg p;
	EnterCriticalSection(&g_state_cs);
	memcpy(&p, &g_rx_msg, sizeof p);
	LeaveCriticalSection(&g_state_cs);
	if (!primed) {
		applied = p.seq;
		primed = 1;
	}
	if (p.seq - applied > MSG_RING)
		applied = p.seq - MSG_RING;
	while (applied != p.seq) {
		applied++;
		const DWORD *a = p.m[p.seq - applied].a;
		((msg_show_t)0x45bf40)(box, NULL, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
		InterlockedIncrement(&g_msgs_replayed);
	}
}

// ---- joining: the host sends the save it loaded; the guest stores it as game0.sav in its own
// co-op save folder (coop_profile/remote, never the player's real cloud saves) and loads it. ----
static volatile LONG g_save_xfer, g_save_acked, g_save_sent_chunks;
static DWORD g_save_xfer_len;
static BYTE g_save_xfer_buf[65536];
static volatile LONG g_save_rx_xfer, g_save_rx_have, g_save_rx_total, g_save_rx_done;
static BYTE g_save_rx_buf[65536];

static void host_start_save_xfer(void)
{
	LONG n = g_last_save_len;
	if (n <= 0)
		return;
	memcpy(g_save_xfer_buf, g_last_save, n);
	g_save_xfer_len = (DWORD)n;
	g_save_acked = 0;
	InterlockedIncrement(&g_save_xfer);
	logf("sync: sending %s (%ld bytes) to the guest, transfer %ld", g_last_save_name, n, g_save_xfer);
}

// Go-back-N: each frame send up to 4 chunks from the guest's acknowledged offset.
static void host_pump_save(void)
{
	if (!g_save_xfer || (DWORD)g_save_acked >= g_save_xfer_len)
		return;
	DWORD off = (DWORD)g_save_acked;
	for (int k = 0; k < 4 && off < g_save_xfer_len; k++) {
		pkt_save c;
		c.xfer = (DWORD)g_save_xfer;
		c.total = g_save_xfer_len;
		c.offset = off;
		c.len = (WORD)(g_save_xfer_len - off < SAVE_CHUNK ? g_save_xfer_len - off : SAVE_CHUNK);
		memcpy(c.data, g_save_xfer_buf + off, c.len);
		net_send(PKT_SAVE, &c, (WORD)(14 + c.len));
		InterlockedIncrement(&g_save_sent_chunks);
		off += c.len;
	}
}

static void host_on_save_ack(const char *buf, int n)
{
	const pkt_save_ack *a = (const pkt_save_ack *)buf;
	if (n != sizeof *a || (LONG)a->xfer != g_save_xfer)
		return;
	LONG before = g_save_acked;
	if ((LONG)a->have > g_save_acked)
		g_save_acked = (LONG)a->have;
	if ((DWORD)before < g_save_xfer_len && (DWORD)g_save_acked >= g_save_xfer_len)   // once per transfer
		logf("sync: guest has the whole save (transfer %ld)", g_save_xfer);
}

static void guest_on_save(const char *buf, int n)
{
	const pkt_save *c = (const pkt_save *)buf;
	if (n < 14 + (int)sizeof(pkt_hdr) || c->total > sizeof g_save_rx_buf || c->len > SAVE_CHUNK ||
		n != (int)(sizeof(pkt_hdr) + 14 + c->len) || c->offset + c->len > c->total)
		return;
	if ((LONG)c->xfer != g_save_rx_xfer) {   // a new transfer starts over
		g_save_rx_xfer = (LONG)c->xfer;
		g_save_rx_have = 0;
		g_save_rx_total = (LONG)c->total;
		g_save_rx_done = 0;
	}
	if ((LONG)c->offset == g_save_rx_have) {
		memcpy(g_save_rx_buf + c->offset, c->data, c->len);
		g_save_rx_have += c->len;
	}
	pkt_save_ack a;
	a.xfer = (DWORD)g_save_rx_xfer;
	a.have = (DWORD)g_save_rx_have;
	net_send(PKT_SAVE_ACK, &a, sizeof a - sizeof(pkt_hdr));
	if (g_save_rx_have == g_save_rx_total && !g_save_rx_done) {
		char path[MAX_PATH * 2];
		real_CreateDirectoryA(g_profile, NULL);
		real_CreateDirectoryA(g_remote, NULL);
		remote_path("game0.sav", path, sizeof path);
		HANDLE h = real_CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		DWORD w = 0;
		if (h != INVALID_HANDLE_VALUE) {
			WriteFile(h, g_save_rx_buf, (DWORD)g_save_rx_total, &w, NULL);
			CloseHandle(h);
		}
		g_save_rx_done = 1;
		logf("sync: received the host's save (%ld bytes) -> co-op slot game0.sav (%s)", g_save_rx_total,
			w == (DWORD)g_save_rx_total ? "written" : "WRITE FAILED");
	}
}

// ---- loading a save: the host's load reloads the guest too ----------------------------------
// The game's only load path (load menu node, also behind the game-over screen, FUN_005330c0 state 4):
// FUN_004524d0(slot) (thiscall on the save manager *(game+0xa20)) reads and checks the slot file into
// the manager's buffer, then game+0x838 = slot and game+8 = 3. The main loop (FUN_00416900) sees 3,
// tears down the menu or level and builds the saved game with FUN_00416180(1) (no re-read).
// Host: on game+8 == 3 it sends PKT_LOAD naming the save transfer of that file. Guest: once that
// transfer is in its co-op slot game0.sav, it does the same two steps for slot 0.
typedef struct { pkt_hdr h; DWORD seq, xfer; } pkt_load;
enum { PKT_LOAD = 18 };
typedef DWORD(__fastcall *save_read_t)(void *mgr, void *edx, DWORD slot);
static DWORD g_load_seq, g_load_sent_until;
static pkt_load g_load_pkt;
static volatile LONG g_load_seen, g_loads_sent;
static volatile LONG g_rx_load_seq, g_rx_load_xfer, g_rx_load_new, g_loads_applied, g_loads_failed;
static volatile LONG g_load_cmd = -1;   // pipe "loadslot" (host tests): the load menu's two steps

static void game_load_slot(DWORD slot)
{
	BYTE *g = game_obj();
	BYTE *mgr = rd_ptr(g, 0xa20);
	if (!mgr)
		return;
	if (!((save_read_t)0x4524d0)(mgr, NULL, slot)) {
		InterlockedIncrement(&g_loads_failed);
		logf("load: slot %lu not loadable (save manager error %lu)", slot, *(DWORD *)(mgr + 0x3a0));
		return;
	}
	*(DWORD *)(g + 0x838) = slot;
	*(DWORD *)(g + 8) = 3;
	logf("load: slot %lu read, game loads it", slot);
}

// Host, every frame: notice a load (before the main loop consumes game+8 == 3 next frame).
static void host_watch_load(void)
{
	static DWORD prev;
	BYTE *g = game_obj();
	DWORD st = g ? *(DWORD *)(g + 8) : 0;
	if (st == 3 && prev != 3) {
		g_load_seen = 1;
		logf("load: host loads slot %ld", *(LONG *)(g + 0x838));
	}
	prev = st;
}

// Host, while connected: once the loaded file's transfer has started, send PKT_LOAD for 3 s.
static void host_send_load(LONG sent_ver)
{
	if (g_load_seen && g_last_save_len && sent_ver == g_last_save_ver) {
		g_load_seen = 0;
		g_load_pkt.seq = ++g_load_seq;
		g_load_pkt.xfer = (DWORD)g_save_xfer;
		g_load_sent_until = GetTickCount() + 3000;
		InterlockedIncrement(&g_loads_sent);
		logf("load: telling the guest to load (seq %lu, save transfer %lu)", g_load_pkt.seq, g_load_pkt.xfer);
	}
	if (g_load_sent_until && GetTickCount() < g_load_sent_until && (g_frames % 4) == 0) {
		pkt_load copy = g_load_pkt;
		net_send(PKT_LOAD, &copy, sizeof copy - sizeof(pkt_hdr));
	}
}

static void guest_on_load(const char *buf, int n)
{
	const pkt_load *l = (const pkt_load *)buf;
	if (n != sizeof *l || (LONG)(l->seq - (DWORD)g_rx_load_seq) <= 0)
		return;
	g_rx_load_xfer = (LONG)l->xfer;
	g_rx_load_seq = (LONG)l->seq;
	g_rx_load_new = 1;
	logf("load: host loaded a save (seq %lu, waiting for save transfer %lu)", l->seq, l->xfer);
}

// Guest, main thread: load once the save is on disk and the game is not mid-transition. A later
// transfer of the same load (the game reads the file twice) also counts.
static void guest_apply_load(void)
{
	BYTE *g = game_obj();
	if (!g_rx_load_new || !g || *(DWORD *)(g + 8) != 0)
		return;
	if (!g_save_rx_done || g_save_rx_xfer < g_rx_load_xfer)
		return;
	g_rx_load_new = 0;
	InterlockedIncrement(&g_loads_applied);
	logf("load: guest loads the host's save (transfer %ld)", g_save_rx_xfer);
	game_load_slot(0);
}

// Host: story flags (161 bytes at *(game+0xa24)) every 15 frames; the host owns them.
static void host_send_flags(void)
{
	BYTE *g = game_obj();
	BYTE *f = rd_ptr(g, 0xa24);
	if (!f)
		return;
	pkt_flags p;
	p.frame = g_sync_frame;
	p.level = (BYTE)level_id();
	memcpy(p.flags, f, sizeof p.flags);
	net_send(PKT_FLAGS, &p, sizeof p - sizeof(pkt_hdr));
}

// Host: notice a level change request (game+8 == 5) and send it for 2 s (UDP may drop some).
static void host_watch_level(void)
{
	BYTE *g = game_obj();
	if (!g)
		return;
	if (*(DWORD *)(g + 8) == 5 && (g_level_sent_until == 0 || strcmp(g_level_pkt.name, (char *)g + 0x590) ||
		g_level_pkt.entry != *(DWORD *)(g + 0x5d0) || GetTickCount() > g_level_sent_until + 1000)) {
		g_level_pkt.seq = ++g_level_seq;
		g_level_pkt.entry = *(DWORD *)(g + 0x5d0);
		strncpy(g_level_pkt.name, (char *)g + 0x590, sizeof g_level_pkt.name - 1);
		g_level_pkt.name[sizeof g_level_pkt.name - 1] = 0;
		g_level_sent_until = GetTickCount() + 2000;
		logf("sync: host level change to %s entry %lu (seq %lu)", g_level_pkt.name, g_level_pkt.entry, g_level_pkt.seq);
	}
	if (g_level_sent_until && GetTickCount() < g_level_sent_until && (g_frames % 4) == 0) {
		pkt_level copy = g_level_pkt;
		net_send(PKT_LEVEL, &copy, sizeof copy - sizeof(pkt_hdr));
	}
}

static void guest_on_level(const char *buf, int n)
{
	const pkt_level *l = (const pkt_level *)buf;
	if (n != sizeof *l || (LONG)(l->seq - (DWORD)g_rx_level_seq) <= 0)
		return;
	EnterCriticalSection(&g_state_cs);
	g_rx_level = *l;
	g_rx_level.name[sizeof g_rx_level.name - 1] = 0;
	g_rx_level_seq = (LONG)l->seq;
	g_rx_level_new = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void guest_on_flags(const char *buf, int n)
{
	if (n != sizeof(pkt_flags))
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_flags, buf, sizeof g_rx_flags);
	g_rx_flags_new = 1;
	LeaveCriticalSection(&g_state_cs);
}

// Guest, main thread: follow the host's level change; copy the host's story flags (same level only).
static void guest_apply_level_and_flags(void)
{
	if (g_rx_level_new) {
		pkt_level l;
		EnterCriticalSection(&g_state_cs);
		l = g_rx_level;
		g_rx_level_new = 0;
		LeaveCriticalSection(&g_state_cs);
		request_level(l.name, l.entry);
		InterlockedIncrement(&g_level_applied);
		logf("sync: guest follows host to %s entry %lu (seq %lu)", l.name, l.entry, l.seq);
	}
	if (g_rx_flags_new) {
		pkt_flags f;
		EnterCriticalSection(&g_state_cs);
		f = g_rx_flags;
		g_rx_flags_new = 0;
		LeaveCriticalSection(&g_state_cs);
		BYTE *dst = rd_ptr(game_obj(), 0xa24);
		if (dst && f.level == (BYTE)level_id()) {
			memcpy(dst, f.flags, sizeof f.flags);
			InterlockedIncrement(&g_flags_applied);
		}
	}
}

// Guest: its own scripts may not change level; only the host decides. TM_LoadLevel and
// TM_EventChangeLevel bodies (vtable slot 3 at 0x649660 and 0x64a960) become no-ops that succeed.
static DWORD __fastcall guest_level_native_blocked(void *self, void *edx)
{
	(void)self; (void)edx;
	InterlockedIncrement(&g_level_blocked);
	logf("sync: guest script level change blocked (the host decides)");
	return 1;
}

// Guest: a state packet arrived (network thread). Keep the newest, apply on the main thread.
// ---- smoothing (1.1) -------------------------------------------------------------------------
// Over the internet state packets arrive unevenly (owner's playtest: RTT 62..78 ms, spikes to 500 ms),
// and snapping each character to the newest packet made remote movement stutter. The guest keeps the
// last SNAPS snapshots with the host's send time and shows the world at "host time - delay", blending
// position and facing between the two snapshots around that time. The delay follows the measured
// jitter: the 90th percentile of how late packets arrive versus the fastest one in the last ~2 s, plus
// one send interval, kept between 20 and 200 ms (OBSCOOP_SMOOTH_MS fixes it; 0 = no smoothing).
#define SNAPS 32
#define JIT_N 128
static pkt_state g_snap[SNAPS];
static double g_snap_t[SNAPS];       // host send time, unwrapped ms
static int g_snap_count, g_snap_head;   // ring: newest at g_snap_head - 1
static double g_jit_off[JIT_N];      // recv - send per packet (includes the clock offset)
static int g_jit_n, g_jit_i;
static double g_last_host_t;
static DWORD g_last_wire_t;
static double g_render_off;          // smoothed (clock offset + delay); local time - this = shown host time
static volatile LONG g_smooth_ms = -1, g_delay_now, g_extrapolated, g_snapped_jumps;
static double g_last_rt;   // host time shown this frame (0 = not smoothing yet)

static void guest_on_state(const char *buf, int n)
{
	const pkt_state *s = (const pkt_state *)buf;
	if (n < (int)(sizeof(pkt_hdr) + STATE_HDR) || s->count > MAX_CHARS ||
		n != (int)(sizeof(pkt_hdr) + STATE_HDR + s->count * sizeof(char_state))) {
		InterlockedIncrement(&g_rx_state_bad);
		return;
	}
	InterlockedIncrement(&g_rx_state_ok);
	double now = clock_ms();
	EnterCriticalSection(&g_state_cs);
	if ((LONG)(s->frame - g_rx_state.frame) > 0 || !g_rx_state.frame) {
		double ht = g_snap_count ? g_last_host_t + (double)(LONG)(s->t_ms - g_last_wire_t) : (double)s->t_ms;
		g_last_host_t = ht;
		g_last_wire_t = s->t_ms;
		memcpy(&g_rx_state, s, n);
		memcpy(&g_snap[g_snap_head], s, n);
		g_snap_t[g_snap_head] = ht;
		g_snap_head = (g_snap_head + 1) % SNAPS;
		if (g_snap_count < SNAPS)
			g_snap_count++;
		g_jit_off[g_jit_i] = now - ht;
		g_jit_i = (g_jit_i + 1) % JIT_N;
		if (g_jit_n < JIT_N)
			g_jit_n++;
	}
	g_rx_state_new = 1;
	LeaveCriticalSection(&g_state_cs);
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static const char_state *snap_char(const pkt_state *s, DWORD id)
{
	for (int i = 0; i < s->count; i++)
		if (s->c[i].id == id)
			return &s->c[i];
	return NULL;
}

// Fills *out with the state to show this frame. Main thread; returns 0 when nothing arrived yet.
static int guest_smoothed_state(pkt_state *out)
{
	if (g_smooth_ms < 0) {
		char v[16];
		g_smooth_ms = GetEnvironmentVariableA("OBSCOOP_SMOOTH_MS", v, sizeof v) ? atoi(v) : 1000;   // 1000 = adaptive
		logf("smooth: %s", g_smooth_ms == 1000 ? "adaptive delay" : g_smooth_ms == 0 ? "off" : "fixed delay");
	}
	EnterCriticalSection(&g_state_cs);
	if (!g_snap_count) {
		LeaveCriticalSection(&g_state_cs);
		return 0;
	}
	int newest = (g_snap_head + SNAPS - 1) % SNAPS;
	if (g_smooth_ms == 0 || g_snap_count < 2) {
		memcpy(out, &g_snap[newest], sizeof *out);
		LeaveCriticalSection(&g_state_cs);
		return 1;
	}
	// delay from the jitter of the last JIT_N packets and the send interval of the last snapshots
	double offs[JIT_N];
	int jn = g_jit_n;
	memcpy(offs, g_jit_off, sizeof(double) * jn);
	qsort(offs, jn, sizeof(double), cmp_double);
	double base = offs[0], jitter = offs[(jn * 9) / 10] - base;
	int k = g_snap_count < 9 ? g_snap_count : 9;
	double interval = (g_snap_t[newest] - g_snap_t[(newest + SNAPS - (k - 1)) % SNAPS]) / (k - 1);
	double delay = g_smooth_ms != 1000 ? g_smooth_ms : jitter + interval + 4.0;
	if (delay < 20.0)
		delay = 20.0;
	if (delay > 200.0)
		delay = 200.0;
	double target = base + delay, now = clock_ms();
	if (g_render_off == 0.0 || __builtin_fabs(target - g_render_off) > 250.0)
		g_render_off = target;   // first time, or the host restarted its clock
	else
		g_render_off += (target - g_render_off) * 0.02;   // drift slowly: no visible jumps
	g_delay_now = (LONG)(g_render_off - base);
	static double last_log;
	if (now - last_log > 30000.0) {
		last_log = now;
		logf("smooth: delay %ld ms (jitter p90 %.0f ms, send interval %.1f ms), extrapolated frames %ld, teleports %ld",
			g_delay_now, jitter, interval, g_extrapolated, g_snapped_jumps);
	}
	double rt = now - g_render_off;
	g_last_rt = rt;
	// the two snapshots around rt (a <= rt < b), walking from the newest back
	int a = -1, b = -1;
	for (int i = 0; i < g_snap_count; i++) {
		int idx = (g_snap_head + SNAPS - 1 - i) % SNAPS;
		if (g_snap_t[idx] <= rt) {
			a = idx;
			break;
		}
		b = idx;
	}
	if (a < 0)   // older than everything kept: show the oldest
		a = b, b = -1;
	memcpy(out, &g_snap[b >= 0 ? (rt - g_snap_t[a] > g_snap_t[b] - rt ? b : a) : a], sizeof *out);
	const pkt_state *sa = &g_snap[a], *sb = b >= 0 ? &g_snap[b] : NULL;
	double t;
	const pkt_state *s0 = sa, *s1 = sb;
	if (sb) {
		t = (rt - g_snap_t[a]) / (g_snap_t[b] - g_snap_t[a]);
	} else {   // past the newest: continue its motion for up to 50 ms
		int prev = (a + SNAPS - 1) % SNAPS;
		double dt = g_snap_t[a] - g_snap_t[prev], ahead = rt - g_snap_t[a];
		if (g_snap_count < 2 || dt <= 0.0 || ahead <= 0.0) {
			LeaveCriticalSection(&g_state_cs);
			return 1;
		}
		if (ahead > 50.0)
			ahead = 50.0;
		s0 = &g_snap[prev];
		s1 = sa;
		t = 1.0 + ahead / dt;
		InterlockedIncrement(&g_extrapolated);
	}
	if (s0->level == s1->level) {
		for (int i = 0; i < out->count; i++) {
			char_state *c = &out->c[i];
			const char_state *p0 = snap_char(s0, c->id), *p1 = snap_char(s1, c->id);
			if (!p0 || !p1)
				continue;
			float d = __builtin_fabsf(p1->pos[0] - p0->pos[0]) + __builtin_fabsf(p1->pos[1] - p0->pos[1]) + __builtin_fabsf(p1->pos[2] - p0->pos[2]);
			if (d > 150.0f) {   // a teleport (door, script): no sliding across the room
				memcpy(c->pos, s1 == sa || t >= 0.5 ? p1->pos : p0->pos, 12);
				InterlockedIncrement(&g_snapped_jumps);
				continue;
			}
			for (int j = 0; j < 3; j++)
				c->pos[j] = (float)(p0->pos[j] + (p1->pos[j] - p0->pos[j]) * t);
			float dir[3], len = 0.0f;
			for (int j = 0; j < 3; j++) {
				dir[j] = (float)(p0->dir[j] + (p1->dir[j] - p0->dir[j]) * (t > 1.0 ? 1.0 : t));
				len += dir[j] * dir[j];
			}
			if (len > 1e-6f) {
				len = __builtin_sqrtf(len);
				for (int j = 0; j < 3; j++)
					c->dir[j] = dir[j] / len;
			}
		}
	}
	LeaveCriticalSection(&g_state_cs);
	return 1;
}

// Enemies at the same host time as the characters (g_last_rt), position and facing blended between
// the two packets around it, matched by list index and vtable. Main thread.
static int guest_smoothed_pnj(pkt_pnj *out)
{
	EnterCriticalSection(&g_state_cs);
	if (!g_pnj_count) {
		LeaveCriticalSection(&g_state_cs);
		return 0;
	}
	int newest = (g_pnj_head + PNJ_SNAPS - 1) % PNJ_SNAPS;
	double rt = g_last_rt;
	if (g_smooth_ms == 0 || rt == 0.0 || g_pnj_count < 2) {
		memcpy(out, &g_pnj_snap[newest], sizeof *out);
		LeaveCriticalSection(&g_state_cs);
		return 1;
	}
	int a = -1, b = -1;
	for (int i = 0; i < g_pnj_count; i++) {
		int idx = (g_pnj_head + PNJ_SNAPS - 1 - i) % PNJ_SNAPS;
		if (g_pnj_t[idx] <= rt) {
			a = idx;
			break;
		}
		b = idx;
	}
	if (a < 0) {   // older than everything kept
		memcpy(out, &g_pnj_snap[b], sizeof *out);
		LeaveCriticalSection(&g_state_cs);
		return 1;
	}
	const pkt_pnj *s0, *s1;
	double t;
	if (b >= 0) {
		s0 = &g_pnj_snap[a];
		s1 = &g_pnj_snap[b];
		t = (rt - g_pnj_t[a]) / (g_pnj_t[b] - g_pnj_t[a]);
	} else {   // past the newest: continue for up to 50 ms
		int prev = (a + PNJ_SNAPS - 1) % PNJ_SNAPS;
		double dt = g_pnj_t[a] - g_pnj_t[prev], ahead = rt - g_pnj_t[a];
		s0 = &g_pnj_snap[prev];
		s1 = &g_pnj_snap[a];
		t = dt > 0.0 ? 1.0 + (ahead > 50.0 ? 50.0 : ahead) / dt : 1.0;
	}
	memcpy(out, t >= 0.5 ? s1 : s0, sizeof *out);
	if (s0->level == s1->level && s0->count == s1->count) {
		for (int i = 0; i < out->count; i++) {
			const pnj_state *p0 = &s0->e[i], *p1 = &s1->e[i];
			if (p0->vtable != p1->vtable || p1->dead58 || p1->dead5c)
				continue;
			float d = __builtin_fabsf(p1->pos[0] - p0->pos[0]) + __builtin_fabsf(p1->pos[1] - p0->pos[1]) +
				__builtin_fabsf(p1->pos[2] - p0->pos[2]);
			if (d > 150.0f)   // a teleport: no sliding
				continue;
			pnj_state *o = &out->e[i];
			float len = 0.0f;
			for (int j = 0; j < 3; j++) {
				o->pos[j] = (float)(p0->pos[j] + (p1->pos[j] - p0->pos[j]) * t);
				o->dir[j] = (float)(p0->dir[j] + (p1->dir[j] - p0->dir[j]) * (t > 1.0 ? 1.0 : t));
				len += o->dir[j] * o->dir[j];
			}
			if (len > 1e-6f) {
				len = __builtin_sqrtf(len);
				for (int j = 0; j < 3; j++)
					o->dir[j] /= len;
			} else {
				memcpy(o->dir, p1->dir, 12);
			}
		}
	}
	LeaveCriticalSection(&g_state_cs);
	return 1;
}

static void put_char(BYTE *c, const char_state *st)
{
	void **vt = *(void ***)c;
	float pos[3], dir[3];
	memcpy(pos, st->pos, 12);
	memcpy(dir, st->dir, 12);
	((set_vec_t)vt[2])(c, NULL, pos);
	BYTE *body = rd_ptr(c, 0x68);
	if (!*(DWORD *)(c + 0x6c) && body) {
		((set_vec_t)(*(void ***)body)[2])(body, NULL, pos);
		BYTE *sub = rd_ptr(body, 0x94);
		if (sub)
			((body_update_t)(*(void ***)sub)[2])(sub, NULL);
	}
	if (dir[0] != 0.0f || dir[1] != 0.0f)
		((set_vec_t)vt[3])(c, NULL, dir);
}

static void guest_apply_state(void)
{
	pkt_state s;   // every frame: the smoothed state (puppets are put there after their own update)
	if (!guest_smoothed_state(&s))
		return;
	g_rx_state_new = 0;
	BYTE *g = game_obj();
	BYTE *chars[MAX_CHARS];
	int n = list_chars(chars, MAX_CHARS);
	g_puppets = 0;
	if (!g || !n || s.level != (BYTE)level_id() || *(DWORD *)(g + 8) == 5) {   // not in the host's level (yet): mirror nothing
		InterlockedIncrement(!g || !n ? &g_skip_nochars : s.level != (BYTE)level_id() ? &g_skip_level : &g_skip_change);
		return;
	}
	for (int i = 0; i < s.count; i++) {
		BYTE *mine = NULL;
		for (int j = 0; j < n; j++)
			if (*(DWORD *)(chars[j] + 0x78) == s.c[i].id)
				mine = chars[j];
		if (!mine) {
			InterlockedIncrement(&g_apply_missing);
			continue;
		}
		if (s.c[i].flags & 2)
			g_guest_char_id = s.c[i].id;
		put_char(mine, &s.c[i]);
		sync_life(mine, s.c[i].hp, 0, 0);
		g_puppet[i].c = mine;
		g_puppet[i].st = s.c[i];
	}
	g_puppets = s.count;
	InterlockedIncrement(&g_applied);
	// Our player 1 controls (and the camera follows) the character the guest plays on the host.
	// Retried every 60 frames, since the game refuses while a scripted scene holds the lock.
	BYTE *rp0 = rd_ptr(g, 0x4fc);
	if (g_guest_char_id != 0xffffffff && rp0 && (g_frames % 60) == 0) {
		BYTE *cur = rd_ptr(rp0, 0xc);
		if (!cur || *(DWORD *)(cur + 0x78) != g_guest_char_id) {
			for (int j = 0; j < n; j++)
				if (*(DWORD *)(chars[j] + 0x78) == g_guest_char_id) {
					// never onto a dead character: it has no body (+0x68) and FUN_0044b7e0 reads it
					if (*(DWORD *)(chars[j] + 0x58) || *(DWORD *)(chars[j] + 0x5c) || !rd_ptr(chars[j], 0x68)) {
						static DWORD warned;
						if (warned != g_guest_char_id)
							logf("sync: not switching to character %lu: it is dead", g_guest_char_id);
						warned = g_guest_char_id;
						continue;
					}
					((control_char_t)0x44b7e0)(rp0, NULL, chars[j]);
					LONG t = InterlockedIncrement(&g_guest_switch_tries);
					cur = rd_ptr(rp0, 0xc);
					logf("sync: switch to character id %lu (try %ld) -> now %lu", g_guest_char_id, t,
						cur ? *(DWORD *)(cur + 0x78) : 0);
				}
		}
	}
}

// CPlayerHuman::Update hook (guest): a mirrored character runs the game's own update with the
// host's move intent, so its state machine and animation follow the host; then it is put back
// at the host's position.
typedef void(__fastcall *update_t)(void *self, void *edx, DWORD dt);
static update_t real_human_update;

// Requested states a mirrored character may run on the guest. Interaction states need local context
// (state 50 = use a door: without the door the guest's FUN_00436bd0 never returns; seen 2026-10-24).
// Anything not listed is not passed on (the puppet keeps standing) and logged once.
// Door use (requested state 50/52) needs character +0x224 = the door (states 0x32/0x34 read it; run 57
// hung without it). The host sends which door; the guest points its copy at its own copy of that door.
static volatile LONG g_door_level_swallowed, g_door_uses;
static BYTE g_door_done[EV_IDS];   // guest: this copy entered the host's current door state
static int guest_door_context(BYTE *c, const char_state *st)
{
	if (!st->door_type && !st->door_id)
		return 0;
	BYTE *it[256];
	int n = list_entities(0x30, it, 256);
	for (int i = 0; i < n; i++)
		if (*(DWORD *)(it[i] + 4) == st->door_type && *(DWORD *)(it[i] + 8) == st->door_id) {
			if (*(BYTE **)(c + 0x224) != it[i]) {
				*(BYTE **)(c + 0x224) = it[i];
				InterlockedIncrement(&g_door_uses);
				logf("sync: character %lu uses door type %lu id %lu (mirrored)", st->id, st->door_type, st->door_id);
			}
			return 1;
		}
	return 0;
}

static int puppet_state_ok(DWORD st)
{
	switch (st) {
	case 10: case 11: case 20: case 30: case 31:   // stand, idle variant, walk, run, run of character 3
	case 100: case 101: case 103:                  // combat: action, aim, swing
		return 1;
	}
	static DWORD logged[64];
	for (int i = 0; i < 64; i++) {
		if (logged[i] == st)
			return 0;
		if (!logged[i]) {
			logged[i] = st;
			logf("sync: requested state %lu not mirrored (not on the safe list)", st);
			return 0;
		}
	}
	return 0;
}

static void host_events_for(DWORD id, struct char_state_s *st)
{
	if (id >= EV_IDS) {
		st->ev_seq = 0;
		return;
	}
	st->ev_seq = g_ev_host[id].seq;
	memcpy(st->ev_val, g_ev_host[id].val, sizeof st->ev_val);
}

static void __fastcall hk_human_update(void *self, void *edx, DWORD dt)
{
	char_state *st = NULL;
	if (g_role == 1) {   // host: see each requested state before the game consumes it
		host_record_wanted(*(DWORD *)((BYTE *)self + 0x78), *(DWORD *)((BYTE *)self + 0x230));
		real_human_update(self, edx, dt);
		return;
	}
	if (g_role == 2)
		for (int i = 0; i < g_puppets; i++)
			if (g_puppet[i].c == self)
				st = &g_puppet[i].st;
	if (st) {
		memcpy((BYTE *)self + 0x238, st->intent, sizeof st->intent);
		*(DWORD *)((BYTE *)self + 0xd8) = st->moving;
		DWORD wanted = st->wanted;
		DWORD id = st->id;
		if (id < EV_IDS) {
			if (!g_ev_guest[id].primed) {   // first snapshot: start from now, do not replay history
				g_ev_guest[id].applied = st->ev_seq;
				g_ev_guest[id].primed = 1;
			} else if (st->ev_seq != g_ev_guest[id].applied) {
				DWORD next = g_ev_guest[id].applied + 1;
				if (st->ev_seq - next >= 4)   // more than 4 behind: skip to the oldest still known
					next = st->ev_seq - 3;
				wanted = st->ev_val[st->ev_seq - next];
				g_ev_guest[id].applied = next;
				InterlockedIncrement(&g_events_replayed);
			}
		}
		// a one-frame door request is lost if this copy isn't idle that frame: keep asking while the
		// host's character is in the door state and this copy is not
		// Only until this copy has been in that state once for this door use: after the door state ends
		// the copy asks for a level change (swallowed below), and forcing it again every frame looped
		// for minutes (1.0 playtest: 41,509 swallowed requests in 2 min 15 s, a character missing on screen).
		DWORD cur = *(DWORD *)((BYTE *)self + 0x60);
		if (id < EV_IDS) {
			if (st->state != 50 && st->state != 52)
				g_door_done[id] = 0;
			else if (cur == st->state)
				g_door_done[id] = 1;
		}
		if ((st->state == 50 || st->state == 52) && cur != st->state && !(id < EV_IDS && g_door_done[id]))
			wanted = st->state;
		if ((wanted == 50 || wanted == 52) && guest_door_context((BYTE *)self, st))
			;   // door use: the guest's copy of the door is the context, see guest_door_context
		else if (!puppet_state_ok(wanted))
			wanted = 10;
		*(DWORD *)((BYTE *)self + 0x230) = wanted;   // requested state; FUN_0043bf60 switches on it
	}
	BYTE *g = game_obj();
	DWORD req = g ? *(DWORD *)(g + 8) : 0;
	real_human_update(self, edx, dt);
	if (st && g && req != 5 && *(DWORD *)(g + 8) == 5) {
		// a puppet's door state ended and asked for a level change (FUN_0043bf60 case 0x32/0x34);
		// only the host changes levels, the guest follows its LEVEL packet
		*(DWORD *)(g + 8) = req;
		LONG k = InterlockedIncrement(&g_door_level_swallowed);
		static DWORD last_log;
		if (GetTickCount() - last_log > 5000) {
			last_log = GetTickCount();
			logf("sync: level change from character %lu's door state ignored (the host leads; %ld so far)",
				*(DWORD *)((BYTE *)self + 0x78), k);
		}
	}
	if (st) {
		put_char((BYTE *)self, st);
		InterlockedIncrement(&g_state_sets);
	}
}

static void sync_install_update(void)
{
	void **slot = (void **)(0x6449e4 + 4);
	if (*slot != (void *)0x43f6e0) {
		logf("sync: CPlayerHuman::Update slot holds %p, not 0x43f6e0; not hooking", *slot);
		return;
	}
	DWORD old;
	real_human_update = (update_t)*slot;
	VirtualProtect(slot, 4, PAGE_READWRITE, &old);
	*slot = (void *)hk_human_update;
	VirtualProtect(slot, 4, old, &old);
	logf("sync: CPlayerHuman::Update hooked (host: requested-state events)");
}

static void sync_install_inv_ui(void);
static void sync_install(void)
{
	sync_install_inv_ui();
	void **slot = (void **)(0x6449e4 + 4);
	if (*slot != (void *)0x43f6e0) {
		logf("sync: CPlayerHuman::Update slot holds %p, not 0x43f6e0; not hooking", *slot);
		return;
	}
	DWORD old;
	real_human_update = (update_t)*slot;
	VirtualProtect(slot, 4, PAGE_READWRITE, &old);
	*slot = (void *)hk_human_update;
	VirtualProtect(slot, 4, old, &old);
	logf("sync: CPlayerHuman::Update hooked");
	void **natives[2] = { (void **)(0x649660 + 12), (void **)(0x64a960 + 12) };
	void *expect[2] = { (void *)0x4ddb80, (void *)0x4d92a0 };
	for (int i = 0; i < 2; i++) {
		if (*natives[i] != expect[i]) {
			logf("sync: level native %d slot holds %p, not %p; not blocking", i, *natives[i], expect[i]);
			continue;
		}
		VirtualProtect(natives[i], 4, PAGE_READWRITE, &old);
		*natives[i] = (void *)guest_level_native_blocked;
		VirtualProtect(natives[i], 4, old, &old);
	}
	logf("sync: guest level-change natives blocked");
}

// Window title: the co-op status, so a player can see at a glance that the mod runs and what it does.
static BOOL CALLBACK find_own_window(HWND h, LPARAM out)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
		*(HWND *)out = h;
		return FALSE;
	}
	return TRUE;
}

static void update_title(void)
{
	static DWORD last;
	static char shown[256];
	static HWND wnd;
	if (GetTickCount() - last < 1000)
		return;
	last = GetTickCount();
	if (!wnd || !IsWindow(wnd))
		EnumWindows(find_own_window, (LPARAM)&wnd);
	if (!wnd)
		return;
	char line[200], title[256];
	net_status_line(line, sizeof line);
	BYTE *gobj = game_obj();
	if (g_role == 2 && g_save_rx_done && (!gobj || *(DWORD *)(gobj + 4) != 2))   // not in a level yet
		_snprintf(line, sizeof line, "co-op guest: host save received - loading (if not: LOAD > 01)");
	_snprintf(title, sizeof title, "Obscure - %s", line);
	title[sizeof title - 1] = 0;
	if (strcmp(title, shown)) {
		SetWindowTextA(wnd, title);
		strcpy(shown, title);
		logf("status: %s", line);
	}
}

// ---- one items section per player ------------------------------------------------------
// The game creates the inventory's items section (CInventorySectionItems, 0x1a8 bytes, ctor
// FUN_00512890, vtable 0x650d6c) only once (flag DAT_006a6868 in the CInventory ctor FUN_0041f6a0),
// so in co-op there is one shared items bag: when player 2 opens it, FUN_0041fb60 takes player 1's
// section away. Here each real player's CInventory (real player +0x10) gets its own section, wired the
// way FUN_0041f8a0 wires player 1's: section +0x10 = CInventory, +0x14 = player index, +0x1c = 0, then
// vtable slot 1 (owner real player, screen position). Player 2's HUD row sits at z 480 (its weapon
// section's position), so its items go to (-390, -18, 480).
typedef void *(__cdecl *game_new_t)(size_t);
typedef void(__fastcall *ctor_t)(void *self, void *edx);
typedef void(__fastcall *section_place_t)(void *self, void *edx, void *owner, float *pos);
static int g_split_items = -1;
static LONG g_items_sections_made;

static void ensure_items_sections(void)
{
	if (g_split_items < 0) {
		char v[8];
		g_split_items = !(GetEnvironmentVariableA("OBSCOOP_SPLIT_ITEMS", v, sizeof v) && v[0] == '0');
		logf("items: one items section per player %s", g_split_items ? "on" : "off (shared, as the original)");
		// Closing player 2's items menu hands its section to player 1 (FUN_0041faf0, guarded by
		// 'jne 0x41fb3a' at 0x41fb1f). With one section each that hand-back must not happen: make it 'jmp'.
		BYTE *j = (BYTE *)0x41fb1f;
		if (g_split_items && j[0] == 0x75 && j[1] == 0x19) {
			DWORD old;
			VirtualProtect(j, 2, PAGE_EXECUTE_READWRITE, &old);
			j[0] = 0xEB;
			VirtualProtect(j, 2, old, &old);
			FlushInstructionCache(GetCurrentProcess(), j, 2);
			logf("items: player 2 no longer hands its items section to player 1 on close");
		} else if (g_split_items && j[0] != 0xEB) {
			logf("items: unexpected bytes at 0x41fb1f (%02x %02x); per-player items off", j[0], j[1]);
			g_split_items = 0;
		}
	}
	if (!g_split_items)
		return;
	BYTE *g = game_obj();
	if (!g || *(DWORD *)(g + 4) != 2)
		return;
	for (int i = 0; i < 2; i++) {
		BYTE *rp = rd_ptr(g, 0x4fc + 4 * i);
		BYTE *inv = rd_ptr(rp, 0x10);
		if (!inv || !rd_ptr(rp, 0xc) || rd_ptr(inv, 0x24))
			continue;
		if (*(DWORD *)(inv + 0x1c) != 3)   // only while that player's inventory is closed
			continue;
		BYTE *sec = ((game_new_t)0x5e3fda)(0x1a8);
		if (!sec)
			return;
		((ctor_t)0x512890)(sec, NULL);
		*(BYTE **)(sec + 0x10) = inv;
		*(DWORD *)(sec + 0x14) = *(DWORD *)(inv + 0xc);
		*(DWORD *)(sec + 0x1c) = 0;
		float pos[3] = { -390.0f, -18.0f, i == 0 ? 80.0f : 480.0f };
		((section_place_t)(*(void ***)sec)[1])(sec, NULL, rd_ptr(inv, 8), pos);
		*(BYTE **)(inv + 0x24) = sec;
		InterlockedIncrement(&g_items_sections_made);
		logf("items: player %d got its own items section %p", i + 1, sec);
	}
}

// ---- watchdog ------------------------------------------------------------------------------
// If the main loop stops for 5 s, log where the main thread is (EIP and the return addresses on its
// stack that fall in the exe) twice, 1 s apart, plus each character's state, so a freeze on a
// friend's PC leaves a trace in obscoop.log. Suspends only our own main thread; no debugger.
static volatile DWORD g_main_tid;
static volatile LONG g_stall_ms;   // pipe "stall" (tests the watchdog)
static char g_view_cmd[64];        // pipe "view" (tests the picture mirror)
static volatile LONG g_view_cmd_set;
static volatile LONG g_msg_cmd_id, g_msg_cmd_ms;   // pipe "msg" (tests the message mirror)

static void watchdog_sample(int n)
{
	HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
	if (!t)
		return;
	CONTEXT ctx;
	DWORD stack[512], ret[24];
	int k = 0;
	memset(&ctx, 0, sizeof ctx);
	ctx.ContextFlags = CONTEXT_CONTROL;
	if (SuspendThread(t) != (DWORD)-1) {
		if (GetThreadContext(t, &ctx)) {
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), (void *)ctx.Esp, stack, sizeof stack, &got);
			for (SIZE_T i = 0; i < got / 4 && k < 24; i++)
				if (stack[i] >= 0x401000 && stack[i] < 0x640000)
					ret[k++] = stack[i];
		}
		ResumeThread(t);
	}
	CloseHandle(t);
	char line[400];
	int m = _snprintf(line, sizeof line, "watchdog %d: main thread stalled, eip %#lx, exe addresses on stack:", n, ctx.Eip);
	for (int i = 0; i < k && m > 0 && m < (int)sizeof line - 12; i++)
		m += _snprintf(line + m, sizeof line - m, " %#lx", ret[i]);
	logf("%s", line);
}

static DWORD WINAPI watchdog(LPVOID arg)
{
	(void)arg;
	LONG last = -1;
	int still = 0, reported = 0;
	for (;;) {
		Sleep(1000);
		LONG f = g_frames;
		if (f != last || !g_main_tid) {
			if (reported)
				logf("watchdog: main loop running again after %d s", still);
			last = f;
			still = reported = 0;
			continue;
		}
		if (++still < 5 || reported >= 2)
			continue;
		watchdog_sample(++reported);
		if (reported == 1) {
			BYTE *chars[MAX_CHARS];
			int n = list_chars(chars, MAX_CHARS);
			BYTE *g = game_obj();
			logf("watchdog: role %d level %d mode %lu req %lu, %d characters", g_role, level_id(), g ? *(DWORD *)(g + 4) : 0,
				g ? *(DWORD *)(g + 8) : 0, n);
			for (int i = 0; i < n; i++)
				logf("watchdog:   character %lu state %lu requested %lu hp %.1f pos %.0f %.0f", *(DWORD *)(chars[i] + 0x78),
					*(DWORD *)(chars[i] + 0x60), *(DWORD *)(chars[i] + 0x230), *(float *)(chars[i] + 0x54),
					*(float *)(chars[i] + 0x20), *(float *)(chars[i] + 0x24));
		}
	}
	return 0;
}

// ---- the host's camera stays on the host's player ------------------------------------------
// The two-player camera manager is game+0x1c (FUN_0040bf10 toggles it); its +8 (game+0x24) is the
// player slot the shared camera follows. In the original same-screen co-op either player takes the
// camera with the real player's +0x34 button (FUN_0044c2b0, 0x44c4fa), so a press on the remote pad
// moved the host's camera to player 2 for good (0.7 playtest). On the host the camera now goes back
// to the host's own player while that player's character is alive; after a death (the players are
// swapped) it follows the survivor as the game does.
typedef void(__fastcall *cam_toggle_t)(void *mgr, void *edx);
static volatile LONG g_cam_restored, g_cam_toggle_cmd;

static void host_hold_camera(void)
{
	BYTE *g = game_obj();
	if (!g || *(DWORD *)(g + 4) != 2 || *(DWORD *)(g + 8) == 5 || !g_remote_rp)
		return;
	BYTE *r0 = rd_ptr(g, 0x4fc), *r1 = rd_ptr(g, 0x500);
	BYTE *local_rp = g_remote_rp == r0 ? r1 : r0;
	DWORD slot = local_rp == r1 ? 1 : 0;
	BYTE *c = rd_ptr(local_rp, 0xc);
	if (!c || *(DWORD *)(c + 0x58) || *(DWORD *)(c + 0x5c) || !*(DWORD *)(g + 0x1c + 0x10))
		return;
	if (*(DWORD *)(g + 0x24) != slot && rd_ptr(g, 0x58 + 4 * slot)) {
		((cam_toggle_t)0x40bf10)(g + 0x1c, NULL);
		InterlockedIncrement(&g_cam_restored);
		static DWORD last;
		if (GetTickCount() - last > 5000)
			logf("camera: back on the host's player (slot %lu)", slot + 1);
		last = GetTickCount();
	}
}

// ---- host pause after a death, and F1 ----------------------------------------------------
// Only the game's player 1 can open the pause menu (main loop around 0x417121: the device of
// *(game+0x4fc)). After a death swap the host is player 2 and its START asks "Player 2 quits the party"
// instead, so the host could not reach LOAD. Here the host gets the pause menu the way the main loop
// opens it (game+0x60c = 1, FUN_005353d0 on the menu manager game+0x628 = node 400, menu+0x10 = the
// device that drives it): 3 s after a player's character dies (both players in), or on F1.
typedef void(__fastcall *menu_open_t)(void *menu, void *edx);
static volatile LONG g_host_pauses, g_pause_cmd;

static int host_open_pause(const char *why)
{
	BYTE *g = game_obj();
	BYTE *menu = rd_ptr(g, 0x628);
	if (!menu || *(DWORD *)(g + 4) != 2 || *(DWORD *)(g + 8) != 0 || *(DWORD *)(g + 0x60c))
		return 0;
	BYTE *r0 = rd_ptr(g, 0x4fc), *r1 = rd_ptr(g, 0x500);
	BYTE *local_rp = g_remote_rp && g_remote_rp == r0 ? r1 : r0;
	if (!local_rp)
		return 0;
	*(DWORD *)(g + 0x60c) = 1;
	((menu_open_t)0x5353d0)(menu, NULL);
	*(DWORD *)(menu + 0x10) = *(DWORD *)(local_rp + 8);
	InterlockedIncrement(&g_host_pauses);
	logf("pause: opened for the host (%s)", why);
	return 1;
}

static int char_dead(BYTE *rp)
{
	BYTE *c = rd_ptr(rp, 0xc);
	return c && (*(DWORD *)(c + 0x58) || *(DWORD *)(c + 0x5c) || *(float *)(c + 0x54) <= 0.0f);
}

static void host_pause_prompts(void)
{
	static DWORD death_at;
	static int was_dead, f1_down;
	BYTE *g = game_obj();
	int f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if ((f1 && !f1_down && pid == GetCurrentProcessId()) || g_pause_cmd) {
		g_pause_cmd = 0;
		host_open_pause("F1");
	}
	f1_down = f1;
	if (!g || *(DWORD *)(g + 4) != 2 || *(DWORD *)(g + 8) != 0) {
		was_dead = 0;
		death_at = 0;
		return;
	}
	BYTE *r0 = rd_ptr(g, 0x4fc), *r1 = rd_ptr(g, 0x500);
	int dead = r0 && r1 && (char_dead(r0) || char_dead(r1));
	if (dead && !was_dead) {
		death_at = GetTickCount();
		logf("pause: a player's character died; pause menu in 3 s");
	}
	was_dead = dead;
	if (death_at && GetTickCount() - death_at > 3000) {
		death_at = 0;
		host_open_pause("a player died");
	}
}

// Per-frame hook: the main loop calls SteamAPI_RunCallbacks once per pass (FUN_00547290).
static void(__cdecl *real_SteamAPI_RunCallbacks)(void);

static void __cdecl hk_SteamAPI_RunCallbacks(void)
{
	real_SteamAPI_RunCallbacks();
	net_frame();   // steam transport: receive and periodic sends on the main thread
	update_title();
	InterlockedIncrement(&g_frames);
	if (!g_main_tid)
		g_main_tid = GetCurrentThreadId();
	if (g_msg_cmd_id) {   // test command: show text id for ms, as a pickup does
		BYTE *box = *(BYTE **)0x6ab8ec;
		if (box)
			((msg_show_t)0x45bf40)(box, NULL, (DWORD)g_msg_cmd_id, (DWORD)g_msg_cmd_ms, 0, 0, 0, 0xffffffff, 0);
		g_msg_cmd_id = 0;
	}
	if (g_view_cmd_set) {   // test command: show a picture the way a pickup does (SFLB_10(name, 1, 0, 0))
		BYTE *viewer = *(BYTE **)0x6ab8f0;
		if (viewer)
			((view_show_t)0x430350)(viewer, NULL, g_view_cmd, 1, 0, 0);
		g_view_cmd_set = 0;
	}
	if (g_stall_ms) {
		LONG ms = g_stall_ms;
		g_stall_ms = 0;
		Sleep((DWORD)ms);
	}
	if (g_level_cmd_id >= 0) {   // test command, host or single copy
		const char *name = level_name((int)g_level_cmd_id);
		logf("sync: level command id %ld -> %s", g_level_cmd_id, name ? name : "(unknown id)");
		request_level(name, (DWORD)g_level_cmd_entry);
		g_level_cmd_id = -1;
	}
	if (g_tp_pending) {   // test command: move player 1 through the game's own setters
		BYTE *rp0 = rd_ptr(game_obj(), g_tp_player ? 0x500 : 0x4fc);
		BYTE *c = rd_ptr(rp0, 0xc);
		if (c) {
			char_state st;
			memset(&st, 0, sizeof st);
			memcpy(st.pos, g_tp_pos, 12);
			memcpy(st.dir, g_tp_dir, 12);
			put_char(c, &st);
			logf("sync: teleport player 1 to %.1f %.1f %.1f", g_tp_pos[0], g_tp_pos[1], g_tp_pos[2]);
		}
		g_tp_pending = 0;
	}
	if (g_sw_player >= 0) {   // test command: real player N takes character id M (FUN_0044b7e0)
		BYTE *rp = rd_ptr(game_obj(), g_sw_player ? 0x500 : 0x4fc);
		BYTE *chars[MAX_CHARS];
		int n = list_chars(chars, MAX_CHARS);
		for (int j = 0; j < n && rp; j++)
			if (*(DWORD *)(chars[j] + 0x78) == (DWORD)g_sw_char)
				((control_char_t)0x44b7e0)(rp, NULL, chars[j]);
		BYTE *now = rd_ptr(rp, 0xc);
		logf("sync: switch command: player %ld -> character %ld, now %lu", g_sw_player, g_sw_char, now ? *(DWORD *)(now + 0x78) : 0);
		g_sw_player = -1;
	}
	if (g_hurtp_player >= 0) {   // test command: change real player N's health through AddLife (vtable slot 6)
		BYTE *c = rd_ptr(rd_ptr(game_obj(), g_hurtp_player ? 0x500 : 0x4fc), 0xc);
		if (c) {
			((add_life_t)(*(void ***)c)[6])(c, NULL, g_hurtp_delta);
			logf("sync: hurtp player %ld by %.1f -> hp %.1f", g_hurtp_player, g_hurtp_delta, *(float *)(c + 0x54));
		}
		g_hurtp_player = -1;
	}
	if (g_hurt_index >= 0) {   // test command: damage enemy N through its own AddLife (vtable slot 6)
		BYTE *e[MAX_PNJ];
		int n = list_entities(0x58, e, MAX_PNJ);
		if (g_hurt_index < n) {
			((add_life_t)(*(void ***)e[g_hurt_index])[6])(e[g_hurt_index], NULL, g_hurt_delta);
			logf("sync: hurt enemy %ld by %.1f -> hp %.1f", g_hurt_index, g_hurt_delta, *(float *)(e[g_hurt_index] + 0x54));
		}
		g_hurt_index = -1;
	}
	if (g_role == 1)
		ensure_items_sections();
	if (g_role == 1 && g_load_cmd >= 0) {   // test command: what the load menu does
		game_load_slot((DWORD)g_load_cmd);
		g_load_cmd = -1;
	}
	if (g_role == 1)
		host_watch_load();
	if (g_role == 1 && g_cam_toggle_cmd) {   // test command: toggle the camera as the +0x34 button does
		BYTE *g = game_obj();
		if (g)
			((cam_toggle_t)0x40bf10)(g + 0x1c, NULL);
		g_cam_toggle_cmd = 0;
	}
	if (g_role == 1)
		host_hold_camera();
	if (g_role == 1)
		host_pause_prompts();
	if (g_role == 1 && g_give_from >= 0)
		host_test_give();
	if (g_role == 1 && g_bag_use) {   // test command: use up one of the team bag's first item
		BYTE *it[1];
		BYTE *c = rd_ptr(rd_ptr(game_obj(), 0x4fc), 0xc);
		if (c && inv_items(NULL, it, 1)) {
			logf("inv: test use of bag item type %lu id %lu (count %lu)", *(DWORD *)(it[0] + 4), *(DWORD *)(it[0] + 8), *(DWORD *)(it[0] + 0x3c));
			((inv_remove_t)0x449410)(c + 0x128, NULL, it[0], 1);
		}
		g_bag_use = 0;
	}
	if (g_role == 1 && g_peer_ok) {
		static LONG sent_ver;
		if (g_last_save_len && g_last_save_ver != sent_ver) {   // a save was loaded that the guest does not have yet
			sent_ver = g_last_save_ver;
			host_start_save_xfer();
		}
		host_send_load(sent_ver);
		host_pump_save();
		host_watch_level();
		// Sends run on a 60 Hz network tick, not per frame: the game runs uncapped (owner: ~275 fps), and
		// per-frame rates meant ~600 packets/s over Steam (1.0). Characters and enemies every tick, the rest
		// at the rates they had at 60 fps.
		static double next_tick;
		static LONG nt;
		double now = clock_ms();
		if (now >= next_tick) {
			next_tick = now - next_tick > 50.0 ? now + 1000.0 / 60 : next_tick + 1000.0 / 60;
			nt++;
			host_send_state();
			host_send_pnj();
			if ((nt % 2) == 0)
				host_send_ui();
			if ((nt % 4) == 1)
				host_send_view();
			if ((nt % 2) == 1)
				host_send_props(nt);
			if ((nt % 6) == 4)
				host_send_breaks(nt);
			if ((nt % 4) == 3)
				host_send_msgs();
			if ((nt % 15) == 0)
				host_send_flags();
			if ((nt % 6) == 3)
				host_send_items();
			if ((nt % 30) == 7)
				host_send_inventories();
		}
	} else if (g_role == 2) {
		guest_apply_load();
		guest_apply_level_and_flags();
		guest_apply_state();
		guest_apply_pnj();
		guest_apply_items();
		guest_apply_props();
		guest_apply_breaks();
		guest_apply_inventories();
		guest_apply_view();
		guest_apply_msgs();
	}
}


// ---- movable props (1.10) ----------------------------------------------------------------------------------
// Chairs, wheeled objects (the TV stands: CNoeCasterObject), basketballs and mobile racks are physics entities of the
// Noe world that move when a character hits them. They hang off CToeWorld (*(game+0x588), list at +0xc, nodes
// {item, next}; the Toe entities of these classes point to the Noe entity at +4). A Noe entity keeps its position at
// +0xb0 (3 floats) and its rotation at +0xbc (a quaternion); vtable slots 0x54 / 0x58 set them (and update the
// matrix and the physics body), slot 0x30(1) freezes its velocity. Both copies build the list in the same order from
// the level data, so a prop is identified by its class and its index among that class. The host sends the props
// that moved (and every prop once a second); the guest applies them with the engine's own setters.
enum { PKT_PROPS = 20 };
enum { MAX_PROPS = 28, MAX_PROP_KEYS = 160 };
typedef struct { BYTE tag, idx, moving, pad; float pos[3]; float rot[4]; } prop_state;
typedef struct { pkt_hdr h; BYTE level, count; WORD pad; prop_state e[MAX_PROPS]; } pkt_props;
static pkt_props g_rx_props;
static volatile LONG g_rx_props_new, g_props_sent, g_props_applied, g_props_moved_here;
typedef void(__fastcall *ent_setv_t)(void *self, void *edx, const float *v);

static const struct { DWORD vt; BYTE tag; const char *name; } g_prop_cls[] = {
	{ 0x64732c, 1, "chair" }, { 0x647318, 2, "caster" }, { 0x647354, 3, "ball" }, { 0x6472d4, 4, "rack" } };

// Noe entities of the prop classes, in list order, with class tag and index within the class.
static int list_props(BYTE **noe, BYTE *tags, BYTE *idx, int max)
{
	BYTE *w = rd_ptr(game_obj(), 0x588);
	if (!w)
		return 0;
	BYTE *node = rd_ptr(w, 0xc);
	BYTE cnt[8] = { 0 };
	int n = 0, guard = 0;
	while (node && n < max && guard++ < 2000) {
		BYTE *item = rd_ptr(node, 0);
		if (!item)
			break;
		DWORD vt = *(DWORD *)item;
		for (int c = 0; c < 4; c++) {
			if (vt != g_prop_cls[c].vt)
				continue;
			BYTE *e = rd_ptr(item, 4);
			if (e && *(DWORD *)e >= 0x600000 && *(DWORD *)e < 0x6b0000) {
				noe[n] = e;
				tags[n] = g_prop_cls[c].tag;
				idx[n] = cnt[g_prop_cls[c].tag]++;
				n++;
			}
			break;
		}
		node = rd_ptr(node, 4);
	}
	return n;
}

static void host_send_props(LONG tick)
{
	static float last[MAX_PROP_KEYS][7];
	static BYTE seen[MAX_PROP_KEYS];
	BYTE *noe[MAX_PROP_KEYS], tags[MAX_PROP_KEYS], idx[MAX_PROP_KEYS];
	int n = list_props(noe, tags, idx, MAX_PROP_KEYS);
	static BYTE last_level = 255;
	if ((BYTE)level_id() != last_level) {   // a new level: forget what was sent
		memset(seen, 0, sizeof seen);
		last_level = (BYTE)level_id();
	}
	pkt_props p;
	memset(&p, 0, sizeof p);
	p.level = (BYTE)level_id();
	int refresh = (tick % 60) < 2, k = 0;
	for (int i = 0; i < n; i++) {
		float cur[7];
		memcpy(cur, noe[i] + 0xb0, 12);
		memcpy(cur + 3, noe[i] + 0xbc, 16);
		int moved = !seen[i];
		for (int j = 0; j < 3 && !moved; j++)
			if (cur[j] - last[i][j] > 0.02f || last[i][j] - cur[j] > 0.02f) moved = 1;
		for (int j = 3; j < 7 && !moved; j++)
			if (cur[j] - last[i][j] > 0.0005f || last[i][j] - cur[j] > 0.0005f) moved = 1;
		if (!moved && !refresh)
			continue;
		prop_state *e = &p.e[k++];
		e->tag = tags[i]; e->idx = idx[i]; e->moving = (BYTE)moved;
		memcpy(e->pos, cur, 12); memcpy(e->rot, cur + 3, 16);
		memcpy(last[i], cur, sizeof cur);
		seen[i] = 1;
		if (k == MAX_PROPS) {
			p.count = (BYTE)k;
			net_send(PKT_PROPS, &p, (WORD)(4 + k * sizeof(prop_state)));
			InterlockedIncrement(&g_props_sent);
			k = 0;
		}
	}
	if (k) {
		p.count = (BYTE)k;
		net_send(PKT_PROPS, &p, (WORD)(4 + k * sizeof(prop_state)));
		InterlockedIncrement(&g_props_sent);
	}
}

static void guest_on_props(const char *buf, int n)
{
	const pkt_props *p = (const pkt_props *)buf;
	if (n < (int)(sizeof(pkt_hdr) + 4) || p->count > MAX_PROPS || n != (int)(sizeof(pkt_hdr) + 4 + p->count * sizeof(prop_state)))
		return;
	EnterCriticalSection(&g_state_cs);
	if (g_rx_props_new && g_rx_props.level == p->level && g_rx_props.count + p->count <= MAX_PROPS) {
		memcpy(&g_rx_props.e[g_rx_props.count], p->e, p->count * sizeof(prop_state));   // keep what has not been applied yet
		g_rx_props.count = (BYTE)(g_rx_props.count + p->count);
	} else {
		memcpy(&g_rx_props, buf, n);
	}
	g_rx_props_new = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void guest_apply_props(void)
{
	pkt_props p;
	if (!g_rx_props_new)
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&p, &g_rx_props, sizeof p);
	g_rx_props_new = 0;
	LeaveCriticalSection(&g_state_cs);
	BYTE *g = game_obj();
	if (!g || p.level != (BYTE)level_id() || *(DWORD *)(g + 8) == 5)
		return;
	BYTE *noe[MAX_PROP_KEYS], tags[MAX_PROP_KEYS], idx[MAX_PROP_KEYS];
	int n = list_props(noe, tags, idx, MAX_PROP_KEYS);
	for (int i = 0; i < p.count; i++) {
		for (int j = 0; j < n; j++) {
			if (tags[j] != p.e[i].tag || idx[j] != p.e[i].idx)
				continue;
			BYTE *e = noe[j];
			void **vt = *(void ***)e;
			int dp = 0, dr = 0;
			for (int c = 0; c < 3; c++)
				if (*(float *)(e + 0xb0 + 4 * c) - p.e[i].pos[c] > 0.02f || p.e[i].pos[c] - *(float *)(e + 0xb0 + 4 * c) > 0.02f) dp = 1;
			for (int c = 0; c < 4; c++)
				if (*(float *)(e + 0xbc + 4 * c) - p.e[i].rot[c] > 0.0005f || p.e[i].rot[c] - *(float *)(e + 0xbc + 4 * c) > 0.0005f) dr = 1;
			if (dp)
				((ent_setv_t)vt[0x54 / 4])(e, NULL, p.e[i].pos);
			if (dr)
				((ent_setv_t)vt[0x58 / 4])(e, NULL, p.e[i].rot);
			if (dp || dr) {
				InterlockedIncrement(&g_props_moved_here);
				static int logged;
				if (logged++ < 12)
					logf("props: %s %d moved to %.0f %.0f %.0f on the host's word", g_prop_cls[p.e[i].tag - 1].name, p.e[i].idx, p.e[i].pos[0], p.e[i].pos[1], p.e[i].pos[2]);
			}
			break;
		}
	}
	InterlockedIncrement(&g_props_applied);
}

// Test commands: "props" lists them; "propset tag idx x y z" moves one with the engine's own setter (this copy).
static void props_status(char *reply, size_t replysz)
{
	BYTE *noe[MAX_PROP_KEYS], tags[MAX_PROP_KEYS], idx[MAX_PROP_KEYS];
	int n = list_props(noe, tags, idx, MAX_PROP_KEYS);
	int k = _snprintf(reply, replysz, "props %d sent %ld applied %ld moved_here %ld:", n, g_props_sent, g_props_applied, g_props_moved_here);
	for (int i = 0; i < n && k > 0 && (size_t)k < replysz - 80; i++)
		k += _snprintf(reply + k, replysz - k, " %d/%d@%.1f,%.1f,%.1f", tags[i], idx[i], *(float *)(noe[i] + 0xb0), *(float *)(noe[i] + 0xb4), *(float *)(noe[i] + 0xb8));
}

static int prop_set(int tag, int index, float x, float y, float z)
{
	BYTE *noe[MAX_PROP_KEYS], tags[MAX_PROP_KEYS], idx[MAX_PROP_KEYS];
	int n = list_props(noe, tags, idx, MAX_PROP_KEYS);
	for (int i = 0; i < n; i++) {
		if (tags[i] == tag && idx[i] == index) {
			float v[3] = { x, y, z };
			((ent_setv_t)(*(void ***)noe[i])[0x54 / 4])(noe[i], NULL, v);
			return 1;
		}
	}
	return 0;
}

// ---- breakable scene objects: windows in walls, vending machines (1.11) ---------------------------------------
// A CNoeBreakableUnbroken (vtable 0x653114; CToeWorld's list nodes point to it at +4) holds its CNoeBreakableGroup at +0x2f4 (the
// group's own vtable is 0x65307c). The group keeps a broken flag at +0x7c; FUN_0052a820 (this = group) sets it and
// switches the group's members from the whole object to the broken one with its effects. The game's own entity
// serializer (slots 3 and 4 of the unbroken entity) sends the same flag. Vending machines are the same class and
// their drink is an ordinary item, which the item sync already carries. Both copies build the list in the same
// order, so an entity is identified by its index among these. Only the break is synced, not the flying pieces.
enum { PKT_BREAKS = 21 };
enum { MAX_BREAKS = 96 };
typedef struct { pkt_hdr h; BYTE level, count; WORD pad; BYTE idx[MAX_BREAKS]; } pkt_breaks;
static pkt_breaks g_rx_breaks;
static volatile LONG g_rx_breaks_new, g_breaks_sent, g_scene_breaks_applied;
typedef void(__fastcall *grp_break_t)(void *group, void *edx);

static int list_breakables(BYTE **grp, int max)
{
	BYTE *w = rd_ptr(game_obj(), 0x588);
	if (!w)
		return 0;
	BYTE *node = rd_ptr(w, 0xc);
	int n = 0, guard = 0;
	while (node && n < max && guard++ < 4000) {
		BYTE *item = rd_ptr(node, 0);
		if (!item)
			break;
		BYTE *e = rd_ptr(item, 4);   // the Toe wrapper points to the Noe entity
		if (e && *(DWORD *)e == 0x653114) {
			BYTE *g = rd_ptr(e, 0x2f4);
			grp[n++] = (g && *(DWORD *)g == 0x65307c) ? g : NULL;
		}
		node = rd_ptr(node, 4);
	}
	return n;
}

static void host_send_breaks(LONG tick)
{
	static BYTE last[MAX_BREAKS], last_n, last_level = 255;
	BYTE *grp[MAX_BREAKS];
	int n = list_breakables(grp, MAX_BREAKS);
	pkt_breaks p;
	memset(&p, 0, sizeof p);
	p.level = (BYTE)level_id();
	for (int i = 0; i < n; i++)
		if (grp[i] && *(DWORD *)(grp[i] + 0x7c))
			p.idx[p.count++] = (BYTE)i;
	int same = last_level == p.level && last_n == p.count && !memcmp(last, p.idx, p.count);
	if (same && (tick % 120) != 0)
		return;
	last_level = p.level;
	last_n = p.count;
	memcpy(last, p.idx, sizeof last);
	if (!p.count && same)
		return;
	net_send(PKT_BREAKS, &p, (WORD)(4 + p.count));
	InterlockedIncrement(&g_breaks_sent);
}

static void guest_on_breaks(const char *buf, int n)
{
	const pkt_breaks *p = (const pkt_breaks *)buf;
	if (n < (int)(sizeof(pkt_hdr) + 4) || p->count > MAX_BREAKS || n != (int)(sizeof(pkt_hdr) + 4 + p->count))
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&g_rx_breaks, buf, n);
	g_rx_breaks_new = 1;
	LeaveCriticalSection(&g_state_cs);
}

static void guest_apply_breaks(void)
{
	pkt_breaks p;
	if (!g_rx_breaks_new)
		return;
	EnterCriticalSection(&g_state_cs);
	memcpy(&p, &g_rx_breaks, sizeof p);
	g_rx_breaks_new = 0;
	LeaveCriticalSection(&g_state_cs);
	BYTE *g = game_obj();
	if (!g || p.level != (BYTE)level_id() || *(DWORD *)(g + 8) == 5)
		return;
	BYTE *grp[MAX_BREAKS];
	int n = list_breakables(grp, MAX_BREAKS);
	for (int i = 0; i < p.count; i++) {
		if (p.idx[i] < n && grp[p.idx[i]] && !*(DWORD *)(grp[p.idx[i]] + 0x7c)) {
			((grp_break_t)0x52a820)(grp[p.idx[i]], NULL);
			InterlockedIncrement(&g_scene_breaks_applied);
			logf("sync: breakable object %d was broken on the host; broken here", p.idx[i]);
		}
	}
}

// Test commands: "sbrk" lists them (index, broken flag); "sbreakit index" breaks one on this copy.
static void sbrk_status(char *reply, size_t replysz)
{
	BYTE *grp[MAX_BREAKS];
	int n = list_breakables(grp, MAX_BREAKS), k = _snprintf(reply, replysz, "scene breakables %d sent %ld applied %ld:", n, g_breaks_sent, g_scene_breaks_applied);
	for (int i = 0; i < n && k > 0 && (size_t)k < replysz - 20; i++)
		k += _snprintf(reply + k, replysz - k, " %d=%d", i, grp[i] ? (int)*(DWORD *)(grp[i] + 0x7c) : -1);
}

static int sbrk_do(int index)
{
	BYTE *grp[MAX_BREAKS];
	int n = list_breakables(grp, MAX_BREAKS);
	if (index < 0 || index >= n || !grp[index])
		return 0;
	((grp_break_t)0x52a820)(grp[index], NULL);
	return 1;
}

// Movement frame of the guest's player (1.9). FUN_0044c2b0 is the real player's per-frame update; it turns the stick
// into a world direction relative to a forward vector, either a locked reference (real player +0xa8/+0xac/+0xb0, kept
// while +0xa0 is 1 and the stick direction stays close to the one it was captured with) or the level's current camera
// (*(*(game+0x514)+0x54), axis at +0x40/+0x44/+0x48). On the host both are the host's cameras, but the guest's screen
// shows his own camera: with two different camera angles his stick moved him in the host's frame, not the one he
// sees (playtest report). The guest sends the camera axis of his own game (a new packet, ignored by older versions);
// on the host the update of the guest's real player runs with that axis in place of the camera's and of the locked
// reference, and everything is put back after the call, so player 1 is unaffected. Equal cameras give equal vectors.
enum { PKT_MOVEREF = 19 };
typedef struct { pkt_hdr h; DWORD flag; float ref[3]; } pkt_moveref;
static volatile DWORD g_ref_rx_tick, g_ref_flag;
static volatile float g_ref_vec[3];
static volatile LONG g_ref_sent, g_ref_applied, g_ref_diff;
static int g_test_ref_on;
static float g_test_ref[3];
typedef void(__fastcall *move_update_t)(void *, void *, int);
static move_update_t g_move_tramp;

static BYTE *level_camera(void)
{
	BYTE *g = game_obj();
	BYTE *a = rd_ptr(g, 0x514);
	return a ? rd_ptr(a, 0x54) : NULL;
}

static void guest_send_moveref(void)
{
	pkt_moveref p;
	memset(&p, 0, sizeof p);
	if (g_test_ref_on) {
		p.flag = 1;
		memcpy(p.ref, g_test_ref, 12);
	} else {
		BYTE *cam = level_camera();
		if (!cam)
			return;
		p.flag = 1;
		memcpy(p.ref, cam + 0x40, 12);
	}
	net_send(PKT_MOVEREF, &p, (WORD)(sizeof p - sizeof(pkt_hdr)));
	InterlockedIncrement(&g_ref_sent);
}

static void host_on_moveref(const char *buf, int n)
{
	const pkt_moveref *p = (const pkt_moveref *)buf;
	if (n != (int)sizeof *p)
		return;
	g_ref_vec[0] = p->ref[0]; g_ref_vec[1] = p->ref[1]; g_ref_vec[2] = p->ref[2];
	g_ref_flag = p->flag;
	g_ref_rx_tick = GetTickCount();
}

static void __fastcall hk_move_update(void *rp, void *edx, int dt)
{
	BYTE *cam = (g_role == 1 && rp && rp == g_remote_rp && g_ref_flag && GetTickCount() - g_ref_rx_tick < 1500) ? level_camera() : NULL;
	float x = g_ref_vec[0], y = g_ref_vec[1], z = g_ref_vec[2];
	if (!cam || !(x * x + y * y > 0.0001f)) {
		g_move_tramp(rp, edx, dt);
		return;
	}
	float save_cam[3], save_ref[3];
	memcpy(save_cam, cam + 0x40, 12);
	memcpy(save_ref, (BYTE *)rp + 0xa8, 12);
	if (fabs_f(save_cam[0] - x) > 0.02f || fabs_f(save_cam[1] - y) > 0.02f) {
		if (InterlockedIncrement(&g_ref_diff) <= 20)
			logf("movref: the guest's player 2 moves relative to (%.2f %.2f %.2f); the host's camera is (%.2f %.2f %.2f)", x, y, z, save_cam[0], save_cam[1], save_cam[2]);
	}
	((float *)(cam + 0x40))[0] = x; ((float *)(cam + 0x40))[1] = y; ((float *)(cam + 0x40))[2] = z;
	((float *)((BYTE *)rp + 0xa8))[0] = x; ((float *)((BYTE *)rp + 0xa8))[1] = y; ((float *)((BYTE *)rp + 0xa8))[2] = z;
	g_move_tramp(rp, edx, dt);
	memcpy(cam + 0x40, save_cam, 12);
	memcpy((BYTE *)rp + 0xa8, save_ref, 12);
	InterlockedIncrement(&g_ref_applied);
}

static void move_hook_install(void)
{
	static const BYTE entry[6] = { 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08 };   // push ebp; mov ebp, esp; mov eax, [ebp+8]
	BYTE *f = (BYTE *)0x44c2b0;
	if (g_move_tramp)
		return;
	if (memcmp(f, entry, sizeof entry)) {
		logf("movref: FUN_0044c2b0 entry bytes not as expected; guest movement frame not corrected");
		return;
	}
	BYTE *t = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t)
		return;
	memcpy(t, entry, sizeof entry);
	t[6] = 0xe9;
	*(LONG *)(t + 7) = (LONG)(f + 6 - (t + 11));
	g_move_tramp = (move_update_t)t;
	DWORD old;
	VirtualProtect(f, 6, PAGE_EXECUTE_READWRITE, &old);
	f[0] = 0xe9;
	*(LONG *)(f + 1) = (LONG)((BYTE *)hk_move_update - (f + 5));
	f[5] = 0x90;
	VirtualProtect(f, 6, old, &old);
	FlushInstructionCache(GetCurrentProcess(), f, 6);
	logf("movref: guest movement frame hook installed (host)");
}

// Test command (pipe "setref x y z", or "setref off"): the guest sends this vector instead of its camera's.
static int test_set_ref(float x, float y, float z)
{
	g_test_ref[0] = x; g_test_ref[1] = y; g_test_ref[2] = z;
	g_test_ref_on = 1;
	return 1;
}

static void sync_status(char *reply, size_t replysz)
{
	BYTE *chars[MAX_CHARS];
	int n = list_chars(chars, MAX_CHARS);
	BYTE *g = game_obj();
	int k = _snprintf(reply, replysz, "sync frames %ld applied %ld missing %ld puppet_updates %ld level %d mode %lu "
		"req %lu levels_applied %ld level_blocked %ld flags_applied %ld pnj_applied %ld pnj_mismatch %ld "
		"life_changes %ld kills %ld items_applied %ld items_removed %ld doors_set %ld save_tx %ld/%lu save_rx %ld/%ld done %ld "
		"events %ld/%ld cam %ld doors %ld/%ld msgs %ld views %ld/%ld ui %ld menus %ld inv +%ld -%ld hold %ld missing %ld state_rx %ld bad %ld skip %ld/%ld/%ld loads %ld/%ld/%ld chars %d", g_frames, g_applied,
		g_apply_missing, g_state_sets, level_id(), g ? *(DWORD *)(g + 4) : 0, g ? *(DWORD *)(g + 8) : 0,
		g_level_applied, g_level_blocked, g_flags_applied, g_pnj_applied, g_pnj_mismatch, g_life_changes, g_kills, g_items_applied,
		g_items_collected_on_guest, g_doors_set, g_save_acked, g_save_xfer_len, g_save_rx_have, g_save_rx_total,
		g_save_rx_done, g_events_recorded, g_events_replayed, g_cam_restored, g_door_uses, g_door_level_swallowed, g_msgs_replayed, g_views_shown, g_views_closed, g_ui_applied, g_ui_menu_opened, g_inv_added, g_inv_removed, g_inv_held, g_inv_missing,
		g_rx_state_ok, g_rx_state_bad, g_skip_nochars, g_skip_level, g_skip_change, g_loads_sent, g_loads_applied, g_loads_failed, n);
	for (int i = 0; i < n && k > 0 && (size_t)k < replysz - 64; i++) {
		BYTE *c = chars[i];
		k += _snprintf(reply + k, replysz - k, " | id %lu pos %.1f %.1f %.1f hp %.1f", *(DWORD *)(c + 0x78),
			*(float *)(c + 0x20), *(float *)(c + 0x24), *(float *)(c + 0x28), *(float *)(c + 0x54));
	}
}
