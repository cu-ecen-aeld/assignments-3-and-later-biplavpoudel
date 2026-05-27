/**
* socket-based program that opens stream socket bounded to port 9000
* returns -1 on failure
* listens/accepts connection
* logs syslog as: 'Accepted conenction from xxxx'
* receives data and appends to '/var/tmp/aesdsocketdata'; creates if it doesn't exist
* returns full content of '/var/tmp/aesdsocketdata' to client
* logs syslog as: 'Closed connection from xxxx'
* restarts connection in a loop until SIGINT and SIGTERM
* completes any open connections, closes open sockets and delete the /var/tmp/aesdsocketdata'
* then, logs syslog as: 'Caught signal, exiting'
 
* >>>UPDATED INSTRUCTION FOR ASSIGNMENT 8 >>>>
* Modify your socket server application developed in assignments 5 and 6 to support and use a build switch USE_AESD_CHAR_DEVICE, set to 1 by default, which:
* 	a. Redirects reads and writes to '/dev/aesdchar' instead of '/var/tmp/aesdsocketdata'
*   b. Removes timestamp printing.
*   c. Ensure you do not remove the '/dev/aesdchar' endpoint after exiting the aesdsocket application.
* In addition, please ensure you are not opening the file descriptor for your driver endpoint from aesdsocket until it is accessed.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "queue.h" //FreeBSD 10 based; thread-safe macros available

#include "../aesd-char-driver/aesd_ioctl.h"

#define PORT "9000"
#define BACKLOG 10					  // no. of queued pending connections before refusal
#define CHUNK_SIZE 4096				  // no. of bytes, we can read/write at once
#define PIDFILE "/tmp/aesdsocket.pid" // pid file to store pid of aesdsocket daemon;

#define _POSIX_C_SOURCE 200809L

#define USE_AESD_CHAR_DEVICE 1			//build switch, set to 1 by default, to redirect reads and writes to '/dev/aesdchar'

#if USE_AESD_CHAR_DEVICE	
#	define RECVFILE "/dev/aesdchar" 
#else
#	define RECVFILE "/var/tmp/aesdsocketdata"
#endif

static char *packet_file = RECVFILE;

// mutex for file operations
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// mutex to protect list operations + any shared flags
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

// we need a global variable that is read/write atomic to inform `accept loop` of execution termination
// also we need to inform compiler that the variable can change outside of the normal flow of code
// like through signal interrupts; so compiler never caches this into register and reloads it time-to-time
static volatile sig_atomic_t exit_requested = 0;

// write pid to "/var/run/aesdsocket.pid"
static int write_pidfile()
{
	int fd = open(PIDFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
	{
		syslog(LOG_ERR, "Failed to open pidfile: %s", strerror(errno));
		return -1;
	}

	char buf[32];
	int pid_len = snprintf(buf, sizeof(buf), "%d\n", getpid());
	if ((write(fd, buf, pid_len)) == -1)
	{
		syslog(LOG_ERR, "Failed to write to pidfile: %s", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

// function to handle graceful termination of socket server
void handle_server_termination(int sig)
{
	(void)sig;
	exit_requested = 1;
}

// struct for each worker thread after accepting incoming socket connection
typedef struct thread_node
{
	pthread_t thread_id; // ID as returned by pthread_create()
	int client_fd;		 // file descriptor for client socket

	char host[NI_MAXHOST]; // NI_MAXHOST and NI_MAXSERV are set from <netdb.h>
	char service[NI_MAXSERV];

	atomic_bool completed; // sets to true if the thread completes

	TAILQ_ENTRY(thread_node) nodes; // the TAILQ_ENTRY macro uses the struct name
} node_t;

/* this creates a head_t that makes it easy for us to pass pointers to
head_t without the compiler complaining. */
typedef TAILQ_HEAD(head_s, thread_node) head_t;

// to free up the linked list of worker threads
static void free_queue_and_join_all(head_t *head)
{
	pthread_mutex_lock(&list_mutex);

	while (!TAILQ_EMPTY(head))
	{
		node_t *e = TAILQ_FIRST(head);
		TAILQ_REMOVE(head, e, nodes);

		pthread_mutex_unlock(&list_mutex);

		pthread_join(e->thread_id, NULL);
		free(e);

		pthread_mutex_lock(&list_mutex);
	}

	pthread_mutex_unlock(&list_mutex);
}

// remove completed worker_threads
static void remove_completed_threads(head_t *head)
{
	pthread_mutex_lock(&list_mutex);

	node_t *e = NULL;
	node_t *next = NULL;

	TAILQ_FOREACH_SAFE(e, head, nodes, next)
	{
		if (atomic_load(&e->completed))
		{
			TAILQ_REMOVE(head, e, nodes);

			pthread_mutex_unlock(&list_mutex);

			pthread_join(e->thread_id, NULL);
			free(e);

			pthread_mutex_lock(&list_mutex);
		}
	}

	pthread_mutex_unlock(&list_mutex);
}

// Unblock all worker threads and close client fds on shutdown request
static void request_exit_all_threads(head_t *head)
{
	pthread_mutex_lock(&list_mutex);

	node_t *e = NULL;
	TAILQ_FOREACH(e, head, nodes)
	{
		if (e->client_fd != -1)
		{
			shutdown(e->client_fd, SHUT_RDWR);
		}
	}

	pthread_mutex_unlock(&list_mutex);
}

// separate cleanup for worker thread for socket connection.
// since p_recv_buffer is a pointer itself, passing "as it is" will only make its local copy NULL, not its original value.
// so we pass the pointer to the pointer of p_recv_buffer to work on the actual value
static void thread_cleanup(node_t *worker, char **p_recv_buffer)
{
	if (p_recv_buffer && *p_recv_buffer)
	{
		free(*p_recv_buffer);
		*p_recv_buffer = NULL;
	}

	if (worker)
	{
		if (worker->client_fd != -1)
		{
			close(worker->client_fd);
			worker->client_fd = -1;
		}
		syslog(LOG_INFO, "Closed connection from %s", worker->host);
		atomic_store(&worker->completed, true);
	}
}

static void *timestamp_thread_fn(void *arg)
{
	(void)arg;

	// Use CLOCK_REALTIME since requirement says "system wall clock time"
	struct timespec next;
	clock_gettime(CLOCK_REALTIME, &next);

	// align to next 10-second boundary (optional but nice)
	next.tv_sec = (next.tv_sec / 10 + 1) * 10;
	next.tv_nsec = 0;

	while (!exit_requested)
	{
		// sleep until next absolute time
		clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next, NULL);

		if (exit_requested)
			break;

		// build timestamp line
		time_t now = time(NULL);
		struct tm tm_now;
		localtime_r(&now, &tm_now);

		char timestr[128];
		// RFC 2822-ish
		strftime(timestr, sizeof(timestr), "%a, %d %b %Y %H:%M:%S %z", &tm_now);

		char line[256];
		int len = snprintf(line, sizeof(line), "timestamp:%s\n", timestr);
		if (len < 0)
			continue;

		// atomic w.r.t. socket threads
		pthread_mutex_lock(&file_mutex);

		int fd = open(packet_file, O_CREAT | O_WRONLY | O_APPEND, 0644);
		if (fd != -1)
		{
			ssize_t off = 0;
			while (off < len)
			{
				ssize_t nw = write(fd, line + off, (size_t)(len - off));
				if (nw == -1)
				{
					if (errno == EINTR)
						continue;
					break;
				}
				off += nw;
			}
			close(fd);
		}

		pthread_mutex_unlock(&file_mutex);

		// schedule next tick
		next.tv_sec += 10;
	}

	return NULL;
}

/** FOR ASSIGNMENT 9 >>>>
  * We need to check if the data in buffer (temp), sent over the socket, contains string in the format AESDCHAR_IOCSEEKTO:X,Y
  * 	where X and Y are unsigned decimal integer values,
  * 	the X should be considered the write command to seek into,
  * 	and the Y should be considered the offset within the write command. 
  *
  * These values should be sent to the aesdchar driver using the AESDCHAR_IOCSEEKTO ioctl. 
  * The ioctl command should be performed before any additional writes to our aesdchar device.
  *
  * We don't write this string command into the aesdchar device as we do with other strings sent to the socket.
  * We send the content of the aesdchar device back over the socket, as we do with any other string received over the socket interface.
  * We need to ensure the "read of the file and return over the socket uses the same" (not closed and re-opened) file descriptor \
  * 	used to send the ioctl, to ensure our file offset is honored for the read command.
*/
void *readWriteSocket(void *arg)
{
	node_t *worker = (node_t *)arg;
	int client_fd = worker->client_fd;

	char *recv_buffer = NULL;
	size_t buffer_size = 0;

	char temp[CHUNK_SIZE];
	ssize_t bytes_read;

	/* We assign file descriptor beforehand for driver's and normal file's use case with conditional definition.
     * For aesd char device, we open without O_APPEND or O_CREAT so that the file offset set by ioctl or llseek is honoured on subsequent reads.
	 * The aesdchar driver handles its own internal write-append semantics; we must not fight it with O_APPEND from our side.
     */

#if USE_AESD_CHAR_DEVICE
    int data_fd = open(packet_file, O_RDWR, 0644);
#else
    int data_fd = open(packet_file, O_CREAT | O_RDWR | O_APPEND, 0644);
#endif
	if (data_fd == -1){
		syslog(LOG_ERR, "open append failed: %s", strerror(errno));
		goto out;
	}

	while ((bytes_read = recv(client_fd, temp, CHUNK_SIZE, 0)) > 0)
	{	
		char *new_buffer = realloc(recv_buffer, buffer_size + (size_t)bytes_read);
		if (!new_buffer)
		{
			syslog(LOG_ERR, "realloc failed");
			goto out;
		}
		recv_buffer = new_buffer;
		memcpy(recv_buffer + buffer_size, temp, (size_t)bytes_read);
		buffer_size += (size_t)bytes_read;

		for (size_t i = 0; i < buffer_size; i++)
		{
			// we write to the file after encountering newline
			if (recv_buffer[i] != '\n')	
				continue;

			size_t packet_len = i + 1;	// includes the `\n` character

			/* After we encounter a newline character,
			 we check the preceding string for the ioctl command*/

#if USE_AESD_CHAR_DEVICE
			// we first compare the first 19 bytes prefixes of the string in buffer with "AESDCHAR_IOCSEEKTO:"
			if (strncmp(recv_buffer, "AESDCHAR_IOCSEEKTO:", strlen("AESDCHAR_IOCSEEKTO:")) == 0){
                struct aesd_seekto seek_to;

                if (sscanf(recv_buffer, "AESDCHAR_IOCSEEKTO:%u,%u", &seek_to.write_cmd, &seek_to.write_cmd_offset) != 2){
                    syslog(LOG_ERR, "AESDCHAR_IOCSEEKTO badly formatted");
                    goto out;
				}
				fprintf(stdout,"\nIOCTL CMD ENCOUNTERED: AESDCHAR_IOCSEEKTO:%u,%u\n", seek_to.write_cmd, seek_to.write_cmd_offset);

                // As per instrcution, we send the ioctl on the SAME fd that we will read from, \
				//  so the driver's offset is preserved for the read below.

                pthread_mutex_lock(&file_mutex);
				// ioctl seek and read should be atomic, hence the locking

                if (ioctl(data_fd, AESDCHAR_IOCSEEKTO, &seek_to) < 0) {
                    syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failed: %s", strerror(errno));
                    pthread_mutex_unlock(&file_mutex);
                    goto out;
                }
				fprintf(stdout,"\nIOCTL CMD SEND AS SYSTEMCALL\n");
                /* now read from the specified ioctl-set offset and send to client*/
                char buf[CHUNK_SIZE];
                ssize_t nr;
				fprintf(stdout, "\nThe sent values are:\n");
                while ((nr = read(data_fd, buf, CHUNK_SIZE)) > 0) {
                    size_t sent = 0;
                    while (sent < (size_t)nr) {
                        ssize_t ns = send(client_fd, buf + sent, (size_t)nr - sent, 0);
						fprintf(stdout, "%s\n", buf+sent);
                        if (ns == -1) {
                            if (errno == EINTR) continue;
                            syslog(LOG_ERR, "send failed: %s", strerror(errno));
                            pthread_mutex_unlock(&file_mutex);
                            goto out;
                        }
                        sent += (size_t)ns;
                    }
                }

				int saved_errno = errno;

                pthread_mutex_unlock(&file_mutex);

				if (nr == -1 && saved_errno != EINTR) {
    				syslog(LOG_ERR, "Read failed: %s", strerror(saved_errno));
    				goto out;
				}
            } else {
				/* For normal packet operation when no AESDCHAR_IOCSEEKTO is sent: we write to device, then read it all back*/
				pthread_mutex_lock(&file_mutex);

				size_t total_written = 0;
				while (total_written < packet_len){
					ssize_t nw = write(data_fd, recv_buffer + total_written, packet_len - total_written);
					if (nw == -1)
					{
						if (errno == EINTR)
							continue;
						close(data_fd);
						pthread_mutex_unlock(&file_mutex);
						syslog(LOG_ERR, "write failed: %s", strerror(errno));
						goto out;
					}
					total_written += (size_t)nw;
				}

				// // not the ioctl command, so we close the file descriptor and reopen for read later
				// close(data_fd);	
				// now we read the data written to the file
				// data_fd = open(packet_file, O_RDONLY);
				// if (data_fd == -1)
				// {
				// 	pthread_mutex_unlock(&file_mutex);
				// 	syslog(LOG_ERR, "open read failed: %s", strerror(errno));
				// 	goto out;
				// }

    			// seek to beginning for full readback
    			if (lseek(data_fd, 0, SEEK_SET) == -1) {
    			    pthread_mutex_unlock(&file_mutex);
    			    syslog(LOG_ERR, "lseek failed: %s", strerror(errno));
    			    goto out;
    			}

				/* now read from the file from start and send it back to client*/
                char buf[CHUNK_SIZE];
                ssize_t nr;
				fprintf(stdout, "\nThe sent values are:\n");
                while ((nr = read(data_fd, buf, CHUNK_SIZE)) > 0) {
                    size_t sent = 0;
                    while (sent < (size_t)nr) {
                        ssize_t ns = send(client_fd, buf + sent, (size_t)nr - sent, 0);
						fprintf(stdout, "%s\n", buf+sent);
                        if (ns == -1) {
                            if (errno == EINTR) continue;
                            syslog(LOG_ERR, "send failed: %s", strerror(errno));
                            pthread_mutex_unlock(&file_mutex);
                            goto out;
                        }
                        sent += (size_t)ns;
                    }
                }

				int saved_errno = errno;
				pthread_mutex_unlock(&file_mutex);

				if (nr == -1 && saved_errno != EINTR) {
    				syslog(LOG_ERR, "Read failed: %s", strerror(saved_errno));
    				goto out;
				}
            }
#else	/* !USE_AESD_CHAR_DEVICE, the code remains unchanged */

			pthread_mutex_lock(&file_mutex);

			size_t total_written = 0;
			while (total_written < packet_len){
				ssize_t nw = write(data_fd, recv_buffer + total_written, packet_len - total_written);
				if (nw == -1)
				{
					if (errno == EINTR)
						continue;
					close(data_fd);
					pthread_mutex_unlock(&file_mutex);
					syslog(LOG_ERR, "write failed: %s", strerror(errno));
					goto out;	
				}
				total_written += (size_t)nw;
			}
			// not the ioctl command, so we close the file descriptor and reopen for read later
			close(data_fd);
			
			// now we read the data written to the file
			data_fd = open(packet_file, O_RDONLY);
			if (data_fd == -1)
			{
				pthread_mutex_unlock(&file_mutex);
				syslog(LOG_ERR, "open read failed: %s", strerror(errno));
				goto out;
			}
			/* now read from the file and send it back to client*/
            char buf[CHUNK_SIZE];
            ssize_t nr;
            while ((nr = read(data_fd, buf, CHUNK_SIZE)) > 0) {
                size_t sent = 0;
                while (sent < (size_t)nr) {
                    ssize_t ns = send(client_fd, buf + sent, (size_t)nr - sent, 0);
                    if (ns == -1) {
                        if (errno == EINTR) continue;
                        syslog(LOG_ERR, "send failed: %s", strerror(errno));
                        pthread_mutex_unlock(&file_mutex);
                        goto out;
                    }
                    sent += (size_t)ns;
                }
            }
			
			int saved_errno = errno;
			close(data_fd);

			//we now reopen fd for next packet's write
			data_fd = open(packet_file, O_CREAT | O_RDWR | O_APPEND, 0644);

			pthread_mutex_unlock(&file_mutex);

			if (nr == -1 && saved_errno != EINTR) {
    			syslog(LOG_ERR, "read failed: %s", strerror(saved_errno));
    			goto out;
			}

			if (data_fd == -1) {
    			syslog(LOG_ERR, "reopen for append failed: %s", strerror(errno));
    			goto out;
			}

#endif		/* USE_AESD_CHAR_DEVICE */


			/* Remove processed packet from buffer */
			size_t remaining = buffer_size - packet_len;
			memmove(recv_buffer, recv_buffer + packet_len, remaining);

			buffer_size = remaining;
			i = (size_t)-1;  /*typecasting and restarting scan from beginning*/
			// -1 in unsigned long is equivalent to SIZE_MAX
		}
	}
	if (bytes_read == -1 && errno != EINTR)
	{
		syslog(LOG_ERR, "recv failed: %s", strerror(errno));
	}

	out:
		if (data_fd != -1)
			close(data_fd);
		thread_cleanup(worker, &recv_buffer);
		return NULL;
}

int main(int argc, char *argv[])
{
	// we first check if this socket server runs as `normal process` or as `daemon` with "-d" argument
	int daemon_mode = 0;
	int opt;

	char host[NI_MAXHOST]; // NI_MAXHOST and NI_MAXSERV are set from <netdb.h>
	char service[NI_MAXSERV];

	// for linked list of worker threads
	head_t head;	   // declaring head
	TAILQ_INIT(&head); // initializing head of the queue

	// getopt returns each option one-by-one; if everything is parsed, it returns -1
	while ((opt = getopt(argc, argv, "d")) != -1)
	{
		switch (opt)
		{
		case 'd':
			daemon_mode = 1;
			syslog(LOG_INFO, "Sever running in daemon_mode");
			break;
		case '?':
		default:
			fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	int listen_fd;

	// first let's get addrinfo to bind the socket using getaddrinfo()
	struct addrinfo hints, *servinfo; // args for getaddrinfo(); *servinfo points to result
	struct addrinfo *p;				  // p loops through all the linked-lists of addrinfo's

	memset(&hints, 0, sizeof(hints)); // making sure struct is empty
	hints.ai_flags = AI_PASSIVE;	  // use my IP
	hints.ai_family = AF_UNSPEC;	  // either IP family
	hints.ai_socktype = SOCK_STREAM;  // TCP type

	int yes = 1; // for setsockopt's *optval
	int status;

	if ((status = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0)
	{
		fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
		return -1;
	}

	// now servinfo points to a linked-list of 1 or more addrinfo's
	// to loop through all the resulting addrinfo's and bind to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next)
	{
		// first we create socket endpoint connection
		if ((listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			fprintf(stderr, "socket create failed: %s\n", strerror(errno));
			continue; // try again to create socket with another struct servinfo in next loop
		}

		// now we set socket options at socket API level
		// SO_REUSEADDR relaxes addr/port reuse at bind time
		// i.e. to avoid 'Address already in use' bind error upon restart, when the address in TIME_WAIT is already free
		if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		{
			fprintf(stderr, "socket options couldn't be set: %s\n", strerror(errno));
			close(listen_fd);
			return -1;
		}

		// now if no errors on creating socket file descriptor, we bind the address to socket
		if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1)
		{
			close(listen_fd);
			fprintf(stderr, "socket couldn't be bound: %s\n", strerror(errno));
			continue; // we try to bind new address from next servinfo to the socket+*96
		}

		// if success, we try to get the socket information using getsockname()
		struct sockaddr_storage sa;
		socklen_t sa_len = sizeof(sa);

		if (getsockname(listen_fd, (struct sockaddr *)&sa, &sa_len) == 0)
		{
			char host[NI_MAXHOST];
			char service[NI_MAXSERV];

			if (getnameinfo((struct sockaddr *)&sa, sa_len, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
			{
				printf("Server successfully bound to %s:%s\n", host, service);
			}
		}

		break; // if successfully bound, we break from the loop
	}

	freeaddrinfo(servinfo); // freeing servinfo to avoid memory leak

	// struct servinfo exhausted. no socket bounded to address!
	if (p == NULL)
	{
		fprintf(stderr, "Server socket failed to bind!\n");
		return -1;
	}

	// now we listen for incoming connection
	if (listen(listen_fd, BACKLOG) == -1)
	{
		fprintf(stderr, "Listening on socket refused: %s\n", strerror(errno));
		return -1;
	}

	// Now we start accepting connections
	printf("Waiting for connections...\n");

	openlog("aesdsocket", LOG_PID | LOG_NDELAY, LOG_USER);

	// sigaction handling in the case of SIGINT or SIGTERM
	struct sigaction grace_term;
	memset(&grace_term, 0, sizeof grace_term);
	grace_term.sa_handler = handle_server_termination;
	sigemptyset(&grace_term.sa_mask);
	grace_term.sa_flags = 0; // restart should be avoided in case of SIGINT or SIGTERM

	if (sigaction(SIGINT, &grace_term, NULL) == -1)
	{
		fprintf(stderr, "sigaction(SIGINT) failed: %s\n", strerror(errno));
		return -1;
	}

	if (sigaction(SIGTERM, &grace_term, NULL) == -1)
	{
		fprintf(stderr, "sigaction(SIGTERM) failed: %s\n", strerror(errno));
		return -1;
	}

	/* Now we daemonize the process before accept loop by:
	 *	1. forking into a child,
	 * 	2. exiting the parent,
	 *	3. creating a new session,
	 *	4. exiting the child again, and
	 *	5. detaching the grandchild process
	 */
	if (daemon_mode)
	{
		// if pidfile already exists, that means aesdsocket is already running as daemon
		// if (access(PIDFILE, F_OK) == 0) {
		//	syslog(LOG_ERR, "Pidfile exists, daemon already running?");
		//	exit(EXIT_FAILURE);
		//}

		pid_t pid = fork();
		if (pid < 0)
		{
			perror("first fork");
			close(listen_fd);
			closelog();
			return -1;
		}
		else if (pid > 0)
			exit(EXIT_SUCCESS); // parent process exits; child is orphaned

		if (setsid() < 0)
		{ // runs the daemon process in a new session; detaches from the old controlling terminal
			perror("setsid");
			close(listen_fd);
			closelog();
			return -1;
		}

		// now we again fork the child process and orphan the grandchild process
		pid = fork();
		if (pid < 0)
		{
			perror("second fork");
			close(listen_fd);
			closelog();
			return -1;
		}
		else if (pid > 0)
			exit(EXIT_SUCCESS);

		// now grandchild is not the session leader of the newly created session; it is fully detached
		// now we change root directory to avoid blocking filesystem unmounts and redirect stdin/stdout/stderr
		if (chdir("/") != 0)
		{
			perror("chdir");
			exit(EXIT_FAILURE);
		}

		if (freopen("/dev/null", "r", stdin) == NULL)
		{
			perror("freopen stdin");
			exit(EXIT_FAILURE);
		}

		if (freopen("/dev/null", "w", stdout) == NULL)
		{
			perror("freopen stdout");
			exit(EXIT_FAILURE);
		}

		if (freopen("/dev/null", "w", stderr) == NULL)
		{
			perror("freopen stderr");
			exit(EXIT_FAILURE);
		}
		// now our process is a true daemon

		// now we write pidfile for start-stop init script
		if (write_pidfile() != 0){
			syslog(LOG_ERR, "Failed to write to pidfile, exiting...");
			exit(EXIT_FAILURE);
		}
		syslog(LOG_INFO, "pid successfully written to %s", PIDFILE);
	}

	pthread_t ts_thread;
	bool ts_started = false;
	#if !USE_AESD_CHAR_DEVICE
		if (pthread_create(&ts_thread, NULL, timestamp_thread_fn, NULL) == 0){
			ts_started = true;
		} else {
			syslog(LOG_ERR, "Failed to start timestamp thread");}
	#endif

	int client_fd; // client_fd for new accepted socket connection; different from default listening listen_fd
	struct sockaddr_storage incoming_addr;

	while (!exit_requested)
	{	
		host[0] = '\0';   // clear from previous iteration
    	service[0] = '\0';

		socklen_t size_inaddr = sizeof(incoming_addr);
		client_fd = accept(listen_fd, (struct sockaddr *)&incoming_addr, &size_inaddr);

		if (client_fd == -1)
		{
			if (errno == EINTR && exit_requested)
				break;
			syslog(LOG_ERR, "accept failed: %s", strerror(errno));
			continue;
		}

		// if connection was established, we log the client information
		int rc = getnameinfo((struct sockaddr *)&incoming_addr, size_inaddr,
							 host, sizeof(host), service, sizeof(service),
							 NI_NUMERICHOST | NI_NUMERICSERV);

		if (rc == 0)
			syslog(LOG_INFO, "Accepted connection from %s", host);
		else
			syslog(LOG_WARNING, "Client information couldn't be determined");

		/* make node */
		node_t *worker_node = calloc(1, sizeof(*worker_node));
		if (!worker_node)
		{
			close(client_fd);
			continue;
		}

		worker_node->client_fd = client_fd;
		strncpy(worker_node->host, host, NI_MAXHOST - 1 );
		worker_node->host[NI_MAXHOST - 1] = '\0';								// or just use snprintf() to ensure null termination
		strncpy(worker_node->service, service, NI_MAXSERV - 1);
		worker_node->service[NI_MAXSERV - 1] = '\0';

		atomic_init(&worker_node->completed, false);

		if (pthread_create(&worker_node->thread_id, NULL, readWriteSocket, worker_node) != 0)
		{
			close(client_fd);
			free(worker_node);
			continue;
		}

		pthread_mutex_lock(&list_mutex);
		TAILQ_INSERT_TAIL(&head, worker_node, nodes);
		pthread_mutex_unlock(&list_mutex);

		remove_completed_threads(&head);
	}
	exit_requested = 1; // already set by signal handler, but harmless
	syslog(LOG_INFO, "Caught signal, exiting");

	// Unblock workers
	request_exit_all_threads(&head);

	// Join workers
	free_queue_and_join_all(&head);

	// Join timestamp thread; not true for aesd char device
	if (ts_started)
	{
		pthread_cancel(ts_thread);
		pthread_join(ts_thread, NULL);
	}

	close(listen_fd);

	/* we do not remove the  /dev/aesdchar endpoint after exiting the aesdsocket application */
	#if !USE_AESD_CHAR_DEVICE
		unlink(packet_file);
	#endif

	if (daemon_mode) unlink(PIDFILE);
	closelog();

	return 0;
}