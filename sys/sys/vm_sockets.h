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

#ifndef _SYS_VM_SOCKET_H_
#define _SYS_VM_SOCKET_H_

#include <sys/socket.h>

#define VMADDR_CID_ANY (-1U)
#define VMADDR_CID_HYPERVISOR 0
#define VMADDR_CID_RESERVED 1
#define VMADDR_CID_HOST 2

#define VMADDR_PORT_ANY (-1U)

struct sockaddr_vm {
	unsigned char	svm_len;
	sa_family_t	svm_family;
	uint32_t	svm_port;
	uint32_t        svm_cid;
	unsigned char	svm_flags;
	char		svm_zero[sizeof(struct sockaddr) -
				sizeof(unsigned char) -
				sizeof(sa_family_t) -
				sizeof(unsigned int) -
				sizeof(unsigned int) -
				sizeof(unsigned char)];
};

#endif /* _SYS_VM_SOCKET_H_ */
