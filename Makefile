PROG=  smnntpd
LDADD+= -lconfuse
MAN=
CFLAGS+= -Wall -pedantic
.include <bsd.prog.mk>
