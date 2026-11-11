"""Pruebas del generador, los agentes y el servidor con archivos temporales."""
import csv
import fcntl
import ctypes
import itertools
import os
from pathlib import Path
import signal
import select
import subprocess
import tempfile
import time

BASE = Path(__file__).resolve().parents[1]
SECTORES = ['Industria', 'Servicios', 'Consumidores', 'Comercio', 'Construccion']
MAXIMOS = {'ES': [1760,1430,2020,1080,290], 'FR': [3850,4240,1670,2860,24020],
           'IT': [5440,3100,2000,1560,1000]}
IDS = {'ES': 0, 'FR': 1, 'IT': 2}
PROCESOS = []

class Encuesta(ctypes.Structure):
    _fields_ = [('id',ctypes.c_long), ('pais',ctypes.c_int),
                ('sector',ctypes.c_int), ('sentimiento',ctypes.c_int)]

class Mensaje(ctypes.Structure):
    _fields_ = [('firma',ctypes.c_uint32), ('tipo',ctypes.c_int),
                ('pid',ctypes.c_int), ('pais',ctypes.c_int),
                ('total',ctypes.c_size_t), ('encuesta',Encuesta),
                ('respuesta',ctypes.c_char*160)]

class Respuesta(ctypes.Structure):
    _fields_ = [('estado',ctypes.c_int), ('texto',ctypes.c_char*120)]

class ClientePrueba:
    def __init__(self, fifo, total):
        self.directorio = tempfile.TemporaryDirectory(prefix='ise-agente-', dir='/tmp')
        self.ruta = Path(self.directorio.name)/'respuesta'
        os.mkfifo(self.ruta, 0o600)
        self.lector = os.open(self.ruta, os.O_RDONLY | os.O_NONBLOCK)
        self.mantener = os.open(self.ruta, os.O_WRONLY | os.O_NONBLOCK)
        self.escritor = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        self.mensaje = Mensaje(firma=0x49534531,tipo=1,pid=os.getpid(),pais=0,total=total,
                               respuesta=os.fsencode(self.ruta))
        self.enviar(1)
        assert self.recibir().estado == 0

    def enviar(self, tipo, valor=5000):
        self.mensaje.tipo = tipo
        self.mensaje.encuesta = Encuesta(1,0,0,valor)
        datos = bytes(self.mensaje)
        assert os.write(self.escritor, datos) == len(datos)

    def recibir(self, timeout=35):
        datos = b''
        limite = time.monotonic()+timeout
        while len(datos) < ctypes.sizeof(Respuesta):
            espera = limite-time.monotonic()
            assert espera > 0 and select.select([self.lector],[],[],espera)[0]
            parte = os.read(self.lector,ctypes.sizeof(Respuesta)-len(datos))
            assert parte
            datos += parte
        return Respuesta.from_buffer_copy(datos)

    def cerrar(self):
        for fd in [self.escritor,self.mantener,self.lector]:
            os.close(fd)
        self.ruta.unlink()
        self.directorio.cleanup()

def ejecutar(programa, *args, cwd=None):
    return subprocess.run([str(BASE / programa), *map(str, args)], cwd=cwd,
                          text=True, capture_output=True, timeout=10)

def fichero(ruta, pais, valores, repeticiones=1):
    with ruta.open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['ID_Encuesta','ID_Pais','Pais','Sector','ID_Sector','Sentimiento'])
        id_encuesta = 0
        for s, valor in enumerate(valores):
            if valor is None:
                continue
            for _ in range(repeticiones):
                id_encuesta += 1
                w.writerow([id_encuesta,IDS[pais],pais,SECTORES[s],s,valor])

def servidor(carpeta, agentes=4, buffer=1, orden=('t','p','a','b')):
    carpeta.mkdir()
    log = (carpeta / 'servidor.log').open('w')
    fifo = carpeta / 'ise_fifo'
    inicio = time.monotonic()
    valores = {'t':'30','p':str(fifo),'a':str(agentes),'b':str(buffer)}
    argumentos = [valor for bandera in orden for valor in ['-'+bandera,valores[bandera]]]
    p = subprocess.Popen([str(BASE/'servidorISE'), *argumentos], stdout=log, stderr=log)
    PROCESOS.append(p)
    for _ in range(100):
        if 'ISE listo' in (carpeta/'servidor.log').read_text():
            return p, fifo, log, inicio
        assert p.poll() is None, (carpeta/'servidor.log').read_text()
        time.sleep(0.02)
    raise AssertionError('Servidor no inicia')

def agente(csv_path, fifo, archivo_primero=False):
    argumentos = ['-n',str(csv_path),'-p',str(fifo)] if archivo_primero else ['-p',str(fifo),'-n',str(csv_path)]
    p = subprocess.Popen([str(BASE/'agentePais'), *argumentos],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    PROCESOS.append(p)
    return p

def finalizar(caso, interrupcion=True, senal=signal.SIGINT, conservar_ruta=False):
    p, fifo, log, inicio = caso
    if interrupcion:
        p.send_signal(senal)
    assert p.wait(timeout=35) == 0
    log.close()
    if not conservar_ruta:
        assert not fifo.exists(), 'El FIFO servidor no se elimino'
    texto = (fifo.parent/'servidor.log').read_text()
    assert 'AddressSanitizer' not in texto and 'runtime error:' not in texto, texto
    return texto, time.monotonic()-inicio

def main():
    with tempfile.TemporaryDirectory(prefix='ise-pruebas-') as temporal:
        t = Path(temporal)
        # Los parametros invalidos deben fallar sin crear el FIFO.
        for args in [[], ['-b','0','-p',str(t/'x'),'-t','30','-a','1'],
                     ['-b','1','-p',str(t/'x'),'-t','29','-a','1'],
                     ['-b','1','-p',str(t/'x'),'-t','121','-a','1'],
                     ['-b','1','-p',str(t/'x'),'-t','30','-a','5']]:
            assert ejecutar('servidorISE', *args).returncode != 0
        assert not (t/'x').exists()
        existente = t/'proteger'
        existente.write_text('NO BORRAR')
        assert ejecutar('servidorISE','-b','1','-p',existente,'-t','30','-a','1').returncode != 0
        assert existente.read_text() == 'NO BORRAR'
        print('OK: CLI, rangos y proteccion de rutas existentes', flush=True)

        for i,orden in enumerate(itertools.permutations(('b','p','t','a'))):
            caso = servidor(t/f'banderas_{i}',1,orden=orden)
            texto,_ = finalizar(caso,senal=signal.SIGTERM if i%2 else signal.SIGINT)
            assert 'RESUMEN GENERAL ISE' in texto
        print('OK: 24 ordenes de banderas del servidor y cierre con SIGINT/SIGTERM', flush=True)

        reemplazado = servidor(t/'reemplazado',1)
        reemplazado[1].unlink()
        reemplazado[1].write_text('ARCHIVO AJENO AL SERVIDOR')
        texto,_ = finalizar(reemplazado,conservar_ruta=True)
        assert reemplazado[1].read_text() == 'ARCHIVO AJENO AL SERVIDOR'
        assert 'se conserva el archivo actual' in texto
        print('OK: la limpieza conserva una ruta reemplazada durante la ejecucion', flush=True)

        # Compara las cantidades generadas con los maximos de la tabla.
        for pais, maximos in MAXIMOS.items():
            for porcentaje in [1,75,100]:
                argumentos = ['-p',pais,'-e',porcentaje] if porcentaje==75 else ['-e',porcentaje,'-p',pais]
                r = ejecutar('generadorCSV',*argumentos,cwd=t)
                assert r.returncode == 0, r.stderr
                with (t/f'encuestas_{pais}.csv').open() as f:
                    filas = list(csv.DictReader(f))
                for s, maximo in enumerate(maximos):
                    sector = [x for x in filas if int(x['ID_Sector']) == s]
                    assert len(sector) == (maximo*porcentaje+50)//100
                    assert all(0 <= float(x['Sentimiento']) <= 100 for x in sector)
        print('OK: generador ES/FR/IT al 1%, 75%, 100%', flush=True)

        # El agente debe esperar mientras un escritor tiene bloqueado el CSV.
        archivo_lock = t/'bloqueado.csv'
        fichero(archivo_lock, 'ES', [50]*5)
        with (t/'bloqueado.csv.lock').open('w') as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX)
            p = agente(archivo_lock, t/'sin_servidor')
            time.sleep(0.2)
            assert p.poll() is None
            p.terminate()
            out,err = p.communicate(timeout=3)
            assert p.returncode != 0 and 'fcntl lock' in err, out+err
        print('OK: bloqueo compartido/exclusivo del CSV e interrupcion mientras espera', flush=True)

        archivo_generado = t/'encuestas_ES.csv'
        original = archivo_generado.read_bytes()
        with (t/'encuestas_ES.csv.lock').open('r+') as lock:
            fcntl.lockf(lock, fcntl.LOCK_SH)
            p = subprocess.Popen([str(BASE/'generadorCSV'),'-p','ES','-e','10'],cwd=t,
                                 stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            PROCESOS.append(p)
            time.sleep(0.2)
            assert p.poll() is None
            p.terminate()
            out,err = p.communicate(timeout=3)
            assert p.returncode != 0 and 'fcntl lock' in err, out+err
        assert archivo_generado.read_bytes() == original
        assert not list(t.glob('*.tmp.*'))
        print('OK: escritor espera a lector, interrupcion conserva CSV y no deja temporales', flush=True)

        for contenido in [
            '1,0,ES,Industria,0,nan\n', '1,0,ES,Industria,0,101\n',
            '1,0,ES,Industria,0,-1\n', '1,0,ES,Servicios,0,20\n',
            '1,1,ES,Industria,0,20\n', '1,0,ES,Industria,0,20,extra\n', '',
            '1,0,ES,Industria,0,20\n2,1,FR,Industria,0,20\n',
            '1,0,ES,Industria,0,50\x00basura\n',
            '1,0,ES,Industria,0,50\rbasura\n']:
            ruta = t/'invalido.csv'
            ruta.write_text(contenido)
            r = ejecutar('agentePais','-n',ruta,'-p',t/'inexistente')
            assert r.returncode != 0, contenido
        print('OK: CSV malformado, vacio, NaN, rangos y paises mezclados', flush=True)
        diagnostico = t/'linea_invalida.csv'
        diagnostico.write_text('ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento\n1,0,ES,Industria,x,50\n')
        r = ejecutar('agentePais','-n',diagnostico,'-p',t/'inexistente')
        assert r.returncode != 0 and 'CSV linea 2: ID_Sector' in r.stderr, r.stderr
        print('OK: el error identifica la linea y el campo del CSV', flush=True)

        exceso = t/'exceso_sector.csv'
        fichero(exceso, 'ES', [50,None,None,None,None], repeticiones=1761)
        r = ejecutar('agentePais','-n',exceso,'-p',t/'inexistente')
        assert r.returncode != 0 and 'se supera el maximo del sector' in r.stderr, r.stderr
        print('OK: se rechaza un CSV que supera el maximo del sector', flush=True)

        archivo_pipe = t/'no_es_fifo'
        archivo_pipe.write_text('CONTENIDO ORIGINAL')
        valido = t/'valido.csv'
        fichero(valido, 'ES', [50]*5)
        r = ejecutar('agentePais','-n',valido,'-p',archivo_pipe)
        assert r.returncode != 0 and 'no es un FIFO' in r.stderr, r.stderr
        assert archivo_pipe.read_text() == 'CONTENIDO ORIGINAL'
        print('OK: el agente rechaza un archivo normal como pipe sin modificarlo', flush=True)

        # Inicia los servidores juntos para comprobar sus plazos de 30 segundos.
        sin_datos = servidor(t/'sin_datos', 1)
        detenido = servidor(t/'sin_fin', 1)
        cliente_detenido = ClientePrueba(detenido[1], 5)
        cliente_detenido.enviar(2)
        casos = []
        for nombre, valores, esperado in [
            ('ponderado',[100,0,50,100,0],'ISE = 55.0000% | Vender'),
            ('neutro',[50]*5,'ISE = 50.0000% | Neutro'),
            ('comprar',[0]*5,'ISE = 0.0000% | Comprar'),
            ('incompleto',[50,None,None,None,None],'ISE INCOMPLETO')]:
            caso = servidor(t/nombre, 1)
            archivo = t/f'{nombre}.csv'
            fichero(archivo,'ES',valores)
            a = agente(archivo, caso[1])
            out, err = a.communicate(timeout=10)
            assert a.returncode == 0, out+err
            assert 'Todas las encuestas fueron procesadas' in out
            casos.append((caso,esperado))
        for caso,esperado in casos:
            texto,_ = finalizar(caso)
            assert esperado in texto, texto
            assert 'PARCIAL' in texto and 'RESUMEN GENERAL' in texto
        print('OK: Comprar, Neutro, Vender, pesos y sectores ausentes', flush=True)

        formato_windows = servidor(t/'windows',1)
        archivo = t/'windows.csv'
        fichero(archivo, 'ES', [50]*5)
        datos = archivo.read_text().replace(',', ' , ').replace('\n','\r\n').rstrip('\r\n')
        archivo.write_bytes(b'\xef\xbb\xbf'+datos.encode())
        p = agente(archivo, formato_windows[1],archivo_primero=True)
        out,err = p.communicate(timeout=5)
        assert p.returncode == 0, out+err
        texto,_ = finalizar(formato_windows)
        assert 'ISE = 50.0000% | Neutro' in texto, texto
        print('OK: CSV con UTF-8 BOM, CRLF y espacios; ambos ordenes de banderas del agente', flush=True)

        for nombre,total,valor in [('fin_prematuro',2,5000),('lectura_invalida',1,10001)]:
            caso = servidor(t/nombre,1)
            cliente = ClientePrueba(caso[1],total)
            try:
                cliente.enviar(2,valor)
                cliente.enviar(3)
                assert cliente.recibir(timeout=5).estado != 0
            finally:
                cliente.cerrar()
            texto,_ = finalizar(caso)
            assert 'ENVIO INCOMPLETO' in texto and 'RESUMEN CON DATOS PARCIALES' in texto, texto
        print('OK: FIN prematuro y mensajes invalidos no se anuncian como exito', flush=True)

        varios = servidor(t/'varios', 4, 1)
        agentes = []
        # Envia los tres paises y un cuarto agente con encuestas de ES.
        for pais in MAXIMOS:
            agentes.append(agente(t/f'encuestas_{pais}.csv', varios[1]))
        cuarto = t/'cuarto.csv'
        fichero(cuarto,'ES',[50]*5)
        agentes.append(agente(cuarto,varios[1]))
        for p in agentes:
            out,err = p.communicate(timeout=20)
            assert p.returncode == 0, out+err
            assert 'Todas las encuestas fueron procesadas' in out
        rechazado = agente(cuarto,varios[1])
        out,err = rechazado.communicate(timeout=5)
        assert rechazado.returncode != 0 and 'rechazada' in out, out+err
        texto,_ = finalizar(varios)
        assert 'Total procesado: 56325' in texto, texto
        assert texto.count('=== PARCIAL') == 4, texto
        # Calcula el ISE desde los CSV para compararlo con el reporte del servidor.
        conteos = [1]*5
        sumas = [50.0]*5
        for pais in MAXIMOS:
            with (t/f'encuestas_{pais}.csv').open() as f:
                for fila in csv.DictReader(f):
                    s = int(fila['ID_Sector'])
                    conteos[s] += 1
                    sumas[s] += float(fila['Sentimiento'])
        esperado = sum(sumas[s]/conteos[s]*peso/100
                       for s,peso in enumerate([40,30,20,5,5]))
        assert f'ISE = {esperado:.4f}%' in texto, texto
        print('OK: 4 agentes concurrentes, buffer=1, 56325 registros, quinto rechazado y media global', flush=True)

        # Interrumpe un agente durante el envio y comprueba el cierre del servidor.
        caido = servidor(t/'caido', 1)
        p = agente(t/'encuestas_FR.csv', caido[1])
        time.sleep(0.01)
        if p.poll() is None:
            p.terminate()
        p.communicate(timeout=5)
        texto,_ = finalizar(caido)
        assert 'RESUMEN GENERAL' in texto
        print('OK: cierre con agente interrumpido', flush=True)

        texto,duracion = finalizar(sin_datos, interrupcion=False)
        assert 29.5 <= duracion <= 35, duracion
        assert 'Total procesado: 0' in texto
        print('OK: temporizador de 30s sin agentes, limpieza de FIFO y cierre', flush=True)
        try:
            respuesta = cliente_detenido.recibir(timeout=5)
            assert respuesta.estado != 0 and b'incompleto' in respuesta.texto
        finally:
            cliente_detenido.cerrar()
        texto,duracion = finalizar(detenido,interrupcion=False)
        assert 29.5 <= duracion <= 36, duracion
        assert '=== PARCIAL INCOMPLETO ES' in texto, texto
        assert 'Total procesado: 1' in texto and 'INCOMPLETO' in texto, texto
        print('OK: expira el plazo con agente conectado sin FIN; confirma error y drena buffer', flush=True)
        print('TODAS LAS PRUEBAS PASARON', flush=True)

if __name__ == '__main__':
    try:
        main()
    finally:
        for p in PROCESOS:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
