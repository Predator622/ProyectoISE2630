/*******************************************************************************
 * Pontificia Universidad Javeriana
 * Estudiante: Juan David Herrera Henao
 * Materia: Sistemas Operativos 2026-30
 * Proyecto: Indicador de Sentimiento Economico
 * Fichero: servidor.c
 * Tema: Proceso servidor con hilos y buffers sincronizados
 ******************************************************************************/

#include "ise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

/* El recolector llena el buffer y el trabajador del agente lo consume. */
typedef struct bufferCircular {
    Mensaje *registros;
    size_t capacidad;      // Cantidad maxima de mensajes.
    size_t entrada;        // Proxima posicion para el productor.
    size_t salida;         // Proxima posicion para el consumidor.
    sem_t libres;          // Permisos para agregar mensajes.
    sem_t ocupados;        // Permisos para retirar mensajes.
    pthread_mutex_t mutex; // Protege el arreglo y los indices del buffer.
} Buffer;
typedef struct datosAgente {
    Buffer buffer;
    pthread_t hilo;
    pid_t pid;
    int pais;
    int finRecibido;  // Lo escribe el recolector.
    int finProcesado; // Lo escribe el trabajador.
    int descriptorRespuesta;
    size_t esperadas;
    size_t recibidas;  // Encuestas aceptadas por el recolector.
    size_t procesadas; // Encuestas sumadas por el trabajador.
    size_t errores;
    size_t porSector[SECTORES];
    Estadisticas estadisticas;
} Agente;
static Agente agentes[MAX_AGENTES];
static int maxAgentes;
static int registrados;
static int descriptorServidor;
static double tiempoLimite;
// Esta bandera es compartida entre hilos: se lee y escribe con operaciones atomicas.
static atomic_bool terminar = 0;
static atomic_bool falloRecepcion = 0;
static sigset_t senalesCierre;
static pthread_mutex_t mutexConsola = PTHREAD_MUTEX_INITIALIZER;
static const char *rutaServidor;
static int fifoCreado;
static struct stat identidadFIFO;

// Elimina los recursos temporales propios al terminar el proceso.
static void limpiarRecursos(void) {
    if (!fifoCreado) {
        return;
    }
    struct stat actual;
    if (lstat(rutaServidor, &actual) == -1) {
        if (errno != ENOENT) {
            perror("lstat FIFO al cerrar");
        }
        return;
    }
    if (!S_ISFIFO(actual.st_mode) || actual.st_dev != identidadFIFO.st_dev ||
        actual.st_ino != identidadFIFO.st_ino) {
        fprintf(stderr, "La ruta del FIFO cambio; se conserva el archivo actual.\n");
        return;
    }
    if (unlink(rutaServidor) == -1 && errno != ENOENT) {
        perror("unlink FIFO");
    }
}

// Espera un permiso del semaforo; reintenta si una senal interrumpe la espera.
static void esperarSemaforo(sem_t *semaforo) {
    while (sem_wait(semaforo) == -1) {
        if (errno != EINTR) {
            errorSistema("sem_wait");
        }
    }
}

// Devuelve un permiso al semaforo y valida el resultado.
static void liberarSemaforo(sem_t *semaforo) {
    if (sem_post(semaforo) == -1) {
        errorSistema("sem_post");
    }
}

// Reserva el buffer e inicia sus dos semaforos y su mutex.
static void iniBuffer(Buffer *buffer, size_t capacidad) {
    buffer->capacidad = capacidad;
    buffer->registros = calloc(capacidad, sizeof *buffer->registros);
    if (!buffer->registros) {
        errorSistema("calloc buffer");
    }
    // pshared=0: estos semaforos sincronizan hilos del mismo proceso.
    if (sem_init(&buffer->libres, 0, (unsigned)capacidad) == -1) {
        errorSistema("sem_init libres");
    }
    if (sem_init(&buffer->ocupados, 0, 0) == -1) {
        errorSistema("sem_init ocupados");
    }
    validarHilo(pthread_mutex_init(&buffer->mutex, NULL), "pthread_mutex_init");
}

// El productor espera un espacio libre y agrega un mensaje al buffer circular.
static int agregarBuffer(Buffer *buffer, const Mensaje *mensaje) {
    // SALIR debe entrar incluso cuando ya vencio el plazo: permite vaciar y cerrar.
    if (mensaje->tipo == SALIR) {
        esperarSemaforo(&buffer->libres);
    } else {
        for (;;) {
            if (atomic_load(&terminar) || obtenerTiempo() >= tiempoLimite) {
                return -1;
            }
            struct timespec esperaSemaforo;
            if (clock_gettime(CLOCK_REALTIME, &esperaSemaforo) == -1) {
                errorSistema("clock_gettime");
            }
            esperaSemaforo.tv_nsec += 100000000;
            if (esperaSemaforo.tv_nsec >= 1000000000) {
                esperaSemaforo.tv_sec++;
                esperaSemaforo.tv_nsec -= 1000000000;
            }
            if (sem_timedwait(&buffer->libres, &esperaSemaforo) == 0) {
                break;
            }
            if (errno != ETIMEDOUT && errno != EINTR) {
                errorSistema("sem_timedwait");
            }
        }
        if (atomic_load(&terminar) || obtenerTiempo() >= tiempoLimite) {
            liberarSemaforo(&buffer->libres);
            return -1;
        }
    }
    validarHilo(pthread_mutex_lock(&buffer->mutex), "mutex lock buffer");
    // El permiso de libres reserva espacio; el mutex protege esta modificacion.
    buffer->registros[buffer->entrada] = *mensaje;
    // El modulo permite volver a la primera posicion al llegar al final.
    buffer->entrada = (buffer->entrada + 1) % buffer->capacidad;
    validarHilo(pthread_mutex_unlock(&buffer->mutex), "mutex unlock buffer");
    liberarSemaforo(&buffer->ocupados);
    return 0;
}

// El consumidor espera un mensaje disponible y libera su espacio al retirarlo.
static Mensaje sacarBuffer(Buffer *buffer) {
    esperarSemaforo(&buffer->ocupados);
    validarHilo(pthread_mutex_lock(&buffer->mutex), "mutex lock buffer");
    Mensaje mensaje = buffer->registros[buffer->salida];
    buffer->salida = (buffer->salida + 1) % buffer->capacidad;
    validarHilo(pthread_mutex_unlock(&buffer->mutex), "mutex unlock buffer");
    liberarSemaforo(&buffer->libres);
    return mensaje;
}

// Abre el FIFO privado del agente despues de comprobar su ruta y propietario.
static int abrirRespuesta(const char *ruta) {
    /* Comprueba la ruta, el tipo de archivo y su propietario antes de responder. */
    if (strncmp(ruta, "/tmp/ise-agente-", 16) || strstr(ruta, "..")) {
        errno = EINVAL;
        return -1;
    }
    int descriptor = open(ruta, O_WRONLY | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor == -1) {
        return -1;
    }
    struct stat estadoFIFO;
    if (fstat(descriptor, &estadoFIFO) == -1 || !S_ISFIFO(estadoFIFO.st_mode) ||
        estadoFIFO.st_uid != getuid()) {
        cerrarDescriptor(descriptor);
        errno = EINVAL;
        return -1;
    }
    return descriptor;
}

// Envia una confirmacion de conexion o de procesamiento al agente.
static void enviarRespuesta(int descriptor, int estado, const char *texto) {
    Respuesta confirmacion = {.estado = estado};
    snprintf(confirmacion.texto, sizeof confirmacion.texto, "%s", texto);
    if (enviarMensaje(descriptor, &confirmacion, sizeof confirmacion, obtenerTiempo() + 1) == -1) {
        perror("agente no disponible para respuesta");
    }
}

// Hilo consumidor: procesa en orden los mensajes del buffer de un agente.
static void *procesarAgente(void *argumento) {
    Agente *agente = (Agente *)argumento;
    for (;;) {
        Mensaje mensaje = sacarBuffer(&agente->buffer);
        if (mensaje.tipo == SALIR) {
            break;
        }
        // El semaforo publica los datos del registro antes de que el trabajador los use.
        if (mensaje.tipo == CONEXION) {
            continue;
        }
        if (mensaje.tipo == LECTURA) {
            sumarEncuesta(&agente->estadisticas, &mensaje.encuesta);
            agente->procesadas++;
        } else if (mensaje.tipo == FIN) {
            agente->finProcesado = 1;
            int completo = agente->procesadas == agente->esperadas && !agente->errores;
            int estadoRespuesta;
            const char *textoRespuesta;
            validarHilo(pthread_mutex_lock(&mutexConsola), "mutex consola");
            char titulo[100];
            snprintf(titulo, sizeof titulo, "PARCIAL %s / PID %ld", paises[agente->pais],
                     (long)agente->pid);
            impReporte(titulo, &agente->estadisticas);
            if (!completo) {
                printf("ENVIO INCOMPLETO: %zu/%zu procesadas; %zu registros invalidos.\n",
                       agente->procesadas, agente->esperadas, agente->errores);
            }
            validarHilo(pthread_mutex_unlock(&mutexConsola), "mutex consola");
            if (completo) {
                estadoRespuesta = 0;
                textoRespuesta = "Todas las encuestas fueron procesadas.";
            } else {
                estadoRespuesta = 1;
                textoRespuesta = "Envio incompleto: cantidad distinta o registros invalidos.";
            }
            enviarRespuesta(agente->descriptorRespuesta, estadoRespuesta, textoRespuesta);
        }
    }
    if (agente->pid && !agente->finProcesado) {
        enviarRespuesta(agente->descriptorRespuesta, 1, "Servidor cerrado: envio incompleto.");
    }
    return NULL;
}

// Comprueba el cupo y registra al agente antes de confirmar su conexion.
static void registrarAgente(const Mensaje *mensaje, int indice) {
    if (!memchr(mensaje->respuesta, '\0', sizeof mensaje->respuesta)) {
        return;
    }
    int descriptor = abrirRespuesta(mensaje->respuesta);
    if (descriptor == -1) {
        perror("abrir respuesta agente");
        return;
    }
    size_t maxTotal = 0;
    for (int sector = 0; sector < SECTORES; sector++) {
        maxTotal += (size_t)maximos[mensaje->pais][sector];
    }
    const char *rechazo = NULL;
    if (indice >= 0) {
        rechazo = "Conexion rechazada: el agente ya esta registrado.";
    } else if (registrados == maxAgentes) {
        rechazo = "Conexion rechazada: no quedan cupos para agentes.";
    } else if (!mensaje->total || mensaje->total > maxTotal) {
        rechazo = "Conexion rechazada: cantidad de encuestas fuera de rango.";
    }
    if (rechazo) {
        enviarRespuesta(descriptor, 1, rechazo);
        cerrarDescriptor(descriptor);
        return;
    }
    Agente *agente = &agentes[registrados];
    agente->pid = mensaje->pid;
    agente->pais = mensaje->pais;
    agente->esperadas = mensaje->total;
    agente->descriptorRespuesta = descriptor;
    /* Encola CONEXION para que el trabajador vea los datos inicializados. */
    if (agregarBuffer(&agente->buffer, mensaje) == -1) {
        enviarRespuesta(descriptor, 1, "Servidor fuera del tiempo de servicio.");
        cerrarDescriptor(descriptor);
        agente->pid = 0;
        agente->descriptorRespuesta = -1;
        return;
    }
    registrados++;
    validarHilo(pthread_mutex_lock(&mutexConsola), "mutex consola");
    printf("Conexion agente %s | PID %ld | %zu encuestas\n", paises[mensaje->pais],
           (long)mensaje->pid, mensaje->total);
    validarHilo(pthread_mutex_unlock(&mutexConsola), "mutex consola");
    enviarRespuesta(descriptor, 0, "Conectado al servidor ISE.");
    return;
}

// El recolector valida el mensaje y lo envia al buffer del agente correspondiente.
static void distribuirMensaje(const Mensaje *mensaje) {
    if (mensaje->firma != FIRMA || mensaje->pid <= 0 || mensaje->pais < 0 ||
        mensaje->pais >= PAISES) {
        return;
    }
    int indice = -1;
    for (int i = 0; i < registrados; i++) {
        if (agentes[i].pid == mensaje->pid) {
            indice = i;
        }
    }
    if (mensaje->tipo == CONEXION) {
        registrarAgente(mensaje, indice);
        return;
    }
    if (indice < 0) {
        return;
    }
    Agente *agente = &agentes[indice];
    if (mensaje->pais != agente->pais || agente->finRecibido) {
        return;
    }
    if (mensaje->tipo == LECTURA) {
        if (agente->recibidas >= agente->esperadas || mensaje->encuesta.pais != agente->pais ||
            mensaje->encuesta.sector < 0 || mensaje->encuesta.sector >= SECTORES ||
            mensaje->encuesta.sentimiento < 0 || mensaje->encuesta.sentimiento > 10000 ||
            mensaje->encuesta.id < 1) {
            agente->errores++;
            return;
        }
        if (agente->porSector[mensaje->encuesta.sector] >=
            (size_t)maximos[agente->pais][mensaje->encuesta.sector]) {
            agente->errores++;
            return;
        }
        if (agregarBuffer(&agente->buffer, mensaje) == 0) {
            agente->recibidas++;
            agente->porSector[mensaje->encuesta.sector]++;
        }
    } else if (mensaje->tipo == FIN && agregarBuffer(&agente->buffer, mensaje) == 0) {
        agente->finRecibido = 1;
    }
}

// Hilo recolector: completa cada mensaje recibido por el FIFO compartido.
static void *recibirAgentes(void *argumento) {
    (void)argumento;
    Mensaje mensaje;
    size_t recibidos = 0;
    while (!atomic_load(&terminar) && obtenerTiempo() < tiempoLimite) {
        /* Acumula las lecturas parciales hasta completar un mensaje. */
        double paso = obtenerTiempo() + 0.1;
        if (paso > tiempoLimite) {
            paso = tiempoLimite;
        }
        if (esperarDescriptor(descriptorServidor, POLLIN, paso) == -1) {
            if (errno == ETIMEDOUT || errno == EINTR) {
                continue;
            }
            perror("poll recolector");
            atomic_store(&falloRecepcion, 1);
            atomic_store(&terminar, 1);
            break;
        }
        ssize_t bytesLeidos =
            read(descriptorServidor, (char *)&mensaje + recibidos, sizeof mensaje - recibidos);
        if (bytesLeidos < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (bytesLeidos <= 0) {
            if (!bytesLeidos) {
                errno = EPIPE;
            }
            perror("read recolector");
            atomic_store(&falloRecepcion, 1);
            atomic_store(&terminar, 1);
            break;
        }
        recibidos += (size_t)bytesLeidos;
        if (recibidos == sizeof mensaje) {
            distribuirMensaje(&mensaje);
            recibidos = 0;
        }
    }
    return NULL;
}

// Hilo temporizador: solicita el cierre al vencer el plazo o recibir SIGINT/SIGTERM.
static void *controlarTiempo(void *argumento) {
    (void)argumento;
    while (!atomic_load(&terminar)) {
        double restante = tiempoLimite - obtenerTiempo();
        if (restante <= 0) {
            break;
        }
        if (restante > 0.1) {
            restante = 0.1;
        }
        struct timespec espera = {.tv_nsec = (long)(restante * 1e9)};
        int numero = sigtimedwait(&senalesCierre, NULL, &espera);
        if (numero == SIGINT || numero == SIGTERM) {
            break;
        }
        if (numero == -1 && errno != EAGAIN && errno != EINTR) {
            errorSistema("sigtimedwait");
        }
    }
    atomic_store(&terminar, 1);
    return NULL;
}

// Funcion de ingreso: valida capacidad, FIFO, tiempo y cantidad de agentes.
static int funcIngreso(int argc, char *argv[], long *capacidad, long *tiempo) {
    int opcion;
    // Ingreso y validacion de los argumentos de ejecucion.
    while ((opcion = getopt(argc, argv, "b:p:t:a:")) != -1) {
        switch (opcion) {
            case 'b':
                *capacidad = validarEntero(optarg, 1, 100000, "buffer");
                break;
            case 'p':
                rutaServidor = optarg;
                break;
            case 't':
                *tiempo = validarEntero(optarg, 30, 120, "tiempo");
                break;
            case 'a':
                maxAgentes = (int)validarEntero(optarg, 1, MAX_AGENTES, "agentes");
                break;
            default:
                return -1;
        }
    }
    if (*capacidad < 0 || *tiempo < 0 || !rutaServidor || !*rutaServidor || !maxAgentes ||
        optind != argc) {
        fprintf(stderr, "Uso: %s -b registros -p pipeNom -t 30..120 -a 1..4\n", argv[0]);
        return -1;
    }
    return 0;
}

// Prepara el cierre por senales antes de crear cualquier hilo.
static void configurarSenales(void) {
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        errorSistema("signal SIGPIPE");
    }
    if (sigemptyset(&senalesCierre) == -1 || sigaddset(&senalesCierre, SIGINT) == -1 ||
        sigaddset(&senalesCierre, SIGTERM) == -1) {
        errorSistema("sigset cierre");
    }
    /* Todos los hilos heredan el bloqueo; el temporizador recibe las senales. */
    validarHilo(pthread_sigmask(SIG_BLOCK, &senalesCierre, NULL), "pthread_sigmask");
    if (atexit(limpiarRecursos) != 0) {
        errorSistema("atexit");
    }
}

// Crea el FIFO de entrada y devuelve el descriptor que evita un fin de archivo prematuro.
static int iniFIFO(void) {
    /* Crea un FIFO nuevo; si la ruta existe, conserva el archivo y devuelve error. */
    if (mkfifo(rutaServidor, 0600) == -1) {
        errorSistema("mkfifo servidor (la ruta debe estar libre)");
    }
    if (lstat(rutaServidor, &identidadFIFO) == -1) {
        errorSistema("lstat FIFO creado");
    }
    if (!S_ISFIFO(identidadFIFO.st_mode)) {
        errno = EINVAL;
        errorSistema("la ruta del FIFO cambio al iniciar");
    }
    fifoCreado = 1;
    descriptorServidor = open(rutaServidor, O_RDONLY | O_NONBLOCK);
    if (descriptorServidor == -1) {
        errorSistema("open FIFO lectura");
    }
    struct stat abierto;
    if (fstat(descriptorServidor, &abierto) == -1) {
        errorSistema("fstat FIFO abierto");
    }
    if (!S_ISFIFO(abierto.st_mode) || abierto.st_dev != identidadFIFO.st_dev ||
        abierto.st_ino != identidadFIFO.st_ino) {
        errno = EINVAL;
        errorSistema("la ruta del FIFO cambio durante la apertura");
    }
    int descriptorMantener = open(rutaServidor, O_WRONLY | O_NONBLOCK);
    if (descriptorMantener == -1) {
        errorSistema("open mantener FIFO");
    }
    return descriptorMantener;
}

// Crea un buffer y un trabajador por cada cupo disponible para agentes.
static void iniAgentes(size_t capacidad) {
    for (int i = 0; i < maxAgentes; i++) {
        agentes[i].descriptorRespuesta = -1;
        iniBuffer(&agentes[i].buffer, capacidad);
        validarHilo(pthread_create(&agentes[i].hilo, NULL, procesarAgente, &agentes[i]),
                    "crear trabajador");
    }
}

// Espera a los consumidores, agrega sus resultados y libera los recursos de cada buffer.
static void cerrarAgentes(void) {
    // SALIR queda detras de los mensajes pendientes; cada trabajador termina en orden.
    Mensaje fin = {.tipo = SALIR};
    for (int i = 0; i < maxAgentes; i++) {
        agregarBuffer(&agentes[i].buffer, &fin);
    }
    Estadisticas general = {0};
    int enviosIncompletos = 0;
    for (int i = 0; i < maxAgentes; i++) {
        Agente *agente = &agentes[i];
        // Solo se consulta el resultado cuando su trabajador ya termino.
        validarHilo(pthread_join(agente->hilo, NULL), "join trabajador");
        if (agente->pid) {
            const char *estadoEnvio;
            int completo =
                agente->finProcesado && agente->procesadas == agente->esperadas && !agente->errores;
            if (completo) {
                estadoEnvio = "COMPLETO";
            } else {
                estadoEnvio = "INCOMPLETO";
                enviosIncompletos++;
            }
            if (!agente->finProcesado && agente->procesadas > 0) {
                char titulo[100];
                snprintf(titulo, sizeof titulo, "PARCIAL INCOMPLETO %s / PID %ld",
                         paises[agente->pais], (long)agente->pid);
                impReporte(titulo, &agente->estadisticas);
            }
            printf("Agente %s PID %ld: %zu/%zu procesadas | %s\n", paises[agente->pais],
                   (long)agente->pid, agente->procesadas, agente->esperadas, estadoEnvio);
        }
        for (int sector = 0; sector < SECTORES; sector++) {
            general.cantidad[sector] += agente->estadisticas.cantidad[sector];
            general.suma[sector] += agente->estadisticas.suma[sector];
        }
        cerrarDescriptor(agente->descriptorRespuesta);
        if (sem_destroy(&agente->buffer.libres) == -1 ||
            sem_destroy(&agente->buffer.ocupados) == -1) {
            errorSistema("sem_destroy");
        }
        validarHilo(pthread_mutex_destroy(&agente->buffer.mutex), "mutex destroy");
        free(agente->buffer.registros);
    }
    if (enviosIncompletos) {
        printf("RESUMEN CON DATOS PARCIALES: %d envios incompletos.\n", enviosIncompletos);
    }
    impReporte("RESUMEN GENERAL ISE", &general);
    validarHilo(pthread_mutex_destroy(&mutexConsola), "mutex destroy consola");
}

// Funcion principal: ingreso, preparacion, servicio y cierre ordenado.
int main(int argc, char *argv[]) {
    long capacidad = -1;
    long tiempo = -1;
    int descriptorMantener;
    pthread_t hiloRecolector;
    pthread_t hiloTiempo;

    // Ingreso de los parametros de ejecucion.
    if (funcIngreso(argc, argv, &capacidad, &tiempo) == -1) {
        return EXIT_FAILURE;
    }

    // Preparacion de las senales, del FIFO y de los buffers con sus trabajadores.
    configurarSenales();
    descriptorMantener = iniFIFO();
    iniAgentes((size_t)capacidad);

    // Inicio del servicio: un hilo recibe mensajes y otro controla el tiempo.
    tiempoLimite = obtenerTiempo() + tiempo;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("ISE listo | FIFO %s | %ld s | %d agentes | buffer %ld\n", rutaServidor, tiempo,
           maxAgentes, capacidad);
    validarHilo(pthread_create(&hiloRecolector, NULL, recibirAgentes, NULL), "crear recolector");
    validarHilo(pthread_create(&hiloTiempo, NULL, controlarTiempo, NULL), "crear temporizador");

    // El temporizador marca el cierre; despues esperamos que deje de entrar informacion.
    validarHilo(pthread_join(hiloTiempo, NULL), "join temporizador");
    validarHilo(pthread_join(hiloRecolector, NULL), "join recolector");
    cerrarDescriptor(descriptorServidor);
    cerrarDescriptor(descriptorMantener);

    // Se consumen los mensajes pendientes, se imprime el resumen y se libera la memoria.
    cerrarAgentes();
    if (atomic_load(&falloRecepcion)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
