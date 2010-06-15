
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
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_udp.h"
#include "iperf_tcp.h"
#include "timer.h"
#include "net.h"
#include "units.h"
#include "tcp_window_size.h"
#include "uuid.h"
#include "locale.h"

jmp_buf   env;			/* to handle longjmp on signal */

/*************************************************************/

/*
 * check to see if client has sent the requested number of bytes to the
 * server yet
 */

/*
 * XXX: should probably just compute this as we go and store it in the
 * iperf_test structure -blt
 */
int
all_data_sent(struct iperf_test * test)
{
    if (test->default_settings->bytes == 0)
	return 0;
    else
    {
	uint64_t  total_bytes = 0;
	struct iperf_stream *sp;

	sp = test->streams;

	while (sp)
	{
	    total_bytes += sp->result->bytes_sent;
	    sp = sp->next;
	}

	if (total_bytes >= (test->num_streams * test->default_settings->bytes))
	{
	    return 1;
	} else
	    return 0;
    }

}

/*********************************************************/

/**
 * iperf_exchange_parameters - handles the param_Exchange part for client
 *
 */

int
iperf_exchange_parameters(struct iperf_test * test)
{
    int       result;
    struct iperf_stream *sp;
    struct param_exchange *param;

    //printf("in exchange_parameters \n");
    sp = test->streams;
    sp->settings->state = PARAM_EXCHANGE;
    param = (struct param_exchange *) sp->buffer;

    get_uuid(test->default_settings->cookie);
    strncpy(param->cookie, test->default_settings->cookie, COOKIE_SIZE);
    //printf("client cookie: %s \n", param->cookie);

    /* setting up exchange parameters  */
    param->state = PARAM_EXCHANGE;
    param->blksize = test->default_settings->blksize;
    param->recv_window = test->default_settings->socket_bufsize;
    param->send_window = test->default_settings->socket_bufsize;
    param->format = test->default_settings->unit_format;

    //printf(" sending exchange params: size = %d \n", (int) sizeof(struct param_exchange));
    result = sp->snd(sp);
    if (result < 0)
    {
	perror("Error sending exchange params to server");
        return -1;
    }

    result = Nread(sp->socket, sp->buffer, sizeof(struct param_exchange), Ptcp);

    if (result < 0)
    {
	perror("Error getting exchange params ack from server");
        return -1;
    }

    if (result > 0 && sp->buffer[0] == ACCESS_DENIED)
    {
	fprintf(stderr, "Busy server Detected. Try again later. Exiting.\n");
	return -1;
    }

    return 0;
}

/*************************************************************/
/**
 * add_to_interval_list -- adds new interval to the interval_list
 *
 */

void
add_to_interval_list(struct iperf_stream_result * rp, struct iperf_interval_results * new)
{
    struct iperf_interval_results *ip = NULL;

    ip = (struct iperf_interval_results *) malloc(sizeof(struct iperf_interval_results));
    memcpy(ip, new, sizeof(struct iperf_interval_results));
    ip->next = NULL;

    if (rp->interval_results == NULL)	/* if 1st interval */
    {
	rp->interval_results = ip;
	rp->last_interval_results = ip; /* pointer to last element in list */
    } else
    {
	/* add to end of list */
	rp->last_interval_results->next = ip;
	rp->last_interval_results = ip;
    }
}

 /*************************************************************/
 /* for debugging only */
void
display_interval_list(struct iperf_stream_result * rp, int tflag)
{
    struct iperf_interval_results *n;
    float gb = 0.;

    n = rp->interval_results;

    printf("----------------------------------------\n");
    while (n!=NULL)
    {
	gb = (float)n->bytes_transferred / (1024. * 1024. * 1024.);
	printf("Interval = %f\tGBytes transferred = %.3f\n", n->interval_duration, gb);
	if (tflag)
	    print_tcpinfo(n);
	n = n->next;
    }
}

/************************************************************/

/**
 * receive_result_from_server - Receives result from server
 */

void
receive_result_from_server(struct iperf_test * test)
{
    int       result;
    struct iperf_stream *sp;
    int       size = 0;
    char     *buf = NULL;

    printf("in receive_result_from_server \n");
    sp = test->streams;
    size = MAX_RESULT_STRING;

    buf = (char *) malloc(size);

    printf("receive_result_from_server: send ALL_STREAMS_END to server \n");
    sp->settings->state = ALL_STREAMS_END;
    sp->snd(sp);		/* send message to server */

    printf("receive_result_from_server: send RESULT_REQUEST to server \n");
    sp->settings->state = RESULT_REQUEST;
    sp->snd(sp);		/* send message to server */

    /* receive from server */

    printf("reading results (size=%d) back from server \n", size);
    do
    {
	result = recv(sp->socket, buf, size, 0);
    } while (result == -1 && errno == EINTR);
    printf("Got size of results from server: %d \n", result);

    printf(server_reporting, sp->socket);
    puts(buf);			/* prints results */
    free(buf);

}

/*************************************************************/

/**
 * connect_msg -- displays connection message
 * denoting sender/receiver details
 *
 */

void
connect_msg(struct iperf_stream * sp)
{
    char      ipl[512], ipr[512];

    inet_ntop(AF_INET, (void *) (&((struct sockaddr_in *) & sp->local_addr)->sin_addr), (void *) ipl, sizeof(ipl));
    inet_ntop(AF_INET, (void *) (&((struct sockaddr_in *) & sp->remote_addr)->sin_addr), (void *) ipr, sizeof(ipr));

    printf("[%3d] local %s port %d connected to %s port %d\n",
	   sp->socket,
	   ipl, ntohs(((struct sockaddr_in *) & sp->local_addr)->sin_port),
	   ipr, ntohs(((struct sockaddr_in *) & sp->remote_addr)->sin_port));
}

/*************************************************************/
/**
 * Display -- Displays results for test
 * Mainly for DEBUG purpose
 *
 */

void
Display(struct iperf_test * test)
{
    struct iperf_stream *n;

    n = test->streams;
    int       count = 1;

    printf("===============DISPLAY==================\n");

    while (n != NULL)
    {
	 if (test->role == 'c')
		printf("position-%d\tsp=%d\tsocket=%d\tMbytes sent=%u\n", count++, (int) n, n->socket, (uint) (n->result->bytes_sent / (float)MB));
	 else
		printf("position-%d\tsp=%d\tsocket=%d\tMbytes received=%u\n", count++, (int) n, n->socket, (uint) (n->result->bytes_received / (float)MB));

         n = n->next;
    }
    printf("=================END====================\n");
    fflush(stdout);
}

/**************************************************************************/

struct iperf_test *
iperf_new_test()
{
    struct iperf_test *testp;

    //printf("in iperf_new_test: reinit default settings \n");
    testp = (struct iperf_test *) malloc(sizeof(struct iperf_test));
    if (!testp)
    {
	perror("malloc");
	return (NULL);
    }
    /* initialise everything to zero */
    memset(testp, 0, sizeof(struct iperf_test));

    testp->default_settings = (struct iperf_settings *) malloc(sizeof(struct iperf_settings));
    memset(testp->default_settings, 0, sizeof(struct iperf_settings));

    /* return an empty iperf_test* with memory alloted. */
    return testp;
}

/**************************************************************************/
void
iperf_defaults(struct iperf_test * testp)
{
    testp->protocol = Ptcp;
    testp->role = 's';
    testp->duration = DURATION;
    testp->server_port = PORT;

    testp->new_stream = iperf_new_tcp_stream;
    testp->stats_callback = iperf_stats_callback;
    testp->reporter_callback = iperf_reporter_callback;

    testp->stats_interval = 0;
    testp->reporter_interval = 0;
    testp->num_streams = 1;

    testp->default_settings->unit_format = 'a';
    testp->default_settings->socket_bufsize = 0;	/* use autotuning */
    testp->default_settings->blksize = DEFAULT_TCP_BLKSIZE;
    testp->default_settings->rate = RATE;	/* UDP only */
    testp->default_settings->state = TEST_START;
    testp->default_settings->mss = 0;
    testp->default_settings->bytes = 0;
    memset(testp->default_settings->cookie, '\0', COOKIE_SIZE);
}

/**************************************************************************/

void
iperf_init_test(struct iperf_test * test)
{
    char      ubuf[UNIT_LEN];
    struct iperf_stream *sp;
    int       i, s = 0;

    if (test->role == 's')
    {				/* server */
	if (test->protocol == Pudp)
	{
	    test->listener_sock_udp = netannounce(Pudp, NULL, test->server_port);
	    if (test->listener_sock_udp < 0)
		exit(0);
	}
	/* always create TCP connection for control messages */
	test->listener_sock_tcp = netannounce(Ptcp, NULL, test->server_port);
	if (test->listener_sock_tcp < 0)
	    exit(0);

	if (test->protocol == Ptcp)
	{
	    if (set_tcp_windowsize(test->listener_sock_tcp, test->default_settings->socket_bufsize, SO_RCVBUF) < 0)
		perror("unable to set TCP window");
	}
	/* make sure that accept call does not block */
	setnonblocking(test->listener_sock_tcp);
	setnonblocking(test->listener_sock_udp);

	printf("-----------------------------------------------------------\n");
	printf("Server listening on %d\n", test->server_port);
	int       x;

	/* make sure we got what we asked for */
	if ((x = get_tcp_windowsize(test->listener_sock_tcp, SO_RCVBUF)) < 0)
	    perror("SO_RCVBUF");

	if (test->protocol == Ptcp)
	{
	    {
		if (test->default_settings->socket_bufsize > 0)
		{
		    unit_snprintf(ubuf, UNIT_LEN, (double) x, 'A');
		    printf("TCP window size: %s\n", ubuf);
		} else
		{
		    printf("Using TCP Autotuning \n");
		}
	    }
	}
	printf("-----------------------------------------------------------\n");

    }
    /* This code is being removed. Commented out until removal
    else if (test->role == 'c')
    {				// Client
	FD_ZERO(&test->write_set);
	FD_SET(s, &test->write_set);

         // XXX: I think we need to create a TCP control socket here too for
         // UDP mode -blt
	for (i = 0; i < test->num_streams; i++)
	{
	    s = netdial(test->protocol, test->server_hostname, test->server_port);
	    if (s < 0)
	    {
		fprintf(stderr, "netdial failed\n");
		exit(0);
	    }
	    FD_SET(s, &test->write_set);
	    test->max_fd = (test->max_fd < s) ? s : test->max_fd;

	    sp = test->new_stream(test);
	    sp->socket = s;
	    iperf_init_stream(sp, test);
	    iperf_add_stream(test, sp);

	    connect_msg(sp);	// print connection established message
	}
    }
    */
}

/* iperf_connect -- client to server connection function */
int
iperf_connect(struct iperf_test *test)
{
    struct iperf_stream *sp;
    int i, s = 0;

    /* For Select: Set the test->write_set select set to zero, then set the s fd */
    FD_ZERO(&test->write_set);

    for (i = 0; i < test->num_streams; i++) {
        s = netdial(test->protocol, test->server_hostname, test->server_port);
        if (s < 0) {
            // Change to new error handling mode
            fprintf(stderr, "error: netdial failed\n");
            exit(1);
        }
        FD_SET(s, &test->write_set);
        test->max_fd = (test->max_fd < s) ? s : test->max_fd;

        sp = test->new_stream(test);
        sp->socket = s;
        iperf_init_stream(sp, test);
        iperf_add_stream(test, sp);

        connect_msg(sp);
    }
    
    return 1;
}

/**************************************************************************/
void
iperf_free_test(struct iperf_test * test)
{
    free(test->default_settings);

    // This funciton needs to be updated to free and close streams
    // Currently it just sets the pointer to the streams list to NULL...

    close(test->listener_sock_tcp);
    close(test->listener_sock_udp);

    test->streams = NULL;
    test->accept = NULL;
    test->stats_callback = NULL;
    test->reporter_callback = NULL;
    test->new_stream = NULL;
    free(test);
}

/**************************************************************************/

/**
 * iperf_stats_callback -- handles the statistic gathering for both the client and server
 *
 *returns void *
 *
 */


void     *
iperf_stats_callback(struct iperf_test * test)
{
    struct iperf_stream *sp = test->streams;
    struct iperf_stream_result *rp = NULL;
    struct iperf_interval_results *ip = NULL, temp;

    //printf("in stats_callback: num_streams = %d role = %c\n", test->num_streams, test->role);

    while (sp != NULL)
    {
	rp = sp->result;

	if (test->role == 'c')
	    temp.bytes_transferred = rp->bytes_sent_this_interval;
	else
	    temp.bytes_transferred = rp->bytes_received_this_interval;
     
	ip = sp->result->interval_results;
	/* result->end_time contains timestamp of previous interval */
        if ( ip != NULL ) /* not the 1st interval */
	    memcpy(&temp.interval_start_time, &sp->result->end_time, sizeof(struct timeval));
        else /* or use timestamp from beginning */
            memcpy(&temp.interval_start_time, &sp->result->start_time, sizeof(struct timeval));
	/* now save time of end of this interval */
	gettimeofday(&sp->result->end_time, NULL);
	memcpy(&temp.interval_end_time, &sp->result->end_time, sizeof(struct timeval));
	temp.interval_duration = timeval_diff(&temp.interval_start_time, &temp.interval_end_time);
	//temp.interval_duration = timeval_diff(&temp.interval_start_time, &temp.interval_end_time);
	if (test->tcp_info)
	    get_tcpinfo(test, &temp);
        //printf(" iperf_stats_callback: adding to interval list: \n");
	add_to_interval_list(rp, &temp);
        rp->bytes_sent_this_interval = rp->bytes_received_this_interval = 0;

	/* for debugging */
	//display_interval_list(rp, test->tcp_info); 
	sp = sp->next;
    }				/* for each stream */

    return 0;
}

/**************************************************************************/

/**
 * iperf_reporter_callback -- handles the report printing
 *
 *returns report
 *
 */

char     *
iperf_reporter_callback(struct iperf_test * test)
{
    int       total_packets = 0, lost_packets = 0, curr_state = 0;
    char     *message = NULL;
    char     *message_final = NULL;
    char      ubuf[UNIT_LEN];
    char      nbuf[UNIT_LEN];
    struct iperf_stream *sp = NULL;
    iperf_size_t bytes = 0, total_bytes = 0;
    double    start_time, end_time;
    struct iperf_interval_results *ip = NULL;

    message = (char *)calloc(MAX_RESULT_STRING, sizeof(char));
    message_final = (char *)calloc(MAX_RESULT_STRING, sizeof(char));

    sp = test->streams;
    curr_state = sp->settings->state;
    //printf("in iperf_reporter_callback: state = %d \n", curr_state);

    if (curr_state == TEST_RUNNING || curr_state == STREAM_RUNNING)
    {
	/* print interval results for each stream */
	while (sp)
	{
	    message_final = print_interval_results(test, sp, message_final);
            bytes += sp->result->interval_results->bytes_transferred; /* sum up all streams */
	    sp = sp->next;
	}
        if (bytes <=0 )  /* this can happen if timer goes off just when client exits */
	     return NULL;

	/* next build string with sum of all streams */
        sp = test->streams; /* reset back to 1st stream */
	if (test->num_streams > 1)
	{
	    ip = test->streams->result->last_interval_results;	/* use 1st stream for timing info */
	    unit_snprintf(ubuf, UNIT_LEN, (double) (bytes), 'A');

            start_time = timeval_diff(&sp->result->start_time,&ip->interval_start_time);
            end_time = timeval_diff(&sp->result->start_time,&ip->interval_end_time);
            unit_snprintf(nbuf, UNIT_LEN, (double) (bytes / ip->interval_duration),
			      test->default_settings->unit_format);
	    sprintf(message, report_sum_bw_format, start_time, end_time, ubuf, nbuf);
            //printf("iperf_reporter_callback 1: start_time: %.3f  end_time: %.3f \n", start_time, end_time);
            //printf("iperf_reporter_callback 1: message = %s \n", message);
	    safe_strcat(message_final, message);

#ifdef NOT_DONE			/* is it usful to figure out a way so sum
				 * TCP_info acrross multiple streams? */
	    if (test->tcp_info)
	    {
		build_tcpinfo_message(ip, message);
		safe_strcat(message_final, message);
	    }
#endif
	}
    } else
    {
	/* print final summary for all intervals */
	if (curr_state == ALL_STREAMS_END || curr_state == RESULT_REQUEST)
	{
	    sp = test->streams;
	    start_time = 0.;
	    end_time = timeval_diff(&sp->result->start_time, &sp->result->end_time);

	    while (sp)
	    {
		if (test->role == 'c')
		    bytes = sp->result->bytes_sent;
		else
		    bytes = sp->result->bytes_received;

                total_bytes += bytes;
		if (test->protocol == Pudp)
		{
		    total_packets += sp->packet_count;
		    lost_packets += sp->cnt_error;
		}

                if (bytes > 0 ) 
	        {
		    unit_snprintf(ubuf, UNIT_LEN, (double) (bytes), 'A');
		    unit_snprintf(nbuf, UNIT_LEN, (double) (bytes / end_time), test->default_settings->unit_format);

		    if (test->protocol == Ptcp)
		    {
		        sprintf(message, report_bw_format, sp->socket, start_time, end_time, ubuf, nbuf);
                        //printf("iperf_reporter_callback 2: message = %s \n", message);
		        safe_strcat(message_final, message);
#if defined(linux) || defined(__FreeBSD__)
		        if (test->tcp_info)
		        {
			    //printf("Final TCP_INFO results: \n");
	                    ip = sp->result->last_interval_results;	
			    build_tcpinfo_message(ip, message);
			    safe_strcat(message_final, message);
		        }
#endif
		    } else
		    {		/* UDP mode */
		        sprintf(message, report_bw_jitter_loss_format, sp->socket, start_time,
			        end_time, ubuf, nbuf, sp->jitter * 1000, sp->cnt_error, 
				    sp->packet_count, (double) (100.0 * sp->cnt_error / sp->packet_count));
		        safe_strcat(message_final, message);
    
		        if (test->role == 'c')
		        {
			    sprintf(message, report_datagrams, sp->socket, sp->packet_count);
			    safe_strcat(message_final, message);
		        }
		        if (sp->outoforder_packets > 0)
			    printf(report_sum_outoforder, start_time, end_time, sp->cnt_error);
		    }
		}
		sp = sp->next;
	    }
	}			/* while (sp) */

	unit_snprintf(ubuf, UNIT_LEN, (double) total_bytes, 'A');
	unit_snprintf(nbuf, UNIT_LEN, (double) total_bytes / end_time, test->default_settings->unit_format);

	if ((test->role == 'c' || (test->role == 's')) && test->num_streams > 1)
	{
	    if (test->protocol == Ptcp)
	    {
		sprintf(message, report_sum_bw_format, start_time, end_time, ubuf, nbuf);
		safe_strcat(message_final, message);
	    } else
	    {
		sprintf(message, report_sum_bw_jitter_loss_format, start_time, end_time, ubuf, nbuf, sp->jitter, lost_packets, total_packets, (double) (100.0 * lost_packets / total_packets));
		safe_strcat(message_final, message);
	    }


	    if ((test->print_mss != 0) && (test->role == 'c'))
	    {
		sprintf(message, "\nThe TCP maximum segment size mss = %d \n", getsock_tcp_mss(sp->socket));
		safe_strcat(message_final, message);
	    }
	}
    }
    free(message);
    return message_final;
}

/**************************************************************************/
char     *
print_interval_results(struct iperf_test * test, struct iperf_stream * sp, char *message_final)
{
    static int       first_stream = 1;
    char      ubuf[UNIT_LEN];
    char      nbuf[UNIT_LEN];
    double    st = 0., et = 0.;
    struct iperf_interval_results *ir = NULL;
    char     *message = (char *) malloc(MAX_RESULT_STRING);

    //printf("in print_interval_results for stream %d \n", sp->socket);
    ir = sp->result->last_interval_results; /* get last entry in linked list */
    if (ir == NULL)
    {
	printf("print_interval_results Error: interval_results = NULL \n");
	return NULL;
    }
    if (first_stream)		/* only print header for 1st stream */
    {
	sprintf(message, report_bw_header);
	safe_strcat(message_final, message);
	first_stream = 0;
    }
    unit_snprintf(ubuf, UNIT_LEN, (double) (ir->bytes_transferred), 'A');

    unit_snprintf(nbuf, UNIT_LEN,
		  (double) (ir->bytes_transferred / ir->interval_duration), test->default_settings->unit_format);
    st = timeval_diff(&sp->result->start_time,&ir->interval_start_time);
    et = timeval_diff(&sp->result->start_time,&ir->interval_end_time);
    sprintf(message, report_bw_format, sp->socket, st, et, ubuf, nbuf);
    //printf("print_interval_results 1: message = %s \n", message);
    safe_strcat(message_final, message);

#if defined(linux) || defined(__FreeBSD__)
    if (test->tcp_info)
    {
	build_tcpinfo_message(ir, message);
	safe_strcat(message_final, message);
    }
#endif
    //printf("reporter_callback: built interval string: %s \n", message_final);
    free(message);
    return message_final;
}

/**************************************************************************/
void
safe_strcat(char *s1, char *s2)
{
    //printf(" adding string %s to end of string %s \n", s1, s1);
    if (strlen(s1) + strlen(s2) < MAX_RESULT_STRING)
	strcat(s1, s2);
    else
    {
	printf("Error: results string too long \n");
	exit(-1);		/* XXX: should return an error instead! */
	/* but code that calls this needs to check for error first */
	//return -1;
    }
}

/**************************************************************************/
void
iperf_free_stream(struct iperf_stream * sp)
{
    /* XXX: need to free interval list too! */
    free(sp->buffer);
    free(sp->settings);
    free(sp->result);
    free(sp->send_timer);
    free(sp);
}

/**************************************************************************/
struct iperf_stream *
iperf_new_stream(struct iperf_test * testp)
{
    int       i = 0;
    struct iperf_stream *sp;

    //printf("in iperf_new_stream \n");
    sp = (struct iperf_stream *) malloc(sizeof(struct iperf_stream));
    if (!sp)
    {
	perror("malloc");
	return (NULL);
    }
    memset(sp, 0, sizeof(struct iperf_stream));

    //printf("iperf_new_stream: Allocating new stream buffer: size = %d \n", testp->default_settings->blksize);
    sp->buffer = (char *) malloc(testp->default_settings->blksize);
    sp->settings = (struct iperf_settings *) malloc(sizeof(struct iperf_settings));
    /* make a per stream copy of default_settings in each stream structure */
    memcpy(sp->settings, testp->default_settings, sizeof(struct iperf_settings));
    sp->result = (struct iperf_stream_result *) malloc(sizeof(struct iperf_stream_result));

    /* fill in buffer with random stuff */
    srandom(time(0));
    for (i = 0; i < testp->default_settings->blksize; i++)
	sp->buffer[i] = random();

    sp->socket = -1;

    sp->packet_count = 0;
    sp->stream_id = (int) sp;
    sp->jitter = 0.0;
    sp->prev_transit = 0.0;
    sp->outoforder_packets = 0;
    sp->cnt_error = 0;

    sp->send_timer = NULL;
    sp->next = NULL;

    sp->result->interval_results = NULL;
    sp->result->last_interval_results = NULL;
    sp->result->bytes_received = 0;
    sp->result->bytes_sent = 0;
    sp->result->bytes_received_this_interval = 0;
    sp->result->bytes_sent_this_interval = 0;
    gettimeofday(&sp->result->start_time, NULL);

    sp->settings->state = STREAM_BEGIN;
    return sp;
}

/**************************************************************************/
void
iperf_init_stream(struct iperf_stream * sp, struct iperf_test * testp)
{
    socklen_t len;

    len = sizeof(struct sockaddr_in);

    if (getsockname(sp->socket, (struct sockaddr *) & sp->local_addr, &len) < 0) {
        perror("getsockname");
    }
    if (getpeername(sp->socket, (struct sockaddr *) & sp->remote_addr, &len) < 0) {
        perror("getpeername");
    }
    if (testp->protocol == Ptcp) {
        if (set_tcp_windowsize(sp->socket, testp->default_settings->socket_bufsize,
                testp->role == 's' ? SO_RCVBUF : SO_SNDBUF) < 0)
            fprintf(stderr, "unable to set window size\n");

        /* set TCP_NODELAY and TCP_MAXSEG if requested */
        set_tcp_options(sp->socket, testp->no_delay, testp->default_settings->mss);
    }
}

/**************************************************************************/
int
iperf_add_stream(struct iperf_test * test, struct iperf_stream * sp)
{
    struct iperf_stream *n;

    if (!test->streams) {
        test->streams = sp;
        return 1;
    } else {
        n = test->streams;
        while (n->next)
            n = n->next;
        n->next = sp;
        return 1;
    }

    return 0;
}


/**************************************************************************/
void
catcher(int sig)
{
    longjmp(env, sig);
}

/**************************************************************************/

int
iperf_client_start(struct iperf_test *test)
{
    int i;
    char *prot, *result_string;
    int64_t delayus, adjustus, dtargus;
    struct iperf_stream *sp, *np;
    struct timer *timer, *stats_interval, *reporter_interval;
    struct sigaction sact;

    test->streams->settings->state = STREAM_BEGIN;
    timer = stats_interval = reporter_interval = NULL;

    // Set signal handling
    sigemptyset(&sact.sa_mask);
    sact.sa_flags = 0;
    sact.sa_handler = catcher;
    sigaction(SIGINT, &sact, NULL);

    // Not sure what the following code does yet. It's UDP, so I'll get to fixing it eventually
    if (test->protocol == Pudp) {
        prot = "UDP";
        dtargus = (int64_t) (test->default_settings->blksize) * SEC_TO_US * 8;
        dtargus /= test->default_settings->rate;

        assert(dtargus != 0);

        delayus = dtargus;
        adjustus = 0;

        printf("iperf_run_client: adjustus: %lld, delayus %lld \n", adjustus, delayus);

        // New method for iterating through streams
        for (sp = test->streams; sp != NULL; sp = sp->next)
            sp->send_timer = new_timer(0, dtargus);

    } else {
        prot = "TCP";
    }

    /* if -n specified, set zero timer */
    // Set timers and print usage message
    if (test->default_settings->bytes == 0) {
        timer = new_timer(test->duration, 0);
        printf("Starting Test: protocol: %s, %d streams, %d byte blocks, %d second test \n",
            prot, test->num_streams, test->default_settings->blksize, test->duration);
    } else {
        timer = new_timer(0, 0);
        printf("Starting Test: protocol: %s, %d streams, %d byte blocks, %d bytes to send\n",
            prot, test->num_streams, test->default_settings->blksize, (int) test->default_settings->bytes);
    }
    if (test->stats_interval != 0)
        stats_interval = new_timer(test->stats_interval, 0);
    if (test->reporter_interval != 0)
        reporter_interval = new_timer(test->reporter_interval, 0);

    // Begin testing...
    while (!all_data_sent(test) && !timer->expired(timer)) {

        // Send data
        for (sp = test->streams; sp != NULL; sp = sp->next) {
	        if (sp->snd(sp) < 0) {
                perror("iperf_client_start: snd");
                // do other stuff on error
            }
        }

        // Perform callbacks
        if ((test->stats_interval != 0) && stats_interval->expired(stats_interval)) {
            test->stats_callback(test);
            update_timer(stats_interval, test->stats_interval, 0);
        }
        if ((test->reporter_interval != 0) && reporter_interval->expired(reporter_interval)) {
            result_string = test->reporter_callback(test);
            puts(result_string);
            update_timer(reporter_interval, test->reporter_interval, 0);
            free(result_string);
        }

        /* detecting Ctrl+C */
        // Why is the setjmp inside of the while??
        if (setjmp(env))
            break;
    }
    
    // Free timers
    free(timer);
    free(stats_interval);
    free(reporter_interval);

    return 0;
}

int
iperf_client_end(struct iperf_test *test)
{
    // Delete the char *
    char *result_string;
    struct iperf_stream *sp, *np;
    printf("Test Complete. Summary Results:\n");
    
    /* send STREAM_END packets */
    test->streams->settings->state = STREAM_END;
    for (sp = test->streams; sp != NULL; sp = sp->next) {
        if (sp->snd(sp) < 0) {
            // Add some error handling code
        }
    }

    /* send ALL_STREAMS_END packet to 1st socket */
    sp = test->streams;
    sp->settings->state = ALL_STREAMS_END;
    sp->snd(sp);
    //printf("Done Sending ALL_STREAMS_END. \n");

    /* show final summary */
    test->stats_callback(test);
    result_string = test->reporter_callback(test);
    puts(result_string);
    free(result_string);

    /* Requesting for result from Server
     *
     * This still needs to be implemented. It looks like the last
     * intern worked on it, but it needs to be finished.
     */

    test->default_settings->state = RESULT_REQUEST;
    //receive_result_from_server(test);	/* XXX: currently broken! */
    //result_string = test->reporter_callback(test);
    //printf("Summary results as measured by the server: \n");
    //puts(result_string);

    //printf("Done getting/printing results. \n");

    //printf("send TEST_END to server \n");
    sp->settings->state = TEST_END;
    sp->snd(sp);		/* send message to server */

    /* Deleting all streams - CAN CHANGE FREE_STREAM FN */
    for (sp = test->streams; sp != NULL; sp = np) {
        close(sp->socket);
        np = sp->next;
        iperf_free_stream(sp);
    }

    return 0;

}

int
iperf_run_client(struct iperf_test * test)
{
    /* Start the client and connect to the server */
    if (iperf_connect(test) < 0) {
        // set error and return
        return -1;
    }

    /* Exchange parameters with the server */
    if (iperf_exchange_parameters(test) < 0) {
        // This needs to set error
        return -1;
    }

    /* Start the iperf test */
    if (iperf_client_start(test) < 0) {
        return -1;
    }

    /* End the iperf test and clean up client specific memory */
    if (iperf_client_end(test) < 0) {
        return -1;
    }

    return 0;
}
