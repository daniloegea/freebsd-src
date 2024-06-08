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
#include <sys/sglist.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/queue.h>
#include <sys/sdt.h>

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

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>

#include <net/vsock.h>

#include <dev/virtio/virtio.h>
#include <dev/virtio/virtqueue.h>
#include <dev/virtio/socket/virtio_socket.h>

#include "sys/socketvar.h"
#include "virtio_if.h"

struct vtsock_softc {
	device_t			vtsock_dev;
	uint64_t			vtsock_features;
	struct virtio_vsock_config	vtsock_config;
	struct mtx			vtsock_mtx;
	struct mtx			vtsock_rxq_mtx;
	struct virtqueue		*vtsock_txvq;
	struct virtqueue		*vtsock_rxvq;
	struct virtqueue		*vtsock_eventvq;
	struct taskqueue		*vtsock_rxtq;
	struct task			vtsock_intrtask;
};

struct virtio_vsock_hdr {
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

struct virtio_vsock_packet {
	struct virtio_vsock_hdr hdr;
	uint8_t data[];
};

#define VTSOCK_LOCK(_sc)	mtx_lock(&(_sc)->vtsock_mtx)
#define VTSOCK_UNLOCK(_sc)	mtx_unlock(&(_sc)->vtsock_mtx)
#define VTSOCK_RXQ_LOCK(_sc)	mtx_lock(&(_sc)->vtsock_rxq_mtx)
#define VTSOCK_RXQ_UNLOCK(_sc)	mtx_unlock(&(_sc)->vtsock_rxq_mtx)
static struct vtsock_softc	*vtsock_softc = NULL;

#define so2vsockpcb(so) \
	((struct vsock_pcb *)((so)->so_pcb))
#define vsockpcb2so(vsockpcb) \
	((struct socket *)((vsockpcb)->so))

MALLOC_DEFINE(M_VSOCK, "virtio_socket", "virtio socket control structures");

static int	vtsock_probe(device_t);
static int	vtsock_attach(device_t);
static int	vtsock_detach(device_t);
static int	vtsock_config_change(device_t);

static int	vtsock_alloc_virtqueues(struct vtsock_softc *);
static int	vtsock_setup_features(struct vtsock_softc *);

static void	vtsock_read_config(struct vtsock_softc *, struct virtio_vsock_config *);
static void	vtsock_event_ctl(void *);
static void	vtsock_rx_intr_handler(void *);
static uint64_t	vtsock_get_local_cid(void);
static int	vtsock_populate_rxvq(struct vtsock_softc *sc);

static int 	vtsock_connect(struct socket *);
static int	vtsock_disconnect(struct socket *);
static int	vtsock_send(struct socket *so, struct mbuf *m);
static int	vtsock_send_reply_reset(struct virtio_vsock_hdr *hdr);
static int	vtsock_send_request_response(struct vsock_pcb *pcb);
static int	vtsock_send_credit_update(struct socket *so);
static void	vtsock_operation_handler(void *buf, size_t len);
static void	vtsock_setup_sysctl(struct vtsock_softc *sc);
static int	vtsock_setup_taskqueue(struct vtsock_softc *sc);
static void	vtsock_rxq_tq_deffered(void *xtxq, int pending __unused);

static uint16_t	_vtsock_get_type(struct vsock_pcb *pcb);
static void	_vtsock_populate_header(struct virtio_vsock_hdr *hdr, uint16_t op, struct vsock_pcb *pcb);
static int	_vtsock_can_send_more(void);

/* VSOCK Transport layer */

#define VTSOCK_BUFSZ 128

#define DEBUG_VTSOCK_HEADER(hdr) \
do { \
	printf("src_cid: %lu, dst_cid: %lu, src_port: %u, dst_port: %u " \
		"len: %u, type: %hu, op: %hu, flags: %ux, buf_alloc: %u " \
		"fwd_cnt: %u\n", \
		hdr->src_cid, hdr->dst_cid, hdr->src_port, hdr->dst_port, \
		hdr->len, hdr->type, hdr->op, hdr->flags, hdr->buf_alloc, \
		hdr->fwd_cnt); \
} while(0)

static struct virtio_feature_desc vtsock_feature_desc[] = {
	{ VIRTIO_VSOCK_F_STREAM,	"StreamSocket"	},
	{ VIRTIO_VSOCK_F_SEQPACKET,	"SeqpacketSocket"	},
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

static struct virtio_transport_ops transport = {
	.get_local_cid = vtsock_get_local_cid,
	.connect = vtsock_connect,
	.disconnect = vtsock_disconnect,
	.send = vtsock_send,
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
vtsock_enqueue_rxvq_buf(struct virtqueue *vq, void *buf, size_t len)
{
	int error;
	struct sglist_seg segs[2];
	struct sglist sg;

	sglist_init(&sg, 2, segs);
	error = sglist_append(&sg, buf, len);
	if (error != 0) {
		return (error);
	}

	return (virtqueue_enqueue(vq, buf, &sg, 0, sg.sg_nseg));
}

static int
vtsock_populate_rxvq(struct vtsock_softc *sc)
{
	int nbufs, error;
	char *buf;
	struct virtqueue *vq = sc->vtsock_rxvq;

	for (nbufs = 0; !virtqueue_full(vq); nbufs++) {
		buf = malloc(VTSOCK_BUFSZ + sizeof(struct virtio_vsock_hdr), M_DEVBUF, M_WAITOK);
		error = vtsock_enqueue_rxvq_buf(vq, buf, VTSOCK_BUFSZ);
		if (error)
			return (error);
	}

	if (nbufs > 0) {
		printf("Number of RX buffers: %d\n", nbufs);
		virtqueue_notify(vq);
	}

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

	virtio_set_feature_desc(dev, vtsock_feature_desc);
	error = vtsock_setup_features(sc);
	if (error) {
		device_printf(dev, "cannot setup features\n");
		goto fail;
	}

	mtx_init(&sc->vtsock_mtx, "vtsockmtx", NULL, MTX_DEF);
	mtx_init(&sc->vtsock_rxq_mtx, "vtsock_rxq_mtx", NULL, MTX_DEF);

	vtsock_read_config(sc, &sc->vtsock_config);

	vtsock_setup_sysctl(sc);

	error = vtsock_alloc_virtqueues(sc);
	if (error) {
		device_printf(dev, "cannot allocate virtqueues\n");
		goto fail;
	}

	error = vtsock_populate_rxvq(sc);
	if (error) {
		device_printf(dev, "cannot populate RX virtqueue\n");
		goto fail;
	}

	error = vtsock_setup_taskqueue(sc);
	if (error) {
		device_printf(dev, "failed to setup taskqueue\n");
		goto fail;
	}

	error = virtio_setup_intr(dev, INTR_TYPE_MISC);
	if (error) {
		device_printf(dev, "cannot setup interruptions\n");
		goto fail;
	}

	error = virtqueue_enable_intr(sc->vtsock_rxvq);
	if (error) {
		device_printf(dev, "cannot enable interruptions on the RX virtqueue\n");
		goto fail;
	}

	error = virtqueue_enable_intr(sc->vtsock_eventvq);
	if (error) {
		device_printf(dev, "cannot enable interruptions on the event virtqueue\n");
		goto fail;
	}


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
	int last = 0;
	void *buf;

	sc = device_get_softc(dev);

	if (device_is_attached(dev)) {
		VTSOCK_LOCK(sc);
		virtqueue_disable_intr(sc->vtsock_rxvq);
		virtqueue_disable_intr(sc->vtsock_eventvq);
		virtio_stop(sc->vtsock_dev);
		VTSOCK_UNLOCK(sc);
	}

	while ((buf = virtqueue_drain(sc->vtsock_rxvq, &last)) != NULL) {
		free(buf, M_DEVBUF);
	}

	while ((buf = virtqueue_drain(sc->vtsock_eventvq, &last)) != NULL) {
		free(buf, M_DEVBUF);
	}

	taskqueue_drain_all(sc->vtsock_rxtq);
	taskqueue_free(sc->vtsock_rxtq);

	mtx_destroy(&sc->vtsock_mtx);

	vsock_transport_deregister();

	return (0);
}

static void
vtsock_read_config(struct vtsock_softc *sc, struct virtio_vsock_config *sockcfg)
{
	device_t dev;

	dev = sc->vtsock_dev;
	virtio_read_device_config(dev,
			   offsetof(struct virtio_vsock_config, guest_cid),
			   &sockcfg->guest_cid, sizeof(sockcfg->guest_cid));
}

static uint64_t
vtsock_get_local_cid(void) {
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

	VQ_ALLOC_INFO_INIT(&vq_info[0], 0, vtsock_rx_intr_handler, sc, &sc->vtsock_rxvq,
				"%s RX", device_get_nameunit(dev));

	VQ_ALLOC_INFO_INIT(&vq_info[1], 0, NULL, NULL, &sc->vtsock_txvq,
				"%s TX", device_get_nameunit(dev));

	VQ_ALLOC_INFO_INIT(&vq_info[2], 0, vtsock_event_ctl, sc, &sc->vtsock_eventvq,
				"%s event", device_get_nameunit(dev));

	error = virtio_alloc_virtqueues(dev, 3, vq_info);
	free(vq_info, M_TEMP);

	return (error);
}

static int
vtsock_setup_features(struct vtsock_softc *sc)
{
	device_t dev;
	int error;

	dev = sc->vtsock_dev;

	sc->vtsock_features = virtio_negotiate_features(dev, 0);
	error = virtio_finalize_features(dev);

	if (virtio_with_feature(dev, VIRTIO_VSOCK_F_STREAM)) {
		printf("f_stream\n");
	}

	if (virtio_with_feature(dev, VIRTIO_VSOCK_F_SEQPACKET)) {
		printf("f_seqpacket\n");
	}

	return (error);
}

static int
vtsock_setup_taskqueue(struct vtsock_softc *sc)
{
	int error;
	device_t dev = sc->vtsock_dev;

	TASK_INIT(&sc->vtsock_intrtask, 0, vtsock_rxq_tq_deffered, sc);
	sc->vtsock_rxtq = taskqueue_create("virtio_socket", M_NOWAIT, taskqueue_thread_enqueue, &sc->vtsock_rxtq);
	if (sc->vtsock_rxtq == NULL) {
		printf("taskqueue_create returned null\n");
	}
	error = taskqueue_start_threads(&sc->vtsock_rxtq, 1, PI_NET, "%s rxq", device_get_nameunit(dev));
	if (error) {
		device_printf(dev, "failed to start RX taskqueue: %d\n", error);
	}

	return error;
}

static void
vtsock_event_ctl(void *ctx) {
	printf("event_ctl %p\n", ctx);
}

static void
vtsock_output(void *buf, int bufsize)
{
	struct sglist_seg segs[2];
	struct sglist sg;
	struct virtqueue *vq = vtsock_softc->vtsock_txvq;
	int error;

	sglist_init(&sg, 2, segs);
	error = sglist_append(&sg, buf, bufsize);

	error = virtqueue_enqueue(vq, buf, &sg, sg.sg_nseg, 0);
	if (error == 0) {
		virtqueue_notify(vq);
		virtqueue_poll(vq, NULL);
	} else {
		printf("Virtqueue_enqueue error %d\n", error);
	}
}

static int
vtsock_connect(struct socket *so)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	struct virtio_vsock_hdr *new_hdr = malloc(VTSOCK_BUFSZ, M_VSOCK, M_NOWAIT | M_ZERO);

	_vtsock_populate_header(new_hdr, VIRTIO_VSOCK_OP_REQUEST, pcb);
	new_hdr->buf_alloc = sbspace(&so->so_rcv);

	vtsock_output((void *) new_hdr, VTSOCK_BUFSZ);

	free(new_hdr, M_VSOCK);

	return 0;
}

static int
vtsock_disconnect(struct socket *so)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	struct virtio_vsock_hdr *new_hdr = malloc(VTSOCK_BUFSZ, M_VSOCK, M_NOWAIT | M_ZERO);

	_vtsock_populate_header(new_hdr, VIRTIO_VSOCK_OP_SHUTDOWN, pcb);
	new_hdr->flags = 3;

	vtsock_output((void *) new_hdr, VTSOCK_BUFSZ);

	free(new_hdr, M_VSOCK);

	return 0;
}

static int
vtsock_send_credit_update(struct socket *so)
{
	struct vsock_pcb *pcb = so2vsockpcb(so);

	char *buf = malloc(VTSOCK_BUFSZ, M_VSOCK, M_NOWAIT | M_ZERO);
	if (buf == NULL) {
		printf("Can't allocate buf so_send\n");
		return -1;
	}

	struct virtio_vsock_hdr *new_hdr = (struct virtio_vsock_hdr *) buf;

	_vtsock_populate_header(new_hdr, VIRTIO_VSOCK_OP_CREDIT_UPDATE, pcb);

	SOCKBUF_LOCK(&so->so_snd);
	new_hdr->buf_alloc = sbspace(&so->so_rcv);
	SOCKBUF_UNLOCK(&so->so_snd);
	new_hdr->fwd_cnt = pcb->fwd_cnt;
	new_hdr->len = 0;

	vtsock_output(buf, VTSOCK_BUFSZ);

	free(buf, M_VSOCK);

	return 0;
}

static int
vtsock_send(struct socket *so, struct mbuf *m)
{
	int total = 0;

	struct virtio_vsock_hdr *new_hdr;
	struct vsock_pcb *pcb = so2vsockpcb(so);
	size_t hdr_len = sizeof(struct virtio_vsock_hdr);
	char *buf, *data;
	int sndlen;

	if (!_vtsock_can_send_more())
		return -ENOBUFS;

	buf = malloc(VTSOCK_BUFSZ, M_VSOCK, M_NOWAIT | M_ZERO);
	if (buf == NULL) {
		printf("Can't allocate buf so_send\n");
		return -ENOBUFS;
	}

	new_hdr = (struct virtio_vsock_hdr *) buf;

	sndlen = MIN(m->m_len, VTSOCK_BUFSZ - hdr_len);
	_vtsock_populate_header(new_hdr, VIRTIO_VSOCK_OP_RW, pcb);

	SOCKBUF_LOCK(&so->so_snd);
	new_hdr->buf_alloc = sbspace(&so->so_rcv);
	SOCKBUF_UNLOCK(&so->so_snd);
	new_hdr->fwd_cnt = pcb->fwd_cnt;
	new_hdr->len = sndlen;

	data = buf + sizeof(struct virtio_vsock_hdr);
	m_copydata(m, 0, sndlen, data);
	m_adj(m, sndlen);
	total += sndlen;

	// TODO: check if there is more than a single mbuf of data to be sent
	m_free(m);

	pcb->tx_cnt += new_hdr->len;
	pcb->peer_credit -= new_hdr->len;

	vtsock_output(buf, VTSOCK_BUFSZ);

	free(buf, M_VSOCK);

	return total;
}

static void
vtsock_rx_intr_handler(void *ctx) {
	struct vtsock_softc *sc = ctx;
	int error;

	error = taskqueue_enqueue(sc->vtsock_rxtq, &sc->vtsock_intrtask);
	if (error) {
		printf("taskqueue_enqueue failed %d\n", error);
	}
}

static void
_vtsock_handle_op_response(struct virtio_vsock_hdr *hdr)
{
	struct vsock_pcb *pcb = NULL;

	pcb = vsock_pcb_lookup_connected(hdr->dst_port, hdr->src_port);

	if (!pcb) {
		return;
	}

	printf("Found the PCB. Connection response.\n");

	if (pcb->so->so_state & SS_ISCONNECTING) {
		printf("Socket was ISCONNECTING. Setting to ISCONNECTED\n");
		soisconnected(pcb->so);
		pcb->peer_credit = hdr->buf_alloc;
	}
}

static void
_vtsock_handle_op_rw(struct virtio_vsock_hdr *hdr, void *buf, size_t len)
{
	struct vsock_pcb *pcb = NULL;
	struct socket *so;

	pcb = vsock_pcb_lookup_connected(hdr->dst_port, hdr->src_port);

	if (!pcb) {
		return;
	}

	printf("Found the PCB. Data received.\n");

	so = vsockpcb2so(pcb);
	if ((so->so_state & SS_ISCONNECTED) == 0) {
		vtsock_send_reply_reset(hdr);
		return;
	}

	if (hdr->len <= 0) {
		return;
	}

	struct mbuf *m = m_get(M_NOWAIT, MT_DATA);
	if (!m_append(m, len - sizeof(*hdr), (char*)buf + sizeof(*hdr))) {
		printf("m_append failed god knows why...\n");
	}
	SOCKBUF_LOCK(&so->so_rcv);
	sbappendstream_locked(&so->so_rcv, m, 0);
	pcb->fwd_cnt += len - sizeof(*hdr);
	pcb->peer_credit = hdr->buf_alloc - (pcb->tx_cnt - hdr->fwd_cnt);

	// WTF is this?
	long buf_alloc = sbspace(&so->so_rcv);
	if (buf_alloc - (pcb->fwd_cnt % buf_alloc) < VTSOCK_BUFSZ * 2)
		vtsock_send_credit_update(so);

	sorwakeup_locked(so);
}

static void
_vtsock_handle_op_reset(struct virtio_vsock_hdr *hdr)
{
	struct vsock_pcb *pcb;
	struct socket *so;

	pcb = vsock_pcb_lookup_connected(hdr->dst_port, hdr->src_port);
	if (!pcb) {
		printf("Can't find disconnecting PCB\n");
		return;
	}

	so = vsockpcb2so(pcb);
	if (so->so_state & SS_ISDISCONNECTING) {
		printf("Socket is disconnecting, finishing\n");
		soisdisconnected(so);
	} else if (so->so_state & SS_ISCONNECTING) {
		printf("Connection refused\n");
		so->so_error = ECONNREFUSED;
		soisdisconnected(so);
	}
}

static void
_vtsock_handle_op_request(struct virtio_vsock_hdr *hdr)
{
	struct vsock_pcb *pcb;
	struct socket *so;
	struct socket *new_socket;
	struct vsock_pcb *new_pcb;

	pcb = vsock_pcb_lookup_bound(hdr->dst_port, 0);

	if (!pcb || !SOLISTENING(pcb->so)) {
		vtsock_send_reply_reset(hdr);
		return;
	}

	printf("Found the PCB. Connection request.\n");

	so = pcb->so;

	CURVNET_SET(so->so_vnet);
	new_socket = sonewconn(so, 0);
	CURVNET_RESTORE();
	new_pcb = new_socket->so_pcb;
	new_pcb->local_addr.svm_port = pcb->local_addr.svm_port;
	new_pcb->local_addr.svm_cid = pcb->local_addr.svm_cid;
	new_pcb->remote_addr.svm_port = hdr->src_port;
	new_pcb->remote_addr.svm_cid = hdr->src_cid;
	new_pcb->fwd_cnt = 0;
	new_pcb->tx_cnt = 0;
	new_pcb->peer_credit = hdr->buf_alloc;

	vsock_pcb_insert_connected(new_pcb);
	soisconnected(new_socket);
	vtsock_send_request_response(new_pcb);
}

static void
_vtsock_handle_op_credit_request(struct virtio_vsock_hdr *hdr)
{
	struct vsock_pcb *pcb;
	struct socket *so;

	pcb = vsock_pcb_lookup_connected(hdr->dst_port, hdr->src_port);

	if (!pcb) {
		return;
	}

	so = vsockpcb2so(pcb);
	if ((so->so_state & SS_ISCONNECTED) == 0) {
		vtsock_send_reply_reset(hdr);
		return;
	}

	printf("Found the PCB. Credit request received.\n");

	vtsock_send_credit_update(so);
}

static void
vtsock_operation_handler(void *buf, size_t len)
{
	struct virtio_vsock_hdr *hdr = buf;

	switch(hdr->op) {
	case VIRTIO_VSOCK_OP_RESPONSE:
		_vtsock_handle_op_response(hdr);
		break;
	case VIRTIO_VSOCK_OP_RW:
		_vtsock_handle_op_rw(hdr, buf, len);
		break;
	case VIRTIO_VSOCK_OP_RST:
		_vtsock_handle_op_reset(hdr);
		break;
	case VIRTIO_VSOCK_OP_REQUEST:
		_vtsock_handle_op_request(hdr);
		break;
	case VIRTIO_VSOCK_OP_CREDIT_REQUEST:
		_vtsock_handle_op_credit_request(hdr);
		break;
	}
}

static int
vtsock_send_reply_reset(struct virtio_vsock_hdr *hdr)
{
	struct virtio_vsock_hdr *new_hdr = malloc(VTSOCK_BUFSZ, M_VSOCK, M_NOWAIT | M_ZERO);

	new_hdr->src_port = hdr->dst_port;
	new_hdr->src_cid = hdr->dst_cid;
	new_hdr->dst_port = hdr->src_port;
	new_hdr->dst_cid = hdr->src_cid;
	new_hdr->op = VIRTIO_VSOCK_OP_RST;
	new_hdr->type = VIRTIO_VSOCK_TYPE_STREAM;
	new_hdr->flags = 0;

	vtsock_output((void *) new_hdr, VTSOCK_BUFSZ);

	free(new_hdr, M_VSOCK);

	return (0);
}

static int
vtsock_send_request_response(struct vsock_pcb *pcb)
{
	struct virtio_vsock_hdr *new_hdr = malloc(VTSOCK_BUFSZ, M_VSOCK, M_NOWAIT | M_ZERO);

	_vtsock_populate_header(new_hdr, VIRTIO_VSOCK_OP_RESPONSE, pcb);
	new_hdr->flags = 0;
	new_hdr->buf_alloc = sbspace(&pcb->so->so_rcv);

	vtsock_output((void *) new_hdr, VTSOCK_BUFSZ);

	free(new_hdr, M_VSOCK);

	return (0);
}



static void
vtsock_rxq_tq_deffered(void *xtxq, int pending __unused) {
	struct vtsock_softc *sc = xtxq;
	struct virtqueue *vq = sc->vtsock_rxvq;
	uint32_t len = 0;
	void *buf;
	int deq;

        VTSOCK_RXQ_LOCK(sc);

again:

	deq = 0;

	while ((buf = virtqueue_dequeue(vq, &len)) != NULL) {
		vtsock_operation_handler(buf, len);
		vtsock_enqueue_rxvq_buf(vq, buf, VTSOCK_BUFSZ);
		deq++;
	}

	if (deq > 0) {
		virtqueue_notify(vq);
	}


	if (virtqueue_enable_intr(vq) != 0) {
                goto again;
        }

        VTSOCK_RXQ_UNLOCK(sc);

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

static uint16_t
_vtsock_get_type(struct vsock_pcb *pcb)
{
	if (pcb->so->so_proto->pr_type == SOCK_STREAM)
		return VIRTIO_VSOCK_TYPE_STREAM;

	return 0;
}

static void
_vtsock_populate_header(struct virtio_vsock_hdr *hdr, uint16_t op, struct vsock_pcb *pcb)
{
	hdr->src_port = pcb->local_addr.svm_port;
	hdr->src_cid = pcb->local_addr.svm_cid;
	hdr->dst_port = pcb->remote_addr.svm_port;
	hdr->dst_cid = pcb->remote_addr.svm_cid;
	hdr->op = op;
	hdr->type = _vtsock_get_type(pcb);
}

static int
_vtsock_can_send_more(void)
{
	return !virtqueue_full(vtsock_softc->vtsock_txvq);
}
