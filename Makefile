PROGRAM_NAME  ="httpc"
CC           ?= gcc
CFLAGS       ?="-O2"

all:
	${CC} httpc.c ${CFLAGS} -o ${PROGRAM_NAME}

debug:
	${CC} httpc.c -Wall -ggdb -pedantic -o ${PROGRAM_NAME}

clean :
	rm -f ${PROGRAM_NAME}



