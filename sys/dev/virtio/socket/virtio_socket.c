/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025, Danilo Egea Gondolfo <danilo@FreeBSD.org>
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
#include <sys/types.h>
#include <sys/refcount.h>
#include <sys/buf_ring.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>

#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/mbuf.h>
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
	struct mtx		vtstx_br_mtx;
	struct cv		vtstx_br_cv;
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
#define PRIVATE_LOCK(_data)	mtx_lock(&(_data)->mtx)
#define PRIVATE_UNLOCK(_data)	mtx_unlock(&(_data)->mtx)
static struct vtsock_softc	*vtsock_softc = NULL;
volatile static u_int		active_sockets = 0;

MALLOC_DEFINE(M_VTSOCK, "virtio_socket", "virtio socket control structures");

SDT_PROVIDER_DEFINE(vtsock);
SDT_PROBE_DEFINE1(vtsock, , , receive, "struct virtio_vsock_hdr *");
SDT_PROBE_DEFINE1(vtsock, , , send, "struct virtio_vsock_hdr *");

static int	vtsock_modevent(module_t mod, int type, void *unused);
static int	vtsock_probe(device_t dev);
static int	vtsock_attach(device_t dev);
static int	vtsock_detach(device_t dev);
static int	vtsock_config_change(device_t dev);
static void	vtsock_read_config(struct vtsock_softc *sc, struct virtio_vtsock_config *cfg);

static int	vtsock_alloc_virtqueues(struct vtsock_softc *sc);
static int	vtsock_setup_features(struct vtsock_softc *sc);
static void	vtsock_setup_sysctl(struct vtsock_softc *sc);
static int	vtsock_populate_rxvq(struct vtsock_rxq *rxq);
static int	vtsock_populate_eventvq(struct vtsock_eventq *eventq);
static int	vtsock_enqueue_rxvq_mbuf(struct vtsock_rxq *rxq, struct mbuf *m);
static int	vtsock_setup_taskqueues(struct vtsock_softc *sc);

static void	vtsock_event_intr(void *ctx);
static void	vtsock_rx_intr(void *ctx);
static void	vtsock_tx_intr(void *ctx);
static void	vtsock_rx_task(void *xtxq, int pending __unused);
static void	vtsock_tx_task(void *xtxq, int pending __unused);

static void	vtsock_input(struct mbuf *m);
static int	vtsock_send_internal(void *transport, struct vsock_addr *src, struct vsock_addr *dst, enum vsock_ops op, struct mbuf *m);
static int	vtsock_send(struct vsock_pcb *pcb, enum vsock_ops op);
static int	vtsock_send_control(struct vsock_addr *src, struct vsock_addr *dst, int op, struct virtio_socket_data *private);
static int	vtsock_output(struct mbuf *m);
static void	vtsock_post_receive(struct vsock_pcb *pcb, uint32_t received);
static uint32_t	vtsock_check_writable(struct vsock_pcb *pcb, bool notify);
static int	vtsock_attach_socket(struct vsock_pcb *pcb);
static void	vtsock_detach_socket(struct vsock_pcb *pcb);
static void	vtsock_setup_header(struct virtio_vtsock_hdr *hdr, struct vsock_addr *src,
		    struct vsock_addr *dst, uint16_t op, uint16_t type, uint32_t flags,
		    uint32_t buf_alloc, uint32_t fwd_cnt);
static uint32_t	vtsock_get_peer_credit(struct virtio_socket_data *private);
static uint64_t	vtsock_get_local_cid(void);

#define	VTSOCK_FEATURES	VIRTIO_VTSOCK_F_STREAM

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
	.get_local_cid	= vtsock_get_local_cid,
	.send_message	= vtsock_send,
	.post_receive	= vtsock_post_receive,
	.check_writable	= vtsock_check_writable,
	.attach_socket	= vtsock_attach_socket,
	.detach_socket	= vtsock_detach_socket,
};

VIRTIO_SIMPLE_PNPINFO(virtio_socket, VIRTIO_ID_VSOCK,
    "VirtIO VSOCK Transport Adapter");

VIRTIO_DRIVER_MODULE(virtio_socket, vtsock_driver, vtsock_modevent, NULL);
MODULE_VERSION(virtio_socket, 1);
MODULE_DEPEND(virtio_socket, virtio, 1, 1, 1);
MODULE_DEPEND(virtio_socket, vsock, 1, 1, 1);

static int
vtsock_modevent(module_t mod, int type, void *unused)
{
	int error;

	switch (type) {
	case MOD_LOAD:
	case MOD_QUIESCE:
	case MOD_UNLOAD:
	case MOD_SHUTDOWN:
		error = 0;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static int
vtsock_probe(device_t dev)
{
	return (VIRTIO_SIMPLE_PROBE(dev, virtio_socket));
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
	mtx_init(&sc->vtsock_txq.vtstx_br_mtx, "vtsocktxvqbrmtx", NULL, MTX_DEF);
	cv_init(&sc->vtsock_txq.vtstx_br_cv, "Conditional variable for TX buf ring");

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
	struct vtsock_rxq *rxq;
	struct vtsock_txq *txq;
	struct vtsock_eventq *eventq;
	struct mbuf *m;
	int last;

	vsock_transport_lock();

	// Do not detach if there are active sockets
	if (refcount_load(&active_sockets) > 0) {
		vsock_transport_unlock();
		device_printf(dev, "Cannot unload module with open sockets.");
		return (EBUSY);
	}

	vsock_transport_deregister();
	vsock_transport_unlock();

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

	last = 0;
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

	if (rxq->vtsrx_sg != NULL) {
		sglist_free(rxq->vtsrx_sg);
	}

	if (txq->vtstx_sg != NULL) {
		sglist_free(txq->vtstx_sg);
	}

	if (eventq->vtsevent_sg != NULL) {
		sglist_free(eventq->vtsevent_sg);
	}

	if (rxq->vtsock_rxq != NULL) {
		taskqueue_drain_all(rxq->vtsock_rxq);
		taskqueue_free(rxq->vtsock_rxq);
	}

	if (txq->vtsock_txq != NULL) {
		taskqueue_drain_all(txq->vtsock_txq);
		taskqueue_free(txq->vtsock_txq);
	}

	if (txq->vtstx_br != NULL) {
		while(!buf_ring_empty(txq->vtstx_br)) {
			m = buf_ring_dequeue_sc(txq->vtstx_br);
			m_free(m);
		}
		buf_ring_free(txq->vtstx_br, M_DEVBUF);
	}

	vtsock_softc = NULL;

	mtx_destroy(&sc->vtsock_mtx);
	mtx_destroy(&txq->vtstx_mtx);
	cv_destroy(&txq->vtstx_cv);
	mtx_destroy(&txq->vtstx_br_mtx);
	cv_destroy(&txq->vtstx_br_cv);

	return (0);
}

static void
vtsock_read_config(struct vtsock_softc *sc, struct virtio_vtsock_config *cfg)
{
	device_t dev = sc->vtsock_dev;

	virtio_read_device_config(dev,
	    offsetof(struct virtio_vtsock_config, guest_cid),
	    &cfg->guest_cid, sizeof(cfg->guest_cid));
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
	struct virtqueue *vq = rxq->vtsrx_vq;
	struct mbuf *m;
	int nbufs, error;

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
	struct virtqueue *vq = eventq->vtsevent_vq;
	struct mbuf *m;
	int error;

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
	struct vtsock_rxq *rxq = &sc->vtsock_rxq;
	struct vtsock_txq *txq = &sc->vtsock_txq;
	device_t dev = sc->vtsock_dev;
	int error;

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

static uint64_t
vtsock_get_local_cid(void)
{
	return (vtsock_softc->vtsock_config.guest_cid);
}

static int
vtsock_config_change(device_t dev)
{
	return (0);
}

static int
vtsock_alloc_virtqueues(struct vtsock_softc *sc)
{
	struct vq_alloc_info *vq_info;
	device_t dev = sc->vtsock_dev;
	int error;

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
	device_t dev = sc->vtsock_dev;
	uint64_t features = VTSOCK_FEATURES;

	sc->vtsock_features = virtio_negotiate_features(dev, features);
	return (virtio_finalize_features(dev));
}

static int
vtsock_send_internal(void *transport, struct vsock_addr *src, struct vsock_addr *dst, enum vsock_ops op, struct mbuf *m)
{
	struct virtio_vtsock_hdr *hdr;
	struct virtio_socket_data *private = transport;
	uint32_t buf_alloc, fwd_cnt, len;
	int error;
	int flags = 0;
	int operation = 0;

	if (op != VSOCK_DATA) {
		m = m_get2(sizeof(struct virtio_vtsock_hdr), M_NOWAIT, MT_DATA, 0);

		if (m == NULL) {
			return (ENOBUFS);
		}

		m->m_len = sizeof(struct virtio_vtsock_hdr);

		hdr = mtod(m, struct virtio_vtsock_hdr *);
		hdr->len = 0;

		len = 0;
	}

	PRIVATE_LOCK(private);
	fwd_cnt = private->fwd_cnt;
	buf_alloc = private->buf_alloc;
	private->last_fwd_cnt = fwd_cnt;
	private->last_buf_alloc = buf_alloc;
	PRIVATE_UNLOCK(private);

	switch (op) {
	case VSOCK_DATA:
		KASSERT(m != NULL, ("vtsock_send_message: sending data but mbuf is NULL"));

		len = m_length(m, NULL);

		M_PREPEND(m, sizeof(struct virtio_vtsock_hdr), M_NOWAIT);
		if (m == NULL) {
			return (ENOBUFS);
		}

		operation = VIRTIO_VTSOCK_OP_RW;
		hdr = mtod(m, struct virtio_vtsock_hdr *);
		hdr->len = len;
		break;
	case VSOCK_REQUEST:
		operation = VIRTIO_VTSOCK_OP_REQUEST;
		break;
	case VSOCK_RESPONSE:
		operation = VIRTIO_VTSOCK_OP_RESPONSE;
		break;
	case VSOCK_RESET:
		operation = VIRTIO_VTSOCK_OP_RST;
		break;
	case VSOCK_DISCONNECT:
	case VSOCK_SHUTDOWN:
		flags = VIRTIO_VTSOCK_SHUTDOWN_F_RECEIVE | VIRTIO_VTSOCK_SHUTDOWN_F_SEND;
		operation = VIRTIO_VTSOCK_OP_SHUTDOWN;
		break;
	case VSOCK_SHUTDOWN_SEND:
		flags = VIRTIO_VTSOCK_SHUTDOWN_F_SEND;
		operation = VIRTIO_VTSOCK_OP_SHUTDOWN;
		break;
	case VSOCK_SHUTDOWN_RECV:
		flags = VIRTIO_VTSOCK_SHUTDOWN_F_RECEIVE;
		operation = VIRTIO_VTSOCK_OP_SHUTDOWN;
		break;
	case VSOCK_CREDIT_UPDATE:
		operation = VIRTIO_VTSOCK_OP_CREDIT_UPDATE;
		break;
	case VSOCK_CREDIT_REQUEST:
		operation = VIRTIO_VTSOCK_OP_CREDIT_REQUEST;
		break;
	default:
		return (EINVAL);
	}

	vtsock_setup_header(hdr, src, dst, operation, VIRTIO_VTSOCK_TYPE_STREAM, flags, buf_alloc, fwd_cnt);

	error = vtsock_output(m);

	if (!error && op == VSOCK_DATA) {
		PRIVATE_LOCK(private);
		private->tx_cnt += len;
		PRIVATE_UNLOCK(private);
	}

	return (error);
}

static int
vtsock_send(struct vsock_pcb *pcb, enum vsock_ops op)
{
	struct mbuf *m;
	struct sockbuf *sb;
	volatile uint32_t writable, towrite, sosnd_size;
	int error = 0;

	if (op != VSOCK_DATA)
		return (vtsock_send_internal(pcb->transport, &pcb->local, &pcb->remote, op, NULL));

	do {
		writable = vtsock_check_writable(pcb, FALSE);

		if (writable == 0) {
			writable = vtsock_check_writable(pcb, TRUE);

			SOCKBUF_LOCK(&pcb->so->so_snd);
			sb = sobuf(pcb->so, SO_SND);
			sb->sb_flags |= SB_WAIT;
			/* Wait until we receive a credit update or check again in 100ms */
			error = msleep_sbt(&sb->sb_acc, soeventmtx(pcb->so, SO_SND),
				PSOCK | PCATCH, "sbwait", SBT_1MS * 100, 0, 0);
			SOCKBUF_UNLOCK(&pcb->so->so_snd);

			if (!(pcb->so->so_state & SS_ISCONNECTED))
				goto out;

			if (error == EWOULDBLOCK)
				continue;

			if (error)
				goto out;

			continue;
		}

		SOCKBUF_LOCK(&pcb->so->so_snd);
		sosnd_size = sbavail(&pcb->so->so_snd);
		SOCKBUF_UNLOCK(&pcb->so->so_snd);
		towrite = MIN(writable, VTSOCK_MAX_MSG_SIZE);
		towrite = MIN(towrite, sosnd_size);

		if (towrite > 0) {
			SOCKBUF_LOCK(&pcb->so->so_snd);
			m = m_copym(pcb->so->so_snd.sb_mb, 0, towrite, M_NOWAIT);
			sbdrop_locked(&pcb->so->so_snd, towrite);
			SOCKBUF_UNLOCK(&pcb->so->so_snd);
			error = vtsock_send_internal(pcb->transport, &pcb->local, &pcb->remote, VSOCK_DATA, m);
			if (error)
				return (error);
		}

	} while(towrite > 0);

out:
	return (error);
}

static int
vtsock_output(struct mbuf *m)
{
	struct vtsock_txq *txq = &vtsock_softc->vtsock_txq;
	int error;
	int tries = 5;

	SDT_PROBE1(vtsock, , , send, mtod(m, struct virtio_vtsock_hdr *));

	mtx_lock(&txq->vtstx_br_mtx);
again:
	error = buf_ring_enqueue(txq->vtstx_br, m);

	if (error) {
		if (error == ENOBUFS && tries > 0) {
			taskqueue_enqueue(txq->vtsock_txq, &txq->vtsock_intrtask);
			// Wait for space in the ring buffer
			cv_wait(&txq->vtstx_br_cv, &txq->vtstx_br_mtx);
			tries--;
			goto again;
		}
		printf("buf_ring_enqueue failed (current lenght: %d): %d\n", buf_ring_count(txq->vtstx_br), error);
		mtx_unlock(&txq->vtstx_br_mtx);
		return (error);
	}

	mtx_unlock(&txq->vtstx_br_mtx);
	return (taskqueue_enqueue(txq->vtsock_txq, &txq->vtsock_intrtask));
}

static int
vtsock_send_control(struct vsock_addr *src, struct vsock_addr *dst, int op, struct virtio_socket_data *private)
{
	struct mbuf *m;
	struct virtio_vtsock_hdr *hdr;
	uint32_t fwd_cnt = 0;
	uint32_t buf_alloc = 0;

	if (private != NULL) {
		fwd_cnt = private->fwd_cnt;
		buf_alloc = private->buf_alloc;
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
		// TODO: what do I do if it fails?
	}
}

static void
vtsock_tx_intr(void *ctx) {
	struct vtsock_txq *txq = ctx;
	struct mbuf *m;
	uint32_t len;

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
	/* TODO: not implemented yet
	 * The one event defined is VIRTIO_VSOCK_EVENT_TRANSPORT_RESET
	 * It happens when the communication is interrupted. Usually when
	 * the guest is migrated to another host.
	 * In this case all the connected sockets MUST be disconnected
	 * and the CID MUST be read again.
	 * Listen sockets MUST be preserved but need to have their CID updated
	 * if they are bound to the guest CID
	 */
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
	struct virtqueue *vq = rxq->vtsrx_vq;
	struct mbuf *m;
	uint32_t len = 0;
	int deq;
	int error;

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
	struct mbuf *m;
	struct sglist *sg = txq->vtstx_sg;
	int error;

	mtx_lock(&txq->vtstx_mtx);
	while((m = buf_ring_dequeue_sc(txq->vtstx_br)) != NULL) {
		sglist_reset(sg);
		error = sglist_append_mbuf(sg, m);
		if (error) {
			printf("sglist_append_mbuf failed: %d\n", error);
			// TODO: What do we do if it fails?
		}

	again:
		error = virtqueue_enqueue(vq, m, sg, sg->sg_nseg, 0);
		if (error == 0) {
			virtqueue_notify(vq);
		} else if (error == ENOSPC || error == EMSGSIZE) {
			// Wait for space in the virtqueue
			cv_wait(&txq->vtstx_cv, &txq->vtstx_mtx);
			goto again;
		} else {
			printf("virtqueue_enqueue error: %d, mbuf->m_len: %d\n", error, m->m_len);
			// TODO: what do we do if it fails?
		}
		// signals the sending thread that the is more space in the ring buffer
		cv_signal(&txq->vtstx_br_cv);
	}
	mtx_unlock(&txq->vtstx_mtx);
}

static void
vtsock_populate_addr(struct virtio_vtsock_hdr *hdr, struct vsock_addr *remote, struct vsock_addr *local)
{
	remote->cid = hdr->src_cid;
	remote->port = hdr->src_port;
	local->cid = hdr->dst_cid;
	local->port = hdr->dst_port;
}

static void
vtsock_input_request(struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct vsock_pcb *pcb, *newpcb;
	struct virtio_socket_data *private;
	struct socket *so, *newso;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);

	pcb = vsock_pcb_lookup_bound(&local);

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	so = pcb->so;

	if (!SOLISTENING(so)) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	CURVNET_SET(so->so_vnet);
	newso = sonewconn(so, 0);
	CURVNET_RESTORE();

	if (newso == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	newpcb = newso->so_pcb;
	newpcb->local.port = local.port;
	newpcb->local.cid = vtsock_get_local_cid();
	newpcb->remote.port = remote.port;
	newpcb->remote.cid = remote.cid;

	private = newpcb->transport;
	private->peer_buf_alloc = hdr->buf_alloc;
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->fwd_cnt = 0;

	soisconnected(newso);
	vsock_pcb_insert_connected(newpcb);

	vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RESPONSE, private);

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input_response(struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct vsock_pcb *pcb;
	struct virtio_socket_data *private;
	struct socket *so;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);

	pcb = vsock_pcb_lookup_bound(&local);

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	so = pcb->so;
	private = pcb->transport;
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;

	if (so->so_state & SS_ISCONNECTING) {
		vsock_pcb_remove_bound(pcb);
		vsock_pcb_insert_connected(pcb);
		soisconnected(so);
	} else {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
	}

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input_reset(struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct vsock_pcb *pcb;
	struct virtio_socket_data *private;
	struct socket *so;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);

	pcb = vsock_pcb_lookup_connected(&local, &remote);

	if (pcb == NULL) {
		pcb = vsock_pcb_lookup_bound(&local);
		if (pcb == NULL) {
			return;
		}

		VSOCK_LOCK(pcb);
		if (!SOLISTENING(pcb->so)) {
			pcb->so->so_error = ECONNRESET;
			soisdisconnected(pcb->so);
		}
		VSOCK_UNLOCK(pcb);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	so = pcb->so;
	private = pcb->transport;
	PRIVATE_LOCK(private);
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;
	PRIVATE_UNLOCK(private);

	if (so->so_state & SS_ISDISCONNECTING) {
		vsock_pcb_remove_connected(pcb);
		soisdisconnected(so);
	} else if (so->so_state & SS_ISCONNECTING) {
		so->so_error = ECONNREFUSED;
		soisdisconnected(so);
	}

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input_shutdown(struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct virtio_socket_data *private;
	struct vsock_pcb *pcb;
	struct socket *so;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);


	pcb = vsock_pcb_lookup_connected(&local, &remote);

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	so = pcb->so;
	private = pcb->transport;
	PRIVATE_LOCK(private);
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;
	PRIVATE_UNLOCK(private);

	if (hdr->flags & VIRTIO_VTSOCK_SHUTDOWN_F_RECEIVE) {
		pcb->peer_shutdown |= VSOCK_SHUT_RCV;
	}

	if (hdr->flags & VIRTIO_VTSOCK_SHUTDOWN_F_SEND) {
		pcb->peer_shutdown |= VSOCK_SHUT_SND;
	}

	/* Both flags set means the peer initiated a disconnection */
	if (pcb->peer_shutdown == VSOCK_SHUT_ALL) {
		soisdisconnected(so);
		vsock_pcb_remove_connected(pcb);
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, private);
	}

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input_credit_update(struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct virtio_socket_data *private;
	struct vsock_pcb *pcb;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);

	pcb = vsock_pcb_lookup_connected(&local, &remote);

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	private = pcb->transport;
	PRIVATE_LOCK(private);
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;
	PRIVATE_UNLOCK(private);

	// Don't wake the sender thread up if the credit is still zero
	if (vtsock_get_peer_credit(private) > 0) {
		sowwakeup(pcb->so);
	}

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input_credit_request(struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct virtio_socket_data *private;
	struct vsock_pcb *pcb;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);

	pcb = vsock_pcb_lookup_connected(&local, &remote);

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	private = pcb->transport;
	PRIVATE_LOCK(private);
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;
	PRIVATE_UNLOCK(private);

	vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_CREDIT_UPDATE, private);

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input_data(struct mbuf *m, struct virtio_vtsock_hdr *hdr)
{
	struct vsock_addr remote, local;
	struct virtio_socket_data *private;
	struct vsock_pcb *pcb;
	struct socket *so;
	u_int m_len;

	NET_EPOCH_ASSERT();

	vtsock_populate_addr(hdr, &remote, &local);

	pcb = vsock_pcb_lookup_connected(&local, &remote);

	if (pcb == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		return;
	}

	VSOCK_LOCK(pcb);

	if (pcb->so == NULL) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}

	so = pcb->so;
	if ((so->so_state & SS_ISCONNECTED) == 0) {
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
		goto out;
	}
	private = pcb->transport;
	PRIVATE_LOCK(private);
	private->peer_fwd_cnt = hdr->fwd_cnt;
	private->peer_buf_alloc = hdr->buf_alloc;
	PRIVATE_UNLOCK(private);

	if (hdr->len == 0) {
		goto out;
	}

	m_len = m_length(m, NULL);

	if (m_len != hdr->len) {
		goto out;
	}

	SOCK_RECVBUF_LOCK(so);

	if (sbspace(&so->so_rcv) < m_len) {
		SOCK_RECVBUF_UNLOCK(so);
		goto out;
	}

	sbappendstream_locked(&so->so_rcv, m, 0);
	sorwakeup_locked(so);

out:
	VSOCK_UNLOCK(pcb);
}

static void
vtsock_input(struct mbuf *m)
{
	struct vsock_addr remote, local;
	struct virtio_vtsock_hdr *hdr;
	struct epoch_tracker et;

	hdr = mtod(m, struct virtio_vtsock_hdr *);

	SDT_PROBE1(vtsock, , , receive, hdr);

	NET_EPOCH_ENTER(et);

	switch (hdr->op) {
	case VIRTIO_VTSOCK_OP_REQUEST:
		vtsock_input_request(hdr);
		break;
	case VIRTIO_VTSOCK_OP_RESPONSE:
		vtsock_input_response(hdr);
		break;
	case VIRTIO_VTSOCK_OP_RST:
		vtsock_input_reset(hdr);
		break;
	case VIRTIO_VTSOCK_OP_SHUTDOWN:
		vtsock_input_shutdown(hdr);
		break;
	case VIRTIO_VTSOCK_OP_CREDIT_UPDATE:
		vtsock_input_credit_update(hdr);
		break;
	case VIRTIO_VTSOCK_OP_CREDIT_REQUEST:
		vtsock_input_credit_request(hdr);
		break;
	case VIRTIO_VTSOCK_OP_RW:
		m_adj(m, sizeof(struct virtio_vtsock_hdr));
		vtsock_input_data(m, hdr);
		m = NULL;
		break;
	default:
		vtsock_populate_addr(hdr, &remote, &local);
		vtsock_send_control(&local, &remote, VIRTIO_VTSOCK_OP_RST, NULL);
	}

	NET_EPOCH_EXIT(et);

	if (m) {
		m_freem(m);
	}
}

static void
vtsock_post_receive(struct vsock_pcb *pcb, uint32_t received)
{
	struct virtio_socket_data *private = pcb->transport;

	PRIVATE_LOCK(private);
	private->fwd_cnt += received;
	PRIVATE_UNLOCK(private);

	/*
	* If the peer's view of our credit is below a threshold (VTSOCK_BUFSZ << 2 here)
	* send a credit update proactively.
	*/
	if ((private->fwd_cnt - private->last_fwd_cnt + (VTSOCK_BUFSZ << 2) ) >= private->last_buf_alloc) {
		vtsock_send_internal(private, &pcb->local, &pcb->remote, VSOCK_CREDIT_UPDATE, NULL);
	}
}

static uint32_t
vtsock_check_writable(struct vsock_pcb *pcb, bool notify)
{
	struct virtio_socket_data *private = pcb->transport;

	if (notify) {
		vtsock_send_control(&pcb->local, &pcb->remote, VIRTIO_VTSOCK_OP_CREDIT_REQUEST, private);
	}

	return (vtsock_get_peer_credit(private));
}

static void
vtsock_setup_sysctl(struct vtsock_softc *sc)
{
	device_t dev = sc->vtsock_dev;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *child;

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

static int
vtsock_attach_socket(struct vsock_pcb *pcb)
{
	struct virtio_socket_data *private;

	private = malloc(sizeof(struct virtio_socket_data), M_VTSOCK, M_NOWAIT | M_ZERO);
	if (private == NULL) {
		return (ENOMEM);
	}

	SOCK_RECVBUF_LOCK(pcb->so);
	private->buf_alloc = pcb->so->so_rcv.sb_hiwat;
	SOCK_RECVBUF_UNLOCK(pcb->so);
	private->last_buf_alloc = VSOCK_RCV_BUFFER_SIZE;

	pcb->transport = private;
	mtx_init(&private->mtx, "virtio_socket_data_mtx", NULL, MTX_DEF);
	refcount_acquire(&active_sockets);

	return (0);
}

static void
vtsock_detach_socket(struct vsock_pcb *pcb)
{
	struct virtio_socket_data *private = pcb->transport;

	mtx_destroy(&private->mtx);
	free(pcb->transport, M_VTSOCK);
	pcb->transport = NULL;
	refcount_release(&active_sockets);
}

static uint32_t
vtsock_get_peer_credit(struct virtio_socket_data *private)
{
	if (private->peer_buf_alloc < (private->tx_cnt - private->peer_fwd_cnt)) {
		return (0);
	}

	return (private->peer_buf_alloc - (private->tx_cnt - private->peer_fwd_cnt));
}
