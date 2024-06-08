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

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/ctype.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/kdb.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sglist.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/queue.h>

#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/sockbuf.h>
#include <sys/mbuf.h>

#include <net/vnet.h>

#include <sys/types.h>
#include <sys/uio.h>

#include <sys/conf.h>
#include <sys/cons.h>
#include <sys/tty.h>

#include <sys/sdt.h>
#include <sys/socketvar.h>

#include "vsock.h"

#define so2vsockpcb(so) \
	((struct vsock_pcb *)((so)->so_pcb))
#define vsockpcb2so(vsockpcb) \
	((struct socket *)((vsockpcb)->so))

MALLOC_DEFINE(M_VSOCK, "virtio_socket", "virtio socket control structures");

static struct mtx 		vsock_pcbs_connected_mtx;
static LIST_HEAD(, vsock_pcb)	vsock_pcbs_connected;
static struct mtx 		vsock_pcbs_bound_mtx;
static LIST_HEAD(, vsock_pcb)	vsock_pcbs_bound;

static struct virtio_transport_ops	*vsock_transport = NULL;
static struct mtx 			vsock_transport_mtx;

static _Atomic(uint32_t) vsock_last_source_port = 123456;

/*
 * VSOCK Transport sockets
 */

SYSCTL_NODE(_net, OID_AUTO, vsock, CTLFLAG_RD, 0, "Virtio VSOCK");
static int vsock_dom_probe(void);


static struct protosw vsock_protosw = {
	.pr_type =		SOCK_STREAM,
	.pr_flags =		PR_CONNREQUIRED,
	.pr_attach =		vsock_attach,
	.pr_bind =		vsock_bind,
	.pr_listen =		vsock_listen,
	.pr_accept =		vsock_accept,
	.pr_connect =		vsock_connect,
	.pr_peeraddr =		vsock_peeraddr,
	.pr_sockaddr =		vsock_sockaddr,
	.pr_soreceive =		soreceive_generic,
	.pr_sopoll =		sopoll_generic,
	.pr_sosend =		sosend_generic,
	.pr_send = 		vsock_send,
	.pr_disconnect =	vsock_disconnect,
	.pr_close =		vsock_close,
	.pr_detach =		vsock_detach,
	.pr_shutdown =		vsock_shutdown,
	.pr_abort =		vsock_abort,
};

static struct domain vsock_domain = {
	.dom_family =	AF_VSOCK,
	.dom_name =	"vsock",
	.dom_flags =   	DOMF_UNLOADABLE,
	.dom_probe =	vsock_dom_probe,
	.dom_nprotosw =	1,
	.dom_protosw =	{ &vsock_protosw },
};

DOMAIN_SET(vsock_);
MODULE_VERSION(vsock, 1);

#define MAX_PORT	((uint32_t)0xFFFFFFFF)
#define MIN_PORT	((uint32_t)0x0)

static int
vsock_dom_probe(void)
{
	return (0);
}



static void
vsock_init(void *arg __unused)
{
	mtx_init(&vsock_pcbs_connected_mtx,
		  "vsock_pcbs_mtx", NULL, MTX_DEF);

	LIST_INIT(&vsock_pcbs_connected);

	mtx_init(&vsock_pcbs_bound_mtx,
		  "vsock_bound_pcbs_mtx", NULL, MTX_DEF);

	LIST_INIT(&vsock_pcbs_bound);

	mtx_init(&vsock_transport_mtx,
		  "vsock_transport_mtx", NULL, MTX_DEF);
}

SYSINIT(vsock_init, SI_SUB_PROTO_DOMAIN, SI_ORDER_THIRD, vsock_init, NULL);

void
vsock_pcb_insert_connected(struct vsock_pcb *pcb)
{
	mtx_lock(&vsock_pcbs_connected_mtx);
	LIST_INSERT_HEAD(&vsock_pcbs_connected, pcb, next);
	mtx_unlock(&vsock_pcbs_connected_mtx);
}

void
vsock_pcb_remove_connected(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	mtx_lock(&vsock_pcbs_connected_mtx);
	LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p == pcb) {
		LIST_REMOVE(pcb, next);
	}
	mtx_unlock(&vsock_pcbs_connected_mtx);
}

void
vsock_pcb_insert_bound(struct vsock_pcb *pcb)
{
	mtx_lock(&vsock_pcbs_bound_mtx);
	LIST_INSERT_HEAD(&vsock_pcbs_bound, pcb, next);
	mtx_unlock(&vsock_pcbs_bound_mtx);
}

void
vsock_pcb_remove_bound(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	mtx_lock(&vsock_pcbs_bound_mtx);
	LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p == pcb) {
		LIST_REMOVE(pcb, next);
	}
	mtx_unlock(&vsock_pcbs_bound_mtx);
}

struct vsock_pcb *
vsock_pcb_lookup_connected(uint32_t local_port, uint32_t remote_port)
{
	struct vsock_pcb *p = NULL;

	mtx_lock(&vsock_pcbs_connected_mtx);
	LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p->so &&
		local_port == p->local_addr.svm_port &&
		remote_port == p->remote_addr.svm_port) {
		mtx_unlock(&vsock_pcbs_connected_mtx);
		return p;
	}
	mtx_unlock(&vsock_pcbs_connected_mtx);
	return p;
}

struct vsock_pcb *
vsock_pcb_lookup_bound(uint32_t local_port, uint32_t remote_port)
{
	struct vsock_pcb *p = NULL;

	mtx_lock(&vsock_pcbs_bound_mtx);
	LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p->so &&
		local_port == p->local_addr.svm_port &&
	remote_port == 0) {
		mtx_unlock(&vsock_pcbs_bound_mtx);
		return p;
	}
	mtx_unlock(&vsock_pcbs_bound_mtx);

	return p;
}

int
vsock_attach(struct socket *so, int proto, struct thread *td)
{
	struct vsock_pcb *pcb;
	int error;

	SDT_PROBE1(vsock, , , create, so);

	pcb = malloc(sizeof(struct vsock_pcb), M_VSOCK, M_NOWAIT | M_ZERO);
	if (pcb == NULL) {
		return (ENOMEM);
	}

	pcb->so = so;
	so->so_pcb = pcb;
	error = soreserve(so, 8196, 8196);

	return (error);
}

void
vsock_detach(struct socket *so)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	SDT_PROBE1(vsock, , , destroy, so);

	if (pcb == NULL)
		return;

	if (SOLISTENING(so)) {
		vsock_pcb_remove_bound(pcb);
	} else {
		vsock_pcb_remove_connected(pcb);
	}
	free(pcb, M_VSOCK);
	so->so_pcb = NULL;

        sbrelease(so, SO_RCV);
        sbrelease(so, SO_SND);
}

int
vsock_bind(struct socket *so, struct sockaddr *addr, struct thread *td)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct sockaddr_vm *sa = (struct sockaddr_vm *) addr;

	if (sa == NULL) {
		return (EINVAL);
	}

	if (pcb == NULL) {
		return (EINVAL);
	}

	if (sa->svm_family != AF_VSOCK) {
		return (EAFNOSUPPORT);
	}

	if (sa->svm_len != sizeof(*sa)) {
		return (EINVAL);
	}

	pcb->local_addr.svm_port = sa->svm_port;
	pcb->local_addr.svm_cid = vsock_transport->get_local_cid();
	pcb->local_addr.svm_family = AF_VSOCK;
	pcb->local_addr.svm_len = sizeof(*sa);

	vsock_pcb_insert_bound(pcb);

	return (0);
}

int
vsock_listen(struct socket *so, int backlog, struct thread *td)
{
	int error;

	SOCK_LOCK(so);
	error = solisten_proto_check(so);
	if (error == 0)
		solisten_proto(so, backlog);
	SOCK_UNLOCK(so);
	return (error);
}

int
vsock_accept(struct socket *so, struct sockaddr *sa)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	memcpy(sa, &pcb->remote_addr, pcb->remote_addr.svm_len);

	return (0);
}

int
vsock_connect(struct socket *so, struct sockaddr *nam, struct thread *td)
{
	struct sockaddr_vm *vsock = (struct sockaddr_vm *) nam;
	struct vsock_pcb *pcb = so2vsockpcb(so);

	if (pcb == NULL) {
		return (EINVAL);
	}

	if (vsock->svm_family != AF_VSOCK) {
		return (EAFNOSUPPORT);
	}

	if (vsock->svm_len != sizeof(*vsock)) {
		return (EINVAL);
	}

	if (so->so_state & SS_ISCONNECTED) {
		return (EISCONN);
	}

	if (so->so_state & (SS_ISDISCONNECTING|SS_ISCONNECTING)) {
		return (EINPROGRESS);
	}

	pcb->local_addr.svm_cid = vsock_transport->get_local_cid();
	pcb->local_addr.svm_port = vsock_last_source_port++;
	pcb->remote_addr.svm_cid = vsock->svm_cid;
	pcb->remote_addr.svm_port = vsock->svm_port;

	soisconnecting(so);
	vsock_pcb_insert_connected(pcb);

	return vsock_transport->connect(so);
}

int
vsock_disconnect(struct socket *so)
{
	int ret = 0;

	if (so->so_state & SS_ISCONNECTED)
		ret = vsock_transport->disconnect(so);

	if ((so->so_state & SS_ISDISCONNECTED) == 0)
		soisdisconnecting(so);

	return (ret);
}

int vsock_send(struct socket *so, int flags, struct mbuf *m,
		    struct sockaddr *addr, struct mbuf *c, struct thread *td)
{
	int res = 0;

	res = vsock_transport->send(so, m);

	return res;
}

int
vsock_peeraddr(struct socket *so, struct sockaddr *sa)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	memcpy(sa, &pcb->remote_addr, pcb->remote_addr.svm_len);

	return (0);
}

int
vsock_sockaddr(struct socket *so, struct sockaddr *sa)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	memcpy(sa, &pcb->local_addr, pcb->local_addr.svm_len);

	return (0);
}

void
vsock_close(struct socket *so)
{
}

void
vsock_abort(struct socket *so)
{
}

int
vsock_shutdown(struct socket *so, enum shutdown_how how)
{
	return (0);
}

void
vsock_transport_register(struct virtio_transport_ops *transport)
{
	mtx_lock(&vsock_transport_mtx);
	vsock_transport = transport;
	mtx_unlock(&vsock_transport_mtx);
}

void
vsock_transport_deregister(void)
{
	mtx_lock(&vsock_transport_mtx);
	vsock_transport = NULL;
	mtx_unlock(&vsock_transport_mtx);
}
