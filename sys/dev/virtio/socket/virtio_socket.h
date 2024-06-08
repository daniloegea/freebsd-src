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

#include <sys/types.h>

struct virtio_vsock_config {
	uint64_t guest_cid;
};

struct virtio_vsock_event {
	uint32_t id;
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

SDT_PROVIDER_DECLARE(vtsock);

#endif /* _VIRTIO_SOCKET_H */
