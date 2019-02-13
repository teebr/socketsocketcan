/*
TODO: error frames aren't being looped back
*/

#include "client.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define HOSTNAME_LEN 128
#define BUF_SZ 100000

/* CONFIG PARAMETERS */
#ifndef DEBUG
#define DEBUG 0
#endif

#ifndef RECV_OWN_MSGS
#define RECV_OWN_MSGS 1// if 1, we well receive messages we sent. useful for logging.
#endif

const int LOOPBACK = RECV_OWN_MSGS;

#ifndef WAIT_FOR_TCP_CONNECTION
#define WAIT_FOR_TCP_CONNECTION 0
#endif

/* DEFINITIONS */
typedef struct
{
    time_t tv_sec;
    suseconds_t tv_usec;
    canid_t id;
    uint8_t dlc;//be careful, when serializing, struct seems to pad this to 4 bytes
    uint8_t data[CAN_MAX_DLEN];
} timestamped_frame;

typedef struct
{
    int tcp_sock;
    int can_sock;
} can_write_sockets; // used to supply multiple thread args.

/* FUNCTION DECLARATIONS */

// controlled exit in event of SIGINT
void handle_signal(int signal);

// print error information and exit (use before therads set-up)
void error(const char* msg);

// open up a TCP connection to the server
int create_tcp_socket(const char* hostname, int port);

// open a CAN socket
int open_can_socket(const char *port);

// read a single CAN frame and add timestamp
// int read_frame(int soc,timestamped_frame* tf);
int read_frame(int soc,struct can_frame* frame,struct timeval* tv);

// continually read CAN and add to buffer
void* read_poll_can(void *args);

// continually dump read CAN buffer to TCP
void* read_poll_tcp(void *args);

// read from TCP and write to CAN socket
void* write_poll(void *args);

// convert bytes back to timestamped_can
void deserialize_frame(char* ptr,timestamped_frame* tf);

// print CAN frame into to stdout (for debug)
void print_frame(timestamped_frame* tf);

/* GLOBALS */
pthread_mutex_t read_mutex = PTHREAD_MUTEX_INITIALIZER;
sig_atomic_t poll = true; // for "infinite loops" in threads.
bool tcp_ready_to_send = true; // only access inside of mutex
size_t socketcan_bytes_available;// only access inside of mutex

pthread_cond_t tcp_send_copied; //signal to enable thread.

char read_buf_can[BUF_SZ]; // where serialized CAN frames are dumped
char read_buf_tcp[BUF_SZ]; // where serialized CAN frames are copied to and sent to the server

/* FUNCTIONS */
void handle_signal(int signal) {
    if (signal == SIGINT)
    {
        poll = false;
    }
    //TODO: else what?
}
void error(const char* msg)
{
    perror(msg);
    exit(0);
}

int create_tcp_socket(const char* hostname, int port)
{
    struct sockaddr_in serv_addr;
    struct hostent *server;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        error("ERROR opening socket");
    }
    server = gethostbyname(hostname);
    if (server == NULL)
    {
        fprintf(stderr, "ERROR, no such host\n");
        exit(0);
    }
    bzero((char *)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
    serv_addr.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
#if WAIT_FOR_TCP_CONNECTION
        return -1;
#else
        error("ERROR connecting");
#endif
    }

    return fd;
}

int open_can_socket(const char *port)
{
    struct ifreq ifr;
    struct sockaddr_can addr;
    int soc;

    // open socket
    soc = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if(soc < 0)
    {
        return (-1);
    }

    // configure socket
    setsockopt(soc, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
               &LOOPBACK, sizeof(LOOPBACK));
    addr.can_family = AF_CAN;
    strcpy(ifr.ifr_name, port);
    if (ioctl(soc, SIOCGIFINDEX, &ifr) < 0)
    {
        return (-1);
    }

    addr.can_ifindex = ifr.ifr_ifindex; // why does this have to be after ioctl?

    if (bind(soc, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        return (-1);
    }

    return soc;
}

int read_frame(int soc,struct can_frame* frame,struct timeval* tv)
{
    //TODO: is it worth doing this, or just pass timeval as a separate argument
    int bytes;

    bytes = read(soc,frame,sizeof(*frame));
    ioctl(soc, SIOCGSTAMP, tv);

    return bytes;
}

void* read_poll_can(void* args)
{
    int fd = (int)args;
    timestamped_frame tf;
    struct can_frame frame;
    struct timeval tv;

    size_t count = 0;
    char* bufpnt = read_buf_can;
    const size_t frame_sz = sizeof(tf);

    while(poll)
    {
        if(count > (BUF_SZ-frame_sz))
        {
            //full buffer, drop data and start over. TODO: ring buffer, print/ debug
            bufpnt = read_buf_can;
            count = 0;
        }

        read_frame(fd,&frame,&tv); //blocking
        memcpy(bufpnt,(char*)(&tv),sizeof(struct timeval));
        bufpnt += sizeof(struct timeval);
        count += sizeof(struct timeval);
        memcpy(bufpnt,(char*)(&frame.can_id),sizeof(uint32_t));
        bufpnt += sizeof(uint32_t);
        count += sizeof(uint32_t);
        memcpy(bufpnt,&(frame.can_dlc),sizeof(uint8_t));
        bufpnt += sizeof(uint8_t);
        count += sizeof(uint8_t);

        memcpy(bufpnt,(char*)(&frame.data),sizeof(frame.data));
        bufpnt += sizeof(frame.data);
        count += sizeof(frame.data);

#if DEBUG
        printf("message read\n");
#endif
        pthread_mutex_lock(&read_mutex);
        if (tcp_ready_to_send) // other thread has said it is able to write to TCP socket
        {
            socketcan_bytes_available = count;
            memcpy(read_buf_tcp,read_buf_can,count);
            tcp_ready_to_send = false;
            const int signal_rv = pthread_cond_signal(&tcp_send_copied);
            if (signal_rv < 0)
            {
                error("could not signal to other thread.\n");
            }

            bufpnt = read_buf_can; //start filling up buffer again
            count = 0;
#if DEBUG
            printf("%d bytes copied to TCP buffer.\n",count);
#endif
        }
        pthread_mutex_unlock(&read_mutex);
    }
}

void* read_poll_tcp(void* args)
{
    int tcp_socket = (int)(args);
    size_t cpy_socketcan_bytes_available;
    int wait_rv;
    while(poll)
    {
        pthread_mutex_lock(&read_mutex);
        tcp_ready_to_send = true;
        while (!socketcan_bytes_available)
        {
            wait_rv = pthread_cond_wait(&tcp_send_copied, &read_mutex);
        }
        if (wait_rv < 0)
        {
            error("could not resume TCP send thread.\n");
        }
        cpy_socketcan_bytes_available = socketcan_bytes_available; // we should only access the original inside a mutex.
        socketcan_bytes_available = 0;
        pthread_mutex_unlock(&read_mutex);

        // don't want to perform the write inside mutex;
#if DEBUG
        printf("ready to send %d bytes\n",cpy_socketcan_bytes_available);
#endif
        int n = write(tcp_socket, read_buf_tcp,cpy_socketcan_bytes_available);
        if (n < cpy_socketcan_bytes_available)
        {
            error("failed to sent all bytes over TCP");
        }
#if DEBUG
        printf("%d bytes written to TCP\n",n);
        timestamped_frame tf;
        deserialize_frame(read_buf_tcp,&tf); //TODO: more than one frame.
        print_frame(&tf);
#endif
    }
}

void* write_poll(void* args)
{
    // CAN write should be quick enough to do in this loop...
    can_write_sockets* socks = (can_write_sockets*)args;
    struct can_frame frame;

    char write_buf[BUF_SZ];
    char* bufpnt = write_buf;
    const size_t frame_sz = 13; // 4 id + 1 dlc + 8 bytes
    const size_t can_struct_sz = sizeof(struct can_frame);

    while(poll)
    {
        int num_bytes_tcp = read(socks->tcp_sock,write_buf,BUF_SZ);
        if(num_bytes_tcp == 0)
        {
            //TODO: don't use error() call handle_signal
            error("Socket closed at other end... exiting.\n");
        }
#if DEBUG
        printf("%d bytes read from TCP.\n",num_bytes_tcp);
#endif
        int num_frames = num_bytes_tcp / frame_sz;

        for(int n = 0;n < num_frames;n++)
        {
            frame.can_id = ((uint32_t)(*bufpnt) << 0) | ((uint32_t)(*(bufpnt+1)) << 8) | ((uint32_t)(*(bufpnt+2)) << 16) | ((uint32_t)(*(bufpnt+3)) << 24);
            frame.can_dlc = (uint8_t)(*(bufpnt+4));
            memcpy(frame.data,bufpnt+5,frame.can_dlc);
#if DEBUG
            printf("frame %d | ID: %x | DLC: %d | Data:",n,frame.can_id,frame.can_dlc);
            for (int m = 0;m < frame.can_dlc;m++)
            {
                printf("%02x ",frame.data[m]);
            }
            printf("\n");
#endif
            int num_bytes_can = write(socks->can_sock, &frame, can_struct_sz);
            if (num_bytes_can < can_struct_sz)
            {
                printf("only send %d bytes of can message.\n",num_bytes_can);
                error("failed to send complete CAN message!\n");
            }
            bufpnt += frame_sz;
        }
        bufpnt = write_buf; //reset.
    }
}

void deserialize_frame(char* ptr,timestamped_frame* tf)
{
    // tf = (timestamped_frame*)ptr; // doesn't work, struct does some padding. manually populate fields?
    size_t count = 0;
    memcpy(&(tf -> tv_sec),ptr,sizeof(time_t));
    count += sizeof(time_t);

    memcpy(&(tf -> tv_usec),ptr+count,sizeof(suseconds_t));
    count += sizeof(suseconds_t);

    memcpy(&(tf -> id),ptr+count,sizeof(canid_t));
    count+= sizeof(canid_t);

    memcpy(&(tf -> dlc),ptr+count,sizeof(uint8_t));
    count += sizeof(uint8_t);

    memcpy(tf ->data,ptr+count,tf -> dlc);
}

void print_frame(timestamped_frame* tf)
{
    printf("\t%d.%d: ID %x | DLC %d | Data: ",tf->tv_sec,tf->tv_usec,tf->id,tf->dlc);
    for (int n=0;n<tf->dlc;n++)
    {
        printf("%02x ",tf->data[n]);
    }
    printf("\n");
}

int run(const char *can_port, const char *hostname, int port)
{
    pthread_t read_can_thread, read_tcp_thread, write_thread;
    int tcp_socket, can_socket;

    // initialising stuff
    if (pthread_mutex_init(&read_mutex, NULL) != 0)
    {
        printf("\n mutex init has failed\n");
        return 1;
    }

    can_socket = open_can_socket(can_port);

#if WAIT_FOR_TCP_CONNECTION
#if DEBUG
    printf("Waiting for TCP connection");
#endif
    do {
#if DEBUG
        printf(".");
        fflush(stdout);
#endif
        tcp_socket = create_tcp_socket(hostname, port);
        sleep(1);
    } while (tcp_socket == -1);
#if DEBUG
    printf("\nConnection established\n");
#endif
#else
    tcp_socket = create_tcp_socket(hostname, port);
#endif

    can_write_sockets write_args = {tcp_socket, can_socket};
    if(pthread_create(&read_can_thread, NULL, read_poll_can, (void*)can_socket) < 0)
    {
        error("unable to create thread");
    }
    if(pthread_create(&read_tcp_thread, NULL, read_poll_tcp, (void*)tcp_socket) < 0)
    {
        error("unable to create thread");
    }

    if(pthread_create(&write_thread, NULL, write_poll, (void*)(&write_args)) < 0)
    {
        error("unable to create thread");
    }

    pthread_join(read_can_thread,NULL);
    pthread_join(read_tcp_thread,NULL);
    pthread_join(write_thread,NULL);

    return 0;
}

int main(int argc, char* argv[])
{
    int port;
    char hostname[HOSTNAME_LEN];
    char can_port[HOSTNAME_LEN];

    // arg parsing
    if (argc < 4)
    {
        fprintf(stderr, "usage %s can-name hostname port\n", argv[0]);
        exit(0);
    }

    strncpy(can_port, argv[1], HOSTNAME_LEN);
    strncpy(hostname, argv[2], HOSTNAME_LEN);
    port = atoi(argv[3]);

    return run(can_port, hostname, port);
}
