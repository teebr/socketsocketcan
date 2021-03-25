/*
TODO: error frames aren't being looped back
*/

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <math.h>
#include <cerrno>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/sockios.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unordered_map>

#define BUF_SZ 100000

/* CONFIG PARAMETERS */
#ifndef DEBUG
#define DEBUG 0
#endif

#ifndef RECV_OWN_MSGS
#define RECV_OWN_MSGS 1 // If 1, we well receive messages we sent. useful for logging.
#endif

const int LOOPBACK = RECV_OWN_MSGS;

#ifndef WAIT_FOR_TCP_CONNECTION
#define WAIT_FOR_TCP_CONNECTION 0
#endif

/* DEFINITIONS */
typedef struct __attribute__((packed, aligned(1)))
{
    time_t tv_sec;
    suseconds_t tv_usec;
    canid_t id;
    uint8_t dlc;
    uint8_t data[CAN_MAX_DLEN];
} timestamped_frame;

typedef struct
{
    int can_sock;
    bool use_unordered_map;
} can_read_args; // used to supply multiple thread args.

typedef struct
{
    int tcp_sock;
    int limit_recv_rate_hz;
} tcp_read_args; // used to supply multiple thread args.

typedef struct
{
    int tcp_sock;
    int can_sock;
} can_write_sockets; // used to supply multiple thread args.

/* FUNCTION DECLARATIONS */

// controlled exit in event of SIGINT
void handle_signal(int signal);

// print error information and exit (use before threads set-up)
void error(const char* msg, int error_code);

// print error information and exit thread
void pthread_error(const char* msg, int error_code);

// open up a TCP connection to the server
int create_tcp_socket(const char* hostname, int port);

// open a CAN socket
int open_can_socket(const char *port, const struct can_filter *filter, int numfilter);

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
void deserialize_frame(const char* ptr, timestamped_frame* tf);

// print CAN frame into to stdout (for debug)
void print_frame(const timestamped_frame* tf);

/* GLOBALS */
pthread_mutex_t read_mutex = PTHREAD_MUTEX_INITIALIZER;
sig_atomic_t poll = true; // for "infinite loops" in threads.
bool tcp_ready_to_send = true; // only access inside of mutex
size_t socketcan_bytes_available = 0;// only access inside of mutex

pthread_cond_t tcp_send_copied; //signal to enable thread.

char read_buf_can[BUF_SZ]; // where serialized CAN frames are dumped
char read_buf_tcp[BUF_SZ]; // where serialized CAN frames are copied to and sent to the server

/* FUNCTIONS */
void handle_signal(int signal) {
    poll = false;
}

void error(const char* msg, int error_code)
{
    poll = false;
    perror(msg);
    exit(error_code);
}

void pthread_error(const char* msg, int error_code)
{
    poll = false;
    fprintf(stderr, "%s: %d\n", msg, error_code);
    pthread_exit(&error_code);
}

int create_tcp_socket(const char* hostname, int port)
{
    struct sockaddr_in serv_addr;
    struct hostent *server;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        error("ERROR opening socket", errno);
    }

    // Enable 1s timeout, so the thread is not blocking
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0)
    {
        error("ERROR setting timeout", errno);
    }

    server = gethostbyname(hostname);
    if (server == NULL)
    {
        error("ERROR, no such host", h_errno);
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
        error("ERROR connecting", errno);
#endif
    }

    return fd;
}

int open_can_socket(const char *port, const struct can_filter *p_filter, int numfilter)
{
    struct ifreq ifr;
    struct sockaddr_can addr;
    int soc;

    // open socket
    soc = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (soc < 0)
    {
        error("ERROR failed to open socket", errno);
    }

    // configure socket
    if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &LOOPBACK, sizeof(LOOPBACK)) < 0)
    {
        error("ERROR setting loopback", errno);
    }

    // Enable 1s timeout, so the thread is not blocking
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0)
    {
        error("ERROR setting timeout", errno);
    }

    const int ERR_MASK = CAN_ERR_MASK; // Enable error frames
    if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &ERR_MASK, sizeof(ERR_MASK)) < 0)
    {
        error("ERROR enabling error frames", errno);
    }

    if (numfilter > 0)
    {
        // Set filter and mask
        if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_FILTER, p_filter, numfilter * sizeof(struct can_filter)) < 0)
        {
            error("ERROR setting user filter and masks", errno);
        }
    }
    else
    {
        // Receive everything
        struct can_filter filter = { .can_id = 0, .can_mask = 0 };
        if (setsockopt(soc, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(struct can_filter)) < 0)
        {
            error("ERROR setting filter and masks", errno);
        }
    }

    addr.can_family = AF_CAN;
    strcpy(ifr.ifr_name, port);
    if (ioctl(soc, SIOCGIFINDEX, &ifr) < 0)
    {
        error("ERROR failed to set ioctl", errno);
    }

    addr.can_ifindex = ifr.ifr_ifindex; // why does this have to be after ioctl?
    if (bind(soc, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        error("ERROR failed to bind to socket", errno);
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
    can_read_args* read_args = (can_read_args*)args;
    int fd = read_args->can_sock;
    bool use_unordered_map = read_args->use_unordered_map;

    timestamped_frame tf;
    struct can_frame frame;
    struct timeval tv;

    size_t count = 0;
    char* bufpnt = read_buf_can;
    const size_t frame_sz = sizeof(tf);

    std::unordered_map<canid_t, timestamped_frame> hash_map;

    while (poll)
    {
        if (count > (BUF_SZ-frame_sz))
        {
            //full buffer, drop data and start over. TODO: ring buffer, print/ debug
            bufpnt = read_buf_can;
            count = 0;
        }

        int num_bytes_can = read_frame(fd, &frame, &tv);
        if (num_bytes_can == -1)
        {
            // This happens when there is a timeout, we simply keep looping, as we want do not want to block while
            // reading the CAN-Bus, as then we would never be able to shut down the threads if there was no activity
            // on the bus
            continue;
        }
        else if (num_bytes_can == 0)
        {
            // This will happen when we shut down the client, so report an success
            pthread_error("Socket closed at other end... exiting", 0);
        }

        if (use_unordered_map)
        {
            tf.tv_sec = tv.tv_sec;
            tf.tv_usec = tv.tv_usec;
            tf.id = frame.can_id;
            tf.dlc = frame.can_dlc;
            memcpy(tf.data, frame.data, sizeof(frame.data));
            hash_map[tf.id] = tf;
        }
        else
        {
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
        }

#if DEBUG
        printf("message read\n");
#endif
        pthread_mutex_lock(&read_mutex);
        if (tcp_ready_to_send) // other thread has said it is able to write to TCP socket
        {
            if (use_unordered_map)
            {
                socketcan_bytes_available = hash_map.size() * sizeof(tf);
                size_t i = 0;
                for (const auto &n : hash_map) {
                    memcpy(read_buf_tcp + i, &n.second, sizeof(tf));
                    i += sizeof(tf);
                }

                tcp_ready_to_send = false;
                const int signal_rv = pthread_cond_signal(&tcp_send_copied);
                if (signal_rv < 0)
                {
                    pthread_mutex_unlock(&read_mutex);
                    pthread_error("could not signal to other thread", signal_rv);
                }

#if DEBUG
                printf("%zu bytes copied to TCP buffer.\n", socketcan_bytes_available);
#endif
                hash_map.clear();
            }
            else
            {
                socketcan_bytes_available = count;
                memcpy(read_buf_tcp, read_buf_can, count);

                tcp_ready_to_send = false;
                const int signal_rv = pthread_cond_signal(&tcp_send_copied);
                if (signal_rv < 0)
                {
                    pthread_mutex_unlock(&read_mutex);
                    pthread_error("could not signal to other thread", signal_rv);
                }

#if DEBUG
                printf("%zu bytes copied to TCP buffer.\n", count);
#endif
                bufpnt = read_buf_can; //start filling up buffer again
                count = 0;
            }
        }
        pthread_mutex_unlock(&read_mutex);
    }

    // Make sure the 'read_poll_tcp' thread is unblocked
    // No need to lock the mutex, as we do not care about predictable scheduling behaviour, as we are about to exit
    (void)pthread_cond_signal(&tcp_send_copied);

    pthread_exit(NULL);
}

void* read_poll_tcp(void* args)
{
    tcp_read_args* read_args = (tcp_read_args*)args;
    int tcp_socket = read_args->tcp_sock;
    int limit_recv_rate_hz = read_args->limit_recv_rate_hz;

    size_t cpy_socketcan_bytes_available;
    int wait_rv = 0;
    while (poll)
    {
        pthread_mutex_lock(&read_mutex);
        tcp_ready_to_send = true;
        while (!socketcan_bytes_available)
        {
            wait_rv = pthread_cond_wait(&tcp_send_copied, &read_mutex);
            if (!poll)
            {
                // Break out if the poll flag has gone low
                socketcan_bytes_available = 0; // We do not care about the data, as we are about to exit
                break;
            }
        }
        if (wait_rv < 0)
        {
            pthread_mutex_unlock(&read_mutex);
            pthread_error("could not resume TCP send thread", wait_rv);
        }
        cpy_socketcan_bytes_available = socketcan_bytes_available; // we should only access the original inside a mutex.
        socketcan_bytes_available = 0;
        pthread_mutex_unlock(&read_mutex);

        // don't want to perform the write inside mutex;
#if DEBUG
        printf("ready to send %zu bytes\n", cpy_socketcan_bytes_available);
#endif
        int n = write(tcp_socket, read_buf_tcp,cpy_socketcan_bytes_available);
        if (n < 0)
        {
            pthread_error("failed to write bytes over TCP socket", EXIT_FAILURE);
        }
        else if ((size_t)n < cpy_socketcan_bytes_available)
        {
            fprintf(stderr, "only send %d bytes of TCP message.\n", n);
            pthread_error("failed to sent all bytes over TCP socket", EXIT_FAILURE);
        }
#if DEBUG
        printf("%d bytes written to TCP\n",n);
        timestamped_frame tf;
        deserialize_frame(read_buf_tcp,&tf); //TODO: more than one frame.
        print_frame(&tf);
#endif

        if (limit_recv_rate_hz > 0)
        {
            struct timespec ts;
            int milliseconds = (int)roundf(1000.0f / (float)limit_recv_rate_hz);
            ts.tv_sec = milliseconds / 1000;
            ts.tv_nsec = (milliseconds % 1000) * 1000000;
            nanosleep(&ts, NULL);
        }
    }

    pthread_exit(NULL);
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

    while (poll)
    {
        int num_bytes_tcp = read(socks->tcp_sock,write_buf,BUF_SZ);
        if (num_bytes_tcp == -1)
        {
            // This happens when there is a timeout, we simply keep looping, as we want do not want to block while
            // reading the CAN-Bus, as then we would never be able to shut down the threads if there was no activity
            // on the bus
            continue;
        }
        else if (num_bytes_tcp == 0)
        {
            // This will happen when we shut down the client, so report an success
            pthread_error("Socket closed at other end... exiting", 0);
        }
#if DEBUG
        printf("%d bytes read from TCP.\n", num_bytes_tcp);
#endif
        int num_frames = num_bytes_tcp / frame_sz;

        for(int n = 0;n < num_frames;n++)
        {
            frame.can_id = ((uint32_t)(*bufpnt) << 0) | ((uint32_t)(*(bufpnt+1)) << 8) | ((uint32_t)(*(bufpnt+2)) << 16) | ((uint32_t)(*(bufpnt+3)) << 24);
            frame.can_dlc = (uint8_t)(*(bufpnt+4));
            memcpy(frame.data,bufpnt+5,frame.can_dlc);
#if DEBUG
            printf("frame %d | ID: %x | DLC: %u | Data:",n,frame.can_id,frame.can_dlc);
            for (int m = 0;m < frame.can_dlc;m++)
            {
                printf("%02x ",frame.data[m]);
            }
            printf("\n");
#endif
            int num_bytes_can = write(socks->can_sock, &frame, can_struct_sz);
            if (num_bytes_can < 0)
            {
                pthread_error("failed to write bytes over CAN socket", EXIT_FAILURE);
            }
            else if ((size_t)num_bytes_can < can_struct_sz)
            {
                fprintf(stderr, "only send %d bytes of can message.\n", num_bytes_can);
                pthread_error("failed to send complete CAN message!", EXIT_FAILURE);
            }
            bufpnt += frame_sz;
        }
        bufpnt = write_buf; //reset.
    }

    pthread_exit(NULL);
}

void deserialize_frame(const char* ptr, timestamped_frame* tf)
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

void print_frame(const timestamped_frame* tf)
{
    printf("\t%ld.%ld: ID %x | DLC %u | Data: ",tf->tv_sec,tf->tv_usec,tf->id,tf->dlc);
    for (int n=0;n<tf->dlc;n++)
    {
        printf("%02x ",tf->data[n]);
    }
    printf("\n");
}

int tcpclient(const char *can_port, const char *hostname, int port, const struct can_filter *filter, int numfilter, bool use_unordered_map, int limit_recv_rate_hz)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    pthread_t read_can_thread, read_tcp_thread, write_thread;
    int tcp_socket, can_socket, thread_rv;

    // initialising stuff
    if (pthread_mutex_init(&read_mutex, NULL) != 0)
    {
        error("mutex init has failed", EXIT_FAILURE);
    }

    can_socket = open_can_socket(can_port, filter, numfilter);
    if (can_socket < 0)
    {
        error("unable to create read can thread", can_socket);
    }

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

    can_read_args read_args_can = { can_socket, use_unordered_map };
    thread_rv = pthread_create(&read_can_thread, NULL, read_poll_can, (void*)&read_args_can);
    if (thread_rv < 0)
    {
        error("unable to create read can thread", thread_rv);
    }

    tcp_read_args read_args_tcp = { tcp_socket, limit_recv_rate_hz };
    thread_rv = pthread_create(&read_tcp_thread, NULL, read_poll_tcp, (void*)&read_args_tcp);
    if (thread_rv < 0)
    {
        error("unable to create read tcp thread", thread_rv);
    }

    can_write_sockets write_args = { tcp_socket, can_socket };
    thread_rv = pthread_create(&write_thread, NULL, write_poll, (void*)&write_args);
    if (thread_rv < 0)
    {
        error("unable to create write thread", thread_rv);
    }

    thread_rv = pthread_join(read_can_thread,NULL);
    if (thread_rv < 0)
    {
        error("read can thread failed", thread_rv);
    }

    thread_rv = pthread_join(read_tcp_thread,NULL);
    if (thread_rv < 0)
    {
        error("read tcp thread failed", thread_rv);
    }

    thread_rv = pthread_join(write_thread,NULL);
    if (thread_rv < 0)
    {
        error("write thread failed", thread_rv);
    }

    return 0;
}

int main(int argc, char* argv[])
{
    // arg parsing
    if (argc < 4)
    {
        fprintf(stderr, "usage %s can-name hostname port\n", argv[0]);
        exit(0);
    }

    char *can_port = argv[1];
    char *hostname = argv[2];
    int port = atoi(argv[3]);

    return tcpclient(can_port, hostname, port, NULL, 0, false, -1);
}
