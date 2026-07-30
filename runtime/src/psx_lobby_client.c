#include "psx_lobby_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(PSX_HAS_LOBBY_CLIENT)

const char *psx_lobby_default_url(void)
{
    return "ws://netplay.technicallycomputers.ca:8765";
}
int  psx_lobby_connect(const char *ws_url) { (void)ws_url; return -1; }
void psx_lobby_disconnect(void) {}
int  psx_lobby_connected(void) { return 0; }
void psx_lobby_set_display_name(const char *name) { (void)name; }
const char *psx_lobby_display_name(void) { return ""; }
const char *psx_lobby_player_id(void) { return ""; }
void psx_lobby_pump(void) {}
void psx_lobby_request_list(void) {}
int  psx_lobby_list_count(void) { return 0; }
int  psx_lobby_list_get(int index, PsxLobbyRow *out) { (void)index; (void)out; return 0; }
void psx_lobby_set_game_identity(const char *a, const char *b) { (void)a; (void)b; }
const char *psx_lobby_game_version(void) { return PSX_GAME_VERSION; }
void psx_lobby_set_max_slots(int max_slots) { (void)max_slots; }
int  psx_lobby_create(const char *a, const char *b, const char *c, const char *d,
                      const char *e, const PsxLobbyMatchCaps *f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return -1; }
int  psx_lobby_join(const char *a, const char *b, const char *c)
{ (void)a; (void)b; (void)c; return -1; }
int  psx_lobby_leave(void) { return -1; }
int  psx_lobby_kick(int slot) { (void)slot; return -1; }
int  psx_lobby_move_member(int from_slot, int to_slot)
{ (void)from_slot; (void)to_slot; return -1; }
int  psx_lobby_in_lobby(void) { return 0; }
int  psx_lobby_is_host(void) { return 0; }
const char *psx_lobby_host_player_id(void) { return ""; }
const PsxLobbyJoinInfo *psx_lobby_join_info(void)
{
    static PsxLobbyJoinInfo z;
    return &z;
}
const PsxLobbyMatchCaps *psx_lobby_match_caps(void)
{
    static PsxLobbyMatchCaps z;
    return &z;
}
int  psx_lobby_set_match_caps(const PsxLobbyMatchCaps *c) { (void)c; return -1; }
int  psx_lobby_member_count(void) { return 0; }
int  psx_lobby_member_get(int index, PsxLobbyMember *out) { (void)index; (void)out; return 0; }
int  psx_lobby_member_latency_ms(int slot) { (void)slot; return -1; }
int  psx_lobby_member_is_host(const PsxLobbyMember *member)
{
    (void)member;
    return 0;
}
int  psx_lobby_send_signal(int type, int flag, const char *text)
{
    (void)type;
    (void)flag;
    (void)text;
    return -1;
}
int  psx_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    (void)type;
    (void)flag;
    (void)text;
    (void)text_cap;
    return 0;
}
void psx_lobby_clear_signals(void) {}
void psx_lobby_set_ice_signal_accept(int accept) { (void)accept; }
int  psx_lobby_request_turn_credentials(void) { return -1; }
const PsxLobbyTurnCredentials *psx_lobby_turn_credentials(void)
{
    static PsxLobbyTurnCredentials z;
    return &z;
}
int  psx_lobby_local_ready(void) { return 0; }
int  psx_lobby_all_ready(void) { return 0; }
int  psx_lobby_set_ready(int ready) { (void)ready; return -1; }
int  psx_lobby_request_start(const PsxLobbyMatchCaps *c) { (void)c; return -1; }
int  psx_lobby_launch_pending(void) { return 0; }
void psx_lobby_clear_launch_pending(void) {}

#else /* PSX_HAS_LOBBY_CLIENT */

#include "rnet_ws.h"
#include "rnet_sha1.h"
#include "recomp_net/address.h"
#include "recomp_net/lan_beacon.h"
#include "recomp_net/rtt_probe.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* Winsock sets WSAGetLastError(), not errno — bare errno checks drop the
 * non-blocking WS handshake on Windows (list/create look permanently dead). */
static int socket_would_block(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

typedef struct {
    int fd;
    int connected;
    int handshake_done;
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    char host[128];
    int port;
    char path[128];
    char rx_http[4096];
    size_t rx_http_len;
    /* Bytes that arrived with the HTTP 101 response after the header end. */
    uint8_t ws_pending[4096];
    size_t ws_pending_len;
    PsxLobbyRow list[PSX_LOBBY_MAX_LIST];
    int list_count;
    int in_lobby;
    int is_host;
    char host_player_id[PSX_LOBBY_ID_LEN];
    char my_bind[PSX_LOBBY_ENDPOINT_LEN];
    char filter_game_name[PSX_LOBBY_NAME_LEN];
    char filter_game_version[PSX_LOBBY_VERSION_LEN];
    PsxLobbyJoinInfo join;
    PsxLobbyMember members[PSX_LOBBY_MAX_MEMBERS];
    int member_count;
    int local_ready;
    int all_ready;
    int launch_pending;
    PsxLobbyMatchCaps match_caps;
    char pending_tx[8][2048];
    int pending_n;
    /* Inbound ICE signals (WS op:signal). */
    struct {
        int type;
        int flag;
        char text[2048];
    } sig_q[32];
    int sig_head;
    int sig_tail;
    int sig_count;
    /* 0 while in lobby / after soft-return — drop stale ICE; launch sets 1. */
    int ice_signal_accept;
    /* Coturn mint from WS get_turn_credentials. */
    PsxLobbyTurnCredentials turn;
    time_t turn_received_at;
    int turn_request_pending;
    /* Waiting-room latency (ms) keyed by pad slot; -1 = unknown. */
    int member_rtt_ms[PSX_LOBBY_MAX_MEMBERS];
    uint64_t rtt_next_ping_ms;
} LobbyClient;

static LobbyClient g_lc = {
    .fd = -1,
    .filter_game_version = PSX_GAME_VERSION,
};

enum {
    PSX_LOBBY_SIG_RTT_PING = 100,
    PSX_LOBBY_SIG_RTT_PONG = 101,
    PSX_LOBBY_SIG_RTT_REPORT = 102
};

static uint64_t lobby_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ull +
               (uint64_t)ts.tv_nsec / 1000000ull;
#endif
    return (uint64_t)time(NULL) * 1000ull;
}

/* Defined later; used by waiting-room RTT signal handling. */
int psx_lobby_send_signal(int type, int flag, const char *text);
static int endpoint_port_is_zero(const char *ep);
static int using_server_input_relay(const PsxLobbyJoinInfo *j);
static void queue_send(const char *msg);
static void flush_pending(void);
static void lobby_rtt_close(void);

static RNetRttProbe *g_rtt_probe;

/* One-shot list latency: burst-ping LAN + public candidates after lobby_list. */
#define PSX_LOBBY_MAX_PROBE_PEND (PSX_LOBBY_MAX_LIST * (PSX_LOBBY_MAX_LAN_EPS + 1))
static RNetRttProbe *g_list_rtt_probe;
static int g_list_rtt_active;
static int g_list_rtt_on_next_list; /* set by request_list / Refresh */
static uint64_t g_list_rtt_deadline_ms;
static unsigned long long g_list_rtt_sent_ts[PSX_LOBBY_MAX_PROBE_PEND];
static int g_list_rtt_lobby_idx[PSX_LOBBY_MAX_PROBE_PEND];
static int g_list_rtt_pend_active[PSX_LOBBY_MAX_PROBE_PEND];
static int g_list_rtt_pend_count;

/* Local UDP broadcast discovery — LAN endpoint never goes to the hub. */
static RNetLanBeacon *g_lan_beacon_pub;
static RNetLanBeacon *g_lan_beacon_listen;

/* Host STUN advertise → set_host_endpoint for list / pre-join RTT. */
enum {
    HOST_ADV_IDLE = 0,
    HOST_ADV_WAIT_TURN,
    HOST_ADV_DONE
};
static int g_host_adv_state;
static uint64_t g_host_adv_deadline_ms;

static void lobby_host_advertise_reset(void)
{
    g_host_adv_state = HOST_ADV_IDLE;
    g_host_adv_deadline_ms = 0;
}

static int host_endpoint_is_loopback(const char *ep)
{
    if (!ep || !ep[0])
        return 0;
    if (strncmp(ep, "127.", 4) == 0)
        return 1;
    if (strncmp(ep, "::1:", 4) == 0 || strcmp(ep, "::1") == 0)
        return 1;
    if (strncmp(ep, "localhost:", 10) == 0 || strcmp(ep, "localhost") == 0)
        return 1;
    return 0;
}

static int endpoint_host_port(const char *ep, char *host, size_t host_cap, int *port_out)
{
    const char *colon;
    size_t n;
    if (!ep || !ep[0] || !host || host_cap == 0 || !port_out)
        return 0;
    colon = strrchr(ep, ':');
    if (!colon || colon == ep || !colon[1])
        return 0;
    n = (size_t)(colon - ep);
    if (n + 1 > host_cap)
        n = host_cap - 1;
    memcpy(host, ep, n);
    host[n] = '\0';
    *port_out = (int)strtoul(colon + 1, NULL, 10);
    return *port_out > 0 && *port_out <= 65535;
}

static int parse_ipv4_dotted(const char *host, unsigned *o)
{
    unsigned a, b, c, d;
    char extra;
    if (!host || !o)
        return 0;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    o[0] = a;
    o[1] = b;
    o[2] = c;
    o[3] = d;
    return 1;
}

static int ipv4_is_rfc1918(const unsigned o[4])
{
    if (!o)
        return 0;
    if (o[0] == 10)
        return 1;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31)
        return 1;
    if (o[0] == 192 && o[1] == 168)
        return 1;
    return 0;
}

static int ipv4_is_link_local(const unsigned o[4])
{
    return o && o[0] == 169 && o[1] == 254;
}

/* Public WAN IPv4 suitable for lobby-list host_endpoint (not LAN/loopback). */
static int endpoint_is_public_ipv4(const char *ep)
{
    char host[128];
    unsigned o[4];
    int port = 0;
    if (!endpoint_host_port(ep, host, sizeof(host), &port))
        return 0;
    if (!parse_ipv4_dotted(host, o))
        return 0;
    if (o[0] == 0 || o[0] == 127 || o[0] >= 224)
        return 0;
    if (ipv4_is_rfc1918(o) || ipv4_is_link_local(o))
        return 0;
    return 1;
}

static void lobby_rtt_ensure(void);
static void lobby_list_rtt_start(int force_all);

/* /24 heuristic — good enough for typical home/small office LANs. */
static int ipv4_same_lan24(const unsigned a[4], const unsigned b[4])
{
    return a && b && a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static int my_bind_port(void)
{
    char host[128];
    int port = 7777;
    if (g_lc.my_bind[0] && endpoint_host_port(g_lc.my_bind, host, sizeof(host), &port))
        return port;
    return 7777;
}

/* Single LAN advertise candidate: the Host Lobby "Advertised IP" / my_bind NIC.
 * Do not enumerate every local interface — only the menu selection. */
static int collect_host_lan_endpoints(char out[][PSX_LOBBY_ENDPOINT_LEN], int max_out)
{
    char bind_host[128];
    unsigned bind_o[4];
    int port = my_bind_port();

    if (!out || max_out <= 0)
        return 0;
    bind_host[0] = '\0';
    if (!g_lc.my_bind[0] ||
        !endpoint_host_port(g_lc.my_bind, bind_host, sizeof(bind_host), &port) ||
        !parse_ipv4_dotted(bind_host, bind_o) || !ipv4_is_rfc1918(bind_o) ||
        strcmp(bind_host, "0.0.0.0") == 0)
        return 0;
    snprintf(out[0], PSX_LOBBY_ENDPOINT_LEN, "%s:%d", bind_host, port);
    return 1;
}

/* Discover a public UDP mapping for the game port. Coturn on the same LAN can
 * return RFC1918 — reject those and fall back to any:/port + public STUN. */
static int lobby_host_stun_public(char *out, size_t out_len)
{
    RNetExternalIpv4Config stun;
    char any_bind[64];
    char endpoint[RNET_ENDPOINT_TEXT_MAX];
    int port = my_bind_port();
    int attempt;
    int last_rc = RNET_EXTERNAL_IPV4_ERR_ARGUMENT;

    if (!out || out_len == 0)
        return -1;
    out[0] = '\0';
    snprintf(any_bind, sizeof(any_bind), "0.0.0.0:%d", port);

    for (attempt = 0; attempt < 4; ++attempt) {
        const char *bind_hp;
        endpoint[0] = '\0';
        rnet_external_ipv4_config_init(&stun);
        if (attempt < 2 && g_lc.turn.valid && g_lc.turn.stun_host[0]) {
            stun.stun_host = g_lc.turn.stun_host;
            stun.stun_port = (unsigned short)g_lc.turn.stun_port;
        }
        /* attempt 0: coturn + my_bind, 1: coturn + any, 2: default + my_bind,
         * 3: default + any */
        bind_hp = (attempt & 1) ? any_bind
                                : (g_lc.my_bind[0] ? g_lc.my_bind : any_bind);
        last_rc = rnet_external_udp_endpoint_discover(&stun, bind_hp, endpoint,
                                                      sizeof(endpoint));
        if (last_rc != RNET_EXTERNAL_IPV4_OK || !endpoint[0])
            continue;
        if (!endpoint_is_public_ipv4(endpoint)) {
            fprintf(stderr,
                    "psx_lobby: STUN mapped private %s (bind=%s stun=%s) — "
                    "retrying\n",
                    endpoint, bind_hp,
                    stun.stun_host ? stun.stun_host : "(default)");
            continue;
        }
        strncpy(out, endpoint, out_len - 1);
        out[out_len - 1] = '\0';
        return 0;
    }
    return last_rc != 0 ? last_rc : -1;
}

static void lobby_lan_beacon_close_all(void)
{
    rnet_lan_beacon_close(&g_lan_beacon_pub);
    rnet_lan_beacon_close(&g_lan_beacon_listen);
}

static void lobby_lan_beacon_publish_update(void)
{
    char lan[PSX_LOBBY_MAX_LAN_EPS][PSX_LOBBY_ENDPOINT_LEN];
    int lan_n;
    if (!g_lc.is_host || !g_lc.in_lobby || g_lc.launch_pending)
        return;
    if (!g_lc.join.lobby_id[0])
        return;
    lan_n = collect_host_lan_endpoints(lan, PSX_LOBBY_MAX_LAN_EPS);
    if (lan_n <= 0) {
        rnet_lan_beacon_close(&g_lan_beacon_pub);
        return;
    }
    if (!g_lan_beacon_pub &&
        rnet_lan_beacon_publish_open(&g_lan_beacon_pub, 0) != 0)
        return;
    if (rnet_lan_beacon_publish_set(g_lan_beacon_pub, g_lc.join.lobby_id, lan[0],
                                    g_lc.filter_game_name) != 0) {
        rnet_lan_beacon_close(&g_lan_beacon_pub);
        return;
    }
    fprintf(stderr, "psx_lobby: LAN beacon publish %s → %s\n",
            g_lc.join.lobby_id, lan[0]);
}

static void lobby_lan_beacon_tick(void)
{
    if (g_lc.is_host && g_lc.in_lobby && !g_lc.launch_pending) {
        if (!g_lan_beacon_pub)
            lobby_lan_beacon_publish_update();
        if (g_lan_beacon_pub)
            (void)rnet_lan_beacon_publish_tick(g_lan_beacon_pub);
    } else if (g_lan_beacon_pub) {
        rnet_lan_beacon_close(&g_lan_beacon_pub);
    }

    /* Guests (and hosts browsing after leave) listen for same-LAN announces. */
    if (!g_lc.is_host || !g_lc.in_lobby) {
        int updated = 0;
        if (!g_lan_beacon_listen)
            (void)rnet_lan_beacon_listen_open(&g_lan_beacon_listen, 0);
        if (g_lan_beacon_listen)
            updated = rnet_lan_beacon_listen_pump(g_lan_beacon_listen);
        /* New beacon → re-probe list rows still missing latency. */
        if (updated > 0 && g_lc.list_count > 0 && !g_list_rtt_active) {
            int i;
            int need = 0;
            for (i = 0; i < g_lc.list_count; ++i) {
                if (g_lc.list[i].latency_ms < 0) {
                    need = 1;
                    break;
                }
            }
            if (need)
                lobby_list_rtt_start(0);
        }
    }
}

static void lobby_host_advertise_tick(void)
{
    char endpoint[RNET_ENDPOINT_TEXT_MAX];
    char msg[384];
    int rc;

    if (g_host_adv_state != HOST_ADV_WAIT_TURN)
        return;
    if (!g_lc.is_host || !g_lc.in_lobby || g_lc.launch_pending) {
        lobby_host_advertise_reset();
        return;
    }
    if (using_server_input_relay(&g_lc.join)) {
        g_host_adv_state = HOST_ADV_DONE;
        return;
    }
    /* Prefer Coturn STUN from turn_credentials; don't block forever. */
    if (!g_lc.turn.valid && lobby_mono_ms() < g_host_adv_deadline_ms)
        return;
    if (!g_lc.my_bind[0]) {
        g_host_adv_state = HOST_ADV_DONE;
        return;
    }

    /* Free the game UDP port for an exclusive STUN bind (skip on loopback hub). */
    if (!host_endpoint_is_loopback(g_lc.join.host_endpoint)) {
        lobby_rtt_close();
        endpoint[0] = '\0';
        rc = lobby_host_stun_public(endpoint, sizeof(endpoint));
        if (rc == 0 && endpoint[0]) {
            strncpy(g_lc.join.host_endpoint, endpoint,
                    sizeof(g_lc.join.host_endpoint) - 1);
            g_lc.join.host_endpoint[sizeof(g_lc.join.host_endpoint) - 1] = '\0';
        } else {
            fprintf(stderr,
                    "psx_lobby: STUN advertise failed (%d) — keeping %s%s\n", rc,
                    g_lc.join.host_endpoint[0] ? g_lc.join.host_endpoint
                                               : "(none)",
                    endpoint_is_public_ipv4(g_lc.join.host_endpoint)
                        ? ""
                        : " (use LAN beacon for local list RTT)");
        }
        /* Answer list/waiting-room PINGs again as soon as STUN frees the port. */
        lobby_rtt_ensure();
    }

    /* Private IPs stay on the LAN beacon only — never on the hub list. */
    lobby_lan_beacon_publish_update();

    g_host_adv_state = HOST_ADV_DONE;
    if (!g_lc.join.host_endpoint[0])
        return;
    snprintf(msg, sizeof(msg),
             "{\"op\":\"set_host_endpoint\",\"host_endpoint\":\"%s\"}",
             g_lc.join.host_endpoint);
    queue_send(msg);
    flush_pending();
    fprintf(stderr, "psx_lobby: advertised host_endpoint=%s (LAN via local beacon)\n",
            g_lc.join.host_endpoint);
}

static void lobby_rtt_close(void)
{
    rnet_rtt_probe_close(&g_rtt_probe);
}

static void lobby_list_rtt_close(void)
{
    rnet_rtt_probe_close(&g_list_rtt_probe);
    g_list_rtt_active = 0;
    g_list_rtt_pend_count = 0;
    memset(g_list_rtt_pend_active, 0, sizeof(g_list_rtt_pend_active));
    memset(g_list_rtt_sent_ts, 0, sizeof(g_list_rtt_sent_ts));
    memset(g_list_rtt_lobby_idx, 0, sizeof(g_list_rtt_lobby_idx));
}

static int collect_local_rfc1918(unsigned out[][4], int max_out)
{
    RNetIpv4Address addrs[16];
    int n;
    int i;
    int count = 0;

    if (!out || max_out <= 0)
        return 0;
    n = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
    if (n < 0)
        n = 0;
    if (n > (int)(sizeof(addrs) / sizeof(addrs[0])))
        n = (int)(sizeof(addrs) / sizeof(addrs[0]));
    for (i = 0; i < n && count < max_out; ++i) {
        unsigned o[4];
        if (!parse_ipv4_dotted(addrs[i].address, o) || !ipv4_is_rfc1918(o))
            continue;
        memcpy(out[count], o, sizeof(o));
        ++count;
    }
    return count;
}

static int cand_already(char cands[][PSX_LOBBY_ENDPOINT_LEN], int n, const char *ep)
{
    int i;
    for (i = 0; i < n; ++i) {
        if (strcmp(cands[i], ep) == 0)
            return 1;
    }
    return 0;
}

/* Prefer local beacon LAN, then legacy server lan_endpoints, then public. */
static int lobby_row_build_candidates(const PsxLobbyRow *row,
                                      char cands[][PSX_LOBBY_ENDPOINT_LEN],
                                      int max_cands)
{
    unsigned local[8][4];
    int local_n;
    int n = 0;
    int i;
    int pass;
    char beacon_ep[PSX_LOBBY_ENDPOINT_LEN];

    if (!row || !cands || max_cands <= 0)
        return 0;

    beacon_ep[0] = '\0';
    if (g_lan_beacon_listen && row->lobby_id[0] &&
        rnet_lan_beacon_lookup(g_lan_beacon_listen, row->lobby_id, beacon_ep,
                               sizeof(beacon_ep)) &&
        beacon_ep[0] && !endpoint_port_is_zero(beacon_ep)) {
        strncpy(cands[n], beacon_ep, PSX_LOBBY_ENDPOINT_LEN - 1);
        cands[n][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
        ++n;
    }

    local_n = collect_local_rfc1918(local, 8);

    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < row->lan_count && n < max_cands; ++i) {
            char host[64];
            int port = 0;
            unsigned o[4];
            int same = 0;
            int j;
            if (!row->lan_endpoints[i][0] ||
                endpoint_port_is_zero(row->lan_endpoints[i]))
                continue;
            if (!endpoint_host_port(row->lan_endpoints[i], host, sizeof(host), &port) ||
                !parse_ipv4_dotted(host, o) || !ipv4_is_rfc1918(o))
                continue;
            for (j = 0; j < local_n; ++j) {
                if (ipv4_same_lan24(local[j], o)) {
                    same = 1;
                    break;
                }
            }
            if (pass == 0 && !same)
                continue;
            if (pass == 1 && same)
                continue; /* already added */
            if (cand_already(cands, n, row->lan_endpoints[i]))
                continue;
            strncpy(cands[n], row->lan_endpoints[i], PSX_LOBBY_ENDPOINT_LEN - 1);
            cands[n][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
            ++n;
        }
    }
    if (n < max_cands && row->host_endpoint[0] &&
        !endpoint_port_is_zero(row->host_endpoint) &&
        !cand_already(cands, n, row->host_endpoint)) {
        strncpy(cands[n], row->host_endpoint, PSX_LOBBY_ENDPOINT_LEN - 1);
        cands[n][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
        ++n;
    }
    return n;
}

/* force_all: Refresh — re-probe every row. Otherwise only rows with unknown RTT. */
static void lobby_list_rtt_start(int force_all)
{
    int i;

    lobby_list_rtt_close();
    if (g_lc.list_count <= 0)
        return;
    /* Drain local beacons before building candidates (may beat WS list). */
    if (!g_lan_beacon_listen)
        (void)rnet_lan_beacon_listen_open(&g_lan_beacon_listen, 0);
    if (g_lan_beacon_listen)
        (void)rnet_lan_beacon_listen_pump(g_lan_beacon_listen);
    if (rnet_rtt_probe_open(&g_list_rtt_probe, NULL) != 0)
        return;

    for (i = 0; i < g_lc.list_count; ++i) {
        char cands[PSX_LOBBY_MAX_LAN_EPS + 1][PSX_LOBBY_ENDPOINT_LEN];
        int cn;
        int c;
        if (force_all)
            g_lc.list[i].latency_ms = -1;
        if (g_lc.list[i].latency_ms >= 0)
            continue;
        cn = lobby_row_build_candidates(&g_lc.list[i], cands,
                                        PSX_LOBBY_MAX_LAN_EPS + 1);
        for (c = 0; c < cn && g_list_rtt_pend_count < PSX_LOBBY_MAX_PROBE_PEND; ++c) {
            unsigned long long sent = 0;
            int slot = g_list_rtt_pend_count;
            if (rnet_rtt_probe_set_peer(g_list_rtt_probe, cands[c]) != 0)
                continue;
            if (rnet_rtt_probe_ping_ts(g_list_rtt_probe, &sent) != 0)
                continue;
            g_list_rtt_sent_ts[slot] = sent;
            g_list_rtt_lobby_idx[slot] = i;
            g_list_rtt_pend_active[slot] = 1;
            ++g_list_rtt_pend_count;
        }
    }

    if (g_list_rtt_pend_count <= 0) {
        lobby_list_rtt_close();
        return;
    }
    g_list_rtt_active = 1;
    /* STUN advertise briefly drops the host answer sock; give guests time. */
    g_list_rtt_deadline_ms = lobby_mono_ms() + 1500ull;
}

static void lobby_list_rtt_tick(void)
{
    int remaining;

    if (!g_list_rtt_active || !g_list_rtt_probe)
        return;

    for (;;) {
        int ms = 0;
        unsigned long long echo = 0;
        int p;
        int got = rnet_rtt_probe_pump_ex(g_list_rtt_probe, &ms, &echo);
        if (got != 1)
            break;
        for (p = 0; p < g_list_rtt_pend_count; ++p) {
            int li;
            int q;
            if (!g_list_rtt_pend_active[p] || g_list_rtt_sent_ts[p] != echo)
                continue;
            li = g_list_rtt_lobby_idx[p];
            if (li >= 0 && li < g_lc.list_count && g_lc.list[li].latency_ms < 0)
                g_lc.list[li].latency_ms = ms;
            /* Drop remaining candidates for this lobby. */
            for (q = 0; q < g_list_rtt_pend_count; ++q) {
                if (g_list_rtt_lobby_idx[q] == li)
                    g_list_rtt_pend_active[q] = 0;
            }
            break;
        }
    }

    remaining = 0;
    {
        int p;
        for (p = 0; p < g_list_rtt_pend_count; ++p) {
            if (g_list_rtt_pend_active[p])
                ++remaining;
        }
    }
    if (remaining <= 0 || lobby_mono_ms() >= g_list_rtt_deadline_ms)
        lobby_list_rtt_close();
}

static void member_rtt_clear(void)
{
    int i;
    for (i = 0; i < PSX_LOBBY_MAX_MEMBERS; ++i)
        g_lc.member_rtt_ms[i] = -1;
    g_lc.rtt_next_ping_ms = 0;
}

static int member_slot_for_player(const char *player_id)
{
    int i;
    if (!player_id || !player_id[0])
        return -1;
    for (i = 0; i < g_lc.member_count; ++i) {
        if (strcmp(g_lc.members[i].player_id, player_id) == 0)
            return g_lc.members[i].slot;
    }
    return -1;
}

static int local_member_slot(void)
{
    return member_slot_for_player(g_lc.player_id);
}

/* Default max_slots for create (clamped 2..8). */
static int g_lobby_max_slots = 2;

void psx_lobby_set_max_slots(int max_slots)
{
    if (max_slots < 2) max_slots = 2;
    if (max_slots > 8) max_slots = 8;
    g_lobby_max_slots = max_slots;
}

static const char *effective_game_version(const char *override_ver)
{
    if (override_ver && override_ver[0]) {
        return override_ver;
    }
    if (g_lc.filter_game_version[0]) {
        return g_lc.filter_game_version;
    }
    return PSX_GAME_VERSION;
}

/* Release builds pin the lobby browser to our exact game_version.
 * Non-release ("dev") shows all versions of our title so testers can see
 * unofficial / mismatched hosts; join still requires an exact version match. */
static int list_filter_version_strict(void)
{
    const char *gv = effective_game_version(NULL);
    return gv && gv[0] && strcmp(gv, "dev") != 0;
}

static void queue_send(const char *json);
static void clear_turn_credentials(void);
static int queue_turn_credentials_request(void);

static void clear_turn_credentials(void)
{
    memset(&g_lc.turn, 0, sizeof(g_lc.turn));
    g_lc.turn_received_at = 0;
    g_lc.turn_request_pending = 0;
}

static int queue_turn_credentials_request(void)
{
    if (!psx_lobby_connected())
        return -1;
    queue_send("{\"op\":\"get_turn_credentials\"}");
    g_lc.turn_request_pending = 1;
    return 0;
}

static void queue_list_request(void)
{
    char msg[384];
    const char *gn = g_lc.filter_game_name;
    const char *gv = effective_game_version(NULL);
    if (list_filter_version_strict() && (gn[0] || (gv && gv[0]))) {
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"list\",\"game_name\":\"%s\",\"game_version\":\"%s\"}",
                 gn, gv ? gv : "dev");
        queue_send(msg);
    } else if (gn[0]) {
        snprintf(msg, sizeof(msg), "{\"op\":\"list\",\"game_name\":\"%s\"}", gn);
        queue_send(msg);
    } else {
        queue_send("{\"op\":\"list\"}");
    }
}

static void match_caps_clear(PsxLobbyMatchCaps *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->aspect_num = 4;
    c->aspect_den = 3;
    c->input_delay = 2;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap);
static void parse_match_caps_object(const char *obj, PsxLobbyMatchCaps *out);
static void ingest_match_caps_from_json(const char *json);
static int append_match_caps_json(char *dst, size_t dst_cap, const PsxLobbyMatchCaps *caps);

const char *psx_lobby_default_url(void)
{
    const char *e = getenv("PSX_NET_LOBBY_URL");
    return (e && e[0]) ? e : "ws://netplay.technicallycomputers.ca:8765";
}

static int parse_ws_url(const char *url, char *host, size_t hcap, int *port, char *path, size_t pcap)
{
    const char *p = url;
    const char *slash;
    char hostport[192];
    char *colon;
    if (!url) {
        return -1;
    }
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        return -1; /* TLS not in this phase */
    }
    slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= sizeof(hostport)) {
            n = sizeof(hostport) - 1;
        }
        memcpy(hostport, p, n);
        hostport[n] = '\0';
        strncpy(path, slash, pcap - 1);
        path[pcap - 1] = '\0';
    } else {
        strncpy(hostport, p, sizeof(hostport) - 1);
        hostport[sizeof(hostport) - 1] = '\0';
        strncpy(path, "/", pcap - 1);
    }
    colon = strrchr(hostport, ':');
    if (colon && strchr(hostport, ']') == NULL) {
        *colon = '\0';
        *port = atoi(colon + 1);
        strncpy(host, hostport, hcap - 1);
    } else {
        strncpy(host, hostport, hcap - 1);
        *port = 8765;
    }
    host[hcap - 1] = '\0';
    return 0;
}

static const char *json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[80];
    const char *p;
    size_t o = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        if (out && cap) {
            out[0] = '\0';
        }
        return NULL;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return NULL;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '"') {
        return NULL;
    }
    ++p;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            default: out[o++] = *p; break;
            }
            ++p;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

/* Parse JSON string array values for key into out[0..max_out). Returns count. */
static int json_parse_str_array(const char *json, const char *key,
                                char out[][PSX_LOBBY_ENDPOINT_LEN], int max_out)
{
    char pat[80];
    const char *p;
    int n = 0;

    if (!json || !key || !out || max_out <= 0)
        return 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return 0;
    p = strchr(p + strlen(pat), '[');
    if (!p)
        return 0;
    ++p;
    while (*p && n < max_out) {
        size_t o = 0;
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            ++p;
        if (*p == ']')
            break;
        if (*p != '"')
            break;
        ++p;
        while (*p && *p != '"' && o + 1 < PSX_LOBBY_ENDPOINT_LEN)
            out[n][o++] = *p++;
        out[n][o] = '\0';
        if (*p == '"')
            ++p;
        if (out[n][0])
            ++n;
    }
    return n;
}

static void lobby_row_lan_fingerprint(const PsxLobbyRow *row, char *out, size_t cap)
{
    size_t o = 0;
    int i;
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!row)
        return;
    for (i = 0; i < row->lan_count; ++i) {
        int wrote;
        if (!row->lan_endpoints[i][0])
            continue;
        wrote = snprintf(out + o, cap > o ? cap - o : 0, "%s%s", o ? "|" : "",
                         row->lan_endpoints[i]);
        if (wrote < 0 || (size_t)wrote >= (cap > o ? cap - o : 0))
            break;
        o += (size_t)wrote;
    }
}

static size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!in || !out || cap == 0) return 0;
    while (*in && o + 2 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (c == '\t') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c < 0x20) {
            continue;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

static void enqueue_signal(int type, int flag, const char *text)
{
    int i;
    if (!g_lc.ice_signal_accept)
        return;
    if (g_lc.sig_count >= (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]))) {
        g_lc.sig_head = (g_lc.sig_head + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
        g_lc.sig_count--;
    }
    i = g_lc.sig_tail;
    g_lc.sig_q[i].type = type;
    g_lc.sig_q[i].flag = flag;
    g_lc.sig_q[i].text[0] = '\0';
    if (text)
        strncpy(g_lc.sig_q[i].text, text, sizeof(g_lc.sig_q[i].text) - 1);
    g_lc.sig_tail = (g_lc.sig_tail + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
    g_lc.sig_count++;
}

void psx_lobby_clear_signals(void)
{
    g_lc.sig_head = 0;
    g_lc.sig_tail = 0;
    g_lc.sig_count = 0;
}

void psx_lobby_set_ice_signal_accept(int accept)
{
    g_lc.ice_signal_accept = accept ? 1 : 0;
}

static int json_get_int(const char *json, const char *key, int def)
{
    char pat[80];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return def;
    }
    return (int)strtol(p + 1, NULL, 10);
}

static int json_get_bool(const char *json, const char *key, int def)
{
    char pat[80];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return def;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (strncmp(p, "true", 4) == 0) {
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        return 0;
    }
    return def;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap)
{
    char pat[80];
    const char *p;
    int depth;
    size_t n;
    if (!json || !key || !out || out_cap < 3) return 0;
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '{') return 0;
    depth = 0;
    n = 0;
    do {
        if (*p == '{') ++depth;
        else if (*p == '}') --depth;
        if (n + 1 >= out_cap) return 0;
        out[n++] = *p++;
    } while (*p && depth > 0);
    out[n] = '\0';
    return depth == 0 && n > 1;
}

static void parse_match_caps_object(const char *obj, PsxLobbyMatchCaps *out)
{
    if (!obj || !out || obj[0] != '{') return;
    match_caps_clear(out);
    out->aspect_num = json_get_int(obj, "aspect_num", 4);
    out->aspect_den = json_get_int(obj, "aspect_den", 3);
    out->turbo_loads = json_get_bool(obj, "turbo_loads", 0);
    out->bios_hle = json_get_bool(obj, "bios_hle", 1);
    out->fast_boot = json_get_bool(obj, "fast_boot", 0);
    out->auto_skip_fmv = json_get_bool(obj, "auto_skip_fmv", 0);
    out->input_delay = json_get_int(obj, "input_delay", 2);
    if (out->input_delay < 0) out->input_delay = 0;
    if (out->input_delay > 16) out->input_delay = 16;
    out->force_input_relay = json_get_bool(obj, "force_input_relay", 0);
    out->force_turn = json_get_bool(obj, "force_turn", 0);
    json_get_str(obj, "language", out->language, sizeof(out->language));
    out->valid = 1;
}

static void ingest_match_caps_from_json(const char *json)
{
    char obj[1024];
    if (json_extract_object(json, "match_caps", obj, sizeof(obj)))
        parse_match_caps_object(obj, &g_lc.match_caps);
}

static int append_match_caps_json(char *dst, size_t dst_cap, const PsxLobbyMatchCaps *caps)
{
    char lang[PSX_LOBBY_LANG_LEN];
    size_t i, o = 0;
    if (!dst || dst_cap < 8 || !caps || !caps->valid) return 0;
    /* Sanitize language for JSON string (alnum / _ / - only). */
    for (i = 0; caps->language[i] && o + 1 < sizeof(lang); ++i) {
        char ch = caps->language[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
            lang[o++] = ch;
    }
    lang[o] = '\0';
    if (!lang[0]) strncpy(lang, "en", sizeof(lang) - 1);
    return snprintf(dst, dst_cap,
                    ",\"match_caps\":{\"v\":1,\"aspect_num\":%d,\"aspect_den\":%d,"
                    "\"turbo_loads\":%s,\"bios_hle\":%s,\"fast_boot\":%s,"
                    "\"auto_skip_fmv\":%s,\"input_delay\":%d,\"force_input_relay\":%s,"
                    "\"force_turn\":%s,\"language\":\"%s\"}",
                    caps->aspect_num, caps->aspect_den,
                    caps->turbo_loads ? "true" : "false",
                    caps->bios_hle ? "true" : "false",
                    caps->fast_boot ? "true" : "false",
                    caps->auto_skip_fmv ? "true" : "false",
                    caps->input_delay,
                    caps->force_input_relay ? "true" : "false",
                    caps->force_turn ? "true" : "false",
                    lang);
}

static void queue_send(const char *json)
{
    if (g_lc.pending_n >= 8) {
        return;
    }
    strncpy(g_lc.pending_tx[g_lc.pending_n], json, sizeof(g_lc.pending_tx[0]) - 1);
    g_lc.pending_tx[g_lc.pending_n][sizeof(g_lc.pending_tx[0]) - 1] = '\0';
    g_lc.pending_n++;
}

static void flush_pending(void)
{
    int i;
    if (!g_lc.handshake_done) {
        return;
    }
    for (i = 0; i < g_lc.pending_n; ++i) {
        rnet_ws_write_text(g_lc.fd, g_lc.pending_tx[i], 1);
    }
    g_lc.pending_n = 0;
}

static int endpoint_port_is_zero(const char *ep)
{
    const char *colon;
    if (!ep || !ep[0]) return 1;
    colon = strrchr(ep, ':');
    if (!colon || !colon[1]) return 1;
    return (int)strtoul(colon + 1, NULL, 10) == 0;
}

/* Prefer a usable host:port among candidates (skip empty / :0). */
static void copy_first_usable_endpoint(char *dst, size_t dst_len, const char *a,
                                       const char *b, const char *c)
{
    const char *cands[3];
    int i;
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    cands[0] = a;
    cands[1] = b;
    cands[2] = c;
    for (i = 0; i < 3; ++i) {
        if (cands[i] && cands[i][0] && !endpoint_port_is_zero(cands[i])) {
            strncpy(dst, cands[i], dst_len - 1);
            dst[dst_len - 1] = '\0';
            return;
        }
    }
}

static int using_server_input_relay(const PsxLobbyJoinInfo *j)
{
    if (g_lc.match_caps.valid && g_lc.match_caps.force_input_relay)
        return 1;
    /* Server rewrote both endpoints to the same relay advertise address. */
    if (j && j->host_endpoint[0] && j->guest_endpoint[0] &&
        !endpoint_port_is_zero(j->host_endpoint) &&
        !endpoint_port_is_zero(j->guest_endpoint) &&
        strcmp(j->host_endpoint, j->guest_endpoint) == 0 &&
        (!g_lc.my_bind[0] || strcmp(j->host_endpoint, g_lc.my_bind) != 0))
        return 1;
    return 0;
}

static void fill_peer_bind_from_join(void)
{
    PsxLobbyJoinInfo *j = &g_lc.join;
    const int force_relay = using_server_input_relay(j);
    const int seats = j->player_count >= 2 ? j->player_count : j->max_slots;
    const int host_hub = (g_lc.is_host && seats >= 3 && !force_relay) ? 1 : 0;
    memset(j->bind_hostport, 0, sizeof(j->bind_hostport));
    memset(j->peer_hostport, 0, sizeof(j->peer_hostport));
    if (force_relay) {
        /* Everyone dials the lobby-server UDP relay — ephemeral local bind
         * (same as LAN guests) so same-PC multi-instance doesn't collide. */
        strncpy(j->bind_hostport, "0.0.0.0:0", sizeof(j->bind_hostport) - 1);
        copy_first_usable_endpoint(j->peer_hostport, sizeof(j->peer_hostport),
                                   j->host_endpoint, j->guest_endpoint, NULL);
    } else if (g_lc.is_host) {
        strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        if (!host_hub) {
            /* 2P P2P: dial guest when they advertised a fixed port. Online
             * guests often join with :0 — leave peer empty (accept-first). */
            if (j->guest_endpoint[0] && !endpoint_port_is_zero(j->guest_endpoint))
                strncpy(j->peer_hostport, j->guest_endpoint, sizeof(j->peer_hostport) - 1);
        }
        /* host_hub: peer stays empty → rnet_session_start_lan_hub */
    } else {
        /* Guests dialing 3+ host hub: ephemeral local UDP (join only probes
         * 7778+ and does not hold the socket). 2P P2P keeps the advertised
         * fixed guest_bind so the host can dial. */
        if (seats >= 3) {
            strncpy(j->bind_hostport, "0.0.0.0:0", sizeof(j->bind_hostport) - 1);
        } else {
            strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        }
        strncpy(j->peer_hostport, j->host_endpoint, sizeof(j->peer_hostport) - 1);
    }
    j->bind_hostport[sizeof(j->bind_hostport) - 1] = '\0';
    j->peer_hostport[sizeof(j->peer_hostport) - 1] = '\0';
}

static void parse_slots_array(const char *json)
{
    const char *p = strstr(json, "\"slots\"");
    int n = 0;
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    if (!p) {
        return;
    }
    p = strchr(p, '[');
    if (!p) {
        return;
    }
    ++p;
    while (*p && n < PSX_LOBBY_MAX_MEMBERS) {
        const char *obj;
        while (*p && *p != '{') {
            if (*p == ']') {
                g_lc.member_count = n;
                return;
            }
            ++p;
        }
        if (*p != '{') {
            break;
        }
        obj = p;
        {
            int depth = 0;
            const char *end = p;
            do {
                if (*end == '{') {
                    ++depth;
                } else if (*end == '}') {
                    --depth;
                }
                ++end;
            } while (*end && depth > 0);
            {
                char chunk[512];
                size_t len = (size_t)(end - obj);
                if (len >= sizeof(chunk)) {
                    len = sizeof(chunk) - 1;
                }
                memcpy(chunk, obj, len);
                chunk[len] = '\0';
                g_lc.members[n].slot = json_get_int(chunk, "slot", n);
                json_get_str(chunk, "player_id", g_lc.members[n].player_id,
                             sizeof(g_lc.members[n].player_id));
                json_get_str(chunk, "display_name", g_lc.members[n].display_name,
                             sizeof(g_lc.members[n].display_name));
                g_lc.members[n].ready = json_get_bool(chunk, "ready", 0);
                if (g_lc.player_id[0] &&
                    strcmp(g_lc.members[n].player_id, g_lc.player_id) == 0) {
                    g_lc.local_ready = g_lc.members[n].ready;
                    g_lc.join.local_slot = g_lc.members[n].slot;
                }
                ++n;
                p = end;
            }
        }
    }
    g_lc.member_count = n;
}

static void ingest_host_player_id(const char *json)
{
    char host_id[PSX_LOBBY_ID_LEN];
    host_id[0] = '\0';
    json_get_str(json, "host_player_id", host_id, sizeof(host_id));
    if (host_id[0]) {
        strncpy(g_lc.host_player_id, host_id, sizeof(g_lc.host_player_id) - 1);
        g_lc.host_player_id[sizeof(g_lc.host_player_id) - 1] = '\0';
    }
}

static void handle_server_json(const char *json);

/* Parse complete unmasked server text frames from ws_pending; leave remainder. */
static void drain_ws_pending(void)
{
    while (g_lc.ws_pending_len >= 2) {
        size_t i = 0;
        uint8_t b0 = g_lc.ws_pending[i++];
        uint8_t b1 = g_lc.ws_pending[i++];
        int opcode = b0 & 0x0f;
        size_t plen = b1 & 0x7f;
        if (b1 & 0x80) {
            /* Server frames must not be masked. */
            g_lc.ws_pending_len = 0;
            return;
        }
        if (plen == 126) {
            if (g_lc.ws_pending_len < i + 2) {
                return;
            }
            plen = ((size_t)g_lc.ws_pending[i] << 8) | g_lc.ws_pending[i + 1];
            i += 2;
        } else if (plen == 127) {
            g_lc.ws_pending_len = 0;
            return;
        }
        if (g_lc.ws_pending_len < i + plen) {
            return;
        }
        if (opcode == 0x1 && plen + 1 < sizeof(g_lc.rx_http)) {
            char text[4096];
            memcpy(text, g_lc.ws_pending + i, plen);
            text[plen] = '\0';
            handle_server_json(text);
        }
        i += plen;
        memmove(g_lc.ws_pending, g_lc.ws_pending + i, g_lc.ws_pending_len - i);
        g_lc.ws_pending_len -= i;
        if (opcode == 0x8) {
            psx_lobby_disconnect();
            return;
        }
    }
}

static void handle_server_json(const char *json)
{
    char op[32];
    json_get_str(json, "op", op, sizeof(op));
    if (strcmp(op, "welcome") == 0) {
        json_get_str(json, "player_id", g_lc.player_id, sizeof(g_lc.player_id));
        if (g_lc.display_name[0]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"op\":\"hello\",\"display_name\":\"%s\"}", g_lc.display_name);
            queue_send(msg);
        }
        queue_list_request();
        /* Prefetch Coturn creds for ICE (no-op reply if server lacks COTURN_*). */
        (void)queue_turn_credentials_request();
        return;
    }
    if (strcmp(op, "turn_credentials") == 0) {
        int ok = json_get_bool(json, "ok", 0);
        g_lc.turn_request_pending = 0;
        memset(&g_lc.turn, 0, sizeof(g_lc.turn));
        g_lc.turn_received_at = 0;
        if (!ok) {
            char err[64];
            json_get_str(json, "error", err, sizeof(err));
            fprintf(stderr,
                    "psx_lobby: turn_credentials failed (%s) — online ICE "
                    "requires Coturn (or PSX_NET_TURN_* / "
                    "PSX_NET_ALLOW_STUN_ONLY=1)\n",
                    err[0] ? err : "unknown");
            return;
        }
        json_get_str(json, "stun_host", g_lc.turn.stun_host,
                     sizeof(g_lc.turn.stun_host));
        json_get_str(json, "turn_host", g_lc.turn.turn_host,
                     sizeof(g_lc.turn.turn_host));
        json_get_str(json, "username", g_lc.turn.username,
                     sizeof(g_lc.turn.username));
        json_get_str(json, "password", g_lc.turn.password,
                     sizeof(g_lc.turn.password));
        g_lc.turn.stun_port = json_get_int(json, "stun_port", 3478);
        g_lc.turn.turn_port = json_get_int(json, "turn_port", 3478);
        g_lc.turn.ttl_secs = (uint32_t)json_get_int(json, "ttl_secs", 86400);
        if (g_lc.turn.turn_host[0] && g_lc.turn.username[0] &&
            g_lc.turn.password[0]) {
            g_lc.turn.valid = 1;
            g_lc.turn_received_at = time(NULL);
            fprintf(stderr,
                    "psx_lobby: turn_credentials ok stun=%s:%d turn=%s:%d "
                    "user=%s ttl=%us\n",
                    g_lc.turn.stun_host[0] ? g_lc.turn.stun_host : "(none)",
                    g_lc.turn.stun_port,
                    g_lc.turn.turn_host, g_lc.turn.turn_port,
                    g_lc.turn.username, (unsigned)g_lc.turn.ttl_secs);
        } else {
            fprintf(stderr,
                    "psx_lobby: turn_credentials ok but incomplete fields\n");
        }
        return;
    }
    if (strcmp(op, "lobby_list") == 0) {
        const char *p = strstr(json, "\"lobbies\"");
        int n = 0;
        /* Keep prior RTTs across server list pushes; Refresh re-probes.
         * Invalidate when host_endpoint or lan_endpoints change. */
        char prev_ids[PSX_LOBBY_MAX_LIST][PSX_LOBBY_ID_LEN];
        char prev_eps[PSX_LOBBY_MAX_LIST][PSX_LOBBY_ENDPOINT_LEN];
        char prev_lan[PSX_LOBBY_MAX_LIST][256];
        int prev_ms[PSX_LOBBY_MAX_LIST];
        int prev_n = g_lc.list_count;
        int i;
        int want_probe = g_list_rtt_on_next_list;
        g_list_rtt_on_next_list = 0;
        for (i = 0; i < prev_n && i < PSX_LOBBY_MAX_LIST; ++i) {
            strncpy(prev_ids[i], g_lc.list[i].lobby_id, PSX_LOBBY_ID_LEN - 1);
            prev_ids[i][PSX_LOBBY_ID_LEN - 1] = '\0';
            strncpy(prev_eps[i], g_lc.list[i].host_endpoint,
                    PSX_LOBBY_ENDPOINT_LEN - 1);
            prev_eps[i][PSX_LOBBY_ENDPOINT_LEN - 1] = '\0';
            lobby_row_lan_fingerprint(&g_lc.list[i], prev_lan[i], sizeof(prev_lan[i]));
            prev_ms[i] = g_lc.list[i].latency_ms;
        }
        g_lc.list_count = 0;
        if (!p) {
            return;
        }
        p = strchr(p, '[');
        if (!p) {
            return;
        }
        ++p;
        while (*p && n < PSX_LOBBY_MAX_LIST) {
            const char *obj;
            while (*p && *p != '{') {
                if (*p == ']') {
                    g_lc.list_count = n;
                    if (want_probe)
                        lobby_list_rtt_start(1);
                    else if (n > 0 && !g_list_rtt_active)
                        lobby_list_rtt_start(0);
                    return;
                }
                ++p;
            }
            if (*p != '{') {
                break;
            }
            obj = p;
            {
                int depth = 0;
                const char *end = p;
                do {
                    if (*end == '{') {
                        ++depth;
                    } else if (*end == '}') {
                        --depth;
                    }
                    ++end;
                } while (*end && depth > 0);
                {
                    char chunk[1536];
                    char lan_fp[256];
                    size_t len = (size_t)(end - obj);
                    if (len >= sizeof(chunk)) {
                        len = sizeof(chunk) - 1;
                    }
                    memcpy(chunk, obj, len);
                    chunk[len] = '\0';
                    memset(&g_lc.list[n], 0, sizeof(g_lc.list[n]));
                    g_lc.list[n].latency_ms = -1;
                    json_get_str(chunk, "lobby_id", g_lc.list[n].lobby_id, sizeof(g_lc.list[n].lobby_id));
                    json_get_str(chunk, "name", g_lc.list[n].name, sizeof(g_lc.list[n].name));
                    json_get_str(chunk, "game_name", g_lc.list[n].game_name, sizeof(g_lc.list[n].game_name));
                    json_get_str(chunk, "game_version", g_lc.list[n].game_version,
                                 sizeof(g_lc.list[n].game_version));
                    /* Missing version stays empty for filter decisions; display
                     * defaults to "dev" only after accept. Old servers omitted
                     * game_version — rewriting to "dev" before the strict pin
                     * hid those rows from release clients. */
                    const int has_game_version = g_lc.list[n].game_version[0] != '\0';
                    /* Drop lobbies that don't match our title (broadcast list
                     * is unfiltered). Release builds also pin game_version;
                     * "dev" keeps other versions visible for testing. */
                    if (g_lc.filter_game_name[0] &&
                        strcmp(g_lc.list[n].game_name, g_lc.filter_game_name) != 0) {
                        p = end;
                        continue;
                    }
                    if (list_filter_version_strict() && has_game_version) {
                        const char *want_ver = effective_game_version(NULL);
                        if (want_ver && want_ver[0] &&
                            strcmp(g_lc.list[n].game_version, want_ver) != 0) {
                            p = end;
                            continue;
                        }
                    }
                    if (!has_game_version) {
                        strncpy(g_lc.list[n].game_version, "dev",
                                sizeof(g_lc.list[n].game_version) - 1);
                    }
                    g_lc.list[n].player_count = json_get_int(chunk, "player_count", 0);
                    g_lc.list[n].max_slots = json_get_int(chunk, "max_slots", 2);
                    g_lc.list[n].has_password = json_get_bool(chunk, "has_password", 0);
                    json_get_str(chunk, "host_endpoint", g_lc.list[n].host_endpoint,
                                 sizeof(g_lc.list[n].host_endpoint));
                    g_lc.list[n].lan_count = json_parse_str_array(
                        chunk, "lan_endpoints", g_lc.list[n].lan_endpoints,
                        PSX_LOBBY_MAX_LAN_EPS);
                    lobby_row_lan_fingerprint(&g_lc.list[n], lan_fp, sizeof(lan_fp));
                    if (!want_probe) {
                        for (i = 0; i < prev_n; ++i) {
                            if (prev_ids[i][0] &&
                                strcmp(prev_ids[i], g_lc.list[n].lobby_id) == 0 &&
                                strcmp(prev_eps[i], g_lc.list[n].host_endpoint) == 0 &&
                                strcmp(prev_lan[i], lan_fp) == 0) {
                                g_lc.list[n].latency_ms = prev_ms[i];
                                break;
                            }
                        }
                    }
                    ++n;
                    p = end;
                }
            }
        }
        g_lc.list_count = n;
        if (want_probe)
            lobby_list_rtt_start(1);
        else {
            int need = 0;
            for (i = 0; i < n; ++i) {
                if (g_lc.list[i].latency_ms < 0 &&
                    (g_lc.list[i].host_endpoint[0] || g_lc.list[i].lan_count > 0)) {
                    need = 1;
                    break;
                }
            }
            /* Restart even if a prior burst is mid-flight — advertise may have
             * just published a public host_endpoint / LAN candidate. */
            if (need)
                lobby_list_rtt_start(0);
        }
        return;
    }
    if (strcmp(op, "created") == 0) {
        g_lc.in_lobby = 1;
        g_lc.is_host = 1;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        member_rtt_clear();
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 0);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = 1;
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lobby_max_slots);
        g_lc.join.last_error[0] = '\0';
        if (g_lc.player_id[0]) {
            strncpy(g_lc.host_player_id, g_lc.player_id, sizeof(g_lc.host_player_id) - 1);
            g_lc.host_player_id[sizeof(g_lc.host_player_id) - 1] = '\0';
        }
        ingest_host_player_id(json);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        if (g_lc.member_count == 0) {
            g_lc.members[0].slot = 0;
            strncpy(g_lc.members[0].player_id, g_lc.player_id, sizeof(g_lc.members[0].player_id) - 1);
            strncpy(g_lc.members[0].display_name, g_lc.display_name,
                    sizeof(g_lc.members[0].display_name) - 1);
            g_lc.members[0].ready = 0;
            g_lc.member_count = 1;
            g_lc.local_ready = 0;
        }
        /* After create: LAN beacon immediately; STUN for public host_endpoint. */
        g_host_adv_state = HOST_ADV_WAIT_TURN;
        g_host_adv_deadline_ms = lobby_mono_ms() + 500ull;
        lobby_lan_beacon_publish_update();
        return;
    }
    if (strcmp(op, "host_endpoint_ok") == 0) {
        return;
    }
    if (strcmp(op, "joined") == 0) {
        g_lc.in_lobby = 1;
        g_lc.is_host = 0;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        member_rtt_clear();
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 1);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", 2);
        g_lc.join.max_slots = json_get_int(json, "max_slots", 2);
        g_lc.join.last_error[0] = '\0';
        ingest_host_player_id(json);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        /* Prefer slots on joined when present; lobby_update usually follows. */
        parse_slots_array(json);
        return;
    }
    if (strcmp(op, "lobby_update") == 0) {
        g_lc.in_lobby = 1;
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        g_lc.all_ready = json_get_bool(json, "all_ready", 0);
        ingest_host_player_id(json);
        if (g_lc.host_player_id[0] && g_lc.player_id[0]) {
            g_lc.is_host = (strcmp(g_lc.host_player_id, g_lc.player_id) == 0);
        }
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        return;
    }
    if (strcmp(op, "launch") == 0) {
        char relay_endpoint[PSX_LOBBY_ENDPOINT_LEN];
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        relay_endpoint[0] = '\0';
        json_get_str(json, "relay_endpoint", relay_endpoint, sizeof(relay_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        ingest_match_caps_from_json(json);
        /* Prefer explicit relay_endpoint when the server opened input relay.
         * Apply after caps ingest: omitted force_input_relay must not leave
         * hosts on the hub path while guests dial the relay. */
        if (relay_endpoint[0] && !endpoint_port_is_zero(relay_endpoint)) {
            strncpy(g_lc.join.host_endpoint, relay_endpoint,
                    sizeof(g_lc.join.host_endpoint) - 1);
            g_lc.join.host_endpoint[sizeof(g_lc.join.host_endpoint) - 1] = '\0';
            strncpy(g_lc.join.guest_endpoint, relay_endpoint,
                    sizeof(g_lc.join.guest_endpoint) - 1);
            g_lc.join.guest_endpoint[sizeof(g_lc.join.guest_endpoint) - 1] = '\0';
            if (!g_lc.match_caps.valid)
                g_lc.match_caps.valid = 1;
            g_lc.match_caps.force_input_relay = 1;
        }
        fill_peer_bind_from_join();
        parse_slots_array(json);
        /* Guest must know host:port (or relay). Host may leave peer empty for
         * accept-first / host-as-relay. Server relay: every peer dials relay. */
        {
            const int force_relay = using_server_input_relay(&g_lc.join);
            const int seats = g_lc.join.player_count >= 2 ? g_lc.join.player_count
                                                         : g_lc.join.max_slots;
            const int host_hub =
                (g_lc.is_host && seats >= 3 && !force_relay) ? 1 : 0;
            const int peer_bad = !g_lc.join.peer_hostport[0] ||
                                 endpoint_port_is_zero(g_lc.join.peer_hostport);
            if (force_relay) {
                if (peer_bad) {
                    strncpy(g_lc.join.last_error, "missing_endpoints",
                            sizeof(g_lc.join.last_error) - 1);
                    g_lc.launch_pending = 0;
                    return;
                }
            } else if (!g_lc.join.host_endpoint[0] ||
                       (g_lc.is_host && !host_hub && !g_lc.join.guest_endpoint[0]) ||
                       (!g_lc.is_host && peer_bad)) {
                strncpy(g_lc.join.last_error, "missing_endpoints",
                        sizeof(g_lc.join.last_error) - 1);
                g_lc.launch_pending = 0;
                return;
            }
        }
        g_lc.join.last_error[0] = '\0';
        g_lc.launch_pending = 1;
        /* Accept ICE for this match; queue was idle (accept=0) during lobby. */
        g_lc.ice_signal_accept = 1;
        lobby_rtt_close(); /* free game UDP port for the session bind */
        rnet_lan_beacon_close(&g_lan_beacon_pub);
        lobby_host_advertise_reset();
        return;
    }
    if (strcmp(op, "signal") == 0) {
        char text_buf[2048];
        char from[PSX_LOBBY_ID_LEN];
        int type = json_get_int(json, "type", 0);
        int flag = json_get_int(json, "flag", 0);
        text_buf[0] = '\0';
        from[0] = '\0';
        json_get_str(json, "text", text_buf, sizeof(text_buf));
        json_get_str(json, "from_player_id", from, sizeof(from));
        /* Legacy WS RTT_PING/PONG ignored — waiting-room latency uses UDP
         * rnet_rtt_probe (peer path). REPORT still accepted from peers. */
        if (type == PSX_LOBBY_SIG_RTT_PING || type == PSX_LOBBY_SIG_RTT_PONG)
            return;
        if (type == PSX_LOBBY_SIG_RTT_REPORT) {
            int slot = member_slot_for_player(from);
            int ms = (int)strtol(text_buf, NULL, 10);
            if (slot >= 0 && slot < PSX_LOBBY_MAX_MEMBERS && ms >= 0 && ms <= 60000)
                g_lc.member_rtt_ms[slot] = ms;
            return;
        }
        enqueue_signal(type, flag, text_buf);
        (void)flag;
        return;
    }
    if (strcmp(op, "error") == 0) {
        char code[64];
        json_get_str(json, "code", code, sizeof(code));
        strncpy(g_lc.join.last_error, code, sizeof(g_lc.join.last_error) - 1);
        g_lc.join.last_error[sizeof(g_lc.join.last_error) - 1] = '\0';
        /* Create/join failures are fatal to the seat. In-lobby ops (kick/move
         * on an older server, not_host, …) must not clear join.ok or the room
         * looks abandoned after a rejected host action. */
        if (!g_lc.in_lobby ||
            strcmp(code, "bad_password") == 0 ||
            strcmp(code, "full") == 0 ||
            strcmp(code, "gone") == 0 ||
            strcmp(code, "already_in_lobby") == 0 ||
            strcmp(code, "lobby_limit") == 0 ||
            strcmp(code, "version_mismatch") == 0 ||
            strcmp(code, "game_mismatch") == 0) {
            g_lc.join.ok = 0;
        }
        return;
    }
    if (strcmp(op, "lobby_closed") == 0 || strcmp(op, "left") == 0 ||
        strcmp(op, "kicked") == 0) {
        lobby_rtt_close();
        lobby_host_advertise_reset();
        g_lc.in_lobby = 0;
        g_lc.is_host = 0;
        g_lc.host_player_id[0] = '\0';
        g_lc.member_count = 0;
        g_lc.local_ready = 0;
        g_lc.all_ready = 0;
        g_lc.launch_pending = 0;
        memset(&g_lc.join, 0, sizeof(g_lc.join));
        match_caps_clear(&g_lc.match_caps);
        member_rtt_clear();
        return;
    }
}

static int set_nonblock(int fd)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

int psx_lobby_connect(const char *ws_url)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;
    char key_raw[16];
    char key_b64[32];
    char req[512];
    int i;

    psx_lobby_disconnect();
#if defined(_WIN32)
    {
        static int wsa;
        if (!wsa) {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
            wsa = 1;
        }
    }
#endif
    if (parse_ws_url(ws_url ? ws_url : psx_lobby_default_url(), g_lc.host, sizeof(g_lc.host),
                     &g_lc.port, g_lc.path, sizeof(g_lc.path)) != 0) {
        return -1;
    }
    snprintf(portstr, sizeof(portstr), "%d", g_lc.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_lc.host, portstr, &hints, &res) != 0) {
        return -2;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return -3;
    }
    g_lc.fd = fd;
    for (i = 0; i < 16; ++i) {
        key_raw[i] = (char)(rand() & 0xff);
    }
    /* base64 16 bytes -> 24 chars; reuse server-side style via sha1 helper file's b64? */
    {
        static const char *B64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int o = 0;
        for (i = 0; i < 16; i += 3) {
            unsigned v = ((unsigned char)key_raw[i] << 16);
            if (i + 1 < 16) {
                v |= ((unsigned char)key_raw[i + 1] << 8);
            }
            if (i + 2 < 16) {
                v |= (unsigned char)key_raw[i + 2];
            }
            key_b64[o++] = B64[(v >> 18) & 63];
            key_b64[o++] = B64[(v >> 12) & 63];
            key_b64[o++] = (i + 1 < 16) ? B64[(v >> 6) & 63] : '=';
            key_b64[o++] = (i + 2 < 16) ? B64[v & 63] : '=';
        }
        key_b64[o] = '\0';
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             g_lc.path, g_lc.host, g_lc.port, key_b64);
    if (send(fd, req, (int)strlen(req), 0) < 0) {
        close(fd);
        g_lc.fd = -1;
        return -4;
    }
    set_nonblock(fd);
    g_lc.connected = 1;
    g_lc.handshake_done = 0;
    g_lc.rx_http_len = 0;
    return 0;
}

void psx_lobby_disconnect(void)
{
    lobby_rtt_close();
    lobby_list_rtt_close();
    lobby_lan_beacon_close_all();
    lobby_host_advertise_reset();
    g_list_rtt_on_next_list = 0;
    if (g_lc.fd >= 0) {
        close(g_lc.fd);
    }
    {
        char dname[PSX_LOBBY_NAME_LEN];
        strncpy(dname, g_lc.display_name, sizeof(dname) - 1);
        memset(&g_lc, 0, sizeof(g_lc));
        g_lc.fd = -1;
        strncpy(g_lc.display_name, dname, sizeof(g_lc.display_name) - 1);
        member_rtt_clear();
    }
}

int psx_lobby_connected(void)
{
    return g_lc.connected && g_lc.fd >= 0;
}

void psx_lobby_set_display_name(const char *name)
{
    if (!name) {
        return;
    }
    strncpy(g_lc.display_name, name, sizeof(g_lc.display_name) - 1);
    g_lc.display_name[sizeof(g_lc.display_name) - 1] = '\0';
}

const char *psx_lobby_display_name(void)
{
    return g_lc.display_name;
}

const char *psx_lobby_player_id(void)
{
    return g_lc.player_id;
}

static int first_guest_member_slot(void)
{
    int i;
    for (i = 0; i < g_lc.member_count; ++i) {
        if (!psx_lobby_member_is_host(&g_lc.members[i]))
            return g_lc.members[i].slot;
    }
    return -1;
}

/* UDP peer RTT for the waiting-room latency column (not WS signal RTT). */
static void lobby_rtt_ensure(void)
{
    const char *bind;
    const char *peer;

    if (!g_lc.in_lobby || g_lc.launch_pending || using_server_input_relay(&g_lc.join)) {
        lobby_rtt_close();
        return;
    }

    if (!g_rtt_probe) {
        bind = g_lc.my_bind[0] ? g_lc.my_bind : NULL;
        /* 3+ guests use ephemeral session binds; probe the same way. */
        if (!g_lc.is_host && g_lc.join.max_slots >= 3)
            bind = NULL;
        if (rnet_rtt_probe_open(&g_rtt_probe, bind) != 0)
            return;
    }

    peer = NULL;
    if (g_lc.join.peer_hostport[0] && !endpoint_port_is_zero(g_lc.join.peer_hostport))
        peer = g_lc.join.peer_hostport;
    else if (!g_lc.is_host && g_lc.join.host_endpoint[0] &&
             !endpoint_port_is_zero(g_lc.join.host_endpoint))
        peer = g_lc.join.host_endpoint;
    if (peer)
        (void)rnet_rtt_probe_set_peer(g_rtt_probe, peer);
}

static void lobby_rtt_tick(void)
{
    int ms = 0;
    int got;

    if (!g_lc.in_lobby || g_lc.launch_pending) {
        lobby_rtt_close();
        return;
    }
    if (using_server_input_relay(&g_lc.join)) {
        lobby_rtt_close();
        return;
    }

    lobby_rtt_ensure();
    if (!g_rtt_probe)
        return;

    got = rnet_rtt_probe_pump(g_rtt_probe, &ms);
    if (got == 1) {
        if (g_lc.is_host) {
            int slot = first_guest_member_slot();
            if (slot >= 0)
                g_lc.member_rtt_ms[slot] = ms;
        } else {
            int slot = local_member_slot();
            if (slot >= 0)
                g_lc.member_rtt_ms[slot] = ms;
            {
                char report[32];
                snprintf(report, sizeof(report), "%d", ms);
                (void)psx_lobby_send_signal(PSX_LOBBY_SIG_RTT_REPORT, 0, report);
            }
        }
    }

    {
        uint64_t now = lobby_mono_ms();
        if (now >= g_lc.rtt_next_ping_ms && rnet_rtt_probe_peer_known(g_rtt_probe)) {
            (void)rnet_rtt_probe_ping(g_rtt_probe);
            g_lc.rtt_next_ping_ms = now + 2500ull;
        }
    }
}

void psx_lobby_pump(void)
{
    char buf[4096];
#if defined(_WIN32)
    int n;
#else
    ssize_t n;
#endif
    if (!psx_lobby_connected()) {
        return;
    }
    if (!g_lc.handshake_done) {
        n = recv(g_lc.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (socket_would_block()) {
                return;
            }
            psx_lobby_disconnect();
            return;
        }
        if (n == 0) {
            psx_lobby_disconnect();
            return;
        }
        if (g_lc.rx_http_len + (size_t)n >= sizeof(g_lc.rx_http)) {
            psx_lobby_disconnect();
            return;
        }
        memcpy(g_lc.rx_http + g_lc.rx_http_len, buf, (size_t)n);
        g_lc.rx_http_len += (size_t)n;
        g_lc.rx_http[g_lc.rx_http_len] = '\0';
        {
            char *hdr_end = strstr(g_lc.rx_http, "\r\n\r\n");
            if (hdr_end) {
                size_t hdr_len;
                size_t leftover;
                if (!strstr(g_lc.rx_http, "101")) {
                    psx_lobby_disconnect();
                    return;
                }
                hdr_len = (size_t)(hdr_end - g_lc.rx_http) + 4;
                leftover = g_lc.rx_http_len > hdr_len ? g_lc.rx_http_len - hdr_len : 0;
                g_lc.handshake_done = 1;
                g_lc.ws_pending_len = 0;
                if (leftover > 0 && leftover <= sizeof(g_lc.ws_pending)) {
                    memcpy(g_lc.ws_pending, g_lc.rx_http + hdr_len, leftover);
                    g_lc.ws_pending_len = leftover;
                }
                g_lc.rx_http_len = 0;
                flush_pending();
                drain_ws_pending();
            }
        }
        return;
    }
    flush_pending();
    drain_ws_pending();
    /* Non-blocking recv into ws_pending + frame parse. Avoid MSG_PEEK /
     * MSG_WAITALL / temporary blocking — those break on Windows MinGW when
     * the socket stays O_NONBLOCK (list/create never see welcome/created). */
    for (;;) {
        size_t available = sizeof(g_lc.ws_pending) - g_lc.ws_pending_len;
        if (available == 0) {
            psx_lobby_disconnect();
            return;
        }
        n = recv(g_lc.fd,
                 (char *)g_lc.ws_pending + g_lc.ws_pending_len,
                 (int)available, 0);
        if (n < 0) {
            if (socket_would_block()) {
                break;
            }
            psx_lobby_disconnect();
            return;
        }
        if (n == 0) {
            psx_lobby_disconnect();
            return;
        }
        g_lc.ws_pending_len += (size_t)n;
        drain_ws_pending();
        if (!psx_lobby_connected()) {
            break;
        }
    }
    /* Host STUN advertise (may briefly close the waiting-room RTT sock). */
    lobby_host_advertise_tick();
    /* Local UDP broadcast: host announce / guest cache for list RTT. */
    lobby_lan_beacon_tick();
    /* Peer-path UDP latency for the lobby seat table. */
    lobby_rtt_tick();
    /* One-shot list latency after Refresh / lobby_list. */
    lobby_list_rtt_tick();
}

void psx_lobby_set_game_identity(const char *game_name, const char *game_version)
{
    if (game_name) {
        strncpy(g_lc.filter_game_name, game_name, sizeof(g_lc.filter_game_name) - 1);
        g_lc.filter_game_name[sizeof(g_lc.filter_game_name) - 1] = '\0';
    } else {
        g_lc.filter_game_name[0] = '\0';
    }
    if (game_version && game_version[0]) {
        strncpy(g_lc.filter_game_version, game_version, sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    } else {
        strncpy(g_lc.filter_game_version, PSX_GAME_VERSION,
                sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    }
}

const char *psx_lobby_game_version(void)
{
    return effective_game_version(NULL);
}

void psx_lobby_request_list(void)
{
    g_list_rtt_on_next_list = 1;
    queue_list_request();
    flush_pending();
}

int psx_lobby_list_count(void)
{
    return g_lc.list_count;
}

int psx_lobby_list_get(int index, PsxLobbyRow *out)
{
    if (!out || index < 0 || index >= g_lc.list_count) {
        return 0;
    }
    *out = g_lc.list[index];
    return 1;
}

int psx_lobby_create(const char *name, const char *game_name, const char *game_version,
                     const char *password, const char *host_bind,
                     const PsxLobbyMatchCaps *match_caps)
{
    char msg[1536];
    char caps_json[512];
    const char *gn;
    const char *gv;
    int n;
    if (!psx_lobby_connected()) {
        return -1;
    }
    gn = game_name && game_name[0] ? game_name
         : (g_lc.filter_game_name[0] ? g_lc.filter_game_name : "Game");
    gv = effective_game_version(game_version);
    if (game_name && game_name[0]) {
        psx_lobby_set_game_identity(game_name, gv);
    }
    strncpy(g_lc.my_bind, host_bind && host_bind[0] ? host_bind : "0.0.0.0:7777",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
    }
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"create\",\"name\":\"%s\",\"game_name\":\"%s\",\"game_version\":\"%s\","
                 "\"password\":\"%s\",\"max_slots\":%d,\"host_bind\":\"%s\",\"display_name\":\"%s\"%s}",
                 name && name[0] ? name : "Lobby", gn, gv,
                 password ? password : "", g_lobby_max_slots, g_lc.my_bind,
                 g_lc.display_name[0] ? g_lc.display_name : "Host", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_join(const char *lobby_id, const char *password, const char *guest_bind)
{
    char msg[1024];
    const char *gn;
    const char *gv;
    if (!psx_lobby_connected() || !lobby_id) {
        return -1;
    }
    gn = g_lc.filter_game_name;
    gv = effective_game_version(NULL);
    strncpy(g_lc.my_bind, guest_bind && guest_bind[0] ? guest_bind : "0.0.0.0:7778",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    snprintf(msg, sizeof(msg),
             "{\"op\":\"join\",\"lobby_id\":\"%s\",\"password\":\"%s\",\"guest_bind\":\"%s\","
             "\"display_name\":\"%s\",\"game_name\":\"%s\",\"game_version\":\"%s\"}",
             lobby_id, password ? password : "", g_lc.my_bind,
             g_lc.display_name[0] ? g_lc.display_name : "Guest",
             gn, gv);
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_leave(void)
{
    queue_send("{\"op\":\"leave\"}");
    flush_pending();
    lobby_rtt_close();
    lobby_host_advertise_reset();
    g_lc.in_lobby = 0;
    g_lc.is_host = 0;
    g_lc.host_player_id[0] = '\0';
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    g_lc.all_ready = 0;
    g_lc.launch_pending = 0;
    g_lc.ice_signal_accept = 0;
    psx_lobby_clear_signals();
    match_caps_clear(&g_lc.match_caps);
    member_rtt_clear();
    return 0;
}

int psx_lobby_kick(int slot)
{
    char msg[64];
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    if (slot < 0 || slot >= PSX_LOBBY_MAX_MEMBERS) {
        return -1;
    }
    snprintf(msg, sizeof(msg), "{\"op\":\"kick\",\"slot\":%d}", slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_move_member(int from_slot, int to_slot)
{
    char msg[96];
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    if (from_slot < 0 || from_slot >= PSX_LOBBY_MAX_MEMBERS ||
        to_slot < 0 || to_slot >= PSX_LOBBY_MAX_MEMBERS ||
        from_slot == to_slot) {
        return -1;
    }
    snprintf(msg, sizeof(msg),
             "{\"op\":\"move\",\"from_slot\":%d,\"to_slot\":%d}",
             from_slot, to_slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_in_lobby(void)
{
    return g_lc.in_lobby;
}

int psx_lobby_is_host(void)
{
    return g_lc.is_host;
}

const char *psx_lobby_host_player_id(void)
{
    return g_lc.host_player_id;
}

const PsxLobbyJoinInfo *psx_lobby_join_info(void)
{
    return &g_lc.join;
}

const PsxLobbyMatchCaps *psx_lobby_match_caps(void)
{
    return &g_lc.match_caps;
}

int psx_lobby_set_match_caps(const PsxLobbyMatchCaps *caps)
{
    char msg[768];
    char caps_json[512];
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host || !caps || !caps->valid)
        return -1;
    g_lc.match_caps = *caps;
    caps_json[0] = '\0';
    append_match_caps_json(caps_json, sizeof(caps_json), caps);
    /* caps_json begins with a comma — strip it for a standalone object field. */
    n = snprintf(msg, sizeof(msg), "{\"op\":\"set_match_caps\"%s}", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_member_count(void)
{
    return g_lc.member_count;
}

int psx_lobby_member_get(int index, PsxLobbyMember *out)
{
    if (!out || index < 0 || index >= g_lc.member_count) {
        return 0;
    }
    *out = g_lc.members[index];
    return 1;
}

int psx_lobby_member_latency_ms(int slot)
{
    if (slot < 0 || slot >= PSX_LOBBY_MAX_MEMBERS)
        return -1;
    if (g_lc.host_player_id[0]) {
        int i;
        for (i = 0; i < g_lc.member_count; ++i) {
            if (g_lc.members[i].slot == slot &&
                strcmp(g_lc.members[i].player_id, g_lc.host_player_id) == 0)
                return -1; /* host row */
        }
    }
    return g_lc.member_rtt_ms[slot];
}

int psx_lobby_member_is_host(const PsxLobbyMember *member)
{
    const char *host_id;
    if (!member || !member->player_id[0])
        return 0;
    host_id = psx_lobby_host_player_id();
    return host_id && host_id[0] && strcmp(member->player_id, host_id) == 0;
}

int psx_lobby_local_ready(void)
{
    return g_lc.local_ready;
}

int psx_lobby_all_ready(void)
{
    return g_lc.all_ready != 0 && g_lc.in_lobby && g_lc.join.player_count >= 2;
}

int psx_lobby_set_ready(int ready)
{
    char msg[64];
    if (!psx_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    snprintf(msg, sizeof(msg), "{\"op\":\"set_ready\",\"ready\":%s}", ready ? "true" : "false");
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_request_start(const PsxLobbyMatchCaps *match_caps)
{
    char msg[768];
    char caps_json[512];
    int n;
    if (!psx_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
    }
    n = snprintf(msg, sizeof(msg), "{\"op\":\"start\"%s}", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int psx_lobby_launch_pending(void)
{
    return g_lc.launch_pending;
}

void psx_lobby_clear_launch_pending(void)
{
    g_lc.launch_pending = 0;
}

int psx_lobby_send_signal(int type, int flag, const char *text)
{
    char esc[4096];
    char msg[4608];
    const char *lid;
    if (!psx_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    lid = g_lc.join.lobby_id[0] ? g_lc.join.lobby_id : "";
    json_escape(text ? text : "", esc, sizeof(esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"signal\",\"lobby_id\":\"%s\",\"to_player_id\":\"\","
             "\"type\":%d,\"flag\":%d,\"text\":\"%s\"}",
             lid, type, flag, esc);
    /* Write immediately — ICE candidates arrive in bursts larger than pending_tx. */
    if (g_lc.handshake_done && g_lc.fd >= 0) {
        if (rnet_ws_write_text(g_lc.fd, msg, 1) < 0)
            return -1;
        return 0;
    }
    queue_send(msg);
    return 0;
}

int psx_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    int i;
    if (g_lc.sig_count <= 0) {
        return 0;
    }
    i = g_lc.sig_head;
    if (type) *type = g_lc.sig_q[i].type;
    if (flag) *flag = g_lc.sig_q[i].flag;
    if (text && text_cap) {
        strncpy(text, g_lc.sig_q[i].text, text_cap - 1);
        text[text_cap - 1] = '\0';
    }
    g_lc.sig_head = (g_lc.sig_head + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
    g_lc.sig_count--;
    return 1;
}

int psx_lobby_request_turn_credentials(void)
{
    if (!psx_lobby_connected())
        return -1;
    if (g_lc.turn.valid && g_lc.turn_received_at > 0 && g_lc.turn.ttl_secs > 0) {
        time_t now = time(NULL);
        if (now >= g_lc.turn_received_at &&
            (uint32_t)(now - g_lc.turn_received_at) + 60u < g_lc.turn.ttl_secs) {
            return 0; /* still fresh (60s skew margin) */
        }
    }
    return queue_turn_credentials_request();
}

const PsxLobbyTurnCredentials *psx_lobby_turn_credentials(void)
{
    if (g_lc.turn.valid && g_lc.turn_received_at > 0 && g_lc.turn.ttl_secs > 0) {
        time_t now = time(NULL);
        if (now < g_lc.turn_received_at ||
            (uint32_t)(now - g_lc.turn_received_at) >= g_lc.turn.ttl_secs) {
            clear_turn_credentials();
        }
    }
    return &g_lc.turn;
}

#endif /* PSX_HAS_LOBBY_CLIENT */
