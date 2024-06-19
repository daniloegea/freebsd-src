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
#include <sys/sx.h>
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

#include <sys/vm_socket.h>
#include <net/vsock_transport.h>
#include <net/vsock_domain.h>

#define so2vsockpcb(so) \
	((struct vsock_pcb *)((so)->so_pcb))
#define vsockpcb2so(vsockpcb) \
	((struct socket *)((vsockpcb)->so))

MALLOC_DEFINE(M_VSOCK, "virtio_socket", "virtio socket control structures");

static struct rwlock 		vsock_pcbs_connected_rwlock;
static LIST_HEAD(, vsock_pcb)	vsock_pcbs_connected;
static struct rwlock 		vsock_pcbs_bound_rwlock;
static LIST_HEAD(, vsock_pcb)	vsock_pcbs_bound;

static struct vsock_transport_ops	*vsock_transport = NULL;
static struct mtx 			vsock_transport_mtx;
static struct sx 			vsock_transport_sx;

static _Atomic(uint32_t) vsock_last_source_port = 123456;

SYSCTL_NODE(_net, OID_AUTO, vsock, CTLFLAG_RD, 0, "Virtio VSOCK");

SDT_PROVIDER_DECLARE(vsock);
SDT_PROBE_DEFINE1(vsock, , ,create, "struct socket *");
SDT_PROBE_DEFINE1(vsock, , ,destroy, "struct socket *");

static int	vsock_dom_probe(void);

static void	vsock_close(struct socket *);
static void	vsock_detach(struct socket *);
static void	vsock_abort(struct socket *);
static int	vsock_attach(struct socket *, int, struct thread *);
static int	vsock_bind(struct socket *, struct sockaddr *, struct thread *);
static int	vsock_listen(struct socket *, int, struct thread *);
static int	vsock_accept(struct socket *, struct sockaddr *);
static int	vsock_connect(struct socket *, struct sockaddr *, struct thread *);
static int	vsock_peeraddr(struct socket *, struct sockaddr *);
static int	vsock_sockaddr(struct socket *, struct sockaddr *);
static int	vsock_receive(struct socket *, struct sockaddr **,
			struct uio *, struct mbuf **, struct mbuf **, int *);
static int	vsock_send(struct socket *so, int flags, struct mbuf *m,
			struct sockaddr *addr, struct mbuf *c, struct thread *td);
static int	vsock_disconnect(struct socket *);
static int	vsock_shutdown(struct socket *, enum shutdown_how);

static void			vsock_pcb_insert_connected(struct vsock_pcb *pcb);
static void			vsock_pcb_remove_connected(struct vsock_pcb *pcb);
static struct vsock_pcb *	vsock_pcb_lookup_connected(uint32_t src_port, uint32_t dst_port);
static void			vsock_pcb_insert_bound(struct vsock_pcb *pcb);
static void			vsock_pcb_remove_bound(struct vsock_pcb *pcb);
static struct vsock_pcb *	vsock_pcb_lookup_bound(uint32_t src_port, uint32_t dst_port);

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
	.pr_soreceive =		vsock_receive,
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

static int
vsock_dom_probe(void)
{
	return (0);
}

void
vsock_transport_lock(void)
{
	sx_xlock(&vsock_transport_sx);
}

void
vsock_transport_unlock(void)
{
	sx_xunlock(&vsock_transport_sx);
}

static void
vsock_init(void *arg __unused)
{
	rw_init(&vsock_pcbs_connected_rwlock, "vsock_pcbs_connected_rwlock");
	LIST_INIT(&vsock_pcbs_connected);
	rw_init(&vsock_pcbs_bound_rwlock, "vsock_pcbs_bound_rwlock");
	LIST_INIT(&vsock_pcbs_bound);
	mtx_init(&vsock_transport_mtx,
		  "vsock_transport_mtx", NULL, MTX_DEF);
	sx_init(&vsock_transport_sx, "vsock_transport_sx");
}

SYSINIT(vsock_init, SI_SUB_PROTO_DOMAIN, SI_ORDER_THIRD, vsock_init, NULL);

static void
vsock_pcb_insert_connected(struct vsock_pcb *pcb)
{
	rw_wlock(&vsock_pcbs_connected_rwlock);
	LIST_INSERT_HEAD(&vsock_pcbs_connected, pcb, next);
	rw_wunlock(&vsock_pcbs_connected_rwlock);
}

static void
vsock_pcb_remove_connected(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	rw_wlock(&vsock_pcbs_connected_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p == pcb) {
		LIST_REMOVE(pcb, next);
	}
	rw_wunlock(&vsock_pcbs_connected_rwlock);
}

static void
vsock_pcb_insert_bound(struct vsock_pcb *pcb)
{
	rw_wlock(&vsock_pcbs_bound_rwlock);
	LIST_INSERT_HEAD(&vsock_pcbs_bound, pcb, next);
	rw_wunlock(&vsock_pcbs_bound_rwlock);
}

static void
vsock_pcb_remove_bound(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	rw_wlock(&vsock_pcbs_bound_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p == pcb) {
		LIST_REMOVE(pcb, next);
	}
	rw_wunlock(&vsock_pcbs_bound_rwlock);
}

static struct vsock_pcb *
vsock_pcb_lookup_connected(uint32_t local_port, uint32_t remote_port)
{
	struct vsock_pcb *p = NULL;

	rw_rlock(&vsock_pcbs_connected_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p->so &&
		local_port == p->local.port &&
		remote_port == p->remote.port) {
		rw_runlock(&vsock_pcbs_connected_rwlock);
		return p;
	}
	rw_runlock(&vsock_pcbs_connected_rwlock);
	return p;
}

static struct vsock_pcb *
vsock_pcb_lookup_bound(uint32_t local_port, uint32_t remote_port)
{
	struct vsock_pcb *p = NULL;

	rw_rlock(&vsock_pcbs_bound_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p->so &&
		local_port == p->local.port &&
	remote_port == 0) {
		rw_runlock(&vsock_pcbs_bound_rwlock);
		return p;
	}
	rw_runlock(&vsock_pcbs_bound_rwlock);

	return p;
}

static int
vsock_attach(struct socket *so, int proto, struct thread *td)
{
	struct vsock_pcb *pcb;
	int error;

	SDT_PROBE1(vsock, , , create, so);

	mtx_lock(&vsock_transport_mtx);

	if (vsock_transport == NULL) {
		mtx_unlock(&vsock_transport_mtx);
		return (ENXIO);
	}
	pcb = malloc(sizeof(struct vsock_pcb), M_VSOCK, M_NOWAIT | M_ZERO);
	if (pcb == NULL) {
		return (ENOMEM);
	}

	pcb->ops = vsock_transport;

	pcb->so = so;
	so->so_pcb = pcb;
	error = soreserve(so, VSOCK_SND_BUFFER_SIZE, VSOCK_RCV_BUFFER_SIZE);

	pcb->fwd_cnt = 0;
	pcb->last_buf_alloc = VSOCK_RCV_BUFFER_SIZE;

	pcb->ops->attach_socket();

	mtx_unlock(&vsock_transport_mtx);

	return (error);
}

static void
vsock_detach(struct socket *so)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	SDT_PROBE1(vsock, , , destroy, so);

	vsock_transport_lock();

	pcb = so2vsockpcb(so);

	if (pcb == NULL) {
		vsock_transport_unlock();
		return;
	}

	vsock_pcb_remove_bound(pcb);
	vsock_pcb_remove_connected(pcb);

	pcb->ops->detach_socket();

	free(pcb, M_VSOCK);
	so->so_pcb = NULL;

	vsock_transport_unlock();
}

static int
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

	pcb->local.port = sa->svm_port;
	pcb->local.cid = pcb->ops->get_local_cid();

	vsock_pcb_insert_bound(pcb);

	return (0);
}

static int
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

static int
vsock_accept(struct socket *so, struct sockaddr *sa)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct sockaddr_vm sockaddr;

	sockaddr.svm_len = sizeof(struct sockaddr_vm);
	sockaddr.svm_family = AF_VSOCK;
	sockaddr.svm_port = pcb->remote.port;
	sockaddr.svm_cid = pcb->remote.cid;

	memcpy(sa, &sockaddr, sockaddr.svm_len);

	return (0);
}

static int
vsock_connect(struct socket *so, struct sockaddr *nam, struct thread *td)
{
	struct sockaddr_vm *vsock = (struct sockaddr_vm *) nam;
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct vsock_addr src, dst;
	long space;
	uint32_t buf_alloc;
	int res;

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

	pcb->local.cid = pcb->ops->get_local_cid();
	pcb->local.port = vsock_last_source_port++;
	pcb->remote.cid = vsock->svm_cid;
	pcb->remote.port = vsock->svm_port;

	soisconnecting(so);
	vsock_pcb_insert_connected(pcb);

	src.port = pcb->local.port;
	src.cid = pcb->local.cid;
	dst.port = pcb->remote.port;
	dst.cid = pcb->remote.cid;
	space = sbspace(&so->so_rcv);
	buf_alloc = space >= 0 ? space : 0;
	res = pcb->ops->connect(&src, &dst, buf_alloc);

	if (so->so_state & SS_NBIO) {
		res = EINPROGRESS;
	}

	return res;
}

static int
vsock_disconnect(struct socket *so)
{
	struct vsock_addr src, dst;
	int ret = 0;
	struct vsock_pcb *pcb = so2vsockpcb(so);

	src.port = pcb->local.port;
	src.cid = pcb->local.cid;
	dst.port = pcb->remote.port;
	dst.cid = pcb->remote.cid;
	if (so->so_state & SS_ISCONNECTED)
		ret = pcb->ops->disconnect(&src, &dst);

	if ((so->so_state & SS_ISDISCONNECTED) == 0)
		soisdisconnecting(so);

	return (ret);
}

static int
vsock_send(struct socket *so, int flags, struct mbuf *m,
		    struct sockaddr *addr, struct mbuf *c, struct thread *td)
{
	struct vsock_addr src, dst;
	long space;
	uint32_t buf_alloc;
	int res = 0;
	struct vsock_pcb *pcb = so2vsockpcb(so);

	src.port = pcb->local.port;
	src.cid = pcb->local.cid;
	dst.port = pcb->remote.port;
	dst.cid = pcb->remote.cid;

	SOCKBUF_LOCK(&so->so_rcv);
	space = sbspace(&so->so_rcv);
	buf_alloc = space >= 0 ? space : 0;
	SOCKBUF_UNLOCK(&so->so_rcv);

	if (m->m_len > pcb->peer_credit) {
		pcb->ops->request_credit_update(&src, &src, buf_alloc, pcb->fwd_cnt);
		printf("data > peer_credit, calling sbwait()\n");
		sbwait(so, SO_SND);
	}

	pcb->tx_cnt += m->m_len;
	pcb->peer_credit -= m->m_len;

	res = pcb->ops->send(&src, &dst, buf_alloc, pcb->fwd_cnt, m);

	pcb->last_fwd_cnt = pcb->fwd_cnt;
	pcb->last_buf_alloc = buf_alloc;

	if (res < 0)
		return res;

	return 0;
}

static int
vsock_receive(struct socket *so, struct sockaddr **psa, struct uio *uio,
    struct mbuf **mp0, struct mbuf **controlp, int *flagsp)
{
	int error;
	uint32_t buf_alloc;
	long space;
	struct vsock_pcb *pcb;

	error = soreceive_generic(so, psa, uio, mp0, controlp, flagsp);

	if (error)
		return (error);

	/*
	* TODO: Improve this logic. Needs to take into account the maximum size
	* of each packet.
	* The idea is to send a credit update when the peer's view of our
	* credit is lower than a certain threshold.
	*/
	if (so->so_state & SS_ISCONNECTED) {
		pcb = so2vsockpcb(so);
		if ((pcb->fwd_cnt - pcb->last_fwd_cnt + 8192) >= pcb->last_buf_alloc) {
			SOCKBUF_LOCK(&so->so_rcv);
			space = sbspace(&so->so_rcv);
			buf_alloc = space >= 0 ? space : 0;
			SOCKBUF_UNLOCK(&so->so_rcv);
			pcb->last_fwd_cnt = pcb->fwd_cnt;
			pcb->last_buf_alloc = buf_alloc;
			pcb->ops->send_credit_update(&pcb->local, &pcb->remote, buf_alloc, pcb->fwd_cnt);
		}
	}

	return (error);
}

static int
vsock_peeraddr(struct socket *so, struct sockaddr *sa)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct sockaddr_vm sockaddr;

	sockaddr.svm_len = sizeof(struct sockaddr_vm);
	sockaddr.svm_family = AF_VSOCK;
	sockaddr.svm_port = pcb->remote.port;
	sockaddr.svm_cid = pcb->remote.cid;

	memcpy(sa, &sockaddr, sockaddr.svm_len);

	return (0);
}

static int
vsock_sockaddr(struct socket *so, struct sockaddr *sa)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct sockaddr_vm sockaddr;

	sockaddr.svm_len = sizeof(struct sockaddr_vm);
	sockaddr.svm_family = AF_VSOCK;
	sockaddr.svm_port = pcb->local.port;
	sockaddr.svm_cid = pcb->local.cid;

	memcpy(sa, &sockaddr, sockaddr.svm_len);

	return (0);
}

static void
vsock_close(struct socket *so)
{
	struct vsock_addr src, dst;
	struct vsock_pcb *pcb;

	vsock_transport_lock();

	pcb = so2vsockpcb(so);

	if (pcb == NULL) {
		vsock_transport_unlock();
		return;
	}

	src.port = pcb->local.port;
	src.cid = pcb->local.cid;
	dst.port = pcb->remote.port;
	dst.cid = pcb->remote.cid;
	if (so->so_state & SS_ISCONNECTED) {
		pcb->ops->disconnect(&src, &dst);
	}

	// Need to wait for the confirmation (or timeout) after the call to
	// disconnect

	if (so->so_state &
	    (SS_ISCONNECTED|SS_ISCONNECTING|SS_ISDISCONNECTING))
		soisdisconnected(so);

	if (SOLISTENING(so)) {
		vsock_pcb_remove_bound(pcb);
	} else {
		vsock_pcb_remove_connected(pcb);
	}

	vsock_transport_unlock();
}

static void
vsock_abort(struct socket *so)
{
}

static int
vsock_shutdown(struct socket *so, enum shutdown_how how)
{
	int error = 0;

	SOCK_LOCK(so);
	if (!(so->so_state & SS_ISCONNECTED)) {
		SOCK_UNLOCK(so);
		error = ENOTCONN;
	}
	SOCK_UNLOCK(so);

	switch (how) {
	case SHUT_RD:
		socantrcvmore(so);
		break;
	case SHUT_RDWR:
		// need to send a RST to the peer?
		socantrcvmore(so);
	case SHUT_WR:
		socantsendmore(so);
		break;
	}

	return (error);
}

void
vsock_transport_register(struct vsock_transport_ops *transport)
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

int
vsock_input(struct vsock_addr *src, struct vsock_addr *dst, enum vsock_ops op,
		uint32_t buf_alloc, uint32_t fwd_cnt, struct mbuf *m)
{
	struct vsock_pcb *pcb;
	struct socket *so;
	long space;

	vsock_transport_lock();

	if (op == VSOCK_RESPONSE) {
		pcb = vsock_pcb_lookup_connected(dst->port, src->port);
		if (!pcb) {
			vsock_transport->send_rst(dst, src);
			vsock_transport_unlock();
			return -1;
		}
		so = vsockpcb2so(pcb);
		if (so->so_state & SS_ISCONNECTING) {
			soisconnected(so);
			pcb->peer_credit = buf_alloc;
		}
	} else if (op == VSOCK_RESET) {
		pcb = vsock_pcb_lookup_connected(dst->port, src->port);
		if (!pcb) {
			vsock_transport_unlock();
			return -1;
		}
		so = vsockpcb2so(pcb);
		if (so->so_state & SS_ISDISCONNECTING) {
			soisdisconnected(so);
		} else if (so->so_state & SS_ISCONNECTING) {
			so->so_error = ECONNREFUSED;
			soisdisconnected(so);
		}

	} else if (op == VSOCK_SHUTDOWN) {
		pcb = vsock_pcb_lookup_connected(dst->port, src->port);
		if (!pcb) {
			vsock_transport_unlock();
			return -1;
		}
		so = vsockpcb2so(pcb);
		socantsendmore(so);
		socantrcvmore(so);
		if ((so->so_state & SS_ISDISCONNECTED) == 0) {
			soisdisconnecting(so);
			pcb->ops->send_rst(dst, src);
			soisdisconnected(so);
		}
	} else if (op == VSOCK_REQUEST) {
		struct socket *new_socket;
		struct vsock_pcb *new_pcb;
		uint32_t local_buf_alloc;

		pcb = vsock_pcb_lookup_bound(dst->port, 0);
		if (!pcb || !SOLISTENING(pcb->so)) {
			vsock_transport_unlock();
			vsock_transport->send_rst(dst, src);
			return -1;
		}
		so = vsockpcb2so(pcb);
		CURVNET_SET(so->so_vnet);
		new_socket = sonewconn(so, 0);
		CURVNET_RESTORE();

		new_pcb = new_socket->so_pcb;
		new_pcb->local.port = pcb->local.port;
		new_pcb->local.cid = pcb->local.cid;
		new_pcb->remote.port = src->port;
		new_pcb->remote.cid = src->cid;
		new_pcb->fwd_cnt = 0;
		new_pcb->last_fwd_cnt = 0;
		new_pcb->tx_cnt = 0;
		new_pcb->peer_credit = buf_alloc;
		new_pcb->ops = pcb->ops;

		space = new_socket->so_rcv.sb_hiwat;
		local_buf_alloc = space >= 0 ? space : 0;
		pcb->ops->request_ack(dst, src, local_buf_alloc);

		vsock_pcb_insert_connected(new_pcb);
		soisconnected(new_socket);
	} else if (op == VSOCK_CREDIT_REQUEST) {
		uint32_t buf_alloc;
		pcb = vsock_pcb_lookup_connected(dst->port, src->port);
		if (!pcb) {
			vsock_transport->send_rst(dst, src);
			vsock_transport_unlock();
			return -1;
		}
		so = vsockpcb2so(pcb);
		if ((so->so_state & SS_ISCONNECTED) == 0) {
			pcb->ops->send_rst(dst, src);
			vsock_transport_unlock();
			return -1;
		}

		SOCKBUF_LOCK(&so->so_rcv);
		space = sbspace(&so->so_rcv);
		buf_alloc = space >= 0 ? space : 0;
		SOCKBUF_UNLOCK(&so->so_rcv);
		pcb->ops->send_credit_update(dst, src, buf_alloc, pcb->fwd_cnt);

	} else if (op == VSOCK_DATA) {
		pcb = vsock_pcb_lookup_connected(dst->port, src->port);
		if (!pcb) {
			vsock_transport->send_rst(dst, src);
			vsock_transport_unlock();
			return -1;
		}
		so = vsockpcb2so(pcb);
		if ((so->so_state & SS_ISCONNECTED) == 0) {
			pcb->ops->send_rst(dst, src);
			vsock_transport_unlock();
			return -1;
		}

		SOCKBUF_LOCK(&so->so_rcv);
		pcb->fwd_cnt += m->m_len;
		sbappendstream_locked(&so->so_rcv, m, 0);
		pcb->peer_credit = buf_alloc - (pcb->tx_cnt - fwd_cnt);
		sorwakeup_locked(so);
	} else if (op == VSOCK_CREDIT_UPDATE) {

		pcb = vsock_pcb_lookup_connected(dst->port, src->port);
		if (!pcb) {
			vsock_transport->send_rst(dst, src);
			vsock_transport_unlock();
			return -1;
		}

		pcb->peer_credit = buf_alloc;
		so = vsockpcb2so(pcb);
		sowwakeup(so);
	}

	vsock_transport_unlock();
	return 0;
}
