/*****************************************************************************\
 *  slurm_protocol_socket_implementation.c - slurm communications interfaces 
 *                                           based upon sockets.
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov>, et. al.
 *  UCRL-CODE-217948.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#define _USE_IRS 1	/* Required for AIX and hstrerror() */

#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <stdlib.h>

#if HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#else
#  if HAVE_SOCKET_H
#    include <socket.h>
#  endif
#endif

#include "src/common/slurm_protocol_interface.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/log.h"
#include "src/common/fd.h"
#include "src/common/xsignal.h"
#include "src/common/xmalloc.h"
#include "src/common/util-net.h"

#define PORT_RETRIES    2
#define MIN_USER_PORT   (IPPORT_RESERVED + 1)
#define MAX_USER_PORT   0xffff
#define RANDOM_USER_PORT ((uint16_t) ((lrand48() % \
                (MAX_USER_PORT - MIN_USER_PORT + 1)) + MIN_USER_PORT))

/*
 *  Maximum message size. Messages larger than this value (in bytes)
 *  will not be received.
 */
#define MAX_MSG_SIZE     (1024*1024)

/****************************************************************
 * MIDDLE LAYER MSG FUNCTIONS
 ****************************************************************/

/* 
 * Return time in msec since "start time"
 */
static int _tot_wait (struct timeval *start_time)
{
        struct timeval end_time;
        int msec_delay;

        gettimeofday(&end_time, NULL);
        msec_delay =   (end_time.tv_sec  - start_time->tv_sec ) * 1000;
        msec_delay += ((end_time.tv_usec - start_time->tv_usec + 500) / 1000);
        return msec_delay;
}

slurm_fd _slurm_init_msg_engine ( slurm_addr * slurm_address )
{
        return _slurm_listen_stream ( slurm_address ) ;
}

slurm_fd _slurm_open_msg_conn ( slurm_addr * slurm_address )
{
        return _slurm_open_stream ( slurm_address, false ) ;
}

slurm_fd _slurm_accept_msg_conn (slurm_fd fd, slurm_addr *addr)
{
        return _slurm_accept_stream(fd, addr);
} 

/*
 * Pick a random port number to use. Use this if the system 
 * selected port can't connect. This may indicate that the 
 * port/address of both the client and server match a defunct 
 * socket record in TIME_WAIT state.
 */
static void _sock_bind_wild(int sockfd)
{
        int rc, retry;
        slurm_addr sin;
        static bool seeded = false;

        if (!seeded) {
                seeded = true;
                srand48((long int) (time(NULL) + getpid()));
        }

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = htons(RANDOM_USER_PORT);

        for (retry=0; retry < PORT_RETRIES ; retry++) {
                rc = bind(sockfd, (struct sockaddr *) &sin, sizeof(sin));
                if (rc >= 0)
                        break;
                sin.sin_port  = htons(RANDOM_USER_PORT);
        }
        return;
}
       
/* 
 * This would be a no-op in a message implementation
 */
int _slurm_close_accepted_conn (slurm_fd fd) 
{
        return _slurm_close (fd);
}

ssize_t _slurm_msg_recvfrom(slurm_fd fd, char **pbuf, size_t *lenp, 
                            uint32_t flags)
{
        return _slurm_msg_recvfrom_timeout(fd, pbuf, lenp, flags, 
                                           SLURM_MESSAGE_TIMEOUT_MSEC_STATIC);
}

ssize_t _slurm_msg_recvfrom_timeout(slurm_fd fd, char **pbuf, size_t *lenp, 
                                    uint32_t flags, int tmout)
{
        ssize_t  len;
        uint32_t msglen;

        len = _slurm_recv_timeout( fd, (char *)&msglen, 
                                   sizeof(msglen), 0, tmout );

        if (len < ((ssize_t) sizeof(msglen))) 
                return SLURM_ERROR;

        msglen = ntohl(msglen);

        if (msglen > MAX_MSG_SIZE) 
                slurm_seterrno_ret(SLURM_PROTOCOL_INSANE_MSG_LENGTH);

        /*
         *  Allocate memory on heap for message
         */
        *pbuf = xmalloc(msglen);

        if (_slurm_recv_timeout(fd, *pbuf, msglen, 0, tmout) != msglen) {
                xfree(*pbuf);
                *pbuf = NULL;
                return SLURM_ERROR;
        }

        *lenp = msglen;
        
        return (ssize_t) msglen;
}

ssize_t _slurm_msg_sendto(slurm_fd fd, char *buffer, size_t size, 
                          uint32_t flags)
{
        return _slurm_msg_sendto_timeout( fd, buffer, size, flags, 
                                          SLURM_MESSAGE_TIMEOUT_MSEC_STATIC);
}

ssize_t _slurm_msg_sendto_timeout(slurm_fd fd, char *buffer, size_t size, 
                                  uint32_t flags, int timeout)
{
        size_t   len;
        uint32_t usize;
        SigFunc *ohandler;

        /* 
         *  Ignore SIGPIPE so that send can return a error code if the 
         *    other side closes the socket 
         */
        ohandler = xsignal(SIGPIPE, SIG_IGN);

        usize = htonl(size);

	if ((len = _slurm_send_timeout( 
				fd, (char *)&usize, sizeof(usize), 0, 
				timeout)) < 0)
		goto done;

	if ((len = _slurm_send_timeout(fd, buffer, size, 0, timeout)) < 0)
		goto done;


     done:
        xsignal(SIGPIPE, ohandler);
        return len;
}

/* Send slurm message with timeout
 * RET message size (as specified in argument) or SLURM_ERROR on error */
int _slurm_send_timeout(slurm_fd fd, char *buf, size_t size, 
                        uint32_t flags, int timeout)
{
        int rc;
        int sent = 0;
        int fd_flags;
        struct pollfd ufds;
        struct timeval tstart;
        int timeleft = timeout;

        ufds.fd     = fd;
        ufds.events = POLLOUT;

        fd_flags = _slurm_fcntl(fd, F_GETFL);
        fd_set_nonblocking(fd);

        gettimeofday(&tstart, NULL);

        while (sent < size) {

                timeleft = timeout - _tot_wait(&tstart);
                if (timeleft <= 0) {
			debug("_slurm_send_timeout at %d of %d, timeout",
				sent, size);
			slurm_seterrno(SLURM_PROTOCOL_SOCKET_IMPL_TIMEOUT);
			sent = SLURM_ERROR;
			goto done;
                }
                if ((rc = poll(&ufds, 1, timeleft)) <= 0) {
			if ((rc == 0) || (errno == EINTR) || (errno == EAGAIN)) 
 				continue;
			else {
				debug("_slurm_send_timeout at %d of %d, "
					"poll error: %s",
					sent, size, strerror(errno));
				slurm_seterrno(SLURM_COMMUNICATIONS_SEND_ERROR);
				sent = SLURM_ERROR;
				goto done;
			}
                }
                rc = _slurm_send(fd, &buf[sent], (size - sent), flags);
                if (rc < 0) {
 			if (errno == EINTR)
				continue;
			else {
				debug("_slurm_send_timeout at %d of %d, "
					"send error: %s",
					sent, size, strerror(errno));
 				slurm_seterrno(SLURM_COMMUNICATIONS_SEND_ERROR);
				sent = SLURM_ERROR;
				goto done;
			}
                }
                if (rc == 0) {
			debug("_slurm_send_timeout at %d of %d, "
				"sent zero bytes", sent, size);
                        slurm_seterrno(SLURM_PROTOCOL_SOCKET_ZERO_BYTES_SENT);
                        sent = SLURM_ERROR;
                        goto done;
                }

                sent += rc;
        }

    done:
	/* Reset fd flags to prior state, preserve errno */
	if (fd_flags != SLURM_PROTOCOL_ERROR) {
		int slurm_err = slurm_get_errno();
		_slurm_fcntl(fd , F_SETFL , fd_flags);
		slurm_seterrno(slurm_err);
	}

        return sent;
        
}

/* Get slurm message with timeout
 * RET message size (as specified in argument) or SLURM_ERROR on error */
int _slurm_recv_timeout(slurm_fd fd, char *buffer, size_t size, 
                        uint32_t flags, int timeout )
{
        int rc;
        int recvlen = 0;
        int fd_flags;
        struct pollfd  ufds;
        struct timeval tstart;
	int timeleft = timeout;

        ufds.fd     = fd;
        ufds.events = POLLIN;

        fd_flags = _slurm_fcntl(fd, F_GETFL);
        fd_set_nonblocking(fd);

        gettimeofday(&tstart, NULL);

        while (recvlen < size) {

                timeleft = timeout - _tot_wait(&tstart);
                if (timeleft <= 0) {
			debug("_slurm_recv_timeout at %d of %d, timeout",
				recvlen, size);
                        slurm_seterrno(SLURM_PROTOCOL_SOCKET_IMPL_TIMEOUT);
                        recvlen = SLURM_ERROR;
                        goto done;
                }

                if ((rc = poll(&ufds, 1, timeleft)) <= 0) {
                        if ((errno == EINTR) || (errno == EAGAIN) || (rc == 0))
                                continue;
                        else {
				debug("_slurm_recv_timeout at %d of %d, "
					"poll error: %s",
					recvlen, size, strerror(errno));
 				slurm_seterrno(
					SLURM_COMMUNICATIONS_RECEIVE_ERROR);
 				recvlen = SLURM_ERROR; 
  				goto done;
                        }
                } 
                rc = _slurm_recv(fd, &buffer[recvlen], (size - recvlen), flags);
                if (rc < 0)  {
                        if (errno == EINTR)
                                continue;
                        else {
				debug("_slurm_recv_timeout at %d of %d, "
					"recv error: %s",
					recvlen, size, strerror(errno));
				slurm_seterrno(
					SLURM_COMMUNICATIONS_RECEIVE_ERROR);
				recvlen = SLURM_ERROR; 
				goto done;
                        }
                }
                if (rc == 0) {
			debug("_slurm_recv_timeout at %d of %d, "
				"recv zero bytes", recvlen, size);
			slurm_seterrno(SLURM_PROTOCOL_SOCKET_ZERO_BYTES_SENT);
			recvlen = SLURM_ERROR;
			goto done;
                }
                recvlen += rc;
        }


    done:
	/* Reset fd flags to prior state, preserve errno */
	if (fd_flags != SLURM_PROTOCOL_ERROR) {
		int slurm_err = slurm_get_errno();
		_slurm_fcntl(fd , F_SETFL , fd_flags);
		slurm_seterrno(slurm_err);
	}

        return recvlen;
}

int _slurm_shutdown_msg_engine ( slurm_fd open_fd )
{
        return _slurm_close ( open_fd ) ;
}

slurm_fd _slurm_listen_stream(slurm_addr *addr)
{
        int rc;
        slurm_fd fd;
        const int one = 1;
        const size_t sz1 = sizeof(one);

        if ((fd = _slurm_create_socket(SLURM_STREAM)) < 0) {
		error("Error creating slurm stream socket: %m");
                return fd;
        }

        rc = _slurm_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sz1);
        if (rc < 0) {
		error("setsockopt SO_REUSEADDR failed: %m");
                goto error; 
        }

        rc = _slurm_bind(fd, (struct sockaddr const *) addr, sizeof(*addr));
        if (rc < 0) {
		error("Error binding slurm stream socket: %m");
                goto error; 
        }

        if (_slurm_listen(fd, SLURM_PROTOCOL_DEFAULT_LISTEN_BACKLOG) < 0) {
		error( "Error listening on slurm stream socket: %m" ) ;
                rc = SLURM_ERROR;
                goto error; 
        }
        
        return fd;

    error:
	if ((_slurm_close_stream(fd) < 0) && (errno == EINTR))
		_slurm_close_stream(fd);	/* try again */
        return rc;
        
}

slurm_fd _slurm_accept_stream(slurm_fd fd, slurm_addr *addr)
{
        socklen_t len = sizeof(slurm_addr);
        return _slurm_accept(fd, (struct sockaddr *)addr, &len);
}

slurm_fd _slurm_open_stream(slurm_addr *addr, bool retry)
{
        int retry_cnt;
        slurm_fd fd;

        if ( (addr->sin_family == 0) || (addr->sin_port  == 0) ) 
                return SLURM_SOCKET_ERROR;

        for (retry_cnt=0; ; retry_cnt++) {
                int rc;
                if ((fd =_slurm_create_socket(SLURM_STREAM)) < 0) {
        		error("Error creating slurm stream socket: %m");
                        slurm_seterrno(errno);
                        return SLURM_SOCKET_ERROR;
                }

                if (retry_cnt) {
                        if (retry_cnt == 1)
                                debug3("Error connecting, picking new stream port");
                        _sock_bind_wild(fd);
                }

                rc = _slurm_connect(fd, (struct sockaddr const *)addr, sizeof(*addr));
                if (rc >= 0)                    /* success */
                        break;
                if ((errno != ECONNREFUSED) || 
                    (!retry) || (retry_cnt >= PORT_RETRIES)) {
                        slurm_seterrno(errno);
                        goto error;
                }

                if ((_slurm_close_stream(fd) < 0) && (errno == EINTR))
                        _slurm_close_stream(fd);        /* try again */
	}

        return fd;

    error:
        debug2("Error connecting slurm stream socket: %m");
	if ((_slurm_close_stream(fd) < 0) && (errno == EINTR))
		_slurm_close_stream(fd);	/* try again */
        return SLURM_SOCKET_ERROR;
}

int _slurm_get_stream_addr(slurm_fd fd, slurm_addr *addr )
{
        socklen_t size = sizeof(addr);
        return _slurm_getsockname(fd, (struct sockaddr *)addr, &size);
}

int _slurm_close_stream ( slurm_fd open_fd )
{
        return _slurm_close ( open_fd ) ;
}


int _slurm_set_stream_non_blocking(slurm_fd fd)
{
        fd_set_nonblocking(fd);
        return SLURM_SUCCESS;
}

int _slurm_set_stream_blocking(slurm_fd fd) 
{
        fd_set_blocking(fd);
        return SLURM_SUCCESS;
}

extern int _slurm_socket (int __domain, int __type, int __protocol)
{
        return socket ( __domain, __type, __protocol ) ;
}        

extern slurm_fd _slurm_create_socket ( slurm_socket_type_t type )
{
        switch ( type )
        {
                case SLURM_STREAM :
                        return _slurm_socket ( AF_INET, SOCK_STREAM, 
                                                IPPROTO_TCP) ;
                        break;
                case SLURM_MESSAGE :
                        return _slurm_socket ( AF_INET, SOCK_DGRAM, 
                                                IPPROTO_UDP ) ;
                        break;
                default :
                        return SLURM_SOCKET_ERROR;
        }
}

/* Create two new sockets, of type TYPE in domain DOMAIN and using
 * protocol PROTOCOL, which are connected to each other, and put file
 * descriptors for them in FDS[0] and FDS[1].  If PROTOCOL is zero,
 * one will be chosen automatically.  Returns 0 on success, -1 for errors.  */
extern int _slurm_socketpair (int __domain, int __type, 
                                int __protocol, int __fds[2])
{
        return SLURM_PROTOCOL_FUNCTION_NOT_IMPLEMENTED ;
}

/* Give the socket FD the local address ADDR (which is LEN bytes long).  */
extern int _slurm_bind (int __fd, struct sockaddr const * __addr, 
                                socklen_t __len)
{
        return bind ( __fd , __addr , __len ) ;
}

/* Put the local address of FD into *ADDR and its length in *LEN.  */
extern int _slurm_getsockname (int __fd, struct sockaddr * __addr, 
                                socklen_t *__restrict __len)
{
        return getsockname ( __fd , __addr , __len ) ;        
}

/* Open a connection on socket FD to peer at ADDR (which LEN bytes long).
 * For connectionless socket types, just set the default address to send to
 * and the only address from which to accept transmissions.
 * Return 0 on success, -1 for errors.  */
extern int _slurm_connect (int __fd, struct sockaddr const * __addr, 
                                socklen_t __len)
{
        return connect ( __fd , __addr , __len ) ;
}

/* Put the address of the peer connected to socket FD into *ADDR
 * (which is *LEN bytes long), and its actual length into *LEN.  */
extern int _slurm_getpeername (int __fd, struct sockaddr * __addr, 
                                socklen_t *__restrict __len)
{
        return getpeername ( __fd , __addr , __len ) ;
}

/* Send N bytes of BUF to socket FD.  Returns the number sent or -1.  */
extern ssize_t _slurm_send (int __fd, __const void *__buf, size_t __n, 
                                int __flags)
{
        return send ( __fd , __buf , __n , __flags ) ;
}

/* Read N bytes into BUF from socket FD.
 * Returns the number read or -1 for errors.  */
extern ssize_t _slurm_recv (int __fd, void *__buf, size_t __n, int __flags)
{
        return recv ( __fd , __buf , __n , __flags ) ;
}

/* Send N bytes of BUF on socket FD to peer at address ADDR (which is
 * ADDR_LEN bytes long).  Returns the number sent, or -1 for errors.  */
extern ssize_t _slurm_sendto (int __fd, __const void *__buf, size_t __n, int __flags, struct sockaddr const * __addr, 
                                socklen_t __addr_len)
{
        return sendto ( __fd , __buf , __n , __flags , __addr, __addr_len) ;
}
/* Read N bytes into BUF through socket FD.
 * If ADDR is not NULL, fill in *ADDR_LEN bytes of it with tha address of
 * the sender, and store the actual size of the address in *ADDR_LEN.
 * Returns the number of bytes read or -1 for errors.  */
extern ssize_t _slurm_recvfrom (int __fd, void *__restrict __buf, 
                                size_t __n, int __flags, 
                                struct sockaddr * __addr, 
                                socklen_t *__restrict __addr_len)
{
        return recvfrom ( __fd , __buf , __n , __flags , __addr, __addr_len) ;
}

/* Send a msg described MESSAGE on socket FD.
 * Returns the number of bytes sent, or -1 for errors.  */
extern ssize_t _slurm_sendmsg (int __fd, __const struct msghdr *__msg, 
                                int __flags)
{
        return sendmsg ( __fd , __msg , __flags ) ;
}

/* Send a msg described MESSAGE on socket FD.
 * Returns the number of bytes read or -1 for errors.  */
extern ssize_t _slurm_recvmsg (int __fd, struct msghdr *__msg, int __flags)
{
        return recvmsg ( __fd , __msg , __flags );
}

/* Put the current value for socket FD's option OPTNAME at protocol level LEVEL
 * into OPTVAL (which is *OPTLEN bytes long), and set *OPTLEN to the value's
 * actual length.  Returns 0 on success, -1 for errors.  */
extern int _slurm_getsockopt (int __fd, int __level, int __optname, 
                                void *__restrict __optval, 
                                socklen_t *__restrict __optlen)
{
        return getsockopt ( __fd , __level , __optname , __optval , __optlen ) ;
}

/* Set socket FD's option OPTNAME at protocol level LEVEL
 * to *OPTVAL (which is OPTLEN bytes long).
 * Returns 0 on success, -1 for errors.  */
extern int _slurm_setsockopt (int __fd, int __level, int __optname, 
                                __const void *__optval, socklen_t __optlen)
{
        return setsockopt ( __fd , __level , __optname , __optval , __optlen ) ;
}


/* Prepare to accept connections on socket FD.
 * N connection requests will be queued before further requests are refused.
 * Returns 0 on success, -1 for errors.  */
extern int _slurm_listen (int __fd, int __n)
{
        return listen ( __fd , __n ) ;
}

/* Await a connection on socket FD.
 * When a connection arrives, open a new socket to communicate with it,
 * set *ADDR (which is *ADDR_LEN bytes long) to the address of the connecting
 * peer and *ADDR_LEN to the address's actual length, and return the
 * new socket's descriptor, or -1 for errors.  */
extern int _slurm_accept (int __fd, struct sockaddr * __addr, 
                                socklen_t *__restrict __addr_len)
{
        return accept ( __fd , __addr , __addr_len ) ;
}

/* Shut down all or part of the connection open on socket FD.
 * HOW determines what to shut down:
 * SHUT_RD   = No more receptions;
 * SHUT_WR   = No more transmissions;
 * SHUT_RDWR = No more receptions or transmissions.
 * Returns 0 on success, -1 for errors.  */
extern int _slurm_shutdown (int __fd, int __how)
{
        return shutdown ( __fd , __how );
}

extern int _slurm_close (int __fd )
{
        return close ( __fd ) ;
}

extern int _slurm_fcntl(int fd, int cmd, ... )
{
        int rc ;
        va_list va ;

        va_start ( va , cmd ) ;
        rc =_slurm_vfcntl ( fd , cmd , va ) ;
        va_end ( va ) ;
        return rc ;
}

extern int _slurm_vfcntl(int fd, int cmd, va_list va )
{
        long arg ;

        switch ( cmd )
        {
                case F_GETFL :
                        return fcntl ( fd , cmd ) ;
                        break ;
                case F_SETFL :
                        arg = va_arg ( va , long ) ;
                        return fcntl ( fd , cmd , arg) ;
                        break ;
                default :
                        return SLURM_PROTOCOL_ERROR ;
                        break ;
        }
}

/* sets the fields of a slurm_addr */
void _slurm_set_addr_uint (slurm_addr *addr, uint16_t port, uint32_t ipaddr)
{
        addr->sin_family      = AF_SLURM ;
        addr->sin_port        = htons(port);
        addr->sin_addr.s_addr = htonl(ipaddr);
}

/* resets the address field of a slurm_addr, port and family are unchanged */
void _reset_slurm_addr (slurm_addr *addr, slurm_addr new_addr)
{
        addr->sin_addr.s_addr = new_addr.sin_addr.s_addr;
}

void _slurm_set_addr_char (slurm_addr * addr, uint16_t port, char *host)
{
        struct hostent * he    = NULL;
        int              h_err = 0;
        char *           h_buf[4096];

        /* 
         * If NULL hostname passed in, we only update the port
         *   of addr
         */
        addr->sin_family = AF_SLURM;
        addr->sin_port   = htons(port);
        if (host == NULL)
                return;

        he = get_host_by_name(host, (void *)&h_buf, sizeof(h_buf), &h_err);

        if (he != NULL)
                memcpy (&addr->sin_addr.s_addr, he->h_addr, he->h_length);
        else {
                error("Unable to resolve \"%s\": %s", host, hstrerror(h_err));
                addr->sin_family = 0;
                addr->sin_port = 0;
        } 
        return;
}

void _slurm_get_addr (slurm_addr *addr, uint16_t *port, char *host, 
                      unsigned int buflen )
{
        struct hostent *he;
        char   h_buf[4096];
        int    h_err  = 0;
        char * s_addr = (char *) &addr->sin_addr.s_addr;
        int    len    = sizeof(addr->sin_addr.s_addr);

        he = get_host_by_addr( s_addr, len, AF_SLURM, 
                               (void *) &h_buf, sizeof(h_buf), &h_err );

        if (he != NULL) {
                *port = ntohs(addr->sin_port);
                strncpy(host, he->h_name, buflen);
        } else {
                error("Lookup failed: %s", host_strerror(h_err));
                *port = 0;
                strncpy(host, "", buflen);
        } 
        return;
}

void _slurm_print_slurm_addr ( slurm_addr * address, char *buf, size_t n )
{
        char addrbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address->sin_addr, addrbuf, INET_ADDRSTRLEN);
        /* warning: silently truncates */
        snprintf(buf, n, "%s:%d", addrbuf, ntohs(address->sin_port));
}
        
void _slurm_pack_slurm_addr(slurm_addr *addr, Buf buffer)
{
        pack32( ntohl( addr->sin_addr.s_addr ), buffer );
        pack16( ntohs( addr->sin_port ), buffer );
}

int _slurm_unpack_slurm_addr_no_alloc(slurm_addr *addr, Buf buffer)
{
        addr->sin_family = AF_SLURM ;
        safe_unpack32(&addr->sin_addr.s_addr, buffer);
        safe_unpack16(&addr->sin_port, buffer);

        addr->sin_addr.s_addr = htonl(addr->sin_addr.s_addr);
        addr->sin_port = htons(addr->sin_port);
        return SLURM_SUCCESS;

    unpack_error:
        return SLURM_ERROR;
}

/*
 * vi: tabstop=8 shiftwidth=8 expandtab
 */
