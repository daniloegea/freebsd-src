PROG=	MdLoad
LIBADD=	sbuf bsdxml
NO_OBJ=	t

MAN=


WARNS?=	4
.include "bsd.prog.mk"

test:	${PROG}
	./${PROG} md34 ../Data/disk.critter.ad0.xml
