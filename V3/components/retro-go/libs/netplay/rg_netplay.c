#ifdef RG_ENABLE_NETPLAY

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/ip_addr.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "rg_system.h"
#include "rg_netplay.h"

// ─── Configuration ─────────────────────────────────────────────────────────────
// 0x02 = frame protocol for the serial link (magic/type/seq/data).
// A device speaking 0x01 talks a different language on port 1235; the
// version check in the INFO handler then politely refuses the connection.
#define NETPLAY_VERSION  0x04

// Serial link frames — values equal to RG_LINK_* in rg_netplay.h
#define LINK_MAGIC       0xA5
#define LINK_FRAME_LEN   4
#define MAX_PLAYERS      8
#define WIFI_SSID        "BLOCKBOY"
// WPA2 instead of an open AP: prevents a random phone that auto-joins open
// networks from occupying the only connection slot (max_connection=1).
// Must be identical across all firmwares — players never notice it.
#define WIFI_PASS        "BlockBoyLink26"
#define WIFI_CHANNEL     6
#define WIFI_CTRL_PORT   1234
#define WIFI_SERIAL_PORT 1235
#define STA_RETRY_MAX    10

// ─── WiFi event flags (set in event_handler, processed in netplay_task) ───────
#define EV_AP_STARTED  (1 << 0)
#define EV_AP_STOPPED  (1 << 1)
#define EV_STA_DISCN   (1 << 2)
#define EV_STA_GOT_IP  (1 << 3)
#define EV_AP_STA_IP   (1 << 4)

static volatile uint32_t wifi_events        = 0;
static volatile int      sta_retries        = 0;
static uint32_t          ap_guest_ip        = 0;
static int64_t           handshake_retry_ts = 0;
static volatile bool     serial_broken      = false;

// Receive buffer for the link frame currently arriving. TCP may split a frame
// across multiple segments, so half a frame must survive between two calls to
// rg_netplay_link_recv().
static uint8_t           link_rx_buf[LINK_FRAME_LEN];
static unsigned          link_rx_have       = 0;

// Tail of a frame that did not go through in one go. It is completed on the
// next send, so the partner stays on a frame boundary.
static uint8_t           link_tx_buf[LINK_FRAME_LEN];
static int               link_tx_left       = 0;

// ─── State ────────────────────────────────────────────────────────────────────
static netplay_status_t   netplay_status   = NETPLAY_STATUS_NOT_INIT;
static netplay_mode_t     netplay_mode     = NETPLAY_MODE_NONE;
static netplay_callback_t netplay_callback = NULL;
static SemaphoreHandle_t  netplay_sync     = NULL;

static netplay_player_t  players[MAX_PLAYERS];
static netplay_player_t *local_player  = NULL;
static netplay_player_t *remote_player = NULL;

static wifi_config_t wifi_cfg;
static esp_netif_t  *netif_ap  = NULL;
static esp_netif_t  *netif_sta = NULL;

// ─── Sockets ──────────────────────────────────────────────────────────────────
static int ctrl_rx_sock    = -1;
static int ctrl_tx_sock    = -1;
static int tcp_listen_sock = -1;
static int serial_sock     = -1;


// ─── Interne hulpfuncties ─────────────────────────────────────────────────────

static void set_status(netplay_status_t s)
{
    bool changed = (s != netplay_status);
    netplay_status = s;
    if (changed && netplay_callback)
        netplay_callback(RG_EVENT_TYPE_NETPLAY | NETPLAY_EVENT_STATUS_CHANGED, &netplay_status);
}

static void send_ctrl_packet(uint32_t dest_ip, uint8_t cmd, uint8_t arg,
                              void *data, uint8_t data_len)
{
    if (ctrl_tx_sock < 0 || !local_player) return;

    netplay_packet_t pkt = {
        .player_id = local_player->id,
        .cmd       = cmd,
        .arg       = arg,
        .data_len  = data_len,
    };
    if (data_len > 0) memcpy(pkt.data, data, data_len);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(WIFI_CTRL_PORT),
        .sin_addr.s_addr = dest_ip,
    };
    size_t len = sizeof(pkt) - sizeof(pkt.data) + data_len;
    if (sendto(ctrl_tx_sock, &pkt, len, 0, (struct sockaddr *)&addr, sizeof addr) <= 0)
        RG_LOGE("netplay: sendto() mislukt: %d\n", errno);
}

// Atomic close: prevents a double close of the same fd from two cores.
// A Pokemon trade can trigger a gnuboy timeout on Core 0 (rg_netplay_close_serial)
// while netplay_task on Core 1 also runs network_cleanup → double close of the
// same fd → lwIP can deadlock.
static portMUX_TYPE close_mux = portMUX_INITIALIZER_UNLOCKED;

static void close_fd(int *fd)
{
    int local_fd = -1;
    portENTER_CRITICAL(&close_mux);
    if (*fd >= 0) { local_fd = *fd; *fd = -1; }
    portEXIT_CRITICAL(&close_mux);
    if (local_fd >= 0) close(local_fd);
}

static void network_cleanup(void)
{
    close_fd(&ctrl_rx_sock);
    close_fd(&ctrl_tx_sock);
    close_fd(&tcp_listen_sock);
    close_fd(&serial_sock);
    link_rx_have = 0;
    link_tx_left = 0;
}

static void setup_local_player(esp_netif_ip_info_t *ip)
{
    int pid = (int)esp_ip4_addr4_16(&ip->ip) - 1;
    if (pid < 0) pid = 0;

    uint32_t game_id = 5381;
    const char *p = rg_system_get_app()->romPath;
    if (p) while (*p) game_id = game_id * 33 + (uint8_t)*p++;

    local_player = &players[pid];
    local_player->id      = (uint8_t)pid;
    local_player->version = NETPLAY_VERSION;
    local_player->game_id = game_id;
    local_player->ip_addr = ip->ip.addr;

    RG_LOGI("netplay: Player %d\n", pid);
}

static int create_ctrl_sockets(void)
{
    struct sockaddr_in rx = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(WIFI_CTRL_PORT),
    };
    ctrl_rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ctrl_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctrl_rx_sock < 0 || ctrl_tx_sock < 0) return -1;

    int bc = 1;
    setsockopt(ctrl_tx_sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof bc);

    if (bind(ctrl_rx_sock, (struct sockaddr *)&rx, sizeof rx) < 0) {
        RG_LOGE("netplay: UDP bind mislukt: %d\n", errno);
        return -1;
    }
    return 0;
}


// ─── Netwerk setup ────────────────────────────────────────────────────────────

static void setup_serial_sock_opts(int fd)
{
    int nd = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
    // TCP keepalive: between matches no serial traffic flows and with
    // WIFI_PS_NONE the guest sends no PSM frames either. Without keepalive
    // the softAP deauths the station after the inactivity default (300s) and
    // the link is dead after a ~5 min pause. A probe every 30s idle keeps the
    // WiFi layer active AND detects a dead partner within ~1 min.
    int ka = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &ka,    sizeof ka);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof cnt);
    // Only O_NONBLOCK — SO_SNDTIMEO caused premature EAGAIN via lwIP mutex
    // waits under busy WiFi traffic (bug #4).
    fcntl(fd, F_SETFL, O_NONBLOCK);
    link_rx_have = 0;  // fresh session: don't carry half a frame over from a previous one
    link_tx_left = 0;
}

static void network_setup_host(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(netif_ap, &ip);
    setup_local_player(&ip);

    if (create_ctrl_sockets() < 0) return;

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(WIFI_SERIAL_PORT),
    };
    tcp_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_listen_sock >= 0) {
        int reuse = 1;
        setsockopt(tcp_listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        bind(tcp_listen_sock, (struct sockaddr *)&sa, sizeof sa);
        listen(tcp_listen_sock, 1);
        RG_LOGI("netplay: TCP serial luistert op poort %d\n", WIFI_SERIAL_PORT);
    }

    set_status(NETPLAY_STATUS_LISTENING);
}

static void network_setup_guest(void)
{
    esp_wifi_set_ps(WIFI_PS_NONE); // disable PSM — prevents TX stalls on serial data
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(netif_sta, &ip);
    setup_local_player(&ip);

    if (create_ctrl_sockets() < 0) return;

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = ip.gw.addr,
        .sin_port        = htons(WIFI_SERIAL_PORT),
    };

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool ok = false;

    for (int i = 0; i < 20 && !ok; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0)
            ok = true;
        else
            vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (ok) {
        setup_serial_sock_opts(fd);
        serial_sock = fd;
        RG_LOGI("netplay: TCP serial connected to host\n");
    } else {
        RG_LOGE("netplay: TCP serial connection failed (errno=%d)\n", errno);
        close(fd);
    }

    set_status(NETPLAY_STATUS_HANDSHAKE);
}


// ─── WiFi event handler ───────────────────────────────────────────────────────

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_AP_START:             wifi_events |= EV_AP_STARTED; break;
            case WIFI_EVENT_AP_STOP:              wifi_events |= EV_AP_STOPPED; break;
            case WIFI_EVENT_STA_CONNECTED:        sta_retries = 0;              break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                // reason code says WHY: 4=INACTIVITY, 8=partner gone, 200=BEACON_TIMEOUT, 201=NO_AP_FOUND
                wifi_event_sta_disconnected_t *ev = data;
                RG_LOGW("WiFi: STA connection lost (reason=%d rssi=%d)\n", ev->reason, ev->rssi);
                wifi_events |= EV_STA_DISCN;
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *ev = data;
                RG_LOGW("WiFi: GUEST WiFi layer interrupted (reason=%d, TCP session guards the link)\n", ev->reason);
                break;
            }
        }
    } else if (base == IP_EVENT) {
        switch (id) {
            case IP_EVENT_STA_GOT_IP:
                wifi_events |= EV_STA_GOT_IP;
                break;
            case IP_EVENT_AP_STAIPASSIGNED: {
                ip_event_ap_staipassigned_t *ev = data;
                ap_guest_ip = ev->ip.addr;
                wifi_events |= EV_AP_STA_IP;
                break;
            }
        }
    }
}


// ─── Pakket verwerking ────────────────────────────────────────────────────────

static void handle_packet(const netplay_packet_t *pkt, int len)
{
    int expected = (int)(sizeof(*pkt) - sizeof(pkt->data)) + pkt->data_len;
    if (expected != len || pkt->player_id >= MAX_PLAYERS || pkt->player_id == local_player->id)
        return;

    netplay_player_t *from = &players[pkt->player_id];
    from->last_contact = (uint32_t)rg_system_timer();

    switch (pkt->cmd)
    {
        case NETPLAY_PACKET_INFO:
            if (pkt->data_len != sizeof(netplay_player_t)) break;
            memcpy(from, pkt->data, sizeof(netplay_player_t));
            remote_player = from;

            RG_LOGI("netplay: Remote player %d game_id=%08"PRIx32"\n",
                    from->id, from->game_id);

            if (from->version != NETPLAY_VERSION) {
                RG_LOGE("netplay: Protocol versie mismatch\n");
                break;
            }

            if (netplay_mode == NETPLAY_MODE_HOST) {
                send_ctrl_packet(from->ip_addr, NETPLAY_PACKET_READY, 0, NULL, 0);
                if (serial_sock >= 0)
                    set_status(NETPLAY_STATUS_CONNECTED);
            } else {
                send_ctrl_packet(from->ip_addr, NETPLAY_PACKET_INFO, 1,
                                 local_player, sizeof(netplay_player_t));
            }
            break;

        case NETPLAY_PACKET_READY:
            set_status(NETPLAY_STATUS_CONNECTED);
            break;

        case NETPLAY_PACKET_SYNC_REQ:
            memcpy(from->sync_data, pkt->data, pkt->data_len);
            xSemaphoreGive(netplay_sync);
            break;

        case NETPLAY_PACKET_SYNC_ACK:
            memcpy(from->sync_data, pkt->data, pkt->data_len);
            if (remote_player)
                send_ctrl_packet(remote_player->ip_addr, NETPLAY_PACKET_SYNC_DONE, pkt->arg, NULL, 0);
            xSemaphoreGive(netplay_sync);
            break;

        case NETPLAY_PACKET_SYNC_DONE:
            xSemaphoreGive(netplay_sync);
            break;

        default:
            RG_LOGE("netplay: Onbekend pakket cmd=0x%02x\n", pkt->cmd);
    }
}


// ─── Hoofd-task ───────────────────────────────────────────────────────────────

static void netplay_task(void *arg)
{
    RG_LOGI("netplay: Task started\n");

    while (1)
    {
        uint32_t ev = wifi_events;

        if (ev & EV_AP_STOPPED) {
            wifi_events &= ~EV_AP_STOPPED;
            set_status(NETPLAY_STATUS_STOPPED);
        }
        if (ev & EV_AP_STARTED) {
            wifi_events &= ~EV_AP_STARTED;
            network_setup_host();
        }
        if (ev & EV_STA_DISCN) {
            wifi_events &= ~EV_STA_DISCN;
            if (sta_retries < STA_RETRY_MAX) {
                RG_LOGI("netplay: STA herverbinding %d/%d\n", ++sta_retries, STA_RETRY_MAX);
                esp_wifi_connect();
            } else {
                set_status(NETPLAY_STATUS_DISCONNECTED);
            }
        }
        if (ev & EV_AP_STA_IP) {
            wifi_events &= ~EV_AP_STA_IP;
            vTaskDelay(pdMS_TO_TICKS(200));
            send_ctrl_packet(ap_guest_ip, NETPLAY_PACKET_INFO, 0,
                             local_player, sizeof(netplay_player_t));
            set_status(NETPLAY_STATUS_HANDSHAKE);
        }
        if (ev & EV_STA_GOT_IP) {
            wifi_events &= ~EV_STA_GOT_IP;
            // On an STA reconnect mid-session (WiFi hiccup) the socket setup
            // already exists — calling create_ctrl_sockets() again leaks 2 fds
            // per hiccup (bind fails on the already-bound port 1234) and leaves
            // ctrl_rx_sock unbound. The TCP session itself just lives on.
            if (ctrl_rx_sock < 0)
                network_setup_guest();
            else
                RG_LOGI("netplay: STA reconnected, existing session kept\n");
        }

        if (ctrl_rx_sock < 0 || netplay_status < NETPLAY_STATUS_HANDSHAKE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Handshake UDP retry
        if (netplay_status == NETPLAY_STATUS_HANDSHAKE && local_player != NULL) {
            int64_t now = rg_system_timer();
            if (now - handshake_retry_ts > 2000000LL) {
                handshake_retry_ts = now;
                if (netplay_mode == NETPLAY_MODE_HOST) {
                    if (remote_player == NULL && ap_guest_ip) {
                        RG_LOGI("netplay: retry INFO → gast\n");
                        send_ctrl_packet(ap_guest_ip, NETPLAY_PACKET_INFO, 0,
                                         local_player, sizeof(netplay_player_t));
                    } else if (remote_player != NULL) {
                        RG_LOGI("netplay: retry READY → gast\n");
                        send_ctrl_packet(remote_player->ip_addr, NETPLAY_PACKET_READY,
                                         0, NULL, 0);
                    }
                } else if (netplay_mode == NETPLAY_MODE_GUEST && remote_player != NULL) {
                    RG_LOGI("netplay: retry INFO → host\n");
                    send_ctrl_packet(remote_player->ip_addr, NETPLAY_PACKET_INFO, 1,
                                     local_player, sizeof(netplay_player_t));
                }
            }
        }

        // TCP socket breuk detectie
        if (serial_broken && netplay_status == NETPLAY_STATUS_CONNECTED) {
            serial_broken = false;
            RG_LOGI("netplay: TCP serial breuk → verbinding verbreken\n");
            set_status(NETPLAY_STATUS_DISCONNECTED);
            network_cleanup();
        }

        // Non-blocking accept (host)
        if (tcp_listen_sock >= 0 && serial_sock < 0) {
            fd_set fds;
            struct timeval tv = {0, 0};
            FD_ZERO(&fds);
            FD_SET(tcp_listen_sock, &fds);
            if (select(tcp_listen_sock + 1, &fds, NULL, NULL, &tv) > 0) {
                serial_sock = accept(tcp_listen_sock, NULL, NULL);
                if (serial_sock >= 0) {
                    setup_serial_sock_opts(serial_sock);
                    close_fd(&tcp_listen_sock); // close the listen socket after accept — we accept only one connection
                    RG_LOGI("netplay: TCP serial accepted\n");
                    if (remote_player != NULL && netplay_status == NETPLAY_STATUS_HANDSHAKE)
                        set_status(NETPLAY_STATUS_CONNECTED);
                }
            }
        }

        // Wait at most 100ms for a control packet
        fd_set fds;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        FD_ZERO(&fds);
        FD_SET(ctrl_rx_sock, &fds);
        if (select(ctrl_rx_sock + 1, &fds, NULL, NULL, &tv) <= 0)
            continue;

        netplay_packet_t pkt;
        memset(&pkt, 0, sizeof pkt);
        int len = recvfrom(ctrl_rx_sock, &pkt, sizeof pkt, 0, NULL, 0);
        if (len > 0) handle_packet(&pkt, len);
    }
}


// ─── WiFi initialisatie (eenmalig) ───────────────────────────────────────────

static void init_wifi_once(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    netif_ap  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (!netif_ap || !netif_sta) {
        esp_netif_init();
        esp_event_loop_create_default();
        if (!netif_ap)  netif_ap  = esp_netif_create_default_wifi_ap();
        if (!netif_sta) netif_sta = esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_rx_buf_num  = 4;
        cfg.dynamic_rx_buf_num = 8;
        cfg.dynamic_tx_buf_num = 8;
        esp_wifi_init(&cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_wifi_set_ps(WIFI_PS_NONE);

    rg_task_create("rg_netplay", netplay_task, NULL, 5120, RG_TASK_PRIORITY_5, 1);
}


// ─── Publieke API ─────────────────────────────────────────────────────────────

void rg_netplay_init(netplay_callback_t callback)
{
    netplay_callback = callback;
}


bool rg_netplay_start(netplay_mode_t mode)
{
    if (netplay_status == NETPLAY_STATUS_NOT_INIT) {
        netplay_status = NETPLAY_STATUS_STOPPED;
        netplay_sync   = xSemaphoreCreateMutex();
        init_wifi_once();
    } else if (netplay_mode != NETPLAY_MODE_NONE) {
        rg_netplay_stop();
    }

    memset(players, 0xFF, sizeof players);
    local_player       = NULL;
    remote_player      = NULL;
    netplay_mode       = mode;
    sta_retries        = 0;
    handshake_retry_ts = 0;
    serial_broken      = false;

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_events = 0;

    esp_err_t ret = ESP_FAIL;

    if (mode == NETPLAY_MODE_HOST) {
        RG_LOGI("netplay: Host mode\n");
        strncpy((char *)wifi_cfg.ap.ssid, WIFI_SSID, sizeof wifi_cfg.ap.ssid);
        strncpy((char *)wifi_cfg.ap.password, WIFI_PASS, sizeof wifi_cfg.ap.password);
        wifi_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
        wifi_cfg.ap.channel        = WIFI_CHANNEL;
        wifi_cfg.ap.max_connection = 1;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
        ret = esp_wifi_start();
        // Default AP inactivity is 300s: without data traffic (a pause
        // between matches) the guest is deauthed after exactly 5 min. Stretch
        // to the max; TCP keepalive keeps the link active every 30s anyway.
        if (ret == ESP_OK) {
            esp_err_t e = esp_wifi_set_inactive_time(WIFI_IF_AP, 65535);
            uint16_t sec = 0;
            esp_wifi_get_inactive_time(WIFI_IF_AP, &sec);
            RG_LOGI("netplay: AP inactive time=%us (set err=0x%x)\n", sec, e);
        }
    } else if (mode == NETPLAY_MODE_GUEST) {
        RG_LOGI("netplay: Guest mode\n");
        strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof wifi_cfg.sta.ssid);
        strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof wifi_cfg.sta.password);
        wifi_cfg.sta.channel = WIFI_CHANNEL;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ret = esp_wifi_connect();
    }

    // Move past STOPPED immediately so the UI status loop doesn't briefly show
    // "Connection failed!" while waiting for async WiFi events to arrive. The
    // real state transitions (LISTENING from netplay_task, HANDSHAKE on packet
    // arrival, CONNECTED on accept) will overwrite this within a few ms.
    if (ret == ESP_OK)
        set_status(mode == NETPLAY_MODE_HOST ? NETPLAY_STATUS_LISTENING
                                              : NETPLAY_STATUS_CONNECTING);

    return ret == ESP_OK;
}


bool rg_netplay_stop(void)
{
    if (netplay_mode == NETPLAY_MODE_NONE) return false;

    network_cleanup();
    esp_wifi_stop();
    netplay_status = NETPLAY_STATUS_STOPPED;
    netplay_mode   = NETPLAY_MODE_NONE;
    if (netplay_sync) xSemaphoreGive(netplay_sync);

    return true;
}


bool rg_netplay_quick_start(void)
{
    rg_display_clear(0);

    const rg_gui_option_t options[] = {
        {1, _("Host Game (P1)"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("Find Game (P2)"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };

    int ret = rg_gui_dialog(_("Game Link"), options, 0);

    if (ret == 1)
        rg_netplay_start(NETPLAY_MODE_HOST);
    else if (ret == 2)
        rg_netplay_start(NETPLAY_MODE_GUEST);
    else
        return false;

    const char *status_msg = _("Initializing...");
    const char *prev_msg   = NULL;
    int64_t connect_start  = rg_system_timer();

    while (1) {
        switch (netplay_status) {
            case NETPLAY_STATUS_CONNECTED:
                return !remote_player
                    || remote_player->game_id == local_player->game_id
                    || rg_gui_confirm(_("Game Link"), _("ROMs differ. Continue?"), 1);
            case NETPLAY_STATUS_HANDSHAKE:    status_msg = _("Exchanging info...");   break;
            case NETPLAY_STATUS_CONNECTING:   status_msg = _("Connecting...");        break;
            case NETPLAY_STATUS_LISTENING:    status_msg = _("Waiting for player..."); break;
            case NETPLAY_STATUS_DISCONNECTED: status_msg = _("Host not found!");      break;
            case NETPLAY_STATUS_STOPPED:      status_msg = _("Connection failed!");   break;
            default:                          status_msg = _("Please wait...");
        }
        if (status_msg != prev_msg) {
            rg_display_clear(0);
            rg_gui_draw_dialog(status_msg, NULL, 0);
            prev_msg = status_msg;
        }
        if (rg_input_key_is_pressed(RG_KEY_B)) break;
        if (rg_system_timer() - connect_start > 20000000LL) {
            RG_LOGI("netplay: Connection timeout after 20s\n");
            break;
        }
        rg_task_delay(10);
    }

    rg_netplay_stop();
    return false;
}


void rg_netplay_sync(void *data_in, void *data_out, uint8_t data_len)
{
    if (netplay_status != NETPLAY_STATUS_CONNECTED) return;

    memcpy(local_player->sync_data, data_in, data_len);

    if (netplay_mode == NETPLAY_MODE_HOST)
        send_ctrl_packet(remote_player->ip_addr, NETPLAY_PACKET_SYNC_REQ, 0, data_in, data_len);

    if (xSemaphoreTake(netplay_sync, pdMS_TO_TICKS(10000)) != pdPASS) {
        RG_LOGE("netplay: Sync timeout\n");
        rg_netplay_stop();
        return;
    }

    memcpy(data_out, remote_player->sync_data, data_len);

    if (netplay_mode == NETPLAY_MODE_GUEST) {
        send_ctrl_packet(remote_player->ip_addr, NETPLAY_PACKET_SYNC_ACK, 0,
                         local_player->sync_data, sizeof local_player->sync_data);
        xSemaphoreTake(netplay_sync, pdMS_TO_TICKS(1000));
    }
}


// Send one link frame. On EAGAIN (full TCP buffer during a retransmit burst)
// keep trying briefly: the callers in hw.c/cpu.c ignore the return value, so a
// silently lost frame makes the partner wait until its safety net trips. A
// half-sent frame is worse than no frame — the partner's stream is then no
// longer on a frame boundary — so we treat that as session loss instead of
// letting it silently desync.
// Writes as much of buf as possible. Returns how much is left over.
static int link_tx_write(const uint8_t *buf, int len)
{
    int sent = 0;
    for (int attempt = 0; attempt < 20 && sent < len; attempt++) {
        int n = send(serial_sock, buf + sent, len - sent, MSG_DONTWAIT);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return len - sent;
}

int rg_netplay_link_send(uint8_t type, uint8_t seq, uint8_t data)
{
    if (serial_sock < 0)
        return -1;

    // First the tail of a previous frame that did not fully go through.
    // Without this the partner would lose the frame boundary. Dropping the
    // session over it is wrong: at 300 exchanges per second a partial send()
    // under WiFi pressure is completely normal, and it used to cost the link
    // mid-match.
    if (link_tx_left > 0) {
        int left = link_tx_write(link_tx_buf + (LINK_FRAME_LEN - link_tx_left), link_tx_left);
        link_tx_left = left;
        if (left > 0) {
            // Still clogged. Dropping this frame is recoverable (the game
            // retries, or the safety net steps in); tearing the stream is not.
            RG_LOGW("link_send: output clogged, frame type=%u seq=%u skipped\n", type, seq);
            return -1;
        }
    }

    uint8_t frame[LINK_FRAME_LEN] = { LINK_MAGIC, type, seq, data };
    int left = link_tx_write(frame, LINK_FRAME_LEN);

    if (left == 0)
        return 0;

    if (left < LINK_FRAME_LEN) {
        // Half sent: keep the rest and finish it on the next call.
        memcpy(link_tx_buf, frame, LINK_FRAME_LEN);
        link_tx_left = left;
        return -1;
    }

    RG_LOGW("link_send: frame type=%u seq=%u data=0x%02X not sent (errno=%d)\n",
            type, seq, data, errno);
    return -1;
}

// Deliver the next complete frame. Nothing is discarded: ordering stays
// intact and the caller decides via seq what is stale. A half-received frame
// survives between two calls.
bool rg_netplay_link_recv(uint8_t *type, uint8_t *seq, uint8_t *data)
{
    if (serial_sock < 0) {
        link_rx_have = 0;
        return false;
    }

    while (link_rx_have < LINK_FRAME_LEN) {
        int n = recv(serial_sock, link_rx_buf + link_rx_have,
                     LINK_FRAME_LEN - link_rx_have, MSG_DONTWAIT);
        if (n > 0) {
            link_rx_have += n;
            // Resynchronize: byte 0 must be the magic. Shift until it is, so
            // a torn stream heals itself instead of endlessly producing
            // nonsense frames.
            while (link_rx_have > 0 && link_rx_buf[0] != LINK_MAGIC) {
                memmove(link_rx_buf, link_rx_buf + 1, --link_rx_have);
                RG_LOGW("netplay: resynchronizing link stream\n");
            }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            if (!serial_broken) {
                serial_broken = true;
                RG_LOGI("netplay: TCP socket closed (n=%d errno=%d)\n", n, errno);
            }
        }
        return false;  // no complete frame (yet)
    }

    *type = link_rx_buf[1];
    *seq  = link_rx_buf[2];
    *data = link_rx_buf[3];
    link_rx_have = 0;
    return true;
}

void rg_netplay_close_serial(void)
{
    // No close() from emulator task — cross-task race with netplay_task
    // which may also call close() in network_cleanup. Signal disconnect only;
    // netplay_task sees serial_broken and does the actual close.
    serial_broken = true;
}

netplay_mode_t rg_netplay_mode(void)
{
    return netplay_mode;
}

netplay_status_t rg_netplay_status(void)
{
    return netplay_status;
}

#endif // RG_ENABLE_NETPLAY
