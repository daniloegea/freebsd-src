/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024, Danilo Egea Gondolfo <danilo@FreeBSD.org>
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

#ifdef _KERNEL

struct vsock_addr;
struct vsock_pcb;

enum vsock_ops {
	VSOCK_INVALID,
	VSOCK_REQUEST,
	VSOCK_RESPONSE,
	VSOCK_RESET,
	VSOCK_SHUTDOWN,
	VSOCK_SHUTDOWN_SEND,
	VSOCK_SHUTDOWN_RECV,
	VSOCK_DISCONNECT,
	VSOCK_DATA,
	VSOCK_CREDIT_UPDATE,
	VSOCK_CREDIT_REQUEST
};

#define VSOCK_LOCK(vpcb) mtx_lock(&(vpcb)->mtx)
#define VSOCK_UNLOCK(vpcb) mtx_unlock(&(vpcb)->mtx)

struct vsock_transport_ops {
	uint64_t (*get_local_cid)(void);
	int (*send_message)(void *transport, struct vsock_addr *, struct vsock_addr *, enum vsock_ops, struct mbuf *);
	void (*post_receive)(struct vsock_pcb *, uint32_t);
	uint32_t (*check_writable)(struct vsock_pcb *, bool);
	int (*attach_socket)(struct vsock_pcb *);
	void (*detach_socket)(struct vsock_pcb *);
};

void	vsock_transport_lock(void);
void	vsock_transport_unlock(void);

void	vsock_transport_register(struct vsock_transport_ops *);
void	vsock_transport_deregister(void);

#endif /* _KERNEL */
#endif /* _NET_VSOCK_TRANSPORT_H_ */
