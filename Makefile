########################################
# Pontificia Universidad Javeriana
# Estudiante: Juan David Herrera Henao
# Materia: Sistemas Operativos 2026-30
# Fichero: automatizacion de compilacion
########################################

# Compilador, interfaz compartida y opciones de comprobacion.
CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g
# Bibliotecas para hilos POSIX y operaciones matematicas.
LDLIBS = -pthread -lm
PROGRAMAS = servidorISE agentePais generadorCSV

# Los tres ejecutables comparten las funciones de src/comun.c.
.PHONY: all clean test sanitize
all: $(PROGRAMAS)
servidorISE: src/servidor.c src/comun.c include/ise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/servidor.c src/comun.c $(LDFLAGS) $(LDLIBS) -o $@
agentePais: src/agente.c src/comun.c include/ise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/agente.c src/comun.c $(LDFLAGS) $(LDLIBS) -o $@
generadorCSV: src/generador.c src/comun.c include/ise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/generador.c src/comun.c $(LDFLAGS) $(LDLIBS) -o $@
# Verificacion automatica de argumentos, CSV, comunicacion y sincronizacion.
test: all
	python3 tests/integracion.py
# Compilacion con deteccion de errores de memoria y comportamiento indefinido.
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie'
# Elimina solamente los tres ejecutables generados.
clean:
	rm -f $(PROGRAMAS)
