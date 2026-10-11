#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
// Zig's optimized builds can define NDEBUG. Checks must always execute,
// including the socket operations inside them.
#undef assert
#define assert(test) do { if (!(test)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); exit(1); } } while (0)
#include "../../components/retro-go/libs/netplay/rg_netplay.h"
#define LINK_MAGIC 0xA5
#define LINK_FRAME_LEN 4
#define LINK_TX_CAPACITY 256
#define MSG_DONTWAIT 0
#define RG_LOGW(...) ((void)0)
#define RG_LOGI(...) ((void)0)
#define RG_LOGE(...) ((void)0)
#define NETPLAY_VERSION 4
#define MAX_PLAYERS 8
static netplay_player_t players[MAX_PLAYERS];
static netplay_player_t *local_player, *remote_player;
static netplay_mode_t netplay_mode;
static netplay_status_t netplay_status;
static int netplay_sync;
static void set_status(netplay_status_t status) { netplay_status = status; }
static uint32_t rg_system_timer(void) { return 0; }
static void xSemaphoreGive(int sem) { (void)sem; }
static void send_ctrl_packet(uint32_t ip, uint8_t cmd, uint8_t arg, void *data, uint8_t len)
{ (void)ip; (void)cmd; (void)arg; (void)data; (void)len; }
static int serial_sock = 1, link_tx_left;
static bool serial_broken;
static unsigned link_rx_have;
static uint8_t link_tx_buf[LINK_TX_CAPACITY], link_rx_buf[4];
static uint8_t wire[1024], incoming[4];
static int wire_len, allowance, hard_error, receive_have, send_error, recv_error;
static int send(int fd, const void *buf, int len, int flags)
{
    (void)fd; (void)flags;
    if (hard_error) { errno = EPIPE; return -1; }
    if (send_error) { errno = send_error; return -1; }
    if (!allowance) { errno = EAGAIN; return -1; }
    if (len > allowance) len = allowance;
    memcpy(wire + wire_len, buf, len);
    wire_len += len; allowance -= len;
    return len;
}
static int recv(int fd, void *buf, int len, int flags)
{
    (void)fd; (void)flags;
    if (recv_error) { errno = recv_error; return -1; }
    if (!receive_have) { errno = EAGAIN; return -1; }
    if (len > receive_have) len = receive_have;
    memcpy(buf, incoming, len);
    receive_have -= len;
    memmove(incoming, incoming + len, receive_have);
    return len;
}
/* SOURCE_FUNCTIONS */
static void reset(void)
{
    serial_broken = false;
    link_tx_left = link_rx_have = wire_len = allowance = hard_error = receive_have = 0;
    send_error = recv_error = 0;
}
int main(void)
{
    uint8_t t, s, d;
    // Resource pressure is recoverable, including after part of a frame left.
    const int resource_errors[] = {ENOMEM, ENOBUFS};
    for (unsigned i = 0; i < sizeof(resource_errors) / sizeof(resource_errors[0]); ++i) {
        reset(); allowance = 2;
        assert(rg_netplay_link_send(1, 7, 42) == 0);
        send_error = resource_errors[i];
        assert(!rg_netplay_link_recv(&t, &s, &d));
        assert(!serial_broken && link_tx_left == 2);
        assert(rg_netplay_link_send(2, 8, 43) == 0);
        assert(!serial_broken && link_tx_left == 6);
        send_error = 0; allowance = 100;
        rg_netplay_link_recv(&t, &s, &d);
        assert(link_tx_left == 0 && wire_len == 8);
        assert(memcmp(wire, (uint8_t[]){0xA5, 1, 7, 42, 0xA5, 2, 8, 43}, 8) == 0);
        reset(); incoming[0] = 0xA5; incoming[1] = 2; receive_have = 2;
        assert(!rg_netplay_link_recv(&t, &s, &d));
        recv_error = resource_errors[i];
        assert(!rg_netplay_link_recv(&t, &s, &d));
        assert(!serial_broken && link_rx_have == 2);
        recv_error = 0; incoming[0] = 17; incoming[1] = 99; receive_have = 2;
        assert(rg_netplay_link_recv(&t, &s, &d));
        assert(t == 2 && s == 17 && d == 99);
    }
    // A split frame must finish even when the game only polls for its reply.
    reset(); allowance = 2;
    assert(rg_netplay_link_send(1, 7, 42) == 0);
    assert(link_tx_left == 2);
    allowance = 100;
    assert(!rg_netplay_link_recv(&t, &s, &d));
    assert(link_tx_left == 0 && wire_len == 4);
    assert(memcmp(wire, (uint8_t[]){0xA5, 1, 7, 42}, 4) == 0);
    // Complete EAGAIN and subsequent frames preserve every byte in FIFO order.
    reset();
    assert(rg_netplay_link_send(1, 8, 43) == 0);
    assert(rg_netplay_link_send(2, 9, 44) == 0);
    allowance = 100;
    rg_netplay_link_recv(&t, &s, &d);
    assert(wire_len == 8 && link_tx_left == 0);
    assert(memcmp(wire, (uint8_t[]){0xA5, 1, 8, 43, 0xA5, 2, 9, 44}, 8) == 0);
    // Fatal write errors and queue exhaustion explicitly mark the session broken.
    reset(); hard_error = 1;
    assert(rg_netplay_link_send(1, 1, 1) == -1 && serial_broken);
    reset();
    for (int i = 0; i < 64; ++i) assert(rg_netplay_link_send(1, i, i) == 0);
    assert(rg_netplay_link_send(1, 65, 65) == -1 && serial_broken);
    // A fragmented reply remains intact across polls.
    reset(); incoming[0] = 0xA5; incoming[1] = 2; receive_have = 2;
    assert(!rg_netplay_link_recv(&t, &s, &d));
    incoming[0] = 17; incoming[1] = 99; receive_have = 2;
    assert(rg_netplay_link_recv(&t, &s, &d));
    assert(t == 2 && s == 17 && d == 99);
    // Reject incompatible INFO before publishing a remote player to TCP accept.
    local_player = &players[0]; local_player->id = 0;
    netplay_mode = NETPLAY_MODE_GUEST;
    netplay_status = NETPLAY_STATUS_HANDSHAKE;
    netplay_packet_t pkt = {.player_id = 1, .cmd = NETPLAY_PACKET_INFO,
                            .data_len = sizeof(netplay_player_t)};
    pkt.data[0] = NETPLAY_VERSION - 1;
    handle_packet(&pkt, 4 + sizeof(netplay_player_t));
    assert(remote_player == NULL && netplay_status == NETPLAY_STATUS_HANDSHAKE);
    pkt.cmd = NETPLAY_PACKET_READY; pkt.data_len = 0;
    handle_packet(&pkt, 4);
    assert(netplay_status == NETPLAY_STATUS_HANDSHAKE);
    pkt.cmd = NETPLAY_PACKET_INFO; pkt.data_len = sizeof(netplay_player_t);
    pkt.data[0] = NETPLAY_VERSION;
    handle_packet(&pkt, 4 + sizeof(netplay_player_t));
    assert(remote_player == &players[1]);
    pkt.cmd = NETPLAY_PACKET_READY; pkt.data_len = 0;
    serial_sock = -1;
    handle_packet(&pkt, 4);
    assert(netplay_status == NETPLAY_STATUS_HANDSHAKE);
    serial_sock = 1;
    handle_packet(&pkt, 4);
    assert(netplay_status == NETPLAY_STATUS_CONNECTED);
    puts("PASS: split send, blocked send, FIFO, fatal error, overflow, split receive");
    puts("PASS: incompatible INFO, premature READY, missing TCP, valid handshake");
    puts("PASS: ENOMEM/ENOBUFS preserve split send, queued frames and split receive");
    return 0;
}
