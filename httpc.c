#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <assert.h>

#include <errno.h>

/*Is this required?*/
#include <sys/types.h>

/*posix*/
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>

#include <netdb.h>
#include <signal.h>

/*tokens are pretty much required to parse http
  without large amount of code.*/
typedef struct {
	char *str;
	size_t len;
}token;


typedef struct _client_data_t client_data_t;
typedef bool (*client_cb_t) (client_data_t *cdata, int efd);
struct _client_data_t {
	/*fd in epoll associated with this struct*/
	int fd;

	/*read and write functions are set here*/
	client_cb_t cb_func;

	/*tokenized version of the request buffer*/
	token *tokens;
	unsigned int tokenslen;

	/*fd of requested file, if one is requested*/
	int rfd; /*read-only, hence the 'r' in 'rfd'*/
	/*These variables are information about the file to be read*/
	size_t tosend;
	off_t offset;
	bool readfile;

	char *response;
	/*This is the amount of data to send, not the actual
	  length of response, which is headerlen just like it is for request*/
	unsigned int responselen;
	unsigned int response_sent;

	char *request;
	/*Since this would always be headerlen, there isn't a point in setting it.*/
	/*unsigned int requestlen;*/
	unsigned int request_recvd;

	/*Set if keepalive is in the http header*/
	bool keepalive;

	/*Is used in the client context allocation functions*/
	bool inuse;
};

/*0.0.0.0 should mean bind to any ipv4 address*/
const char IPANY[] = "0.0.0.0";

/*cdata->request and cdata->response are of headerlen*/
const unsigned int headerlen   = 4096;
/*cdata->tokens is of length maxtokens*/
const unsigned int maxtokens   = 128;

/*Used in the event loop to stop the program when ctrl+c is used*/
bool end_program = false;

/*Tracking variables*/
size_t gcdata_len;
client_data_t **gcdata;

/*Utility header*/
#include "utils.h"

/*Prototypes*/
int create_epoll(int lsock);
bool setnonblocking(int sfd);
bool cb_recv(client_data_t *cdata, int efd);
bool cb_send(client_data_t *cdata, int efd);
bool cb_accept(client_data_t *cdata, int efd);
void close_client(client_data_t *data, int efd);
int create_sock(const char *address, const char* port, bool client);
void event_loop(int efd, int lsock, struct epoll_event *events, int maxevents);

client_data_t *alloc_cdata(void);
void free_cdata(client_data_t *ptr);

void end_sig(int param)
{
	end_program = true;
}

/*Code*/
int main(int argc, char *argv[])
{
	int maxevents;
	unsigned int i;
	/*listen, client sock, and event poll desc*/
	int lsock,  efd;
	client_data_t *cdata;
	struct epoll_event *events;

	/*seems like a good limit to the maximum number of events*/
	maxevents = 20;

	signal(SIGINT, end_sig);
	signal(SIGPIPE, SIG_IGN);

	/*Since the listening socket is included in SOMAXCONN,
	  SOMAXCONN seems like a reasonable limit to the amount of client data structs*/
	gcdata_len = SOMAXCONN;

	/*Initializing memory*/
	gcdata = palloc(sizeof(client_data_t*), gcdata_len);
	events = palloc(sizeof(struct epoll_event), maxevents);
		/*The maximum amount of events is the maximum
		  amount of clients to be served in one event loop cycle
		  before the call to epoll_wait is made to check other fd's'*/

	for (i = 0; i < gcdata_len; i++) {
		cdata           = palloc(sizeof(client_data_t), 1);
		cdata->tokens   = palloc(sizeof(token)        , maxtokens);
		cdata->response = palloc(sizeof(char)         , headerlen);
		cdata->request  = palloc(sizeof(char)         , headerlen);
		cdata->inuse    = false;
		gcdata[i]       = cdata;
	}

	/*Create listening socket*/
	lsock = create_sock(IPANY, "8081", false);
		/*8081 is a good port number, since 8080 is likely to be taken*/
	if (lsock < 0)
		die("Failed to create listen socket\n");

	/*10 seems like a good backlog*/
	if (listen(lsock, 10))
		die("Failed to put socket into listen mode\n");

	/*Create event poll*/
	efd = create_epoll(lsock);

	/*event loop*/
	event_loop(efd, lsock, events, maxevents);

	/*Memory cleanup*/
	for (i = 0; i < gcdata_len; i++) {/*lsock is cleaned up in this loop*/
		cdata = gcdata[i];

		if (cdata->inuse) {
			if (epoll_ctl(efd, EPOLL_CTL_DEL, cdata->fd, NULL) < 0) {
				fprintf(stderr, "EPOLL_CTL_DEL in cleanup\n");
				fflush(stderr);
			}

			close(cdata->fd);
			if (cdata->rfd > 0)
				close(cdata->rfd);
		}

		free(cdata->response);
		free(cdata->request);
		free(cdata->tokens);
		free(cdata);
	}

	if (close(efd))
		die("efd\n");

	free(gcdata);
	free(events);
	return 0;
}


void event_loop(int efd, int lsock, struct epoll_event *events, int maxevents)
{
	int n;
	int nfds;
	client_data_t *cdata;
	struct epoll_event event;

	while (((nfds = epoll_wait(efd, events, maxevents, -1)) >= 0) && !end_program) {
		for (n = 0; n < nfds; n++) {
			assert(n < maxevents);
			event = events[n];
			cdata = event.data.ptr;

			if (!(event.events & (EPOLLERR | EPOLLHUP))) {
				assert(cdata->cb_func);
				cdata->cb_func(cdata, efd);
			} else {
				close_client(cdata, efd);
			}
		}
	}
}

bool cb_accept(client_data_t *cdata, int efd)
{
	int lsock, csock;
	socklen_t caddrlen;
	struct sockaddr caddr;
	struct epoll_event event;

	lsock = cdata->fd;

	while (true) {
		caddrlen = sizeof(caddr);
		csock    = accept(lsock, &caddr, &caddrlen);
		if (csock < 0) {
			/*Means all connections that can be accepted without
			  blocking have been.*/
			if (errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				break;
			else {
				fprintf(stderr, "accept: %d\n", errno);
				continue;
			}
		}

		if (!setnonblocking(csock)) {
			if (close(csock))
				die("Set nonblocking\n");
			continue;
		}

		cdata = alloc_cdata();
		if (!cdata) {
			close(csock);
			continue;
		}

		cdata->request_recvd = 0;
		cdata->rfd           = -1;
		cdata->keepalive     = true; /*Default according to the http/1.1 spec*/
		cdata->fd            = csock;
		cdata->cb_func       = cb_recv;
		event.data.ptr       = cdata;
		/*What doees edge triggered mean in software?*/
		event.events         = EPOLLIN | EPOLLET;

		if (epoll_ctl(efd, EPOLL_CTL_ADD, csock, &event) < 0)
			die("EPOLL_CTL_ADD\n");
	}

	return true;
}

/*EPOLLIN:  Request for notification that the socket is ready to be read from*/
/*EPOLLOUT: Request for notification that the socket is ready to written to*/
/*EPOLLET: As far as I can understand, it means not to wake up epoll_wait until
           there is enough data not to waste a lot of CPU time reading small amounts
           of data each time it trickles in.*/
/*mod_epoll_event = Modify the events set for a particular file descriptor*/
void mod_epoll_event(client_data_t *cdata, int efd, int eflags)
{
	struct epoll_event event;

	event.data.ptr = cdata;
	event.events   = eflags | EPOLLERR | EPOLLHUP;
	if(epoll_ctl(efd, EPOLL_CTL_MOD, cdata->fd, &event) < 0)
		die("mod_epoll_event\n");
}

/*Handles client requests*/
bool cb_recv(client_data_t *cdata, int efd)
{
	ssize_t ret;
	bool endofheader;

	/*Not really sure what flags can be applied to recv that would be relevant*/
	/*amount_read = recv(int socket_fd, void *buffer, size_t buffer_length, int flags)*/
	ret = recv(cdata->fd, cdata->request+cdata->request_recvd, ((headerlen - cdata->request_recvd)-1) , 0);
	if (ret <= 0) {
		close_client(cdata, efd);
		return false;
	}
				/*returns NULL if sequence isn't found,
				 and the pointer to the position if it is.*/
	endofheader           = strstr(cdata->request, "\r\n\r\n");
	cdata->request_recvd += (size_t)ret;

	if (!endofheader && (!(cdata->request_recvd - headerlen)))
		close_client(cdata, efd);
	else if (endofheader) {
		/*recv shouldn't read more than (headerlen - cdata->request_recvd)-1*/
		assert(cdata->request_recvd < headerlen);
		/*Writes null byte to the end of the request header for tokenizing/parsing purposes*/
		cdata->request[cdata->request_recvd+1] = '\0'; 

		/*Parses requests, and generates a response*/
		header_tokenize(cdata);
		if (!gen_response(cdata)) {
			close_client(cdata, efd);
			return true;
		}

		mod_epoll_event(cdata, efd, EPOLLOUT | EPOLLET);
		cdata->cb_func = cb_send;
		return true;
	}

	return true;
}

/*The zero copy method is supposed to be the most efficient way to send data in a user space program.*/
bool cb_sendfile(client_data_t *cdata, int efd)
{
	off_t offset;
	ssize_t ret;


	offset = cdata->offset;
	/*offset gets updated with the current position*/
	/*amount_read = sendfile(int write_fd, int read_fd, off_t *offset_in_read_fd, size_t amount_left_to_send)*/
	ret = sendfile(cdata->fd, cdata->rfd, &cdata->offset, cdata->tosend);
	if (ret <= 0) {
		fprintf(stderr, "cb_sendfile\n");
		close_client(cdata, efd);
		return false;
	}

	/*If data isn't sent, will epoll not raise another event because sendfile was at least called?*/
	assert(offset != cdata->offset);
	/*It should always be greater than zero, right?*/
	assert(cdata->offset > 0);
	/*sendfile shouldn't send more than requested*/
	assert(((size_t)ret) <= cdata->tosend);

	/*Subtract out what's been sent already*/
	cdata->tosend -= (size_t)ret;

	/*Sets flag in client struct when done reading*/
	if (!cdata->tosend) {
		if (cdata->keepalive) {
			cdata->request_recvd = 0;
			cdata->cb_func       = cb_recv;
			mod_epoll_event(cdata, efd, EPOLLIN | EPOLLET);
		} else
			close_client(cdata, efd);
	}

	return true;
}

/*Handles the response header to clients*/
bool cb_send(client_data_t *cdata, int efd)
{
	ssize_t ret;

	/*Not really sure what send flags could be applicable here.*/
	/*amount_sent = send(int socket_fd, void *buffer, size_t buffer_length, int flags)*/
	ret = send( cdata->fd,                                  \
		   (cdata->response + cdata->response_sent),    \
		   (cdata->responselen - cdata->response_sent), \
		    0);
	if (ret <= 0) {
		fprintf(stderr, "cb_send\n");
		close_client(cdata, efd);
		return false;
	}
	cdata->response_sent += ret; /*Add up what has been sent*/

	if (cdata->response_sent == cdata->responselen) {
		if (!cdata->keepalive) {
			close_client(cdata, efd);
			return true;
		}

		if (cdata->readfile) {
			cdata->offset   = 0;
			/*cdata->tosend = insert stat here when code is written*/
			cdata->cb_func  = cb_sendfile;
			/*I wonder if epoll has to be set every time.
			  Can it affect performance?*/
			mod_epoll_event(cdata, efd, EPOLLOUT | EPOLLET);
		} else {
			/*Look for another header*/
			cdata->request_recvd = 0;
			cdata->cb_func       = cb_recv;
			mod_epoll_event(cdata, efd, EPOLLIN | EPOLLET);
		}
	}
	
	return true;
}

void close_client(client_data_t *cdata, int efd)
{
	int errval;

	errval = 0;
	/*the event struct must be NULL when deleting fd's from the fd buffer stored in the kernel.*/
	/*error_status = epoll_ctl(int epoll_fd, int operation_to_perform, int fd_being_watched, struct epoll_event *event)*/
	if (epoll_ctl(efd, EPOLL_CTL_DEL, cdata->fd, NULL) < 0) {
		errval = errno;

		if (errval != EBADF)
			die("errno: %d\n", errval);
	}
	/*closes socket connection*/
	if (errval != EBADF) {
		if (close(cdata->fd))
			die("Failed to close fd\n");
	}
	/*if a file was being sent, this closes the connection*/
	if (cdata->rfd >= 0)
		if (close(cdata->rfd))
			die("close_client: rfd\n");
	free_cdata(cdata);
}

/*Returns true, if the file descriptor is
  set to nonblocking false otherwise.*/
bool setnonblocking(int fd)
{
	int flags;

	/*Gets file descriptor flags*/
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return false;

	/*Sets nonblocking without clobbering the flags already set on the file descriptor*/
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;

	return true;
}

/*returns the open socket, -1 on error*/
/*Make address NULL for a wildcard address*/
int create_sock(const char *address, const char *port, bool client)
{
	int sock;
	struct addrinfo hints, *results, *result;

	bzero(&hints, sizeof(struct addrinfo));
	/*Allows either IPv4 or IPv6*/
	hints.ai_family     = AF_UNSPEC;
	/*I'm not exactly sure what this controls,
	  since socktype and protocol family is set*/
	hints.ai_protocol   = 0;
	/*I wonder what this has to do with anything*/
	hints.ai_socktype   = SOCK_STREAM;
	/*AI_PASSIVE is the wildcard flag*/
	hints.ai_flags      = (address) ? 0 : AI_PASSIVE;
	/*Since these pointers are not in use, they need to be cleared*/
	hints.ai_canonname  = NULL; /*I wonder what this is for*/
	hints.ai_addr       = NULL; /*current address*/
	hints.ai_next       = NULL; /*Net address in line*/

	/*Error on non-zero return*/
	if (getaddrinfo(address, port, &hints, &results))
		return -1;

	/*Tries every result recursively*/
	for (result = results; result != NULL; result = result->ai_next) {
		sock = socket(result->ai_family,
			      result->ai_socktype | SOCK_NONBLOCK,
			      result->ai_protocol);
		if (sock < 0)
			continue;

		/*breaks on success*/
		if (client) {
			if (connect(sock, result->ai_addr, result->ai_addrlen) <= 0)
				break;
		} else {
			if (bind(sock, result->ai_addr, result->ai_addrlen) <= 0)
				break;
		}

		close(sock);
	}

	freeaddrinfo(results);

	if ((!result) || (sock < 0))
		return -1;

	return sock;
}

/*Do epoll file descriptors have to be closed?*/
int create_epoll(int lsock)
{
	int efd;
	client_data_t *data;
	struct epoll_event event;

	/*Only one valid flag for epoll_create, and that is close on execute*/
	efd = epoll_create1(0); /*epoll_create(int size) is obsolete*/
	if (efd < 0)
		die("Failed to create epoll file descriptor.\n");

	/*Gets a data structure for the listening socket and adds to epoll*/
	data           = alloc_cdata();
	data->cb_func  = cb_accept;
	data->fd       = lsock;
	event.data.ptr = data;
	event.events   = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, lsock, &event) < 0)
		die("Failed to add listening socket to epoll.\n");

	return efd;
}

/*This essentially pulls the first data structure that is free for use by a connection client*/
client_data_t *alloc_cdata(void)
{
	size_t i;
	client_data_t *ret;

	for (i = 0; i < gcdata_len; i++) {
		ret = gcdata[i];

		if (!ret->inuse) {
			ret->inuse = true;
			return ret;
		}
	}

	return NULL;
}

/*This makes the client data structure available for reuse*/
void free_cdata(client_data_t *p)
{
	if (!p->inuse)
		die("free_cdata\n");
	p->inuse = false;
}






