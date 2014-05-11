all : main

#if not compile on mac, comment next line
MAC := -D_ON_MAC_

CFLAGS := ${MAC}

main : main.c coroutine.c
	gcc -g -Wall ${CFLAGS} -o $@ $^

clean :
	rm main
