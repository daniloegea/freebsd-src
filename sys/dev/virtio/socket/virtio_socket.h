/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023, Danilo Egea Gondolfo <danilo@FreeBSD.org>
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

struct virtio_vtsock_config {
	uint64_t guest_cid;
};

struct virtio_vtsock_event {
	uint32_t id;
};

struct virtio_vtsock_hdr {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint32_t len;
	uint16_t type;
	uint16_t op;
	uint32_t flags;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
} __packed;

struct virtio_socket_data {
	struct mtx	mtx;
	uint32_t	fwd_cnt;
	uint32_t	buf_alloc;
	uint32_t	tx_cnt;
	uint32_t	peer_buf_alloc;
	uint32_t	peer_fwd_cnt;
	uint32_t	last_fwd_cnt;
	uint32_t	last_buf_alloc;
};

/* Features */
#define VIRTIO_VTSOCK_F_STREAM		0
#define VIRTIO_VTSOCK_F_SEQPACKET	1

/* Operations */
#define VIRTIO_VTSOCK_OP_INVALID	0
#define VIRTIO_VTSOCK_OP_REQUEST	1
#define VIRTIO_VTSOCK_OP_RESPONSE	2
#define VIRTIO_VTSOCK_OP_RST		3
#define VIRTIO_VTSOCK_OP_SHUTDOWN	4
#define VIRTIO_VTSOCK_OP_RW		5
#define VIRTIO_VTSOCK_OP_CREDIT_UPDATE	6
#define VIRTIO_VTSOCK_OP_CREDIT_REQUEST	7
#define VIRTIO_VTSOCK_OP_MAX		7

/* Socket types */
#define VIRTIO_VTSOCK_TYPE_STREAM	1
#define VIRTIO_VTSOCK_TYPE_SEQPACKET	2

/* Transport events */
#define VIRTIO_VTSOCK_EVENT_TRANSPORT_RESET	0

/* Flags */
#define VIRTIO_VTSOCK_SHUTDOWN_F_RECEIVE	0x1
#define VIRTIO_VTSOCK_SHUTDOWN_F_SEND		0x2

#define VTSOCK_BUFSZ			(16 * 1024)
#define VTSOCK_TX_RINGBUFFER_SIZE	1024

#endif /* _VIRTIO_SOCKET_H */
