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
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/sysproto.h>
#include <sys/sockbuf.h>
#include <sys/mbuf.h>

#include <net/vnet.h>

#include <sys/types.h>
#include <sys/uio.h>

#include <sys/sdt.h>

#include <sys/vm_sockets.h>
#include <net/vsock_transport.h>
#include <net/vsock_domain.h>

#define so2vsockpcb(so) \
	((struct vsock_pcb *)((so)->so_pcb))
#define vsockpcb2so(vsockpcb) \
	((struct socket *)((vsockpcb)->so))

MALLOC_DEFINE(M_VSOCK, "vsock", "AF_VSOCK data");

static struct rwlock 			vsock_pcbs_connected_rwlock;
static CK_LIST_HEAD(, vsock_pcb)	vsock_pcbs_connected = CK_LIST_HEAD_INITIALIZER(vsock_pcbs_connected);
static struct rwlock 			vsock_pcbs_bound_rwlock;
static CK_LIST_HEAD(, vsock_pcb)	vsock_pcbs_bound = CK_LIST_HEAD_INITIALIZER(vsock_pcbs_bound);

static struct vsock_transport_ops	*vsock_transport = NULL;
static struct mtx 			vsock_transport_mtx;
static struct sx 			vsock_transport_sx;

static _Atomic(uint32_t) vsock_last_source_port = 123456;

SYSCTL_NODE(_net, OID_AUTO, vsock, CTLFLAG_RD, 0, "AF_VSOCK");

SDT_PROVIDER_DEFINE(vsock);
SDT_PROBE_DEFINE1(vsock, , ,create, "struct socket *");
SDT_PROBE_DEFINE1(vsock, , ,destroy, "struct socket *");

static int	vsock_dom_probe(void);

static void	vsock_close(struct socket *so);
static void	vsock_detach(struct socket *so);
static void	vsock_abort(struct socket *so);
static int	vsock_attach(struct socket *so, int, struct thread *td);
static int	vsock_bind(struct socket *so, struct sockaddr *sa, struct thread *td);
static int	vsock_listen(struct socket *so, int, struct thread *td);
static int	vsock_accept(struct socket *so, struct sockaddr *td);
static int	vsock_connect(struct socket *so, struct sockaddr *sa, struct thread *td);
static int	vsock_peeraddr(struct socket *so, struct sockaddr *sa);
static int	vsock_sockaddr(struct socket *so, struct sockaddr *sa);
static int	vsock_sosend(struct socket *so, struct sockaddr *addr, struct uio *uio,
			struct mbuf *top, struct mbuf *control, int flags, struct thread *td);
int		vsock_receive(struct socket *so, struct sockaddr **psa, struct uio *uio,
			struct mbuf **mp, struct mbuf **controlp, int *flagsp);
static int	vsock_disconnect(struct socket *so);
static int	vsock_shutdown(struct socket *so, enum shutdown_how);
static int	vsock_control(struct socket *so, unsigned long cmd, void *data,
			 struct ifnet *ifp, struct thread *td);
static struct vsock_pcb *	vsock_pcballoc(void);
static void	vsock_pcbfree(struct vsock_pcb *pcb);


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
	.pr_sosend =		vsock_sosend,
	.pr_disconnect =	vsock_disconnect,
	.pr_close =		vsock_close,
	.pr_detach =		vsock_detach,
	.pr_shutdown =		vsock_shutdown,
	.pr_abort =		vsock_abort,
	.pr_control =		vsock_control,
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
	CK_LIST_INIT(&vsock_pcbs_connected);
	rw_init(&vsock_pcbs_bound_rwlock, "vsock_pcbs_bound_rwlock");
	CK_LIST_INIT(&vsock_pcbs_bound);
	mtx_init(&vsock_transport_mtx,
		  "vsock_transport_mtx", NULL, MTX_DEF);
	sx_init(&vsock_transport_sx, "vsock_transport_sx");
}

SYSINIT(vsock_init, SI_SUB_PROTO_DOMAIN, SI_ORDER_THIRD, vsock_init, NULL);

void
vsock_pcb_insert_connected(struct vsock_pcb *pcb)
{
	rw_wlock(&vsock_pcbs_connected_rwlock);
	CK_LIST_INSERT_HEAD(&vsock_pcbs_connected, pcb, next);
	rw_wunlock(&vsock_pcbs_connected_rwlock);
}

void
vsock_pcb_remove_connected(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	rw_wlock(&vsock_pcbs_connected_rwlock);
	CK_LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p == pcb) {
		CK_LIST_REMOVE(pcb, next);
		rw_wunlock(&vsock_pcbs_connected_rwlock);
		return;
	}
	rw_wunlock(&vsock_pcbs_connected_rwlock);
}

void
vsock_pcb_insert_bound(struct vsock_pcb *pcb)
{
	rw_wlock(&vsock_pcbs_bound_rwlock);
	CK_LIST_INSERT_HEAD(&vsock_pcbs_bound, pcb, next);
	rw_wunlock(&vsock_pcbs_bound_rwlock);
}

void
vsock_pcb_remove_bound(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	rw_wlock(&vsock_pcbs_bound_rwlock);
	CK_LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p == pcb) {
		CK_LIST_REMOVE(pcb, next);
		rw_wunlock(&vsock_pcbs_bound_rwlock);
		return;
	}
	rw_wunlock(&vsock_pcbs_bound_rwlock);
}

struct vsock_pcb *
vsock_pcb_lookup_connected(struct vsock_addr *local_addr, struct vsock_addr *remote_addr)
{
	struct vsock_pcb *pcb = NULL;

	CK_LIST_FOREACH(pcb, &vsock_pcbs_connected, next)
	if (pcb->so &&
		local_addr->port == pcb->local.port &&
		remote_addr->port == pcb->remote.port &&
		local_addr->cid == pcb->local.cid &&
		remote_addr->cid == pcb->remote.cid) {
		return (pcb);
	}
	return (pcb);
}

struct vsock_pcb *
vsock_pcb_lookup_bound(struct vsock_addr *addr)
{
	struct vsock_pcb *pcb = NULL;

	CK_LIST_FOREACH(pcb, &vsock_pcbs_bound, next)
	if (pcb->so &&
		addr->port == pcb->local.port &&
		(addr->cid == pcb->local.cid || pcb->local.cid == VMADDR_CID_ANY)) {
		return (pcb);
	}

	return (pcb);
}

static int
vsock_attach(struct socket *so, int proto, struct thread *td)
{
	struct vsock_pcb *pcb;
	int error;

	SDT_PROBE1(vsock, , , create, so);

	/* sonewconn() is called from the transport code when a new
	 * connection is received. The transport will be inside a NET_EPOCH
	 * while a new packet is being processed (so it can't sleep) and
	 * sonewconn() calls pr_attach() with td == NULL. We use this trick
	 * here to differentiate when pr_attach() is called from user space
	 * and from the transport.
	 *
	 * The transport lock is acquired here so we can check reliably if
	 * there are active sockets when the transport module is unloaded.
	 * We don't want to unload the transport while there are sockets in use by
	 * the vsock layer. We also don't want to create a new a new socket after the
	 * transport being unloaded.
	 */
	if (td != NULL) {
		vsock_transport_lock();
	}

	if (vsock_transport == NULL) {
		error = ENXIO;
		goto out;
	}
	pcb = vsock_pcballoc();
	if (pcb == NULL) {
		error = ENOMEM;
		goto out;
	}

	pcb->ops = vsock_transport;

	pcb->so = so;
	so->so_pcb = pcb;
	error = soreserve(so, 0, VSOCK_RCV_BUFFER_SIZE);

	if (error != 0) {
		goto out;
	}

	pcb->local.cid = VMADDR_CID_ANY;
	pcb->local.port = VMADDR_PORT_ANY;

	error = pcb->ops->attach_socket(pcb);

out:
	if (td != NULL) {
		vsock_transport_unlock();
	}
	return (error);
}

static void
vsock_pcb_destroy_cb(struct epoch_context *ctx)
{
	struct vsock_pcb *pcb;

	pcb = __containerof(ctx, struct vsock_pcb, epoch_ctx);

	vsock_pcbfree(pcb);
}

static void
vsock_detach(struct socket *so)
{
	struct vsock_pcb *pcb;

	SDT_PROBE1(vsock, , , destroy, so);

	pcb = so2vsockpcb(so);

	KASSERT(pcb != NULL, ("vsock_detach: pcb == NULL"));

	vsock_pcb_remove_bound(pcb);
	vsock_pcb_remove_connected(pcb);

	pcb->ops->detach_socket(pcb);

	VSOCK_LOCK(pcb);
	pcb->so = NULL;
	VSOCK_UNLOCK(pcb);

	so->so_pcb = NULL;

	NET_EPOCH_CALL(vsock_pcb_destroy_cb, &pcb->epoch_ctx);
}

static int
vsock_bind(struct socket *so, struct sockaddr *addr, struct thread *td)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct sockaddr_vm *sa = (struct sockaddr_vm *) addr;
	struct vsock_addr sockaddr;

	KASSERT(pcb != NULL, ("vsock_bind: pcb == NULL"));

	if (addr->sa_family != AF_VSOCK) {
		return (EAFNOSUPPORT);
	}

	if (sa->svm_len != sizeof(*sa)) {
		return (EINVAL);
	}

	if (sa->svm_cid == VMADDR_CID_HYPERVISOR ||
		sa->svm_cid == VMADDR_CID_RESERVED ||
		sa->svm_cid == VMADDR_CID_HOST) {
		return (EADDRNOTAVAIL);
	}

	VSOCK_LOCK(pcb);
	pcb->local.port = sa->svm_port;
	pcb->local.cid = sa->svm_cid;

	sockaddr.cid = sa->svm_cid;
	sockaddr.port = sa->svm_port;
	if (vsock_pcb_lookup_bound(&sockaddr) != NULL) {
		VSOCK_UNLOCK(pcb);
		return (EADDRINUSE);
	}

	vsock_pcb_insert_bound(pcb);

	VSOCK_UNLOCK(pcb);

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
	int error;

	KASSERT(pcb != NULL, ("vsock_connect: pcb == NULL"));

	if (nam->sa_family != AF_VSOCK) {
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

	VSOCK_LOCK(pcb);

	pcb->local.cid = pcb->ops->get_local_cid();
	if (pcb->local.port == VMADDR_PORT_ANY)
		pcb->local.port = vsock_last_source_port++;
	pcb->remote.cid = vsock->svm_cid;
	pcb->remote.port = vsock->svm_port;

	VSOCK_UNLOCK(pcb);

	soisconnecting(so);
	vsock_pcb_insert_bound(pcb);

	error = pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, VSOCK_REQUEST, NULL);

	if (so->so_state & SS_NBIO) {
		error = EINPROGRESS;
	}

	return (error);
}

static int
vsock_disconnect(struct socket *so)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	int error = 0;

	KASSERT(pcb != NULL, ("vsock_diconnect: pcb == NULL"));

	soisdisconnecting(so);

	SOCK_LOCK(so);
	if ((so->so_state & SS_ISDISCONNECTED) == 0) {
		error = pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, VSOCK_DISCONNECT, NULL);
		if (error)
			goto out;

		error = msleep(&so->so_timeo, &so->so_lock, PSOCK | PCATCH, "disconnect", hz);

		if (error) {
			error = pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, VSOCK_RESET, NULL);
		}
	}

	vsock_pcb_remove_connected(pcb);

out:
	SOCK_UNLOCK(so);
	return (error);
}

int
vsock_sosend(struct socket *so, struct sockaddr *addr, struct uio *uio,
    struct mbuf *top, struct mbuf *control, int flags, struct thread *td)
{
	int error;
	ssize_t resid;
	uint32_t writable, towrite;
	struct vsock_pcb *pcb = so2vsockpcb(so);

	error = SOCK_IO_SEND_LOCK(so, SBLOCKWAIT(flags));
	if (error)
		return (error);

	resid = uio->uio_resid;

	if (addr != NULL && addr->sa_family != AF_VSOCK) {
		error = EAFNOSUPPORT;
		goto out;
	}

	if (resid < 0) {
		error = EINVAL;
		goto out;
	}

	do {
		SOCK_SENDBUF_LOCK(so);

		if (so->so_snd.sb_state & SBS_CANTSENDMORE) {
			SOCK_SENDBUF_UNLOCK(so);
			error = EPIPE;
			goto out;
		}

		if (so->so_error) {
			error = so->so_error;
			so->so_error = 0;
			SOCK_SENDBUF_UNLOCK(so);
			goto out;
		}

		if ((so->so_state & SS_ISCONNECTED) == 0) {
			if ((so->so_proto->pr_flags & PR_CONNREQUIRED)) {
				SOCK_SENDBUF_UNLOCK(so);
				error = ENOTCONN;
				goto out;
			}
		}

		writable = pcb->ops->check_writable(pcb, FALSE);

		if (writable == 0) {
			// XXX: should call check_writable(pcb, TRUE) from here too?
			if (so->so_state & SS_NBIO) {
				error = EWOULDBLOCK;
				SOCK_SENDBUF_UNLOCK(so);
				goto out;
			} else {
				writable = pcb->ops->check_writable(pcb, TRUE);
				error = sbwait(so, SO_SND);
				SOCK_SENDBUF_UNLOCK(so);
				if (error)
					break;

				continue;
			}
		}

		SOCK_SENDBUF_UNLOCK(so);

		towrite = MIN(writable, VSOCK_MAX_MSG_SIZE);
		towrite = MIN(towrite, resid);
		top = m_uiotombuf(uio, M_WAITOK, towrite, 0, 0);

		if (top == NULL) {
			error = EFAULT;
			goto out;
		}

		resid = uio->uio_resid;

		error =  pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, VSOCK_DATA, top);
		if (error) {
			goto out;
		}
		top = NULL;

	} while (resid);

out:
	if (top != NULL)
		m_freem(top);
	if (control != NULL)
		m_freem(control);

	SOCK_IO_SEND_UNLOCK(so);
	return (error);
}

int
vsock_receive(struct socket *so, struct sockaddr **psa, struct uio *uio,
		struct mbuf **mp, struct mbuf **controlp, int *flagsp)
{
	int error;

	ssize_t resid_orig = uio->uio_resid;
	struct vsock_pcb *pcb = so->so_pcb;

	KASSERT(pcb != NULL, ("vsock_receive: pcb == NULL"));

	error = soreceive_stream(so, psa, uio, mp, controlp, flagsp);

	if (!error) {
		pcb->ops->post_receive(pcb, resid_orig - uio->uio_resid);
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
	struct vsock_pcb *pcb;

	pcb = so2vsockpcb(so);

	KASSERT(pcb != NULL, ("vsock_close: pcb == NULL"));

	if (SOLISTENING(so)) {
		vsock_pcb_remove_bound(pcb);
	}
}

static void
vsock_abort(struct socket *so)
{
	struct vsock_pcb *pcb;

	pcb = so2vsockpcb(so);

	KASSERT(pcb != NULL, ("vsock_abort: pcb == NULL"));

	if (SOLISTENING(so)) {
		vsock_pcb_remove_bound(pcb);
	}

	if (so->so_state & SS_ISCONNECTED) {
		sodisconnect(so);
	}
}

static int
vsock_shutdown(struct socket *so, enum shutdown_how how)
{
	struct vsock_pcb *pcb;
	enum vsock_ops op = VSOCK_SHUTDOWN;

	SOCK_LOCK(so);

	pcb = so2vsockpcb(so);

	KASSERT(pcb != NULL, ("vsock_shutdown: pcb == NULL"));

	if (SOLISTENING(so)) {
		if (how != SHUT_WR) {
			so->so_error = ECONNABORTED;
			solisten_wakeup(so);	/* unlocks so */
		} else
			SOCK_UNLOCK(so);
		return (ENOTCONN);
	} else if ((so->so_state & (SS_ISCONNECTED | SS_ISCONNECTING | SS_ISDISCONNECTING)) == 0) {
		SOCK_UNLOCK(so);
		return (ENOTCONN);
	}

	SOCK_UNLOCK(so);

	switch (how) {
	case SHUT_RD:
		sorflush(so);
		op = VSOCK_SHUTDOWN_RECV;
		break;
	case SHUT_WR:
		socantsendmore(so);
		op = VSOCK_SHUTDOWN_SEND;
		break;
	case SHUT_RDWR:
		socantsendmore(so);
		sorflush(so);
	}

	return (pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, op, NULL));
}

static int
vsock_control(struct socket *so, u_long cmd, void *data, struct ifnet *ifp,
	      struct thread *td)
{
	uint32_t *cid = data;
	struct vsock_pcb *pcb = so2vsockpcb(so);
	int error = 0;

	VSOCK_LOCK(pcb);

	if (pcb->ops == NULL) {
		error = EINVAL;
		goto out;
	}

	switch (cmd) {
	case IOCTL_VM_SOCKETS_GET_LOCAL_CID:
		*cid = pcb->ops->get_local_cid();
		break;
	default:
		error = EINVAL;
		goto out;
	}

out:
	VSOCK_UNLOCK(pcb);
	return (error);
}

static struct vsock_pcb *
vsock_pcballoc(void)
{
	struct vsock_pcb *pcb;

	pcb = malloc(sizeof(struct vsock_pcb), M_VSOCK, M_NOWAIT | M_ZERO);
	if (pcb == NULL) {
		return (NULL);
	}

	mtx_init(&pcb->mtx, "vsock PCB lock", NULL, MTX_DEF);

	return (pcb);
}

static void
vsock_pcbfree(struct vsock_pcb *pcb)
{
	mtx_destroy(&pcb->mtx);
	free(pcb, M_VSOCK);
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

/*
* TODO
* - Improve port allocation
* - Store the PCBs in a hash table instead of a list
* - Security
*   - Jails integration
*   - User credential validation
*   - Deny ports < 1024 to non-root
* - Loopback?
* - VNET?
*/
