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

#ifndef _NET_VSOCK_H_
#define _NET_VSOCK_H_

#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/queue.h>
#include "sys/sdt.h"

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

struct virtio_transport_ops {
	uint64_t (*get_local_cid)(void);
	int (*connect)(struct socket *);
	int (*disconnect)(struct socket *);
	int (*send)(struct socket *, struct mbuf *);
};


SDT_PROVIDER_DECLARE(vsock);
SDT_PROBE_DEFINE1(vsock, , ,create, "struct socket *");
SDT_PROBE_DEFINE1(vsock, , ,destroy, "struct socket *");

void	vsock_transport_register(struct virtio_transport_ops *);
void	vsock_transport_deregister(void);

void			vsock_pcb_insert_connected(struct vsock_pcb *pcb);
void			vsock_pcb_remove_connected(struct vsock_pcb *pcb);
void			vsock_pcb_insert_bound(struct vsock_pcb *pcb);
void			vsock_pcb_remove_bound(struct vsock_pcb *pcb);
struct vsock_pcb *	vsock_pcb_lookup_connected(uint32_t src_port, uint32_t dst_port);
struct vsock_pcb *	vsock_pcb_lookup_bound(uint32_t src_port, uint32_t dst_port);

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
int	vsock_send(struct socket *so, int flags, struct mbuf *m,
		struct sockaddr *addr, struct mbuf *c, struct thread *td);
int	vsock_disconnect(struct socket *);
int	vsock_shutdown(struct socket *, enum shutdown_how);
#endif /* _NET_VSOCK_H_ */
