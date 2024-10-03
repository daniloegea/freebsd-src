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
#include <sys/socketvar.h>
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

static struct rwlock 		vsock_pcbs_connected_rwlock;
static LIST_HEAD(, vsock_pcb)	vsock_pcbs_connected = LIST_HEAD_INITIALIZER(vsock_pcbs_connected);
static struct rwlock 		vsock_pcbs_bound_rwlock;
static LIST_HEAD(, vsock_pcb)	vsock_pcbs_bound = LIST_HEAD_INITIALIZER(vsock_pcbs_bound);

static struct vsock_transport_ops	*vsock_transport = NULL;
static struct mtx 			vsock_transport_mtx;
static struct sx 			vsock_transport_sx;

static _Atomic(uint32_t) vsock_last_source_port = 123456;

SYSCTL_NODE(_net, OID_AUTO, vsock, CTLFLAG_RD, 0, "AF_VSOCK");

SDT_PROVIDER_DEFINE(vsock);
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
static int	vsock_send(struct socket *so, int flags, struct mbuf *m,
			struct sockaddr *addr, struct mbuf *c, struct thread *td);
int		vsock_receive(struct socket *so, struct sockaddr **psa, struct uio *uio,
			struct mbuf **mp, struct mbuf **controlp, int *flagsp);
static int	vsock_disconnect(struct socket *);
static int	vsock_shutdown(struct socket *, enum shutdown_how);


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

void
vsock_pcb_insert_connected(struct vsock_pcb *pcb)
{
	rw_wlock(&vsock_pcbs_connected_rwlock);
	LIST_INSERT_HEAD(&vsock_pcbs_connected, pcb, next);
	rw_wunlock(&vsock_pcbs_connected_rwlock);
}

void
vsock_pcb_remove_connected(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	rw_wlock(&vsock_pcbs_connected_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p == pcb) {
		LIST_REMOVE(pcb, next);
		rw_wunlock(&vsock_pcbs_connected_rwlock);
		return;
	}
	rw_wunlock(&vsock_pcbs_connected_rwlock);
}

void
vsock_pcb_insert_bound(struct vsock_pcb *pcb)
{
	rw_wlock(&vsock_pcbs_bound_rwlock);
	LIST_INSERT_HEAD(&vsock_pcbs_bound, pcb, next);
	rw_wunlock(&vsock_pcbs_bound_rwlock);
}

void
vsock_pcb_remove_bound(struct vsock_pcb *pcb)
{
	struct vsock_pcb *p;

	rw_wlock(&vsock_pcbs_bound_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p == pcb) {
		LIST_REMOVE(pcb, next);
		rw_wunlock(&vsock_pcbs_bound_rwlock);
		return;
	}
	rw_wunlock(&vsock_pcbs_bound_rwlock);
}

struct vsock_pcb *
vsock_pcb_lookup_connected(struct vsock_addr *local_addr, struct vsock_addr *remote_addr)
{
	struct vsock_pcb *p = NULL;

	rw_rlock(&vsock_pcbs_connected_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_connected, next)
	if (p->so &&
		local_addr->port == p->local.port &&
		remote_addr->port == p->remote.port &&
		local_addr->cid == p->local.cid &&
		remote_addr->cid == p->remote.cid) {
		rw_runlock(&vsock_pcbs_connected_rwlock);
		return p;
	}
	rw_runlock(&vsock_pcbs_connected_rwlock);
	return p;
}

struct vsock_pcb *
vsock_pcb_lookup_bound(struct vsock_addr *addr)
{
	struct vsock_pcb *p = NULL;

	rw_rlock(&vsock_pcbs_bound_rwlock);
	LIST_FOREACH(p, &vsock_pcbs_bound, next)
	if (p->so &&
		addr->port == p->local.port &&
		(addr->cid == p->local.cid || p->local.cid == VMADDR_CID_ANY)) {
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

	if (vsock_transport == NULL) {
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

	pcb->ops->attach_socket(pcb);

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

	pcb->ops->detach_socket(pcb);

	free(pcb, M_VSOCK);
	so->so_pcb = NULL;

	vsock_transport_unlock();
}

static int
vsock_bind(struct socket *so, struct sockaddr *addr, struct thread *td)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	struct sockaddr_vm *sa = (struct sockaddr_vm *) addr;
	struct vsock_addr sockaddr;

	KASSERT(pcb != NULL, ("vsock_bind: pcb == NULL"));

	if (sa->svm_len != sizeof(*sa)) {
		return (EINVAL);
	}

	if (sa->svm_cid == VMADDR_CID_HYPERVISOR ||
		sa->svm_cid == VMADDR_CID_RESERVED ||
		sa->svm_cid == VMADDR_CID_HOST) {
		return (EADDRNOTAVAIL);
	}

	pcb->local.port = sa->svm_port;
	pcb->local.cid = sa->svm_cid;

	sockaddr.cid = sa->svm_cid;
	sockaddr.port = sa->svm_port;
	if (vsock_pcb_lookup_bound(&sockaddr) != NULL) {
		return (EADDRINUSE);
	}

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
	int error;

	KASSERT(pcb != NULL, ("vsock_connect: pcb == NULL"));

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
	if (pcb->local.port == VMADDR_PORT_ANY)
		pcb->local.port = vsock_last_source_port++;
	pcb->remote.cid = vsock->svm_cid;
	pcb->remote.port = vsock->svm_port;

	soisconnecting(so);

	error = pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, VSOCK_REQUEST, NULL);

	vsock_pcb_insert_bound(pcb);

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

static int
vsock_send(struct socket *so, int flags, struct mbuf *m,
		    struct sockaddr *addr, struct mbuf *c, struct thread *td)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);
	int len;
	int error;

	KASSERT(pcb != NULL, ("vsock_send: pcb == NULL"));

	len = m_length(m, NULL);

	if (len == 0)
		return (EINVAL);

	SOCK_SENDBUF_LOCK(so);

	if (so->so_snd.sb_state & SBS_CANTSENDMORE) {
		if (m != NULL) {
			m_freem(m);
		}
		SOCK_SENDBUF_UNLOCK(so);
		return (EPIPE);
	}

	error =  pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, VSOCK_DATA, m);

	SOCK_SENDBUF_UNLOCK(so);

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

	error = soreceive_generic(so, psa, uio, mp, controlp, flagsp);

	/*
	 * fwd_cnt is the number of bytes sent to the application so it needs
	 * to be tracked here.
	 *
	 * TODO: fwd_cnt is current part of the vsock PCB but it's a concept
	 * from virtio_socket so it should probably be in the private data.
	 *
	 * To do that it will be necessary to write a custom vsock_soreceive that calls
	 * the transport to dequeue data from the socket buffer, copy to user space and
	 * records fwd_cnt correctly
	 *
	 * TODO: write a custom soreceive and drop the fwd_cnt from the PCB
	 * OR: create a transport->post_recv() function to record the fwd_cnt and check if
	 * it should send a credit_update message. It would probably be necessary to protect this call
	 * with SOCK_IO_RECV_LOCK() so there will be no races when updating fwd_cnt.
	 */
	if (!error) {
		pcb->fwd_cnt += (resid_orig - uio->uio_resid);
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

	vsock_transport_lock();

	pcb = so2vsockpcb(so);

	KASSERT(pcb != NULL, ("vsock_close: pcb == NULL"));

	if (SOLISTENING(so)) {
		vsock_pcb_remove_bound(pcb);
	}
	vsock_transport_unlock();
}

static void
vsock_abort(struct socket *so)
{
	// TODO: implement abort()
	printf("Abort called for socket: %p\n", so);
}

static int
vsock_shutdown(struct socket *so, enum shutdown_how how)
{
	struct vsock_pcb *pcb;
	int error = 0;
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

	error =  pcb->ops->send_message(pcb->transport, &pcb->local, &pcb->remote, op, NULL);

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

/*
* TODO
* - Implement IOCTL_VM_SOCKETS_GET_LOCAL_CID ioctl
* - Improve port allocation
* - Store the PCBs in a hash table instead of a list
* - Security
*   - Jails integration
*   - User credential validation
*   - Deny ports < 1024 to non-root
*/
