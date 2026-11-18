# Indicador de Sentimiento Económico

**Juan David Herrera Henao · Sistemas Operativos 2026-30**

El proyecto recibe encuestas de España, Francia e Italia y calcula un resultado por agente y otro general. Para explicar cómo funciona, sigo una encuesta desde el CSV hasta el reporte. En ese recorrido se ven dos temas del curso: la comunicación entre procesos y la coordinación entre hilos.

- [Organización del código](#organización-del-código)
- [Cómo viaja una encuesta](#cómo-viaja-una-encuesta)
- [Cálculo del ISE](#cálculo-del-ise)
- [Demostración en Linux](#demostración-en-linux)
- [Pruebas](#pruebas)
- [Lectura del código por etapas](#lectura-del-código-por-etapas)
- [Qué revisar si una prueba falla](#qué-revisar-si-una-prueba-falla)
- [Conclusiones y recomendaciones](#conclusiones-y-recomendaciones)

## Organización del código

| Archivo | Qué hace |
| --- | --- |
| [Generador](src/generador.c) | Crea el CSV con las encuestas de un país. |
| [Agente](src/agente.c) | Valida el CSV, se conecta y envía las encuestas. |
| [Servidor](src/servidor.c) | Recibe los mensajes, reparte el trabajo y presenta los resultados. |
| [Funciones comunes](src/comun.c) | Manejan la carga del CSV, el envío de mensajes y el cálculo del reporte. |
| [Estructuras](include/ise.h) | Definen la encuesta y los mensajes que comparten los programas. |

## Cómo viaja una encuesta

```mermaid
flowchart LR
    G[Generador] --> C[CSV]
    C --> A[Agente]
    A --> F[FIFO]
    F --> R[Recolector]
    R --> B[Búfer por agente]
    B --> T[Trabajador]
    T --> I[Reporte ISE]
```

### Del CSV al servidor

El generador escribe en un archivo temporal y publica el CSV terminado con [rename](src/generador.c#L146). El generador y el agente se coordinan con el mismo archivo de bloqueo. La escritura toma un bloqueo exclusivo y la lectura toma uno compartido.

El agente [carga y valida el CSV](src/agente.c#L138). Primero anuncia la conexión, después [envía un mensaje LECTURA por encuesta](src/agente.c#L178) y al final manda FIN. Cada mensaje cabe en PIPE_BUF y se envía con una sola escritura para evitar que se intercale con el de otro agente. El receptor acumula bytes cuando una lectura devuelve una parte del mensaje.

### Del recolector al trabajador

El FIFO comunica al agente con el servidor. Dentro del servidor, el recolector encola cada lectura en el búfer del agente correspondiente. El trabajador retira el mensaje y actualiza la suma y la cantidad de encuestas del sector.

- **Libres** cuenta los espacios disponibles. El recolector [espera un espacio](src/servidor.c#L138) antes de insertar.
- **Ocupados** cuenta los registros disponibles. El trabajador [espera un registro](src/servidor.c#L161) y, cuando lo retira, devuelve un cupo.
- **Mutex** protege la copia del registro y el movimiento de los índices del búfer circular.

Con un búfer de uno, si ya hay un mensaje pendiente, el recolector tiene que esperar a que el trabajador lo retire. Esa espera puede retrasar la atención a otros agentes.

### Cómo termina

FIN avisa que el agente acabó de enviar y permite obtener su reporte parcial. Al vencer el plazo, el servidor deja de recibir, [encola SALIR](src/servidor.c#L499) detrás del trabajo pendiente y espera a los trabajadores con pthread_join. Después junta las estadísticas para el reporte general.

## Cálculo del ISE

En la [función de reporte](src/comun.c#L399) se divide la suma de cada sector por su cantidad de encuestas y se multiplica ese promedio por el peso. En el resultado general se reúnen primero las sumas y cantidades de los agentes por sector. Así cada encuesta cuenta una vez, aunque unos agentes envíen más datos que otros. Si falta algún sector, el resultado queda incompleto y no se emite recomendación.

| Sector | Peso | Promedio de la demo | Aporte |
| --- | ---: | ---: | ---: |
| Industria | 40 % | 100 | 40 |
| Servicios | 30 % | 0 | 0 |
| Consumidores | 20 % | 50 | 10 |
| Comercio | 5 % | 100 | 5 |
| Construcción | 5 % | 0 | 0 |
| **ISE** | **100 %** | | **55 %** |

Por encima de 50 se muestra **Vender**, por debajo se muestra **Comprar** y en 50 se muestra **Neutro**.

## Demostración en Linux

Compilo con GCC y Make. Para ejecutar las pruebas de integración también necesito Python 3.

```sh
make
make test
```

Para repetir la demostración, preparo en el directorio del proyecto un CSV con una encuesta por sector.

```sh
cat > encuestas_demo.csv <<'EOF'
ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento
1,0,ES,Industria,0,100.00
2,0,ES,Servicios,1,0.00
3,0,ES,Consumidores,2,50.00
4,0,ES,Comercio,3,100.00
5,0,ES,Construccion,4,0.00
EOF
```

En la primera terminal arranco el servidor con un búfer de un registro, 30 segundos y un cupo de agente.

```sh
./servidorISE -b 1 -p TORVALDS -t 30 -a 1
```

Cuando aparece ISE listo, paso a la segunda terminal en el mismo directorio.

```sh
ls -l TORVALDS
ps -T -C servidorISE
./agentePais -n encuestas_demo.csv -p TORVALDS
```

La letra p en los permisos de TORVALDS indica que es un FIFO. El nombre puede cambiar, siempre que los dos programas usen la misma ruta con -p. Con ps -T se ven el hilo principal, el trabajador, el recolector y el temporizador. Al enviar el CSV se espera ver cinco encuestas procesadas y **55 % · Vender**. El resumen general aparece al cerrar el servidor.

Para probar datos generados, uso el comando siguiente y luego paso encuestas_ES.csv al agente. El indicador cambia según los valores que salgan.

```sh
./generadorCSV -p ES -e 1
```

## Pruebas

| Caso | Qué comprobé |
| --- | --- |
| Cinco encuestas con resultado conocido | El servidor procesa las cinco y el índice coincide con el cálculo a mano. |
| Cuatro agentes y búfer de uno | El caso de integración procesa 56.325 encuestas y compara el resultado general con un cálculo independiente. |
| CSV inválido | El agente detecta datos incorrectos antes del envío. |
| Sector sin encuestas | El reporte indica que el ISE está incompleto. |
| Vencimiento del plazo | El servidor cierra la recepción y distingue los envíos incompletos. |

El código de las pruebas está en [tests/integracion.py](tests/integracion.py).

## Lectura del código por etapas

### Cómo se compila y qué recibe cada programa

El [Makefile](Makefile) compila tres ejecutables de C por separado. Cada uno tiene su propio main y comparte comun.c e ise.h. La compilación usa C11, pthreads para la concurrencia y libm para las operaciones matemáticas. Esta prueba usa los ejecutables en la misma máquina Linux.

| Programa | Parámetro | Uso |
| --- | --- | --- |
| Generador | -p ES / FR / IT | País del CSV. |
| Generador | -e porcentaje | Porcentaje del máximo de cada sector. |
| Servidor | -b capacidad | Tamaño de cada búfer circular, en registros. |
| Servidor | -p ruta | Ruta del FIFO común. |
| Servidor | -t segundos | Ventana de recepción, entre 30 y 120 s. |
| Servidor | -a agentes | Cupos de agente de esa ejecución, de 1 a 4. |
| Agente | -n archivo | CSV que va a cargar. |
| Agente | -p ruta | FIFO común donde se conecta. |

### Los datos que comparten los programas

| Tipo | Contenido | Dónde verlo |
| --- | --- | --- |
| Encuesta | Identificador, país, sector y sentimiento en centésimas. | [ise.h:29](include/ise.h#L29-L34) |
| Mensaje | Firma del protocolo, tipo, PID, país, cantidad anunciada, encuesta y ruta del FIFO de respuesta. Cada tipo usa los campos que necesita. | [ise.h:38](include/ise.h#L38-L46) |
| Respuesta | Código de estado y texto que devuelve el servidor. Cero indica éxito. | [ise.h:47](include/ise.h#L47-L50) |
| Estadisticas | Dos arreglos de cinco posiciones. Uno cuenta encuestas y el otro suma los sentimientos. | [ise.h:55](include/ise.h#L55-L58) |

La constante FIRMA ayuda a reconocer el formato ISE. Los dos programas interpretan las estructuras binarias con el mismo formato de compilación. Si fuera a comunicar equipos con representaciones distintas, definiría cómo convertir cada campo a un formato explícito antes de enviarlo.

### Generación y validación del CSV

1. En [comun.c:24–41](src/comun.c#L24-L41) están el orden de países y sectores, la tabla de máximos y los pesos. El país y el sector de una encuesta son índices de esas tablas.
2. En [generador.c:109–124](src/generador.c#L109-L124) se recorren los cinco sectores. La cantidad se calcula con **(máximo × porcentaje + 50) / 100**, mediante división entera. Para España al 1 %, industria pasa de 1760 a 18 y construcción de 290 a 3.
3. La parte entera y el resto de dividir el sentimiento entre 100 se escriben separados por punto. Por ejemplo, 5025 sale como 50.25. El identificador se incrementa por fila.
4. En [generador.c:128–146](src/generador.c#L128-L146), fflush vacía el búfer de la biblioteca, fsync sincroniza el archivo y rename publica el temporal en la ruta final. Si alguien está leyendo con el bloqueo compartido, el escritor debe esperar a que termine.
5. En [comun.c:257–284](src/comun.c#L257-L284), el lector comprueba los campos y convierte el sentimiento de vuelta a centésimas. Una fila de 50.25 se guarda como 5025. La validación revisa la coherencia del país y el sector antes de aceptar sus índices.
6. [cargarEncuestas](src/comun.c#L293) lee las líneas del archivo, lleva el conteo y aumenta la memoria cuando hace falta. Un error de datos identifica la línea del CSV, lo que ayuda a localizar la fila que debe corregirse.

### Conexión y respuestas

El agente crea un FIFO privado en un directorio temporal. El servidor tiene una ruta común de entrada, pero las respuestas vuelven por la ruta privada que anuncia el agente. Eso permite que cada agente lea su propia confirmación.

| Mensaje | Quién lo manda y cómo se usa |
| --- | --- |
| CONEXION | El agente anuncia su PID, país, cuántas encuestas trae y su ruta de respuesta. [registrarAgente](src/servidor.c#L248) acepta o rechaza la conexión. |
| LECTURA | El agente envía una encuesta. [distribuirMensaje](src/servidor.c#L297) ubica el cupo por PID y valida país, sector, sentimiento, cantidad y máximos antes de encolar. |
| FIN | El agente avisa que enviará cero lecturas adicionales. El servidor lo encola junto al trabajo previo para que el hilo llegue a ese punto en orden. |
| SALIR | El servidor lo agrega al cierre de la cola para terminar al trabajador después de consumir lo que queda. |

La cantidad anunciada al conectar, la cantidad recibida por el recolector y la cantidad procesada por el trabajador describen etapas distintas. El registro se cuenta como recibido después de encolarlo. En el reporte se puede detectar que el agente no terminó aunque haya alcanzado a enviar parte del archivo.

### Qué hacen poll, read y write

- [esperarDescriptor](src/comun.c#L100) usa poll para esperar a que el descriptor esté listo dentro de un plazo. Un aviso de disponibilidad permite intentar la operación.
- [enviarMensaje](src/comun.c#L140) intenta escribir el mensaje completo. [Las comprobaciones en compilación](include/ise.h#L52-L54) mantienen los mensajes y las respuestas dentro del mínimo POSIX para escrituras atómicas al FIFO.
- [recibirAgentes](src/servidor.c#L342-L382) conserva el número de bytes que ya tiene. Cada read comienza donde terminó el anterior y pide solo lo que falta. Cuando reúne una estructura completa, distribuye el mensaje y reinicia el conteo.
- EINTR indica que una llamada fue interrumpida. EAGAIN indica que no hay datos disponibles en ese momento. En el recolector esos casos permiten volver a intentar. Otros errores de recepción activan el cierre con estado de fallo.

### Seguir un registro dentro del búfer

| Paso | Recolector, productor | Trabajador, consumidor |
| --- | --- | --- |
| 1 | Espera un espacio libre. | Espera un registro ocupado. |
| 2 | Toma el mutex. | Toma el mutex. |
| 3 | Copia en el índice de escritura y avanza el índice. | Copia desde el índice de lectura y avanza el índice. |
| 4 | Libera el mutex y avisa que hay un registro ocupado. | Libera el mutex y devuelve un espacio libre. |

[iniBuffer](src/servidor.c#L103) parte con tantos espacios libres como cupos y cero registros ocupados. Los índices avanzan con módulo para volver al comienzo al llegar al final. Con capacidad uno, siempre se vuelve a la misma posición. La espera se hace antes de tomar el mutex para que el otro hilo pueda entrar y cambiar el estado de la cola. Las operaciones están en [agregarBuffer](src/servidor.c#L120-L158) y [sacarBuffer](src/servidor.c#L161-L169).

### Qué pasa al cerrar

[controlarTiempo](src/servidor.c#L385) comprueba el plazo y espera SIGINT o SIGTERM con sigtimedwait. Cuando se cumple una condición de salida, publica la marca de cierre. El hilo principal espera al temporizador y al recolector. Luego [cerrarAgentes](src/servidor.c#L499) coloca SALIR para que los trabajadores terminen en orden. El hilo principal espera con pthread_join antes de leer las estadísticas definitivas y generar el reporte general. Eso evita mezclar la lectura final de estadísticas con una actualización aún en curso.

### Por qué el resumen junta sumas y cantidades

[sumarEncuesta](src/comun.c#L393) actualiza solo la posición del sector de esa encuesta. Por ejemplo, si un agente aporta una encuesta de industria con 100 y otro aporta tres con 0, la media conjunta es **100 / 4 = 25**. Industria aporta **25 × 0,40 = 10** puntos al ISE. Por eso el servidor reúne sumas y cantidades antes de calcular las medias. La [acumulación general](src/servidor.c#L531-L545) y el [reporte](src/comun.c#L399) se pueden seguir por separado.

## Qué revisar si una prueba falla

| Lo que veo | Qué reviso primero |
| --- | --- |
| El agente no conecta | Espero a que el servidor imprima ISE listo y compruebo que ambos usen la misma ruta en -p. Si la ruta es relativa, ejecuto los dos desde el mismo directorio. |
| Error en el CSV | Reviso la línea indicada, el orden de los campos y que los identificadores correspondan al país y sector. |
| El agente anuncia cinco y el resumen cuenta menos | Reviso si se agotó el plazo, si el agente alcanzó a enviar FIN y si el servidor reportó lecturas rechazadas. |
| El ISE queda incompleto | Busco el sector sin datos. Un sector con media cero sí pudo tener encuestas. |
| Quiero ver el efecto de cambiar el búfer | Repito la prueba con -b 2 y el mismo CSV. La capacidad cambia y el resultado esperado sigue siendo 55 %. |

## Conclusiones y recomendaciones

La prueba de cinco filas me permitió seguir una encuesta desde el CSV y comprobar el resultado sin depender de valores aleatorios. La prueba de cuatro agentes sirvió para revisar el procesamiento concurrente. Con el búfer de uno se ve bien la relación entre los dos semáforos: cada mensaje que entra usa un espacio libre y cada mensaje que sale lo devuelve.

Para repetir la demostración, empezaría por el CSV de cinco filas y comprobaría tanto el conteo como el porcentaje. Luego cambiaría el tamaño del búfer con los mismos datos para verificar que el resultado se conserva. Para comparar rendimiento haría varias corridas con los mismos archivos y la misma cantidad de agentes. Mediría el tiempo total y cuánto dura la espera del recolector cuando una cola se llena, porque esa espera puede retrasar otros mensajes.
