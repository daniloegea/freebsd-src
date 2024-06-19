
SYSDIR?=${SRCTOP}/sys
.PATH: ${SYSDIR}/net
.include "${SYSDIR}/conf/kern.opts.mk"

KMOD=	vsock
SRCS=	vsock.c vsock_domain.h vsock_transport.h

.include <bsd.kmod.mk>
