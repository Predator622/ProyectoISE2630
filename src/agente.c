/*******************************************************************************
 * Pontificia Universidad Javeriana
 * Estudiante: Juan David Herrera Henao
 * Materia: Sistemas Operativos 2026-30
 * Proyecto: Indicador de Sentimiento Economico
 * Fichero: agente.c
 * Tema: Proceso cliente para lectura y envio de encuestas
 ******************************************************************************/

#include "ise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

// Parametros de ejecucion del agente.
typedef struct parametrosAgente {
    const char *archivo;
    const char *rutaServidor;
} ParametrosAgente;

static char directorio[] = "/tmp/ise-agente-XXXXXX";
static char rutaRespuesta[RUTA_RESPUESTA];
// El manejador solo marca la interrupcion; el flujo principal hace la limpieza.
static void marcarInterrupcion(int numero) {
    (void)numero;
    cancelado = 1;
}

// Elimina los recursos temporales propios al terminar el proceso.
static void limpiarRecursos(void) {
    if (*rutaRespuesta && unlink(rutaRespuesta) == -1 && errno != ENOENT) {
        perror("unlink respuesta");
    }
    if (*rutaRespuesta && rmdir(directorio) == -1) {
        perror("rmdir agente");
    }
}

// Espera la confirmacion del servidor y termina si este rechaza el envio.
static void recibirRespuesta(int descriptorRespuesta, double tiempoLimite) {
    Respuesta confirmacion;
    if (recibirMensaje(descriptorRespuesta, &confirmacion, sizeof confirmacion, tiempoLimite) ==
        -1) {
        errorSistema("esperar respuesta ISE");
    }
    confirmacion.texto[sizeof confirmacion.texto - 1] = '\0';
    puts(confirmacion.texto);
    if (confirmacion.estado != 0) {
        exit(EXIT_FAILURE);
    }
}

// Funcion de ingreso: recibe y valida las opciones de la linea de comandos.
static int funcIngreso(int argc, char *argv[], ParametrosAgente *parametros) {
    int opcion;
    // Ingreso y validacion de los argumentos de ejecucion.
    while ((opcion = getopt(argc, argv, "n:p:")) != -1) {
        switch (opcion) {
            case 'n':
                parametros->archivo = optarg;
                break;
            case 'p':
                parametros->rutaServidor = optarg;
                break;
            default:
                return -1;
        }
    }
    if (parametros->archivo == NULL || parametros->archivo[0] == '\0' ||
        parametros->rutaServidor == NULL || parametros->rutaServidor[0] == '\0' || optind != argc) {
        fprintf(stderr, "Uso: %s -n encuestas_XX.csv -p pipeNom\n", argv[0]);
        return -1;
    }
    return 0;
}

// Reintenta la conexion y comprueba que el descriptor abierto corresponde a un FIFO.
static int conectarServidor(const char *rutaServidor, double tiempoLimite) {
    int descriptorServidor;
    for (;;) {
        if (cancelado) {
            errno = EINTR;
            errorSistema("conexion interrumpida");
        }
        descriptorServidor = open(rutaServidor, O_WRONLY | O_NONBLOCK);
        if (descriptorServidor >= 0) {
            break;
        }
        if (errno != ENOENT && errno != ENXIO && errno != EINTR) {
            errorSistema("open FIFO servidor");
        }
        if (obtenerTiempo() >= tiempoLimite) {
            errno = ETIMEDOUT;
            errorSistema("conectar servidor");
        }
        struct timespec pausa = {.tv_nsec = 20000000};
        while (nanosleep(&pausa, &pausa) == -1) {
            if (errno != EINTR || cancelado) {
                errorSistema("nanosleep");
            }
        }
    }
    struct stat estadoFIFO;
    if (fstat(descriptorServidor, &estadoFIFO) == -1) {
        errorSistema("fstat FIFO");
    }
    if (!S_ISFIFO(estadoFIFO.st_mode)) {
        errno = EINVAL;
        errorSistema("la ruta no es un FIFO");
    }
    return descriptorServidor;
}

// Funcion principal: ingreso, preparacion, trabajo y liberacion de recursos.
int main(int argc, char *argv[]) {
    ParametrosAgente parametros = {0};
    if (funcIngreso(argc, argv, &parametros) == -1) {
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        errorSistema("signal SIGPIPE");
    }
    struct sigaction accion = {.sa_handler = marcarInterrupcion};
    if (sigemptyset(&accion.sa_mask) == -1 || sigaction(SIGINT, &accion, NULL) == -1 ||
        sigaction(SIGTERM, &accion, NULL) == -1) {
        errorSistema("sigaction agente");
    }
    // Lectura y validacion completa del CSV antes de conectar con el servidor.
    size_t cantidad;
    int pais;
    Encuesta *datos = cargarEncuestas(parametros.archivo, &cantidad, &pais);
    printf("Pais: %s | Leidas %zu encuestas. Conectando...\n", paises[pais], cantidad);
    if (fflush(stdout) == EOF) {
        errorSistema("mostrar inicio de envio");
    }
    // FIFO privado por proceso para recibir las dos confirmaciones del servidor.
    if (!mkdtemp(directorio)) {
        errorSistema("mkdtemp agente");
    }
    snprintf(rutaRespuesta, sizeof rutaRespuesta, "%s/respuesta", directorio);
    if (atexit(limpiarRecursos) != 0) {
        errorSistema("atexit");
    }
    if (mkfifo(rutaRespuesta, 0600) == -1) {
        errorSistema("mkfifo respuesta");
    }
    int descriptorRespuesta = open(rutaRespuesta, O_RDONLY | O_NONBLOCK);
    if (descriptorRespuesta == -1) {
        errorSistema("open respuesta");
    }
    /* Mantiene abierto el FIFO mientras llega la respuesta del servidor. */
    int descriptorMantener = open(rutaRespuesta, O_WRONLY | O_NONBLOCK);
    if (descriptorMantener == -1) {
        errorSistema("open mantener respuesta");
    }
    // Se reintenta la conexion sin bloquear indefinidamente el proceso.
    double tiempoLimite = obtenerTiempo() + 125.0;
    int descriptorServidor = conectarServidor(parametros.rutaServidor, tiempoLimite);
    // Primer mensaje: solicitar un cupo indicando el pais y el total de encuestas.
    Mensaje mensaje = {
        .firma = FIRMA, .tipo = CONEXION, .pid = getpid(), .pais = pais, .total = cantidad};
    snprintf(mensaje.respuesta, sizeof mensaje.respuesta, "%s", rutaRespuesta);
    if (enviarMensaje(descriptorServidor, &mensaje, sizeof mensaje, tiempoLimite) == -1) {
        errorSistema("enviar conexion");
    }
    recibirRespuesta(descriptorRespuesta, tiempoLimite);
    /* El servidor ya abrio el FIFO de respuesta. Si se cierra, el agente
       debe detectar la desconexion mientras espera la confirmacion final. */
    cerrarDescriptor(descriptorMantener);
    // Segundo paso: enviar las encuestas, una escritura atomica por registro.
    mensaje.tipo = LECTURA;
    for (size_t i = 0; i < cantidad; i++) {
        mensaje.encuesta = datos[i];
        if (enviarMensaje(descriptorServidor, &mensaje, sizeof mensaje, tiempoLimite) == -1) {
            errorSistema("enviar encuesta");
        }
        if ((i + 1) % 500 == 0 || i + 1 == cantidad) {
            printf("%zu/%zu encuestas enviadas\n", i + 1, cantidad);
            if (fflush(stdout) == EOF) {
                errorSistema("mostrar progreso de envio");
            }
        }
    }
    // Ultimo mensaje: indicar que ya no hay mas encuestas para este agente.
    mensaje.tipo = FIN;
    if (enviarMensaje(descriptorServidor, &mensaje, sizeof mensaje, tiempoLimite) == -1) {
        errorSistema("enviar fin");
    }
    /* Espera la confirmacion de que el servidor proceso las encuestas. */
    recibirRespuesta(descriptorRespuesta, tiempoLimite);
    printf("Envio de %s completado.\n", paises[pais]);
    // Libera las encuestas y cierra los descriptores que siguen abiertos.
    free(datos);
    cerrarDescriptor(descriptorServidor);
    cerrarDescriptor(descriptorRespuesta);
    return EXIT_SUCCESS;
}
