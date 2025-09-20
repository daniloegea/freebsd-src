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

#ifndef _NET_VSOCK_DOMAIN_H_
#define _NET_VSOCK_DOMAIN_H_

#ifdef _KERNEL
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/queue.h>
#include <sys/ck.h>
#include <sys/epoch.h>

#define	VSOCK_RCV_BUFFER_SIZE	(256 * 1024)
#define	VSOCK_MAX_MSG_SIZE	(64 * 1024)

#define	VSOCK_SHUT_RCV		0x01
#define	VSOCK_SHUT_SND		0x02
#define	VSOCK_SHUT_ALL		0x03

struct vsock_addr {
	uint32_t port;
	uint32_t cid;
};

struct vsock_pcb {
	CK_LIST_ENTRY(vsock_pcb)	next;
	struct socket			*so;
	struct vsock_addr		local;
	struct vsock_addr		remote;
	uint32_t			peer_shutdown;
	struct vsock_transport_ops 	*ops;
	struct mtx			mtx;
	struct epoch_context		epoch_ctx;

	/* Transport private data */
	void				*transport;
};


struct vsock_pcb *	vsock_pcb_lookup_connected(struct vsock_addr *, struct vsock_addr *);
struct vsock_pcb *	vsock_pcb_lookup_bound(struct vsock_addr *);
void			vsock_pcb_insert_connected(struct vsock_pcb *);
void			vsock_pcb_remove_connected(struct vsock_pcb *);
void			vsock_pcb_insert_bound(struct vsock_pcb *);
void			vsock_pcb_remove_bound(struct vsock_pcb *);


#endif /* _KERNEL */
#endif /* _NET_VSOCK_DOMAIN_H_ */
