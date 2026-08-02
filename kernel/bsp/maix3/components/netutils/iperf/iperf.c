/**
* iperf-liked network performance tool
*
*/

#include <rtthread.h>

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

#define DBG_SECTION_NAME               "iperf"
#define DBG_LEVEL                      DBG_INFO
#include <rtdbg.h>

#define IPERF_PORT          5001
#define IPERF_BUFSZ         (4 * 1024)
#define IPERF_UDP_DATAGRAM  1470
#define IPERF_UDP_BANDWIDTH 0ULL
#define IPERF_UDP_AUTO_START_BPS 10000000ULL
#define IPERF_UDP_AUTO_STEP_BPS  1000000ULL
#define IPERF_UDP_AUTO_MIN_BPS   100000ULL
#define IPERF_UDP_AUTO_BACKOFF_MIN 4U
#define IPERF_UDP_AUTO_BACKOFF_PERCENT 1U
#define IPERF_INTERVAL      5
#define IPERF_MAX_THREADS   16

#define IPERF_MODE_STOP     0
#define IPERF_MODE_SERVER   1
#define IPERF_MODE_CLIENT   2

// #if (RT_VER_NUM >= 0x50000)
// #define IPERF_GET_THREAD_NAME(th) (th->parent.name)
// #else
#define IPERF_GET_THREAD_NAME(th) (th->name)
// #endif

typedef struct
{
    volatile int mode;
    char *host;
    int port;
    int interval;
    int duration;
    rt_uint64_t bandwidth_bps;
} IPERF_PARAM;
static IPERF_PARAM param = {
    IPERF_MODE_STOP, NULL, IPERF_PORT, IPERF_INTERVAL, 0,
    IPERF_UDP_BANDWIDTH
};
static volatile int active_workers;

static rt_bool_t iperf_should_run(rt_tick_t started)
{
    if (param.mode == IPERF_MODE_STOP)
    {
        return RT_FALSE;
    }
    if (param.duration > 0 &&
        (rt_tick_t)(rt_tick_get() - started) >=
            (rt_tick_t)param.duration * RT_TICK_PER_SECOND)
    {
        return RT_FALSE;
    }
    return RT_TRUE;
}

static void iperf_worker_done(void)
{
    rt_enter_critical();
    if (active_workers > 0)
    {
        active_workers--;
    }
    if (!active_workers)
    {
        param.mode = IPERF_MODE_STOP;
    }
    rt_exit_critical();
}

static void iperf_report(rt_thread_t thread, rt_uint64_t bytes,
                         rt_tick_t ticks, const char *kind)
{
    rt_uint64_t kbps;

    if (!ticks)
    {
        return;
    }
    kbps = bytes * 8U * RT_TICK_PER_SECOND / ticks / 1000U;
    LOG_I("%s: %llu.%03llu Mbps %s", IPERF_GET_THREAD_NAME(thread),
          (unsigned long long)(kbps / 1000U),
          (unsigned long long)(kbps % 1000U), kind);
}

static void iperf_udp_client(void *thread_param)
{
    int sock;
    int result;
    rt_uint32_t *buffer;
    struct sockaddr_in server;
    rt_uint32_t packet_count = 0;
    rt_uint32_t send_errors = 0;
    rt_uint32_t interval_backoffs = 0;
    rt_uint32_t interval_packets = 0;
    rt_uint64_t sentlen = 0;
    rt_tick_t started;
    rt_tick_t interval_started;
    rt_tick_t pace_updated;
    rt_tick_t now;
    rt_uint64_t pace_tokens;
    rt_uint64_t pace_bucket;
    rt_uint64_t packet_bits;
    rt_uint64_t target_bps;
    rt_bool_t auto_rate;
    int send_size;

    (void)thread_param;
    started = rt_tick_get();
    interval_started = started;
    pace_updated = started;
    send_size = IPERF_BUFSZ > IPERF_UDP_DATAGRAM
                    ? IPERF_UDP_DATAGRAM : IPERF_BUFSZ;
    packet_bits = (rt_uint64_t)send_size * 8U;
    auto_rate = param.bandwidth_bps == 0;
    target_bps = auto_rate ? IPERF_UDP_AUTO_START_BPS
                           : param.bandwidth_bps;
    pace_tokens = packet_bits;
    /* Bound catch-up bursts so a delayed iperf thread cannot immediately
     * overflow a shallow network-driver queue. */
    pace_bucket = packet_bits * 8U;
    buffer = rt_malloc(IPERF_BUFSZ);
    if (buffer == NULL)
    {
        LOG_E("cannot allocate UDP buffer");
        iperf_worker_done();
        return;
    }
    rt_memset(buffer, 0x00, IPERF_BUFSZ);
    sock = socket(PF_INET, SOCK_DGRAM, 0);
    if(sock < 0)
    {
        LOG_E("can't create socket!");
        rt_free(buffer);
        iperf_worker_done();
        return;
    }
    server.sin_family = PF_INET;
    server.sin_port = htons(param.port);
    server.sin_addr.s_addr = inet_addr(param.host);
    if (!auto_rate)
    {
        LOG_I("UDP client started: %s:%d, datagram=%d bytes, offered=%llu kbps",
              param.host, param.port, send_size,
              (unsigned long long)(param.bandwidth_bps / 1000U));
    }
    else
    {
        LOG_I("UDP client started: %s:%d, datagram=%d bytes, bandwidth=auto, start=%llu kbps",
              param.host, param.port, send_size,
              (unsigned long long)(target_bps / 1000U));
    }
    while (iperf_should_run(started))
    {
        now = rt_tick_get();
        if (now != pace_updated)
        {
            rt_uint64_t refill =
                (rt_uint64_t)(rt_tick_t)(now - pace_updated) *
                target_bps / RT_TICK_PER_SECOND;

            if (refill >= pace_bucket - pace_tokens)
            {
                pace_tokens = pace_bucket;
            }
            else
            {
                pace_tokens += refill;
            }
            pace_updated = now;
        }
        if (pace_tokens < packet_bits)
        {
            rt_thread_delay(1);
            continue;
        }
        pace_tokens -= packet_bits;

        buffer[0] = htonl(packet_count + 1U);
        buffer[1] = htonl(now / RT_TICK_PER_SECOND);
        buffer[2] = htonl((now % RT_TICK_PER_SECOND) *
                          (1000000U / RT_TICK_PER_SECOND));
        result = sendto(sock, buffer, send_size, 0,
                        (struct sockaddr *)&server, sizeof(server));
        if (result > 0)
        {
            packet_count++;
            interval_packets++;
            sentlen += result;
        }
        else
        {
            send_errors++;
            interval_backoffs++;
            rt_thread_mdelay(1);
        }

        now = rt_tick_get();
        if ((rt_tick_t)(now - interval_started) >=
            (rt_tick_t)param.interval * RT_TICK_PER_SECOND)
        {
            rt_tick_t interval_ticks = now - interval_started;

            iperf_report(rt_thread_self(), sentlen, now - interval_started,
                         "sent");
            if (auto_rate)
            {
                rt_uint64_t actual_bps = sentlen * 8U * RT_TICK_PER_SECOND /
                                         interval_ticks;
                /* Scheduler jitter can cause isolated queue-full returns.
                 * Reduce the target only when backpressure is sustained. */
                rt_bool_t congested =
                    interval_backoffs >= IPERF_UDP_AUTO_BACKOFF_MIN &&
                    (rt_uint64_t)interval_backoffs * 100U >=
                        ((rt_uint64_t)interval_packets + interval_backoffs) *
                            IPERF_UDP_AUTO_BACKOFF_PERCENT;

                if (congested)
                {
                    target_bps = actual_bps * 95U / 100U;
                    if (target_bps < IPERF_UDP_AUTO_MIN_BPS)
                    {
                        target_bps = IPERF_UDP_AUTO_MIN_BPS;
                    }
                }
                else if (!interval_backoffs &&
                         target_bps <=
                         1000000000ULL - IPERF_UDP_AUTO_STEP_BPS)
                {
                    target_bps += IPERF_UDP_AUTO_STEP_BPS;
                }
                LOG_I("UDP auto target=%llu kbps backoffs=%u",
                      (unsigned long long)(target_bps / 1000U),
                      interval_backoffs);
            }
            interval_started = now;
            sentlen = 0;
            interval_backoffs = 0;
            interval_packets = 0;
        }
    }
    now = rt_tick_get();
    if (sentlen && now != interval_started)
    {
        iperf_report(rt_thread_self(), sentlen, now - interval_started,
                     "sent");
    }
    buffer[0] = htonl((rt_uint32_t)-(rt_int32_t)packet_count);
    sendto(sock, buffer, send_size, 0, (struct sockaddr *)&server,
           sizeof(server));
    if (send_errors)
    {
        if (!auto_rate)
        {
            LOG_W("UDP send backoffs: %u", send_errors);
        }
        else
        {
            LOG_I("UDP auto-rate backoffs: %u", send_errors);
        }
    }
    closesocket(sock);
    rt_free(buffer);
    iperf_worker_done();
}

static void iperf_udp_server(void *thread_param)
{
    int sock;
    int reuse = 1;
    rt_uint32_t *buffer;
    struct sockaddr_in server;
    struct sockaddr_in sender;
    socklen_t sender_len;
    int r_size;
    rt_uint64_t received_len = 0;
    rt_uint32_t packet_count;
    rt_uint32_t last_packet = 0;
    rt_uint32_t lost = 0;
    rt_uint32_t received = 0;
    rt_bool_t have_packet = RT_FALSE;
    rt_tick_t started;
    rt_tick_t interval_started;
    rt_tick_t now;
    struct timeval timeout;

    (void)thread_param;
    started = rt_tick_get();
    interval_started = started;
    buffer = rt_malloc(IPERF_BUFSZ);
    if (buffer == NULL)
    {
        LOG_E("cannot allocate UDP buffer");
        iperf_worker_done();
        return;
    }
    sock = socket(PF_INET, SOCK_DGRAM, 0);
    if(sock < 0)
    {
        LOG_E("can't create socket! exit!");
        rt_free(buffer);
        iperf_worker_done();
        return;
    }
    server.sin_family = PF_INET;
    server.sin_port = htons(param.port);
    server.sin_addr.s_addr = inet_addr("0.0.0.0");
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1)
    {
        LOG_E("setsockopt failed!");
        closesocket(sock);
        rt_free(buffer);
        iperf_worker_done();
        return;
    }
    if (bind(sock, (struct sockaddr *)&server, sizeof(struct sockaddr_in)) < 0)
    {
        LOG_E("iperf server bind failed! exit!");
        closesocket(sock);
        rt_free(buffer);
        iperf_worker_done();
        return;
    }

    LOG_I("UDP server listening on port %d", param.port);
    while (iperf_should_run(started))
    {
        sender_len = sizeof(sender);
        r_size = recvfrom(sock, buffer, IPERF_BUFSZ, 0,
                          (struct sockaddr *)&sender, &sender_len);
        if (r_size >= 12)
        {
            packet_count = ntohl(buffer[0]);
            if ((rt_int32_t)packet_count >= 0)
            {
                if (have_packet && packet_count > last_packet + 1U)
                {
                    lost += packet_count - last_packet - 1U;
                }
                if (!have_packet || packet_count > last_packet)
                {
                    last_packet = packet_count;
                }
                have_packet = RT_TRUE;
                received++;
                received_len += r_size;
            }
        }

        now = rt_tick_get();
        if ((rt_tick_t)(now - interval_started) >=
            (rt_tick_t)param.interval * RT_TICK_PER_SECOND)
        {
            if (received_len)
            {
                iperf_report(rt_thread_self(), received_len,
                             now - interval_started, "received");
                LOG_I("UDP packets: received=%u lost=%u", received, lost);
            }
            interval_started = now;
            received_len = 0;
            received = 0;
            lost = 0;
        }
    }
    now = rt_tick_get();
    if (received_len && now != interval_started)
    {
        iperf_report(rt_thread_self(), received_len,
                     now - interval_started, "received");
        LOG_I("UDP packets: received=%u lost=%u", received, lost);
    }
    closesocket(sock);
    rt_free(buffer);
    iperf_worker_done();
}

static void iperf_client(void *thread_param)
{
    int i;
    int sock;
    int ret;
    int tips = 1;
    uint8_t *send_buf;
    rt_uint64_t sentlen;
    rt_tick_t started;
    rt_tick_t interval_started;
    rt_tick_t now;
    struct sockaddr_in addr;

    (void)thread_param;
    started = rt_tick_get();
    send_buf = (uint8_t *) rt_malloc(IPERF_BUFSZ);
    if (!send_buf)
    {
        LOG_E("cannot allocate TCP buffer");
        iperf_worker_done();
        return;
    }

    for (i = 0; i < IPERF_BUFSZ; i ++)
        send_buf[i] = i & 0xff;

    while (iperf_should_run(started))
    {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
        {
            LOG_E("create socket failed!");
            rt_thread_mdelay(1000);
            continue;
        }

        addr.sin_family = PF_INET;
        addr.sin_port = htons(param.port);
        addr.sin_addr.s_addr = inet_addr((char *)param.host);

        ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
        if (ret == -1)
        {
            if (tips)
            {
                LOG_E("Connect to iperf server faile, Waiting for the server to open!");
                tips = 0;
            }
            closesocket(sock);
            rt_thread_mdelay(1000);
            continue;
        }

        LOG_I("Connect to iperf server successful!");

        {
            int flag = 1;

            setsockopt(sock,
                       IPPROTO_TCP,     /* set option at TCP level */
                       TCP_NODELAY,     /* name of option */
                       (void *) &flag,  /* the cast is historical cruft */
                       sizeof(int));    /* length of option value */
        }

        sentlen = 0;
        interval_started = rt_tick_get();
        while (iperf_should_run(started))
        {
            now = rt_tick_get();
            if ((rt_tick_t)(now - interval_started) >=
                (rt_tick_t)param.interval * RT_TICK_PER_SECOND)
            {
                iperf_report(rt_thread_self(), sentlen,
                             now - interval_started, "sent");
                interval_started = now;
                sentlen = 0;
            }

            ret = send(sock, send_buf, IPERF_BUFSZ, 0);
            if (ret > 0)
            {
                sentlen += ret;
            }

            if (ret <= 0) break;
        }

        now = rt_tick_get();
        if (sentlen && now != interval_started)
        {
            iperf_report(rt_thread_self(), sentlen,
                         now - interval_started, "sent");
        }

        closesocket(sock);

        if (!iperf_should_run(started))
        {
            break;
        }
        rt_thread_mdelay(2000);
        LOG_W("Disconnected, iperf server shut down");
        tips = 1;
    }
    rt_free(send_buf);
    iperf_worker_done();
}

void iperf_server(void *thread_param)
{
    uint8_t *recv_data;
    socklen_t sin_size;
    rt_tick_t started;
    rt_tick_t interval_started;
    rt_tick_t now;
    int sock = -1, connected, bytes_received;
    int reuse = 1;
    rt_uint64_t recvlen;
    struct sockaddr_in server_addr, client_addr;
    fd_set readset;
    struct timeval timeout;

    (void)thread_param;
    started = rt_tick_get();
    connected = -1;
    recv_data = (uint8_t *)rt_malloc(IPERF_BUFSZ);
    if (recv_data == RT_NULL)
    {
        LOG_E("No memory!");
        goto __exit;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        LOG_E("Socket error!");
        goto __exit;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(param.port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    rt_memset(&(server_addr.sin_zero), 0x0, sizeof(server_addr.sin_zero));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)
    {
        LOG_E("Unable to bind!");
        goto __exit;
    }

    if (listen(sock, 5) == -1)
    {
        LOG_E("Listen error!");
        goto __exit;
    }

    LOG_I("TCP server listening on port %d", param.port);
    while (iperf_should_run(started))
    {
        FD_ZERO(&readset);
        FD_SET(sock, &readset);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        if (select(sock + 1, &readset, RT_NULL, RT_NULL, &timeout) <= 0)
            continue;

        sin_size = sizeof(struct sockaddr_in);

        connected = accept(sock, (struct sockaddr *)&client_addr, &sin_size);
        if (connected < 0)
        {
            continue;
        }

        LOG_I("new client connected from (%s, %d)",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        {
            int flag = 1;

            setsockopt(connected,
                       IPPROTO_TCP,     /* set option at TCP level */
                       TCP_NODELAY,     /* name of option */
                       (void *) &flag,  /* the cast is historical cruft */
                       sizeof(int));    /* length of option value */
        }
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));

        recvlen = 0;
        interval_started = rt_tick_get();
        while (iperf_should_run(started))
        {
            bytes_received = recv(connected, recv_data, IPERF_BUFSZ, 0);
            if (bytes_received <= 0) break;

            recvlen += bytes_received;

            now = rt_tick_get();
            if ((rt_tick_t)(now - interval_started) >=
                (rt_tick_t)param.interval * RT_TICK_PER_SECOND)
            {
                iperf_report(rt_thread_self(), recvlen,
                             now - interval_started, "received");
                interval_started = now;
                recvlen = 0;
            }
        }
        now = rt_tick_get();
        if (recvlen && now != interval_started)
        {
            iperf_report(rt_thread_self(), recvlen,
                         now - interval_started, "received");
        }
        LOG_W("client disconnected (%s, %d)",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        if (connected >= 0) closesocket(connected);
        connected = -1;
    }

__exit:
    if (connected >= 0) closesocket(connected);
    if (sock >= 0) closesocket(sock);
    if (recv_data) rt_free(recv_data);
    iperf_worker_done();
}

void iperf_usage(void)
{
    rt_kprintf("Usage: iperf -s|-c <host> [options]\n");
    rt_kprintf("       iperf --stop\n");
    rt_kprintf("  -s              run as server\n");
    rt_kprintf("  -c <host>       run as client\n");
    rt_kprintf("  -u              use UDP (accepted in any position)\n");
    rt_kprintf("  -p <port>       server port (default %d)\n", IPERF_PORT);
    rt_kprintf("  -i <seconds>    report interval (default %d)\n", IPERF_INTERVAL);
    rt_kprintf("  -t <seconds>    stop after this duration\n");
    rt_kprintf("  -b <rate>       UDP bandwidth (default auto; 0/max or K/M/G suffix)\n");
    rt_kprintf("  -P <count>      parallel client threads (alias: -m)\n");
    rt_kprintf("  -h, --help      show this help\n");
}

static rt_bool_t iperf_parse_int(const char *text, int minimum, int maximum,
                                 int *value)
{
    char *end;
    long parsed;

    if (!text || !*text)
    {
        return RT_FALSE;
    }
    parsed = strtol(text, &end, 10);
    if (*end || parsed < minimum || parsed > maximum)
    {
        return RT_FALSE;
    }
    *value = (int)parsed;
    return RT_TRUE;
}

static rt_bool_t iperf_parse_rate(const char *text, rt_uint64_t *rate)
{
    char *end;
    unsigned long long parsed;
    rt_uint64_t multiplier = 1U;

    if (!text)
    {
        return RT_FALSE;
    }
    if (!strcmp(text, "max"))
    {
        *rate = 0;
        return RT_TRUE;
    }
    if (text[0] < '0' || text[0] > '9')
    {
        return RT_FALSE;
    }
    parsed = strtoull(text, &end, 10);
    if (*end)
    {
        if (end[1])
        {
            return RT_FALSE;
        }
        if (*end == 'k' || *end == 'K')
        {
            multiplier = 1000U;
        }
        else if (*end == 'm' || *end == 'M')
        {
            multiplier = 1000000U;
        }
        else if (*end == 'g' || *end == 'G')
        {
            multiplier = 1000000000U;
        }
        else
        {
            return RT_FALSE;
        }
    }
    if (parsed > UINT64_MAX / multiplier ||
        (rt_uint64_t)parsed * multiplier > 1000000000ULL)
    {
        return RT_FALSE;
    }
    *rate = (rt_uint64_t)parsed * multiplier;
    return RT_TRUE;
}

int iperf(int argc, char **argv)
{
    rt_thread_t threads[IPERF_MAX_THREADS] = { RT_NULL };
    int mode = IPERF_MODE_STOP;
    char *host = NULL;
    char *new_host = NULL;
    int port = IPERF_PORT;
    int numtid = 1;
    int use_udp = 0;
    int interval = IPERF_INTERVAL;
    int duration = 0;
    rt_uint64_t bandwidth_bps = IPERF_UDP_BANDWIDTH;
    rt_bool_t bandwidth_set = RT_FALSE;
    int created = 0;
    int i;

    if (argc <= 1)
    {
        goto __usage;
    }
    if (argc == 2 && !strcmp(argv[1], "--stop"))
    {
        param.mode = IPERF_MODE_STOP;
        return 0;
    }

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            goto __usage;
        }
        if (!strcmp(argv[i], "-u"))
        {
            use_udp = 1;
            continue;
        }
        if (!strcmp(argv[i], "-s"))
        {
            if (mode != IPERF_MODE_STOP)
            {
                LOG_E("select exactly one of -s or -c");
                return -RT_EINVAL;
            }
            mode = IPERF_MODE_SERVER;
            continue;
        }
        if (!strcmp(argv[i], "-c"))
        {
            if (mode != IPERF_MODE_STOP || ++i >= argc)
            {
                LOG_E("-c requires a host and cannot be combined with -s");
                return -RT_EINVAL;
            }
            mode = IPERF_MODE_CLIENT;
            host = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "-p"))
        {
            if (++i >= argc || !iperf_parse_int(argv[i], 1, 65535, &port))
            {
                LOG_E("invalid port");
                return -RT_EINVAL;
            }
            continue;
        }
        if (!strcmp(argv[i], "-i"))
        {
            if (++i >= argc || !iperf_parse_int(argv[i], 1, 3600,
                                                &interval))
            {
                LOG_E("invalid report interval");
                return -RT_EINVAL;
            }
            continue;
        }
        if (!strcmp(argv[i], "-t"))
        {
            if (++i >= argc || !iperf_parse_int(argv[i], 1, 86400,
                                                &duration))
            {
                LOG_E("invalid duration");
                return -RT_EINVAL;
            }
            continue;
        }
        if (!strcmp(argv[i], "-b"))
        {
            if (++i >= argc || !iperf_parse_rate(argv[i], &bandwidth_bps))
            {
                LOG_E("invalid bandwidth (use max, bits/s, or K/M/G suffix)");
                return -RT_EINVAL;
            }
            bandwidth_set = RT_TRUE;
            continue;
        }
        if (!strcmp(argv[i], "-P") || !strcmp(argv[i], "-m"))
        {
            if (++i >= argc || !iperf_parse_int(argv[i], 1,
                                                IPERF_MAX_THREADS, &numtid))
            {
                LOG_E("invalid thread count (1-%d)", IPERF_MAX_THREADS);
                return -RT_EINVAL;
            }
            continue;
        }

        LOG_E("unknown option: %s", argv[i]);
        return -RT_EINVAL;
    }

    if (mode == IPERF_MODE_STOP)
    {
        LOG_E("select -s or -c");
        return -RT_EINVAL;
    }
    if (mode == IPERF_MODE_SERVER && numtid != 1)
    {
        LOG_E("parallel threads are supported only in client mode");
        return -RT_EINVAL;
    }
    if (bandwidth_set && (!use_udp || mode != IPERF_MODE_CLIENT))
    {
        LOG_E("-b is supported only for a UDP client");
        return -RT_EINVAL;
    }
    if (param.mode != IPERF_MODE_STOP || active_workers)
    {
        LOG_E("iperf is already running; stop it with: iperf --stop");
        return -RT_EBUSY;
    }
    if (host)
    {
        new_host = rt_strdup(host);
        if (!new_host)
        {
            return -RT_ENOMEM;
        }
    }

    if (param.host)
    {
        rt_free(param.host);
    }
    param.host = new_host;
    param.port = port;
    param.interval = interval;
    param.duration = duration;
    param.bandwidth_bps = bandwidth_bps;
    param.mode = mode;

    for (i = 0; i < numtid; i++)
    {
        char tid_name[RT_NAME_MAX + 1] = { 0 };
        void (*function)(void *parameter);

        if (use_udp)
        {
            if (mode == IPERF_MODE_CLIENT)
            {
                rt_snprintf(tid_name, sizeof(tid_name), "iperfc%02d", i + 1);
                function = iperf_udp_client;
            }
            else
            {
                rt_snprintf(tid_name, sizeof(tid_name), "iperfd%02d", i + 1);
                function = iperf_udp_server;
            }
        }
        else if (mode == IPERF_MODE_CLIENT)
        {
            rt_snprintf(tid_name, sizeof(tid_name), "iperfc%02d", i + 1);
            function = iperf_client;
        }
        else
        {
            rt_snprintf(tid_name, sizeof(tid_name), "iperfd%02d", i + 1);
            function = iperf_server;
        }

        threads[created] = rt_thread_create(tid_name, function, RT_NULL,
                                             IPERF_THREAD_STACK_SIZE, 20, 20);
        if (!threads[created])
        {
            LOG_E("cannot create iperf thread %d", i + 1);
            break;
        }
        created++;
    }

    if (!created)
    {
        param.mode = IPERF_MODE_STOP;
        return -RT_ENOMEM;
    }

    active_workers = created;
    for (i = 0; i < created; i++)
    {
        if (rt_thread_startup(threads[i]) != RT_EOK)
        {
            LOG_E("cannot start iperf thread %d", i + 1);
            rt_thread_delete(threads[i]);
            iperf_worker_done();
        }
    }

    return 0;

__usage:
    iperf_usage();
    return 0;
}

#ifdef FINSH_USING_MSH
#include <finsh.h>
MSH_CMD_EXPORT(iperf, the network bandwidth measurement tool);
#endif
