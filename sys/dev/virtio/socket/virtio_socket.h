/*-
 * Copyright (c) 2023, Danilo Egea Gondolfo <danilo@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _VIRTIO_SOCKET_H
#define _VIRTIO_SOCKET_H

#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/queue.h>

struct virtio_vsock_config {
	uint64_t guest_cid;
};

#define VIRTIO_VSOCK_F_STREAM		0
#define VIRTIO_VSOCK_F_SEQPACKET	1

#define VIRTIO_VSOCK_OP_INVALID		0
/* Connect operations */
#define VIRTIO_VSOCK_OP_REQUEST		1
#define VIRTIO_VSOCK_OP_RESPONSE	2
#define VIRTIO_VSOCK_OP_RST		3
#define VIRTIO_VSOCK_OP_SHUTDOWN	4
/* To send payload */
#define VIRTIO_VSOCK_OP_RW		5
/* Tell the peer our credit info */
#define VIRTIO_VSOCK_OP_CREDIT_UPDATE	6
/* Request the peer to send the credit info to us */
#define VIRTIO_VSOCK_OP_CREDIT_REQUEST	7

#define VIRTIO_VSOCK_TYPE_STREAM	1
#define VIRTIO_VSOCK_TYPE_SEQPACKET	2

#define VIRTIO_VSOCK_EVENT_TRANSPORT_RESET	0

/* Flags */
#define VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE	0
#define VIRTIO_VSOCK_SHUTDOWN_F_SEND	1


#define VSOCK_BOUND_LIST	0x1
#define VSOCK_CONNECTED_LIST	0x2

struct virtio_vsock_event {
	uint32_t id;
};

struct sockaddr_vm {
	unsigned char	svm_len;
	sa_family_t	svm_family;
	uint32_t	svm_port;
	uint32_t        svm_cid;
	unsigned char	svm_zero[sizeof(struct sockaddr) -
				sizeof(sa_family_t) -
				sizeof(unsigned char) -
				sizeof(unsigned int) -
				sizeof(unsigned int)];
};

struct vsock_pcb {
	struct socket		*so;
	struct sockaddr_vm	local_addr;
	struct sockaddr_vm	remote_addr;
	uint32_t		fwd_cnt;
	uint32_t		tx_cnt;
	uint32_t		peer_credit;
	LIST_ENTRY(vsock_pcb)	next;
};

void	vsock_close(struct socket *);
void	vsock_detach(struct socket *);
void	vsock_abort(struct socket *);
int	vsock_attach(struct socket *, int, struct thread *);
int	vsock_bind(struct socket *, struct sockaddr *, struct thread *);
int	vsock_listen(struct socket *, int, struct thread *);
int	vsock_accept(struct socket *, struct sockaddr *);
int	vsock_connect(struct socket *, struct sockaddr *, struct thread *);
int	vsock_peeraddr(struct socket *, struct sockaddr *);
int	vsock_sockaddr(struct socket *, struct sockaddr *);
int	vsock_soreceive(struct socket *, struct sockaddr **,
		struct uio *, struct mbuf **, struct mbuf **, int *);
int	vsock_sosend(struct socket *, struct sockaddr *, struct uio *,
		struct mbuf *, struct mbuf *, int, struct thread *);
int	vsock_disconnect(struct socket *);
int	vsock_shutdown(struct socket *, enum shutdown_how);
#endif /* _VIRTIO_SOCKET_H */
