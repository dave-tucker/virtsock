/*
 * A Hyper-V socket benchmarking program
 */
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 3049197C-9A4E-4FBF-9367-97F792F16994 */
DEFINE_GUID(BM_CTRL_GUID,
    0x3049197c, 0x9a4e, 0x4fbf, 0x93, 0x67, 0x97, 0xf7, 0x92, 0xf1, 0x69, 0x94);
/* 9DC644A1-9F8F-4EE6-AF95-BF9E31D46D9D */
DEFINE_GUID(BM_DATA_GUID,
    0x9dc644a1, 0x9f8f, 0x4ee6, 0xaf, 0x95, 0xbf, 0x9e, 0x31, 0xd4, 0x6d, 0x9d);

#ifdef _MSC_VER
static WSADATA wsaData;
#endif


/* We send a single 32bit word as a benchmark control from the client
 * to the server.  The top 4 bits define the benchmark and the low
 * bits define an argument.
 */
#define HVS_BM_CMD_MSG_RTT   0x1     /* Message Round Trip Time */
#define HVS_BM_CMD_CON_RTT   0x2     /* Connection Round Trip Time */
#define HVS_BM_CMD_CON_RDY   0xf     /* Ready (sent by server) */
#define HVS_BM_CMD_msk   0xf
#define HVS_BM_CMD_shift 28
#define HVS_BM_CMD(_x) (((_x) & HVS_BM_CMD_msk) << HVS_BM_CMD_shift)
#define HVS_BM_CMD_of(_x) (((_x) >> HVS_BM_CMD_shift) & HVS_BM_CMD_msk)

#define HVS_BM_ARG_msk   0x0fffffff
#define HVS_BM_ARG_shift 0x0
#define HVS_BM_ARG(_x) (((_x) & HVS_BM_ARG_msk) << HVS_BM_ARG_shift)
#define HVS_BM_ARG_of(_x) (((_x) >> HVS_BM_ARG_shift) & HVS_BM_ARG_msk)

/* Use a static buffer for send and receive. */
#define MAX_BUF_LEN (2 * 1024 * 1024)
static char buf[MAX_BUF_LEN];

/* Number of iterations for RTT tests */
#define HVS_BM_RTT_ITER  5000

/* Message sizes for RTT tests */
static int rtt_msg_sizes[] =
    {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};


static SOCKET svr_init(void)
{
    SOCKET lsock = INVALID_SOCKET;
    SOCKADDR_HV sa;
    int ret;

    lsock = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (lsock == INVALID_SOCKET) {
        sockerr("socket()");
        return 1;
    }

    sa.Family = AF_HYPERV;
    sa.Reserved = 0;
    sa.VmId = HV_GUID_WILDCARD;
    sa.ServiceId = BM_DATA_GUID;

    ret = bind(lsock, (const struct sockaddr *)&sa, sizeof(sa));
    if (ret == SOCKET_ERROR) {
        sockerr("bind()");
        goto err_out;
    }

    ret = listen(lsock, SOMAXCONN);
    if (ret == SOCKET_ERROR) {
        sockerr("listen()");
        goto err_out;
    }

    return lsock;

err_out:
    closesocket(lsock);
    return INVALID_SOCKET;
}

/* Message Round Trip Times
 *
 * Measure the RTT of fixed sized messages over an existing
 * connection. The server is simply echoing back the messages
 * received. The client is recording the time each message takes.
 *
 * We assume here that the client can always send the @msg_sz worth of
 * data without blocking. This assumption may not hold for largish
 * message sizes (greater than 4k???) so we avoid those.
 */
static int msg_rtt_svr(SOCKET ctrl_sock, int msg_sz)
{
    SOCKET lsock, fd;
    SOCKADDR_HV sac;
    socklen_t socklen = sizeof(sac);
    uint32_t bm_res;
    int received, sent;
    int ret;

    lsock = svr_init();
    if (lsock == INVALID_SOCKET)
        goto err_out;

    bm_res = HVS_BM_CMD(HVS_BM_CMD_CON_RDY);
    ret = send(ctrl_sock, &bm_res, sizeof(bm_res), 0);
    if (ret != sizeof(bm_res)) {
        sockerr("send rdy");
        goto err_out;
    }

    fd = accept(lsock, (struct sockaddr *)&sac, &socklen);
    if (fd == INVALID_SOCKET) {
        sockerr("accept()");
        ret = -1;
        goto err_out;
    }

    for (;;) {
        received = recv(fd, buf, msg_sz, 0);
        if (received == 0) {
            break;
        } else if (received == SOCKET_ERROR) {
            sockerr("recv()");
            ret = -1;
            goto err_loop;
        }

        sent = 0;
        while (sent < received) {
            ret = send(fd, buf + sent, received - sent, 0);
            if (ret == SOCKET_ERROR) {
                sockerr("send()");
                ret = -1;
                goto err_loop;
            }
            sent += ret;
        }
    }

    ret = 0;

err_loop:
    closesocket(fd);
err_out:
    closesocket(lsock);
    return ret;
}

static int msg_rtt_cli(GUID target, int msg_sz)
{
    SOCKET fd = INVALID_SOCKET;
    SOCKADDR_HV sa;
    int sent, received;
    int ret, i;

    fd = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (fd == INVALID_SOCKET) {
        sockerr("socket()");
        return -1;
    }

    sa.Family = AF_HYPERV;
    sa.Reserved = 0;
    sa.VmId = target;
    sa.ServiceId = BM_DATA_GUID;

    ret = connect(fd, (const struct sockaddr *)&sa, sizeof(sa));
    if (ret == SOCKET_ERROR) {
        sockerr("connect()");
        goto err_out;
    }

    for (i = 0; i < HVS_BM_RTT_ITER; i++) {
        sent = 0;
        while (sent < msg_sz) {
            ret = send(fd, buf + sent, msg_sz - sent, 0);
            if (ret == SOCKET_ERROR) {
                sockerr("send()");
                ret = -1;
                goto err_out;
            }
            sent += ret;
        }

        received = 0;
        while (received < msg_sz) {
            ret = send(fd, buf + received, msg_sz - received, 0);
            if (ret == SOCKET_ERROR) {
                sockerr("send()");
                ret = -1;
                goto err_out;
            }
            received += ret;
        }
    }

err_out:
    closesocket(fd);
    return ret;
}



static void server(void)
{
    SOCKET lsock, csock;
    SOCKADDR_HV sa, sac;
    socklen_t socklen = sizeof(sac);
    uint32_t bm_cmd;
    int ret;

    lsock = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (lsock == INVALID_SOCKET) {
        sockerr("socket()");
        return;
    }

    sa.Family = AF_HYPERV;
    sa.Reserved = 0;
    sa.VmId = HV_GUID_WILDCARD;
    sa.ServiceId = BM_CTRL_GUID;

    ret = bind(lsock, (const struct sockaddr *)&sa, sizeof(sa));
    if (ret == SOCKET_ERROR) {
        sockerr("bind()");
        closesocket(lsock);
        return;
    }

    ret = listen(lsock, SOMAXCONN);
    if (ret == SOCKET_ERROR) {
        sockerr("listen()");
        closesocket(lsock);
        return;
    }

    csock = accept(lsock, (struct sockaddr *)&sac, &socklen);
    if (csock == INVALID_SOCKET) {
        sockerr("accept()");
        closesocket(lsock);
        return;
    }

    while(1) {
        ret = recv(csock, &bm_cmd, sizeof(bm_cmd), 0);
        if (ret != sizeof(bm_cmd))
            break;

        ret = 0;
        switch(HVS_BM_CMD_of(bm_cmd)) {
        case HVS_BM_CMD_MSG_RTT:
            ret = msg_rtt_svr(csock, HVS_BM_ARG_of(bm_cmd));
            break;

        default:
            ret = 1;
            break;
        }

        if (ret)
            break;
    }

    closesocket(csock);
    closesocket(lsock);
}


static void client(GUID target)
{

}
