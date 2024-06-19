/*-
 * Copyright (c) 2024, Danilo Egea Gondolfo <danilo@FreeBSD.org>
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

#ifndef _NET_VSOCK_TRANSPORT_H_
#define _NET_VSOCK_TRANSPORT_H_

#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/queue.h>
#include "sys/sdt.h"

struct vsock_addr {
	uint32_t port;
	uint32_t cid;
};

struct vsock_pcb {
	struct socket			*so;
	struct vsock_addr		local;
	struct vsock_addr		remote;
	uint32_t			fwd_cnt;
	uint32_t			tx_cnt;
	uint32_t			peer_credit;
	struct vsock_transport_ops 	*ops;
	LIST_ENTRY(vsock_pcb)		next;
};

struct vsock_transport_ops {
	uint64_t (*get_local_cid)(void);
	int (*connect)(struct vsock_addr *, struct vsock_addr *, uint32_t);
	int (*disconnect)(struct vsock_addr *, struct vsock_addr *);
	int (*send)(struct vsock_addr *, struct vsock_addr *, uint32_t, uint32_t, struct mbuf *);
	int (*send_rst)(struct vsock_addr *, struct vsock_addr *);
	int (*send_credit_update)(struct vsock_addr *, struct vsock_addr *, uint32_t, uint32_t);
	int (*request_ack)(struct vsock_addr *, struct vsock_addr *, uint32_t);
	void (*attach_socket)(void);
	void (*detach_socket)(void);
};

enum vsock_ops {
	VSOCK_REQUEST = 0,
	VSOCK_RESPONSE = 1,
	VSOCK_RESET = 2,
	VSOCK_SHUTDOWN = 3,
	VSOCK_DATA = 4,
	VSOCK_CREDIT_UPDATE = 5,
	VSOCK_CREDIT_REQUEST = 6,
};

void			vsock_pcb_insert_connected(struct vsock_pcb *pcb);
void			vsock_pcb_remove_connected(struct vsock_pcb *pcb);
void			vsock_pcb_insert_bound(struct vsock_pcb *pcb);
void			vsock_pcb_remove_bound(struct vsock_pcb *pcb);
struct vsock_pcb *	vsock_pcb_lookup_connected(uint32_t src_port, uint32_t dst_port);
struct vsock_pcb *	vsock_pcb_lookup_bound(uint32_t src_port, uint32_t dst_port);
void			vsock_transport_lock(void);
void			vsock_transport_unlock(void);

void	vsock_transport_register(struct vsock_transport_ops *);
void	vsock_transport_deregister(void);
void	vsock_transport_ops_lock(void);
void	vsock_transport_ops_unlock(void);

int	vsock_input(struct vsock_addr *src, struct vsock_addr *dst, enum vsock_ops op,
		uint32_t buf_alloc, uint32_t fw_cnt, struct mbuf *mbuf);

#endif /* _NET_VSOCK_TRANSPORT_H_ */
