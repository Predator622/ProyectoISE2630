# Indicador de Sentimiento Económico

Proyecto de Sistemas Operativos 2026-30. El generador crea archivos CSV de encuestas para España, Francia o Italia. Cada agente valida y envía las encuestas de un país por un FIFO. El servidor usa un hilo recolector y un búfer circular por agente. Un hilo por agente procesa las lecturas y el temporizador cierra la recepción al vencer el plazo.

## Compilar y probar en Linux

Se requiere GCC, Make y Python 3 para las pruebas.

```sh
make
make test
```

## Demostración

En la primera terminal, desde la carpeta del proyecto:

```sh
./generadorCSV -p ES -e 1
./servidorISE -b 2 -p TORVALDS -t 60 -a 1
```

En otra terminal, desde la misma carpeta:

```sh
./agentePais -n encuestas_ES.csv -p TORVALDS
```

Al terminar el agente, el servidor muestra su reporte parcial. Al vencer el plazo, publica el resumen general. El ISE pondera industria (40 %), servicios (30 %), consumidores (20 %), comercio (5 %) y construcción (5 %). Si falta un sector, el reporte indica que el cálculo está incompleto.

El semáforo de espacios libres limita el búfer y el de registros ocupados despierta al consumidor. Un mutex protege los índices del búfer. Las pruebas incluyen cuatro agentes concurrentes con un búfer de un registro, CSV inválidos y envíos que quedan incompletos al vencer el plazo.
