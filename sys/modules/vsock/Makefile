
SYSDIR?=${SRCTOP}/sys
.PATH: ${SYSDIR}/net
.include "${SYSDIR}/conf/kern.opts.mk"

KMOD=	vsock
SRCS=	vsock.c vsock.h

.include <bsd.kmod.mk>
