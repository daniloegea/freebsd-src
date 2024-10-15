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

/* Driver for VirtIO socket devices. */

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
#include <sys/condvar.h>
#include <sys/sglist.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/queue.h>
#include <sys/sdt.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/mbuf.h>
#include <sys/types.h>
#include <sys/refcount.h>
#include <sys/buf_ring.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>

#include <net/vnet.h>
#include <net/vsock_domain.h>
#include <net/vsock_transport.h>

#include <dev/virtio/virtio.h>
#include <dev/virtio/virtqueue.h>
#include <dev/virtio/socket/virtio_socket.h>

#include "virtio_if.h"

struct vtsock_txq {
	struct mtx		vtstx_mtx;
	struct cv		vtstx_cv;
	struct vtsock_softc	*vtstx_sc;
	struct buf_ring		*vtstx_br;
	struct virtqueue	*vtstx_vq;
	struct sglist		*vtstx_sg;
	struct taskqueue	*vtsock_txq;
	struct task		vtsock_intrtask;
};

struct vtsock_rxq {
	struct vtsock_softc	*vtsrx_sc;
	struct virtqueue	*vtsrx_vq;
	struct sglist		*vtsrx_sg;
	struct taskqueue	*vtsock_rxq;
	struct task		vtsock_intrtask;
};

struct vtsock_eventq {
	struct vtsock_softc	*vtsevent_sc;
	struct virtqueue	*vtsevent_vq;
	struct sglist		*vtsevent_sg;
};

struct vtsock_softc {
	device_t			vtsock_dev;
	uint64_t			vtsock_features;
	struct mtx			vtsock_mtx;
	struct virtio_vtsock_config	vtsock_config;
	struct vtsock_txq		vtsock_txq;
	struct vtsock_rxq		vtsock_rxq;
	struct vtsock_eventq		vtsock_eventq;
};

#define VTSOCK_LOCK(_sc)	mtx_lock(&(_sc)->vtsock_mtx)
#define VTSOCK_UNLOCK(_sc)	mtx_unlock(&(_sc)->vtsock_mtx)
static struct vtsock_softc	*vtsock_softc = NULL;
volatile static u_int		active_sockets = 0;

MALLOC_DEFINE(M_VTSOCK, "virtio_socket", "virtio socket control structures");

SDT_PROVIDER_DEFINE(vtsock);
SDT_PROBE_DEFINE1(vtsock, , , receive, "struct virtio_vsock_hdr *");
SDT_PROBE_DEFINE1(vtsock, , , send, "struct virtio_vsock_hdr *");

static int	vtsock_probe(device_t);
static int	vtsock_attach(device_t);
static int	vtsock_detach(device_t);
static int	vtsock_config_change(device_t);
static void	vtsock_read_config(struct vtsock_softc *, struct virtio_vtsock_config *);

static int	vtsock_alloc_virtqueues(struct vtsock_softc *);
static int	vtsock_setup_features(struct vtsock_softc *);
static void	vtsock_setup_sysctl(struct vtsock_softc *sc);
static int	vtsock_populate_rxvq(struct vtsock_rxq *rxq);
static int	vtsock_populate_eventvq(struct vtsock_eventq *eventq);
static int	vtsock_enqueue_rxvq_mbuf(struct vtsock_rxq *rxq, struct mbuf *m);
static int	vtsock_setup_taskqueues(struct vtsock_softc *);

static void	vtsock_event_intr(void *);
static void	vtsock_rx_intr(void *);
static void	vtsock_tx_intr(void *);
static void	vtsock_rx_task(void *xtxq, int pending __unused);
static void	vtsock_tx_task(void *xtxq, int pending __unused);

static void	vtsock_input(struct mbuf *m);
static int	vtsock_send(void *transport, struct vsock_addr *src, struct vsock_addr *dst, enum vsock_ops op, struct mbuf *m);
static int	vtsock_send_control(struct vsock_addr *src, struct vsock_addr *dst, int op, struct virtio_socket_data *private);
static int	vtsock_output(struct mbuf *m);
static void	vtsock_post_receive(struct vsock_pcb *);
static uint32_t	vtsock_check_writable(struct vsock_pcb *, bool);
static void	vtsock_attach_socket(struct vsock_pcb *);
static void	vtsock_detach_socket(struct vsock_pcb *);
static void	vtsock_setup_header(struct virtio_vtsock_hdr *hdr, struct vsock_addr *src,
			struct vsock_addr *dst, uint16_t op, uint16_t type, uint32_t flags,
			uint32_t buf_alloc, uint32_t fwd_cnt);
static uint32_t	vtsock_get_peer_credit(struct virtio_socket_data *private);
static uint64_t	vtsock_get_local_cid(void);

static struct virtio_feature_desc vtsock_feature_desc[] = {
	{ VIRTIO_VTSOCK_F_STREAM,	"StreamSocket"	},
	//{ VIRTIO_VTSOCK_F_SEQPACKET,	"SeqpacketSocket"	},	// not supported
	{ 0, NULL }
};

static device_method_t vtsock_methods[] = {
	/* Device methods. */
	DEVMETHOD(device_probe,		vtsock_probe),
	DEVMETHOD(device_attach,	vtsock_attach),
	DEVMETHOD(device_detach,	vtsock_detach),

	/* VirtIO methods. */
	DEVMETHOD(virtio_config_change,	vtsock_config_change),

	DEVMETHOD_END
};

static driver_t vtsock_driver = {
	"vtsock",
	vtsock_methods,
	sizeof(struct vtsock_softc)
};

static struct vsock_transport_ops transport = {
	.get_local_cid = vtsock_get_local_cid,
	.send_message = vtsock_send,
	.post_receive = vtsock_post_receive,
	.check_writable = vtsock_check_writable,
	.attach_socket = vtsock_attach_socket,
	.detach_socket = vtsock_detach_socket,
};

VIRTIO_SIMPLE_PNPINFO(virtio_socket, VIRTIO_ID_VSOCK,
    "VirtIO VSOCK Transport Adapter");

static int
vtsock_modevent(module_t mod, int type, void *unused)
{
	int error;

	switch (type) {
	case MOD_LOAD:
		error = 0;
		break;
	case MOD_QUIESCE:
		error = 0;
		break;
	case MOD_UNLOAD:
		error = 0;
		break;
	case MOD_SHUTDOWN:
		error = 0;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

VIRTIO_DRIVER_MODULE(virtio_socket, vtsock_driver, vtsock_modevent, NULL);
MODULE_VERSION(virtio_socket, 1);
MODULE_DEPEND(virtio_socket, virtio, 1, 1, 1);
MODULE_DEPEND(virtio_socket, vsock, 1, 1, 1);

static int
vtsock_probe(device_t dev)
{
	return (VIRTIO_SIMPLE_PROBE(dev, virtio_socket));
}

static int
vtsock_enqueue_rxvq_mbuf(struct vtsock_rxq *rxq, struct mbuf *m)
{
	int error;

	struct sglist *sg = rxq->vtsrx_sg;

	sglist_reset(sg);
	error = sglist_append_mbuf(sg, m);
	if (error != 0) {
		return (error);
	}

	return (virtqueue_enqueue(rxq->vtsrx_vq, m, sg, 0, sg->sg_nseg));
}

static int
vtsock_populate_rxvq(struct vtsock_rxq *rxq)
{
	int nbufs, error;
	struct virtqueue *vq = rxq->vtsrx_vq;
	struct mbuf *m;

	error = 0;

	for (nbufs = 0; !virtqueue_full(vq); nbufs++) {
		m = m_get3(VTSOCK_BUFSZ, M_NOWAIT, MT_DATA, 0);
		if (m == NULL) {
			return (ENOBUFS);
		}
		m->m_len = VTSOCK_BUFSZ;
		error = vtsock_enqueue_rxvq_mbuf(rxq, m);
		if (error) {
			return (error);
		}
	}

	if (nbufs > 0) {
		virtqueue_notify(vq);
	}

	return (error);
}

static int
vtsock_populate_eventvq(struct vtsock_eventq *eventq)
{
	int error;
	struct virtqueue *vq = eventq->vtsevent_vq;
	struct mbuf *m;

	// Enqueue a single mbuf to the event virtqueue

	m = m_get2(sizeof(struct virtio_vtsock_event), M_NOWAIT, MT_DATA, 0);
	if (m == NULL) {
		return (ENOBUFS);
	}
	m->m_len = sizeof(struct virtio_vtsock_event);

	struct sglist *sg = eventq->vtsevent_sg;
	sglist_reset(sg);
	error = sglist_append_single_mbuf(sg, m);
	if (error != 0) {
		return (error);
	}

	error = virtqueue_enqueue(vq, m, sg, 0, sg->sg_nseg);
	if (error) {
		return (error);
	}

	virtqueue_notify(vq);

	return (error);
}


static int
vtsock_setup_taskqueues(struct vtsock_softc *sc)
{
	int error;
	device_t dev = sc->vtsock_dev;
	struct vtsock_rxq *rxq = &sc->vtsock_rxq;
	struct vtsock_txq *txq = &sc->vtsock_txq;

	TASK_INIT(&rxq->vtsock_intrtask, 0, vtsock_rx_task, rxq);
	TASK_INIT(&txq->vtsock_intrtask, 0, vtsock_tx_task, txq);

	rxq->vtsock_rxq = taskqueue_create("virtio_socket RX", M_NOWAIT, taskqueue_thread_enqueue, &rxq->vtsock_rxq);
	if (rxq->vtsock_rxq == NULL) {
		device_printf(dev, "RX taskqueue_create failed\n");
		error = ENOMEM;
		goto out;
	}

	error = taskqueue_start_threads(&rxq->vtsock_rxq, 1, PI_NET, "%s rxq", device_get_nameunit(dev));
	if (error) {
		device_printf(dev, "failed to start RX taskqueue: %d\n", error);
		goto out;
	}

	txq->vtsock_txq = taskqueue_create("virtio_socket TX", M_NOWAIT, taskqueue_thread_enqueue, &txq->vtsock_txq);
	if (txq->vtsock_txq == NULL) {
		device_printf(dev, "TX taskqueue_create failed\n");
		error = ENOMEM;
		goto out;
	}

	error = taskqueue_start_threads(&txq->vtsock_txq, 1, PI_NET, "%s txq", device_get_nameunit(dev));
	if (error) {
		device_printf(dev, "failed to start TX taskqueue: %d\n", error);
	}

out:
	return (error);
}

static int
vtsock_attach(device_t dev)
{
	struct vtsock_softc *sc;
	int error;

	sc = device_get_softc(dev);
	vtsock_softc = sc;
	sc->vtsock_dev = dev;

	sc->vtsock_txq.vtstx_sc = sc;
	sc->vtsock_rxq.vtsrx_sc = sc;
	sc->vtsock_eventq.vtsevent_sc = sc;

	virtio_set_feature_desc(dev, vtsock_feature_desc);
	error = vtsock_setup_features(sc);
	if (error) {
		device_printf(dev, "cannot setup features\n");
		goto fail;
	}

	mtx_init(&sc->vtsock_mtx, "vtsockmtx", NULL, MTX_DEF);
	mtx_init(&sc->vtsock_txq.vtstx_mtx, "vtsocktxvqmtx", NULL, MTX_DEF);
	cv_init(&sc->vtsock_txq.vtstx_cv, "Conditional variable for TX queue");

	vtsock_read_config(sc, &sc->vtsock_config);

	vtsock_setup_sysctl(sc);

	error = vtsock_alloc_virtqueues(sc);
	if (error) {
		device_printf(dev, "cannot allocate virtqueues\n");
		goto fail;
	}

	sc->vtsock_txq.vtstx_sg = sglist_alloc(VSOCK_MAX_MSG_SIZE / PAGE_SIZE + 1, M_NOWAIT);
	if (sc->vtsock_txq.vtstx_sg == NULL) {
		error = ENOMEM;
		goto fail;
	}

	sc->vtsock_rxq.vtsrx_sg = sglist_alloc(VTSOCK_BUFSZ / PAGE_SIZE + 1, M_NOWAIT);
	if (sc->vtsock_rxq.vtsrx_sg == NULL) {
		error = ENOMEM;
		goto fail;
	}

	sc->vtsock_eventq.vtsevent_sg = sglist_alloc(2, M_NOWAIT);
	if (sc->vtsock_eventq.vtsevent_sg == NULL) {
		error = ENOMEM;
		goto fail;
	}

	sc->vtsock_txq.vtstx_br = buf_ring_alloc(VTSOCK_TX_RINGBUFFER_SIZE, M_DEVBUF, M_NOWAIT, &sc->vtsock_txq.vtstx_mtx);
	if (sc->vtsock_txq.vtstx_br == NULL) {
		error = ENOMEM;
		goto fail;
	}

	error = vtsock_populate_rxvq(&sc->vtsock_rxq);
	if (error) {
		device_printf(dev, "cannot populate RX virtqueue\n");
		goto fail;
	}

	error = vtsock_populate_eventvq(&sc->vtsock_eventq);
	if (error) {
		device_printf(dev, "cannot populate event virtqueue\n");
		goto fail;
	}

	error = virtio_setup_intr(dev, INTR_TYPE_NET);
	if (error) {
		device_printf(dev, "cannot setup interruptions\n");
		goto fail;
	}

	error = vtsock_setup_taskqueues(sc);
	if (error) {
		goto fail;
	}

	error = virtqueue_enable_intr(sc->vtsock_rxq.vtsrx_vq);
	if (error) {
		device_printf(dev, "cannot enable interruptions on the RX virtqueue\n");
		goto fail;
	}

	error = virtqueue_enable_intr(sc->vtsock_txq.vtstx_vq);
	if (error) {
		device_printf(dev, "cannot enable interruptions on the TX virtqueue\n");
		goto fail;
	}

	error = virtqueue_enable_intr(sc->vtsock_eventq.vtsevent_vq);
	if (error) {
		device_printf(dev, "cannot enable interruptions on the event virtqueue\n");
		goto fail;
	}

	refcount_init(&active_sockets, 0);
	vsock_transport_register(&transport);

fail:
	if (error)
		vtsock_detach(dev);

	return (error);
}

static int
vtsock_detach(device_t dev)
{
	struct vtsock_softc *sc;
	struct mbuf *m;
	struct vtsock_rxq *rxq;
	struct vtsock_txq *txq;
	struct vtsock_eventq *eventq;
	int last = 0;

	// Do not detach if there are active sockets
	// TODO: there is a race here between the moment we inc/dec the counter and
	// the moment we check it.
	if (refcount_load(&active_sockets) > 0) {
		return EBUSY;
	}

	sc = device_get_softc(dev);

	rxq = &sc->vtsock_rxq;
	txq = &sc->vtsock_txq;
	eventq = &sc->vtsock_eventq;

	if (device_is_attached(dev)) {
		virtqueue_disable_intr(rxq->vtsrx_vq);
		virtqueue_disable_intr(txq->vtstx_vq);
		virtqueue_disable_intr(eventq->vtsevent_vq);
		virtio_stop(sc->vtsock_dev);
	}

	while (rxq->vtsrx_vq && (m = virtqueue_drain(rxq->vtsrx_vq, &last)) != NULL) {
		m_freem(m);
	}

	last = 0;
	while (txq->vtstx_vq && (m = virtqueue_drain(txq->vtstx_vq, &last)) != NULL) {
		m_freem(m);
	}

	last = 0;
	while (eventq->vtsevent_vq && (m = virtqueue_drain(eventq->vtsevent_vq, &last)) != NULL) {
		m_freem(m);
	}

	if (rxq->vtsrx_sg) {
		sglist_free(rxq->vtsrx_sg);
	}

	if (txq->vtstx_sg) {
		sglist_free(txq->vtstx_sg);
	}

	if (eventq->vtsevent_sg) {
		sglist_free(eventq->vtsevent_sg);
	}

	if (rxq->vtsock_rxq) {
		taskqueue_drain_all(rxq->vtsock_rxq);
		taskqueue_free(rxq->vtsock_rxq);
	}

	if (txq->vtsock_txq) {
		taskqueue_drain_all(txq->vtsock_txq);
		taskqueue_free(txq->vtsock_txq);
	}

	if (txq->vtstx_br) {
		while(!buf_ring_empty(txq->vtstx_br)) {
			m = buf_ring_dequeue_sc(txq->vtstx_br);
			m_free(m);
		}
		buf_ring_free(txq->vtstx_br, M_DEVBUF);
	}

	mtx_destroy(&sc->vtsock_mtx);
	mtx_destroy(&txq->vtstx_mtx);
	cv_destroy(&txq->vtstx_cv);

	vsock_transport_deregister();

	return (0);
}

static void
vtsock_read_config(struct vtsock_softc *sc, struct virtio_vtsock_config *sockcfg)
{
	device_t dev;

	dev = sc->vtsock_dev;
	virtio_read_device_config(dev,
		offsetof(struct virtio_vtsock_config, guest_cid),
		&sockcfg->guest_cid, sizeof(sockcfg->guest_cid));
}

static uint64_t
vtsock_get_local_cid(void)
{
	return vtsock_softc->vtsock_config.guest_cid;
}

static int
vtsock_config_change(device_t dev)
{
	return (0);
}

static int
vtsock_alloc_virtqueues(struct vtsock_softc *sc)
{
	device_t dev;
	struct vq_alloc_info *vq_info;
	int error;

	dev = sc->vtsock_dev;

	vq_info = malloc(sizeof(struct vq_alloc_info) * 3, M_TEMP, M_NOWAIT);
	if (vq_info == NULL)
		return (ENOMEM);

	VQ_ALLOC_INFO_INIT(&vq_info[0], 0, vtsock_rx_intr, &sc->vtsock_rxq, &sc->vtsock_rxq.vtsrx_vq,
				"%s RX", device_get_nameunit(dev));

	VQ_ALLOC_INFO_INIT(&vq_info[1], 0, vtsock_tx_intr, &sc->vtsock_txq, &sc->vtsock_txq.vtstx_vq,
				"%s TX", device_get_nameunit(dev));

	VQ_ALLOC_INFO_INIT(&vq_info[2], 0, vtsock_event_intr, &sc->vtsock_eventq, &sc->vtsock_eventq.vtsevent_vq,
				"%s event", device_get_nameunit(dev));

	error = virtio_alloc_virtqueues(dev, 3, vq_info);
	free(vq_info, M_TEMP);

	return (error);
}

static int
vtsock_setup_features(struct vtsock_softc *sc)
{
	device_t dev;
	uint64_t features = VIRTIO_VTSOCK_F_STREAM;

	dev = sc->vtsock_dev;

	sc->vtsock_features = virtio_negotiate_features(dev, features);
	return (virtio_finalize_features(dev));
}

static int
vtsock_send(void *transport, struct vsock_addr *src, struct vsock_addr *dst, enum vsock_ops op, struct mbuf *m)
{
	int error = 0;
	uint32_t len = 0;
	int flags = 0;
	int operation = 0;
	uint32_t buf_alloc, fwd_cnt;
	struct virtio_vtsock_hdr *hdr;
	struct virtio_socket_data *private = transport;
	struct vsock_pcb *pcb = private->so->so_pcb;

	if (op != VSOCK_DATA) {
		m = m_get2(sizeof(struct virtio_vtsock_hdr), M_NOWAIT, MT_DATA, 0);

		if (m == NULL) {
			return (ENOBUFS);
		}

		m->m_len = sizeof(struct virtio_vtsock_hdr);

		hdr = mtod(m, struct virtio_vtsock_hdr *);
		hdr->len = 0;
	}

	fwd_cnt = pcb->fwd_cnt;
	SOCK_RECVBUF_LOCK(private->so);
	buf_alloc = private->so->so_rcv.sb_hiwat;
	SOCK_RECVBUF_UNLOCK(private->so);
	private->last_fwd_cnt = fwd_cnt;
	private->last_buf_alloc = buf_alloc;

	if (op == VSOCK_DATA) {
		KASSERT(m != NULL, ("vtsock_send_message: sending data but mbuf is NULL"));

		len = m_length(m, NULL);

		M_PREPEND(m, sizeof(struct virtio_vtsock_hdr), M_NOWAIT);
		if (m == NULL) {
			return (ENOBUFS);
		}

		operation = VIRTIO_VTSOCK_OP_RW;
		hdr = mtod(m, struct virtio_vtsock_hdr *);
		hdr->len = len;

	} else if(op == VSOCK_REQUEST) {
		operation = VIRTIO_VTSOCK_OP_REQUEST;
	} else if(op == VSOCK_RESPONSE) {
		operation = VIRTIO_VTSOCK_OP_RESPONSE;
	} else if(op == VSOCK_RESET) {
		operation = VIRTIO_VTSOCK_OP_RST;
	} else if (op == VSOCK_DISCONNECT || op == VSOCK_SHUTDOWN) {
		flags = VIRTIO_VTSOCK_SHUTDOWN_F_RECEIVE | VIRTIO_VTSOCK_SHUTDOWN_F_SEND;
		operation = VIRTIO_VTSOCK_OP_SHUTDOWN;
	} else if(op == VSOCK_SHUTDOWN_SEND) {
		flags = VIRTIO_VTSOCK_SHUTDOWN_F_SEND;
		operation = VIRTIO_VTSOCK_OP_SHUTDOWN;
	} else if(op == VSOCK_SHUTDOWN_RECV) {
		flags = VIRTIO_VTSOCK_SHUTDOWN_F_RECEIVE;
		operation = VIRTIO_VTSOCK_OP_SHUTDOWN;
	} else if (op == VSOCK_CREDIT_UPDATE) {
		operation = VIRTIO_VTSOCK_OP_CREDIT_UPDATE;
	} else if (op == VSOCK_CREDIT_REQUEST) {
		operation = VIRTIO_VTSOCK_OP_CREDIT_REQUEST;
	}

	vtsock_setup_header(hdr, src, dst, operation, VIRTIO_VTSOCK_TYPE_STREAM, flags, buf_alloc, fwd_cnt);

	error = vtsock_output(m);

	if (error) {
		return (error);
	}

	private->tx_cnt += len;

	return (error);
}

static int
vtsock_output(struct mbuf *m)
{
	struct vtsock_txq *txq = &vtsock_softc->vtsock_txq;
	int error;

	SDT_PROBE1(vtsock, , , send, mtod(m, struct virtio_vtsock_hdr *));

	error = buf_ring_enqueue(txq->vtstx_br, m);

	if (error) {
		printf("buf_ring_enqueue failed (current lenght: %d): %d\n", buf_ring_count(txq->vtstx_br), error);
	}

	error = taskqueue_enqueue(txq->vtsock_txq, &txq->vtsock_intrtask);

	return (error);
}

static int
vtsock_send_control(struct vsock_addr *src, struct vsock_addr *dst, int op, struct virtio_socket_data *private)
{
	uint32_t fwd_cnt = 0;
	uint32_t buf_alloc = 0;
	struct mbuf *m;
	struct virtio_vtsock_hdr *hdr;
	struct vsock_pcb *pcb = NULL;

	if (private != NULL) {
		pcb = private->so->so_pcb;
		fwd_cnt = pcb->fwd_cnt;
		SOCK_RECVBUF_LOCK(private->so);
		buf_alloc = private->so->so_rcv.sb_hiwat;
		SOCK_RECVBUF_UNLOCK(private->so);
	}

	m = m_get2(sizeof(struct virtio_vtsock_hdr), M_NOWAIT, MT_DATA, 0);

	if (m == NULL) {
		return (ENOBUFS);
	}

	m->m_len = sizeof(struct virtio_vtsock_hdr);

	hdr = mtod(m, struct virtio_vtsock_hdr *);
	hdr->len = 0;

	vtsock_setup_header(hdr, src, dst, op, VIRTIO_VTSOCK_TYPE_STREAM, 0, buf_alloc, fwd_cnt);

	return (vtsock_output(m));
}

static void
vtsock_rx_intr(void *ctx) {
	struct vtsock_rxq *rxq = ctx;
	int error;

	// TODO: should I use a ring buffer here too?
	error = taskqueue_enqueue(rxq->vtsock_rxq, &rxq->vtsock_intrtask);
	if (error) {
		printf("taskqueue_enqueue failed %d\n", error);
	}
}

static void
vtsock_tx_intr(void *ctx) {
	struct vtsock_txq *txq = ctx;
	uint32_t len;
	struct mbuf *m;

	mtx_lock(&txq->vtstx_mtx);

again:

	while ((m = virtqueue_dequeue(txq->vtstx_vq, &len)) != NULL) {
		m_freem(m);
	}

	if (virtqueue_postpone_intr(txq->vtstx_vq, VQ_POSTPONE_LONG) != 0) {
                goto again;
        }

	cv_signal(&txq->vtstx_cv);

	mtx_unlock(&txq->vtstx_mtx);
}

static void
vtsock_event_intr(void *ctx)
{
	// TODO: not implemented yet
	struct vtsock_eventq *q = ctx;
	struct sglist *sg = q->vtsevent_sg;
	struct mbuf *m;
	int len;

again:
	while ((m = virtqueue_dequeue(q->vtsevent_vq, &len)) != NULL) {
		struct virtio_vtsock_event *event = mtod(m, struct virtio_vtsock_event *);
		printf("event code %d\n", event->id);
		sglist_reset(sg);
		sglist_append_mbuf(sg, m);

		virtqueue_enqueue(q->vtsevent_vq, m, sg, 0, sg->sg_nseg);
	}

	if (virtqueue_enable_intr(q->vtsevent_vq) != 0) {
		// There are more buffers ready in the queue
		goto again;
	}
}

static void
vtsock_rx_task(void *ctx, int pending __unused)
{
	struct vtsock_rxq *rxq = ctx;
	struct virtqueue *vq;
	uint32_t len = 0;
	struct mbuf *m;
	int deq;
	int error;

	vq = rxq->vtsrx_vq;

again:

	deq = 0;

	while ((m = virtqueue_dequeue(vq, &len)) != NULL) {
		m->m_len = len;
		vtsock_input(m);
		m = m_get3(VTSOCK_BUFSZ, M_NOWAIT, MT_DATA, 0);
		if (m == NULL) {
			printf("Cannot allocate new mbuf\n");
			break;
		}
		m->m_len = VTSOCK_BUFSZ;
		error = vtsock_enqueue_rxvq_mbuf(rxq, m);
		if (error) {
			printf("virtqueue is out of space: %d\n", error);
		}
		deq++;
	}

	if (deq > 0) {
		virtqueue_notify(vq);
	}

	if (virtqueue_enable_intr(vq) != 0) {
		// There are more buffers ready in the queue
                goto again;
        }
}

static void
vtsock_tx_task(void *ctx, int pending __unused)
{
	struct vtsock_txq *txq = ctx;
	struct virtqueue *vq = txq->vtstx_vq;
	struct sglist *sg = txq->vtstx_sg;
	int error;
	struct mbuf *m;

	mtx_lock(&txq->vtstx_mtx);
	while((m = buf_ring_dequeue_sc(txq->vtstx_br)) != NULL) {
		sglist_reset(sg);
		error = sglist_append_mbuf(sg, m);
		if (error) {
			printf("sglist_append_mbuf failed: %d\n", error);
		}

	again:
		error = virtqueue_enqueue(vq, m, sg, sg->sg_nseg, 0);
		if (error == 0) {
			virtqueue_notify(vq);
		} else if (error == ENOSPC || error == EMSGSIZE) {
			cv_wait(&txq->vtstx_cv, &txq->vtstx_mtx);
			goto again;
		} else {
			printf("virtqueue_enqueue error: %d, mbuf->m_len: %d\n", error, m->m_len);
		}

	}
	mtx_unlock(&txq->vtstx_mtx);
}

static void
vtsock_input(struct mbuf *m)
{
	struct vsock_addr remote, local;
	struct virtio_vtsock_hdr *hdr;
	struct virtio_socket_data *private, *newprivate;
	struct vsock_pcb *pcb, *newpcb;
	struct socket *so, *newso;

	m = m_pullup(m, sizeof(struct virtio_vtsock_hdr));
	hdr = mtod(m, struct virtio_vtsock_hdr *);

	remote.cid = hdr->src_cid;
	remote.port = hdr->src_port;
	local.cid = hdr->dst_cid;
	local.port = hdr->dst_port;

	SDT_PROBE1(vtsock, , , receive, hdr);

	vsock_transport_lock();

	if (hdr->op == VIRTIO_VTSOCK_OP_REQUEST || hdr->op == VIRTIO_VTSOCK_OP_RESPONSE) {
		pcb = vsock_pcb_lookup_bound(&local);
	} else {
		pcb = vsock_pcb_lookup_connected(&local, &remote);
	}

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	so = pcb->so;
	private = pcb->transport;
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;

	if (hdr->op == VIRTIO_VTSOCK_OP_REQUEST) {
		if (!SOLISTENING(so)) {
			vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
			goto out;
		}

		/*
		 * TODO: handle the case when a new connection cannot be accepted.
		 * sonewconn() might return NULL.
		*/

		CURVNET_SET(so->so_vnet);
		newso = sonewconn(so, 0);
		CURVNET_RESTORE();

		newpcb = newso->so_pcb;
		newpcb->local.port = pcb->local.port;
		newpcb->local.cid = vtsock_get_local_cid();
		newpcb->remote.port = remote.port;
		newpcb->remote.cid = remote.cid;
		newpcb->ops = pcb->ops;
		newpcb->fwd_cnt = 0;

		newprivate = newpcb->transport;
		newprivate->peer_buf_alloc = private->peer_buf_alloc;
		newprivate->peer_fwd_cnt = private->peer_fwd_cnt;

		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RESPONSE, newprivate);

		soisconnected(newso);
		vsock_pcb_insert_connected(newpcb);
	} else if (hdr->op == VIRTIO_VTSOCK_OP_RESPONSE) {
		if (so->so_state & SS_ISCONNECTING) {
			soisconnected(so);
			vsock_pcb_remove_bound(pcb);
			vsock_pcb_insert_connected(pcb);
		} else {
			vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		}
	} else if (hdr->op == VIRTIO_VTSOCK_OP_RST) {
		if (so->so_state & SS_ISDISCONNECTING) {
			soisdisconnected(so);
		} else if (so->so_state & SS_ISCONNECTING) {
			so->so_error = ECONNREFUSED;
			soisdisconnected(so);
		}
	} else if (hdr->op == VIRTIO_VTSOCK_OP_SHUTDOWN) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, private);
		vsock_pcb_remove_connected(pcb);
		soisdisconnected(so);
		sowwakeup(so);
	} else if (hdr->op == VIRTIO_VTSOCK_OP_CREDIT_UPDATE) {
		// Don't wake the sender thread up if the credit is still zero
		if (vtsock_get_peer_credit(private) > 0) {
			sowwakeup(private->so);
		}
	} else if (hdr->op == VIRTIO_VTSOCK_OP_CREDIT_REQUEST) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_CREDIT_UPDATE, private);
	} else if (hdr->op == VIRTIO_VTSOCK_OP_RW) {
		if (hdr->len <= 0) {
			goto out;
		}
		if (m_length(m, NULL) - sizeof(*hdr) > hdr->len) {
			goto out;
		}

		if ((so->so_state & SS_ISCONNECTED) == 0) {
			vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
			goto out;
		}

		m_adj(m, sizeof(struct virtio_vtsock_hdr));
		SOCK_RECVBUF_LOCK(so);
		sbappendstream_locked(&so->so_rcv, m, 0);
		sorwakeup_locked(so);
		m = NULL;

	} else {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
	}

out:
	if (m) {
		m_freem(m);
	}
	vsock_transport_unlock();
}

static void
vtsock_post_receive(struct vsock_pcb *pcb)
{
	struct virtio_socket_data *private = pcb->transport;

	/*
	* If the peer's view of our credit is below a threshold (VTSOCK_BUFSZ << 2 here)
	* send a credit update proactively.
	*/
	if ((pcb->fwd_cnt - private->last_fwd_cnt + (VTSOCK_BUFSZ << 2) ) >= private->last_buf_alloc) {
		vtsock_send(private, &pcb->local, &pcb->remote, VSOCK_CREDIT_UPDATE, NULL);
	}
}

static uint32_t
vtsock_check_writable(struct vsock_pcb *pcb, bool notify)
{
	struct virtio_socket_data *private = pcb->transport;
	uint32_t credit = vtsock_get_peer_credit(private);

	if (notify) {
		vtsock_send_control(&pcb->local, &pcb->remote, VIRTIO_VTSOCK_OP_CREDIT_REQUEST, private);
	}

	return credit;
}



static void
vtsock_setup_sysctl(struct vtsock_softc *sc)
{
	device_t dev;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *child;

	dev = sc->vtsock_dev;
	ctx = device_get_sysctl_ctx(dev);
	tree = device_get_sysctl_tree(dev);
	child = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "guest_cid",
			CTLFLAG_RD, &sc->vtsock_config.guest_cid, sizeof(uint64_t),
			"Guest context ID");
}

static void
vtsock_setup_header(struct virtio_vtsock_hdr *hdr, struct vsock_addr *src,
		    struct vsock_addr *dst, uint16_t op, uint16_t type, uint32_t flags,
		    uint32_t buf_alloc, uint32_t fwd_cnt)
{
	hdr->src_port = src->port;
	hdr->src_cid = src->cid;
	hdr->dst_port = dst->port;
	hdr->dst_cid = dst->cid;
	hdr->op = op;
	hdr->type = type;
	hdr->flags = flags;
	hdr->buf_alloc = buf_alloc;
	hdr->fwd_cnt = fwd_cnt;
}

static void
vtsock_attach_socket(struct vsock_pcb *pcb)
{
	pcb->transport = malloc(sizeof(struct virtio_socket_data), M_VTSOCK, M_NOWAIT | M_ZERO);
	((struct virtio_socket_data *)pcb->transport)->so = pcb->so;
	((struct virtio_socket_data *)pcb->transport)->last_buf_alloc = VSOCK_RCV_BUFFER_SIZE;
	refcount_acquire(&active_sockets);
}

static void
vtsock_detach_socket(struct vsock_pcb *pcb)
{
	free(pcb->transport, M_VTSOCK);
	refcount_release(&active_sockets);
}

static uint32_t
vtsock_get_peer_credit(struct virtio_socket_data *private)
{
	if (private->peer_buf_alloc < (private->tx_cnt - private->peer_fwd_cnt))
		return 0;
	return private->peer_buf_alloc - (private->tx_cnt - private->peer_fwd_cnt);
}
