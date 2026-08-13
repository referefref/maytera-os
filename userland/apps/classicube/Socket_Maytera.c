/*
 * Socket_Maytera.c - ClassiCube's TCP socket layer for MayteraOS (task #28).
 *
 * WHY THIS IS A SEPARATE FILE. Upstream puts Socket_* in Platform_<os>.c.
 * Here it is split out so the socket surface (which is the networking lane's
 * responsibility) does not collide with the platform lane's file. Platform_
 * Maytera.c MUST NOT define any of: ReturnCode_SocketInProgess,
 * ReturnCode_SocketWouldBlock, ReturnCode_SocketDropped, SockAddr_ToString,
 * Socket_ParseAddress, Socket_Create, Socket_SetNonBlocking, Socket_Close,
 * Socket_Connect, Socket_Read, Socket_Write, Socket_Poll. Every one of them
 * lives here.
 *
 * THE REAL SOCKET SURFACE MayteraOS EXPOSES TO RING 3. There are TWO, and
 * mixing them is a trap, because both hand back small non-negative integers
 * that look interchangeable and are not:
 *   1. The LEGACY one: SYS_SOCKET (60) / SYS_CONNECT (61) / SYS_SEND (62) /
 *      SYS_RECV (63) / SYS_TCP_CLOSE (64) / SYS_TCP_STATE (65). The value it
 *      returns is a KERNEL TCB SLOT INDEX, not a file descriptor.
 *   2. The BSD one (#524): SYS_SOCK_OPEN (343) through SYS_SOCK_SHUTDOWN
 *      (355), wrapped by userland/libc/sys/socket.{h,c} as the usual
 *      socket/bind/listen/accept/connect/send/recv/select/shutdown. The value
 *      it returns is a real FILE DESCRIPTOR in the process fd table.
 * This file uses (2) exclusively. Passing a BSD fd to SYS_TCP_STATE, or a
 * legacy slot to recv(), would index the wrong table and quietly do the wrong
 * thing rather than fail.
 *
 * SO YES, MULTIPLAYER IS IN SCOPE. Client TCP is genuinely available to Ring
 * 3: connect(), send(), recv() and select() are all implemented in
 * kernel/net/socket.c against the real TCP stack, and select() is a true
 * wait_event-driven implementation (not the libc timeout fallback that
 * userland/libc/sys/socket.c still documents, which only fires when the
 * syscall itself returns < 0). Nothing in this file needs a single-player-only
 * fallback.
 *
 * NON-BLOCKING, AND THE ONE THING THAT DOES NOT WORK. ClassiCube wants a
 * non-blocking socket: a connect that returns immediately, then a writability
 * poll, then non-blocking reads inside the frame loop. On MayteraOS today:
 *   - recv/send DO honour MSG_DONTWAIT (kernel/net/socket.c sock_recv_core /
 *     sock_send_core), so non-blocking I/O works per call.
 *   - fcntl(F_SETFL, O_NONBLOCK) is ACCEPTED AND IGNORED. In kernel/proc/
 *     fdlayer.c, sys_fcntl()'s F_SETFL case is literally "return 0" with a
 *     comment saying "accept, ignore". It reports success and changes
 *     nothing, which is exactly the class of bug this project keeps getting
 *     bitten by, so this file does not trust it: Socket_SetNonBlocking WRITES
 *     the flag and then READS IT BACK, and only believes the kernel honours
 *     O_NONBLOCK if the readback proves it. Otherwise it falls back to
 *     per-call MSG_DONTWAIT.
 *   - connect() has no MSG_DONTWAIT equivalent; it decides blocking vs not
 *     from that same ignored O_NONBLOCK flag. So on a kernel that ignores
 *     F_SETFL, the fallback sets SO_SNDTIMEO to 1 ms first: sys_sock_connect
 *     still issues the SYN, then gives up waiting almost immediately and
 *     returns ETIMEDOUT, which this file maps to ReturnCode_SocketInProgess.
 *     The handshake continues in the background (the kernel main loop
 *     net_poll() drains RX independently of syscalls) and Socket_Poll picks
 *     up the completion. SO_SNDTIMEO is restored afterwards.
 * If and when sys_fcntl learns F_SETFL, the readback starts succeeding and
 * this file uses the native path with no further change.
 *
 * A FAILED CONNECT IS DETECTED, NOT WAITED OUT. getsockopt(SO_ERROR) exists in
 * the kernel but NOTHING EVER SETS s->so_error (grep: it is only zeroed and
 * read), so it would always answer "no error" - another check that reports
 * success while doing nothing, and deliberately not used here. What IS real:
 * sock_file_poll() reports POLL_OUT only in ESTABLISHED, and reports POLL_IN
 * for a CLOSED slot (tcp_rx_pending returns -1 for terminal states). During a
 * connect no data can have arrived yet, so "readable but not writable" means
 * the handshake died. Socket_Poll(SOCKET_POLL_WRITE) tests both in one
 * select() and returns ReturnCode_SocketDropped in that case, turning a
 * refused connection from a 15 second "Connecting.." into an immediate
 * failure message.
 *
 * BUILD INTEGRATION: needs -I<ClassiCube>/src and -I<userland>/libc.
 */
#include "Core.h"
#include "Platform.h"
#include "String_.h"
#include "Errors.h"

/* MayteraOS userland libc */
#include "syscall.h"
#include "sys/socket.h"
#include "errno.h"
#include "fcntl.h"
#include "unistd.h"

/* ClassiCube's three well-known socket result codes. Server.c compares
 * against these by value, so they must be the errno values this libc really
 * produces (errno.h: EINPROGRESS 115, EAGAIN 11, ECONNRESET 104). */
const cc_result ReturnCode_SocketInProgess  = EINPROGRESS;
const cc_result ReturnCode_SocketWouldBlock = EWOULDBLOCK;
const cc_result ReturnCode_SocketDropped    = ECONNRESET;

/* A MayteraOS process is capped at 64 fds (kernel/net/socket.c sizes fd_set
 * as a single 64 bit word for exactly this reason), so a flat table indexed
 * by fd is the whole bookkeeping this file needs. */
#define MSOCK_MAX_FDS 64
static cc_bool sock_wantNonblock[MSOCK_MAX_FDS];
static cc_bool sock_kernelNonblock[MSOCK_MAX_FDS];

static cc_bool Sock_Valid(cc_socket s) { return s >= 0 && s < MSOCK_MAX_FDS; }

static int Sock_MsgFlags(cc_socket s) {
	/* MSG_DONTWAIT is harmless when the kernel already honours O_NONBLOCK, so
	 * it is set whenever the caller asked for non-blocking. */
	return (Sock_Valid(s) && sock_wantNonblock[s]) ? MSG_DONTWAIT : 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Addresses--------------------------------------------------------*
*#########################################################################################################################*/
/* Compile-time check that cc_sockaddr can hold what this platform stores in
 * it. A typedef rather than upstream's unused static array, so it cannot trip
 * -Wunused-variable on a -Werror build. */
typedef char maytera_sockaddr_size_check[sizeof(struct sockaddr_in) < CC_SOCKETADDR_MAXSIZE ? 1 : -1];

cc_bool SockAddr_ToString(const cc_sockaddr* addr, cc_string* dst) {
	const struct sockaddr_in* addr4 = (const struct sockaddr_in*)addr->data;
	cc_uint32 ip;
	int a, b, c, d, port;

	if (addr4->sin_family != AF_INET) return false;

	/* s_addr is network order; ntohl gives the a.b.c.d ordering. */
	ip   = (cc_uint32)ntohl(addr4->sin_addr.s_addr);
	port = (int)ntohs(addr4->sin_port);
	a = (ip >> 24) & 0xFF; b = (ip >> 16) & 0xFF;
	c = (ip >>  8) & 0xFF; d =  ip        & 0xFF;

	String_Format4(dst, "%i.%i.%i.%i", &a, &b, &c, &d);
	String_Format1(dst, ":%i", &port);
	return true;
}

/* Fills in one IPv4 sockaddr. host_ip is in HOST order (a<<24 | b<<16 ...),
 * which is what SYS_DNS_START/POLL hand back; the kernel's own
 * copy_sockaddr_in() runs ntohl on what it is given, so it must be stored in
 * NETWORK order here. Getting this backwards would connect to a valid-looking
 * but completely different address, so it is spelled out rather than assumed. */
static void FillAddr(cc_sockaddr* dst, cc_uint32 host_ip, int port) {
	struct sockaddr_in* addr4 = (struct sockaddr_in*)dst->data;

	Mem_Set(addr4, 0, sizeof(struct sockaddr_in));
	addr4->sin_family      = AF_INET;
	addr4->sin_port        = htons((unsigned short)port);
	addr4->sin_addr.s_addr = htonl(host_ip);
	dst->size              = sizeof(struct sockaddr_in);
}

/* Resolution deadline and pacing for the DNS path below. */
#define MSOCK_DNS_TIMEOUT_MS 5000
#define MSOCK_DNS_STEP_MS      50

/* Static, not a stack array: MPConnection_BeginConnect already puts a
 * cc_sockaddr addrs[5] (~2.5 KB) on the stack before calling this, and a
 * MayteraOS user process does not have a generous stack. */
static char dns_hostBuffer[256];

cc_result Socket_ParseAddress(const cc_string* address, int port, cc_sockaddr* addrs, int* numValidAddrs) {
	char* host = dns_hostBuffer;
	in_addr_t literal;
	unsigned int ip = 0;
	int rc, waited;

	*numValidAddrs = 0;
	if (address->length >= (int)sizeof(dns_hostBuffer)) return ERR_INVALID_ARGUMENT;
	String_EncodeUtf8(host, address);

	/* Fast path: a dotted quad needs no resolver at all, which matters
	 * because "Direct connect" in the launcher hands over an IP far more
	 * often than a hostname. */
	literal = inet_addr(host);
	if (literal != INADDR_NONE) {
		FillAddr(&addrs[0], (cc_uint32)ntohl(literal), port);
		*numValidAddrs = 1;
		return 0;
	}

	/* DNS. The kernel exposes only the poll-split resolver (SYS_DNS_START /
	 * SYS_DNS_POLL); there is no blocking resolve syscall to hand the wait to,
	 * and no wait queue reachable from Ring 3. So this is a paced, bounded
	 * wait: each step sleeps in the kernel via SYS_SLEEP (the same primitive
	 * sleep()/usleep() use), it is NOT a spin, and the total is capped at
	 * MSOCK_DNS_TIMEOUT_MS. It runs on the connect path only, which upstream
	 * also blocks on (getaddrinfo), never inside the render loop. */
	rc = dns_start(host, &ip);
	if (rc < 0) return ERR_INVALID_ARGUMENT;

	for (waited = 0; rc == 0 && waited < MSOCK_DNS_TIMEOUT_MS; waited += MSOCK_DNS_STEP_MS) {
		usleep(MSOCK_DNS_STEP_MS * 1000);
		rc = dns_poll(&ip);
	}

	if (rc != 1 || !ip) return ERR_INVALID_ARGUMENT;

	FillAddr(&addrs[0], (cc_uint32)ip, port);
	*numValidAddrs = 1;
	return 0;
}


/*########################################################################################################################*
*---------------------------------------------------------Sockets---------------------------------------------------------*
*#########################################################################################################################*/
cc_result Socket_Create(cc_socket* s, cc_sockaddr* addr) {
	const struct sockaddr_in* addr4 = (const struct sockaddr_in*)addr->data;

	*s = socket(addr4->sin_family, SOCK_STREAM, 0);
	if (*s < 0) return errno;

	if (Sock_Valid(*s)) {
		sock_wantNonblock[*s]   = false;
		sock_kernelNonblock[*s] = false;
	}
	return 0;
}

cc_result Socket_SetNonBlocking(cc_socket s, cc_bool nonblocking) {
	int flags, want, got;
	if (!Sock_Valid(s)) return EBADF;

	sock_wantNonblock[s]   = nonblocking;
	sock_kernelNonblock[s] = false;

	flags = fcntl(s, F_GETFL, 0);
	if (flags < 0) return 0;   /* per-call MSG_DONTWAIT still covers I/O */

	want = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	fcntl(s, F_SETFL, want);

	/* PROVE IT. sys_fcntl() currently returns 0 for F_SETFL without storing
	 * anything, so "no error" says nothing about whether the flag took. */
	got = fcntl(s, F_GETFL, 0);
	if (got >= 0 && (((got & O_NONBLOCK) != 0) == (nonblocking != 0))) {
		sock_kernelNonblock[s] = nonblocking;
	}
	return 0;
}

void Socket_Close(cc_socket s) {
	shutdown(s, SHUT_RDWR);
	close(s);
	if (Sock_Valid(s)) {
		sock_wantNonblock[s]   = false;
		sock_kernelNonblock[s] = false;
	}
}

cc_result Socket_Connect(cc_socket s, cc_sockaddr* addr) {
	struct timeval tv;
	cc_bool shim;
	int res, err;

	/* Native path: the kernel accepted O_NONBLOCK, so connect() returns
	 * EINPROGRESS by itself. */
	shim = Sock_Valid(s) && sock_wantNonblock[s] && !sock_kernelNonblock[s];

	if (shim) {
		/* Make sys_sock_connect's own wait expire almost at once. It still
		 * sends the SYN before waiting, so this starts the handshake rather
		 * than skipping it. */
		tv.tv_sec = 0; tv.tv_usec = 1000;
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}

	res = connect(s, (const struct sockaddr*)addr->data, (socklen_t)addr->size);
	err = res < 0 ? errno : 0;

	if (shim) {
		/* 0 restores the kernel default (sock_deadline treats 0 as "never"),
		 * so subsequent blocking sends are unaffected. */
		tv.tv_sec = 0; tv.tv_usec = 0;
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		/* The SYN is out and the handshake is proceeding in the background. */
		if (err == ETIMEDOUT || err == EAGAIN) return ReturnCode_SocketInProgess;
	}
	return err;
}

cc_result Socket_Read(cc_socket s, cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	long n = recv(s, data, count, Sock_MsgFlags(s));

	if (n >= 0) { *modified = (cc_uint32)n; return 0; }
	*modified = 0;
	return errno;
}

cc_result Socket_Write(cc_socket s, const cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	long n = send(s, data, count, Sock_MsgFlags(s));

	if (n >= 0) { *modified = (cc_uint32)n; return 0; }
	*modified = 0;
	return errno;
}

cc_result Socket_Poll(cc_socket s, int timeoutMS, int mode, cc_bool* success) {
	fd_set read_set, write_set;
	struct timeval tv;
	int n;

	*success = false;
	if (!Sock_Valid(s)) return EBADF;

	FD_ZERO(&read_set);
	FD_ZERO(&write_set);
	/* Both directions are always requested: in WRITE mode the read bit is what
	 * distinguishes "still handshaking" from "handshake died" (see the header
	 * comment), and in READ mode the write bit costs nothing. */
	FD_SET(s, &read_set);
	FD_SET(s, &write_set);

	tv.tv_sec  = timeoutMS / 1000;
	tv.tv_usec = (timeoutMS % 1000) * 1000;

	n = select(s + 1, &read_set, &write_set, NULL, &tv);
	if (n < 0) return errno;

	if (mode == SOCKET_POLL_READ) {
		*success = FD_ISSET(s, &read_set) != 0;
		return 0;
	}

	if (FD_ISSET(s, &write_set)) { *success = true; return 0; }

	/* Not writable. Readable anyway means the TCB reached a terminal state
	 * (tcp_rx_pending returns -1 for CLOSED/CLOSE_WAIT/LAST_ACK/TIME_WAIT);
	 * nothing can have been received on a connection that never came up, so
	 * this is a dead handshake, not buffered data. */
	if (FD_ISSET(s, &read_set)) return ReturnCode_SocketDropped;
	return 0;
}
