
/*
 * Copyright (c) 2009, The Regents of the University of California, through
 * Lawrence Berkeley National Laboratory (subject to receipt of any required
 * approvals from the U.S. Dept. of Energy).  All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <netinet/tcp.h>


#include "iperf.h"
#include "iperf_api.h"
#include "iperf_server_api.h"
#include "units.h"
#include "locale.h"


int iperf_run(struct iperf_test *);

/**************************************************************************/

static struct option longopts[] =
{
    {"client", required_argument, NULL, 'c'},
    {"server", no_argument, NULL, 's'},
    {"time", required_argument, NULL, 't'},
    {"port", required_argument, NULL, 'p'},
    {"parallel", required_argument, NULL, 'P'},
    {"udp", no_argument, NULL, 'u'},
    {"tcpInfo", no_argument, NULL, 'T'},
    {"bandwidth", required_argument, NULL, 'b'},
    {"length", required_argument, NULL, 'l'},
    {"window", required_argument, NULL, 'w'},
    {"interval", required_argument, NULL, 'i'},
    {"bytes", required_argument, NULL, 'n'},
    {"NoDelay", no_argument, NULL, 'N'},
    {"Print-mss", no_argument, NULL, 'm'},
    {"Set-mss", required_argument, NULL, 'M'},
    {"version", no_argument, NULL, 'v'},
    {"verbose", no_argument, NULL, 'V'},
    {"debug", no_argument, NULL, 'd'},
    {"help", no_argument, NULL, 'h'},
    {"daemon", no_argument, NULL, 'D'},
    {"format", required_argument, NULL, 'f'},
    {"reverse", no_argument, NULL, 'R'},

/*  The following ifdef needs to be split up. linux-congestion is not necessarily supported
 *  by systems that support tos.
 */
#ifdef ADD_WHEN_SUPPORTED
    {"tos",        required_argument, NULL, 'S'},
    {"linux-congestion", required_argument, NULL, 'Z'},
#endif
    {NULL, 0, NULL, 0}
};

/**************************************************************************/

int
main(int argc, char **argv)
{
    char ch, role;
    struct iperf_test *test;
    int port = PORT;

#ifdef TEST_PROC_AFFINITY
    /* didnt seem to work.... */
    /*
     * increasing the priority of the process to minimise packet generation
     * delay
     */
    int rc = setpriority(PRIO_PROCESS, 0, -15);

    if (rc < 0) {
        perror("setpriority:");
        printf("setting priority to valid level\n");
        rc = setpriority(PRIO_PROCESS, 0, 0);
    }
    
    /* setting the affinity of the process  */
    cpu_set_t cpu_set;
    int affinity = -1;
    int ncores = 1;

    sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set);
    if (errno)
        perror("couldn't get affinity:");

    if ((ncores = sysconf(_SC_NPROCESSORS_CONF)) <= 0)
        err("sysconf: couldn't get _SC_NPROCESSORS_CONF");

    CPU_ZERO(&cpu_set);
    CPU_SET(affinity, &cpu_set);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) != 0)
        err("couldn't change CPU affinity");
#endif

    test = iperf_new_test();
    iperf_defaults(test);	/* sets defaults */

    while ((ch = getopt_long(argc, argv, "c:p:st:uP:b:l:w:i:n:mRNTvhVdM:f:", longopts, NULL)) != -1) {
        switch (ch) {
            case 'c':
                test->role = 'c';
                role = test->role;
                test->server_hostname = (char *) malloc(strlen(optarg)+1);
                strncpy(test->server_hostname, optarg, strlen(optarg));
                break;
            case 'p':
                test->server_port = atoi(optarg);
                port = test->server_port;
                break;
            case 's':
                test->role = 's';
                role = test->role;
                break;
            case 't':
                test->duration = atoi(optarg);
                if (test->duration > MAX_TIME) {
                    fprintf(stderr, "\n Error: test duration too long. Maximum value = %d \n\n",  (int)MAX_TIME);
                    usage_long();
                    exit(1);
                }
                break;
            case 'u':
                test->protocol = Pudp;
                test->default_settings->blksize = DEFAULT_UDP_BLKSIZE;
                test->new_stream = iperf_new_udp_stream;
                break;
            case 'P':
                test->num_streams = atoi(optarg);
                if (test->num_streams > MAX_STREAMS) {
                    fprintf(stderr, "\n Error: Number of parallel streams too large. Maximum value = %d \n\n",  MAX_STREAMS);
                    usage_long();
                    exit(1);
                }
                break;
            case 'b':
                test->default_settings->rate = unit_atof(optarg);
                break;
            case 'l':
                test->default_settings->blksize = unit_atoi(optarg);
                if (test->default_settings->blksize > MAX_BLOCKSIZE) {
                    fprintf(stderr, "\n Error: Block size too large. Maximum value = %d \n\n",  MAX_BLOCKSIZE);
                    usage_long();
                    exit(1);
                }
                break;
            case 'w':
                test->default_settings->socket_bufsize = unit_atof(optarg);
                if (test->default_settings->socket_bufsize > MAX_TCP_BUFFER) {
                    fprintf(stderr, "\n Error: TCP buffer too large. Maximum value = %d \n\n",  MAX_TCP_BUFFER);
                    usage_long();
                    exit(1);
                }
                break;
            case 'i':
                /* could potentially want separate stat collection and reporting intervals,
                   but just set them to be the same for now */
                test->stats_interval = atoi(optarg);
                test->reporter_interval = atoi(optarg);
                if (test->stats_interval > MAX_INTERVAL) {
                    fprintf(stderr, "\n Error: Report interval too large. Maximum value = %d \n\n",  MAX_INTERVAL);
                    usage_long();
                    exit(1);
                }
                break;
            case 'n':
                test->default_settings->bytes = unit_atoi(optarg);
                printf("total bytes to be transferred = %llu\n", test->default_settings->bytes);
                break;
            case 'm':
                test->print_mss = 1;
                break;
            case 'N':
                test->no_delay = 1;
                break;
            case 'M':
                test->default_settings->mss = atoi(optarg);
                if (test->default_settings->mss > MAX_MSS) {
                    fprintf(stderr, "\n Error: MSS too large. Maximum value = %d \n\n",  MAX_MSS);
                    usage_long();
                    exit(1);
                }
                break;
            case 'f':
                test->default_settings->unit_format = *optarg;
                break;
            case 'T':
                test->tcp_info = 1;
                break;
            case 'V':
                test->verbose = 1;
                break;
            case 'd':
                test->debug = 1;
                break;
            case 'R':
                test->reverse = 1;
                break;
            case 'v':
                printf(version);
                exit(0);
            case 'h':
            default:
                usage_long();
                exit(1);
        }
    }

    /* For subsequent calls to getopt */
#ifdef __APPLE__
    optreset = 1;
#endif
    optind = 0;
    

    /* exit until this is done.... */
    if (test->protocol == Pudp) {
        printf("UDP mode not yet supported. Exiting. \n");
        iperf_free_test(test);
        return 0;
    }

    if (iperf_run(test) < 0) {
        fprintf(stderr, "An error occurred. Exiting...\n");
        return -1;
    }

    iperf_free_test(test);

    printf("\niperf Done.\n");

    return 0;
}

/**************************************************************************/
int
iperf_run(struct iperf_test * test)
{
    switch (test->role) {
        case 's':
            return iperf_run_server(test);
        case 'c':
            return iperf_run_client(test);
        default:
            usage();
            return 0;
    }
}

