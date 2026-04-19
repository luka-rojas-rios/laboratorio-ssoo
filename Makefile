CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS =

all: banco usuario monitor

banco: banco.c estructuras.h
	$(CC) $(CFLAGS) banco.c -o banco -lrt

usuario: usuario.c estructuras.h
	$(CC) $(CFLAGS) usuario.c -o usuario -lpthread -lrt

monitor: monitor.c estructuras.h
	$(CC) $(CFLAGS) monitor.c -o monitor -lrt

clean:
	rm -f banco usuario monitor

distclean: clean
	rm -f cuentas.dat transacciones.log
	sed -i 's/^PROXIMO_ID=.*/PROXIMO_ID=1001/' config.txt

.PHONY: all clean distclean
