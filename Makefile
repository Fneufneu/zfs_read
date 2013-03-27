# $FreeBSD: stable/9/sys/boot/zfs/Makefile 243219 2012-11-18 12:28:29Z avg $

PROG=		zfs_read
NO_MAN=

SRCS+=		zfs.c part.c

CFLAGS+=	-I${.CURDIR}/common -I.
CFLAGS+=	-I${.CURDIR}/libstand
CFLAGS+=	-I${.CURDIR}/zfs

CFLAGS+=	-mpreferred-stack-boundary=2
CFLAGS+=	-m32

CFLAGS+=	-Wformat -Wall

CFLAGS+=	-DBOOT2=1

.include <bsd.prog.mk>

