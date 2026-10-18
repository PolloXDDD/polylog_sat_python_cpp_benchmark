#!/usr/bin/env python3
"""Regenerate the Spanish report from recorded results (no new measurements)."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
summary = json.loads((ROOT/"results/summary.json").read_text())
env = json.loads((ROOT/"results/environment.json").read_text())
validation = json.loads((ROOT/"validation.json").read_text())
manifest = json.loads((ROOT/"results/manifest.json").read_text())
runs = [json.loads(line) for line in (ROOT/"results/raw.jsonl").read_text().splitlines()]
index = {(r['family'], r['n'], r['engine']): r for r in summary}

def at(family, n, engine):
    return index[family, n, engine]

def milliseconds(r, field="total_seconds"):
    return f"{r[field + '_median']*1000:,.3f}"

def count(x):
    return f"{x:,.0f}" if float(x).is_integer() else f"{x:,.1f}"

text = ["# Benchmark: búsqueda por configuraciones", "",
        "Implementación del motor de la sección 3 del paper de Kaoru Aguilera Katayama (1 de octubre de 2026), con un adaptador CNF explícito y un control modular. La correspondencia y los supuestos están en README.md.", "",
        f"**{sum(r['correct'] for r in runs)}/{len(runs)} ejecuciones con respuesta correcta**, sobre {len(manifest)} casos distintos. {env['parameters']['repeats']} repeticiones por caso y lenguaje. Ningún timeout en esta corrida. Además, {validation['total_checks']} comprobaciones previas superadas.", "",
        "## Medición", "",
        f"- Fecha UTC: {env['created_utc']}.",
        f"- CPU: {env['cpu']}; un solo proceso a la vez, fijado al CPU {env['affinity']}.",
        f"- Python: {env['python'].splitlines()[0]}.",
        f"- C++: {env['compiler']}; `-O3 -DNDEBUG -std=c++17`.",
        "- Todos los tiempos son medianas de tiempo de pared. Los solvers trabajan en un solo hilo.",
        "- **Total interno** incluye lectura de CNF, preparación, grafo, reconstrucción y comprobación del certificado; excluye arranque, importaciones y serialización de salida.",
        "- **Grafo** incluye solamente búsqueda y reconstrucción. Los tiempos de proceso completo también están en los CSV.",
        "- Memoria: máximo residente `VmHWM` de Linux, en MiB; incluye runtime y entrada. No se resta una línea base.",
        "- Procesos nuevos, orden de lenguajes alternado y una ejecución pequeña de calentamiento por lenguaje. Watchdog externo: 30 s por ejecución.", "",
        "## 3-SAT aleatorio", "",
        "Ocho instancias por tamaño, tres variables diferentes por cláusula, signos equiprobables y m = round(4.26n). La referencia de SAT/UNSAT es un DPLL independiente. Las medianas agrupan las ocho semillas y sus tres repeticiones: 24 observaciones por celda.", "",
        "| Variables | Cláusulas | SAT / UNSAT | Python total (ms) | C++ total (ms) | Python grafo (ms) | C++ grafo (ms) | Estados/capa, mediana |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|"]
for n in env['parameters']['random_sizes']:
    py, cpp = at('random3sat', n, 'python'), at('random3sat', n, 'cpp')
    text.append(f"| {n} | {round(4.26*n)} | {py['sat_cases']} / {py['unsat_cases']} | {milliseconds(py)} | {milliseconds(cpp)} | {milliseconds(py,'graph_seconds')} | {milliseconds(cpp,'graph_seconds')} | {count(py['peak_frontier_median'])} |")
text += ["", "Las medianas de cantidades enteras pueden terminar en .5 al agregar un número par de instancias. Son estadísticas de varias fórmulas, no una cantidad fraccionaria de estados de una ejecución.", "",
         "## CNF estructurados", "",
         "Medianas de tres repeticiones por caso; tiempo total interno. Todas las igualdades son SAT. Las cadenas UNSAT fuerzan valores diferentes en dos extremos que deberían coincidir.", "",
         "| Caso | Variables | Cláusulas | Python (ms) | C++ (ms) | Máximo de estados/capa |",
         "|---|---:|---:|---:|---:|---:|"]
labels = {'chain_sat':'Cadena SAT', 'chain_unsat':'Cadena UNSAT',
          'separated_equalities':'Igualdades, orden separado', 'interleaved_equalities':'Igualdades, orden intercalado'}
for family, n in [('chain_sat',100),('chain_sat',1000),('chain_sat',10000),('chain_unsat',10000)] + [(f,2*h) for h in env['parameters']['gaps'] for f in ('separated_equalities','interleaved_equalities')]:
    py, cpp = at(family,n,'python'),at(family,n,'cpp')
    m = next(c['m'] for c in manifest if c['family']==family and c['n']==n)
    text.append(f"| {labels[family]} | {n:,} | {m:,} | {milliseconds(py)} | {milliseconds(cpp)} | {count(py['peak_frontier_median'])} |")
text += ["", "**Hallazgo:** con 18 igualdades xᵢ = yᵢ y 36 variables, leer primero todos los x conserva 262 144 estados diferentes; intercalar cada x con su y deja un máximo de dos. Es exactamente el mismo problema tras renombrar variables. Se observa una dependencia del orden y de la memoria del verificador, incluso para fórmulas sencillas de 2-SAT.", "",
         "El aumento de 2^h es exacto para esta familia: después de leer los h valores de x, cada prefijo exige una continuación y diferente. Antes de leer y, esas memorias no se pueden fusionar con este orden y este estado.", "",
         "## Control modular", "",
         "Como máximo M residuos por capa y q = ceil(log2 M)² bits de certificado. Tiempo de grafo; este control no es SAT. La complejidad se expresa en M, no en la longitud binaria de M.", "",
         "| M | q | Configuraciones totales | Python grafo (ms) | C++ grafo (ms) |",
         "|---:|---:|---:|---:|---:|"]
for c in manifest:
    if c['family'] != 'modular': continue
    py,cpp=at('modular',c['n'],'python'),at('modular',c['n'],'cpp')
    text.append(f"| {c['n']:,} | {c['bits']} | {count(py['configurations_median'])} | {milliseconds(py,'graph_seconds')} | {milliseconds(cpp,'graph_seconds')} |")
text += ["", "Para M = 4096 y q = 144 hay 2^144 certificados posibles, pero la búsqueda visita 548 863 configuraciones. Este dato muestra la fusión efectiva de prefijos; no supone que SAT tenga un verificador con esas mismas cotas.", "",
         "## Memoria residente", "",
         "Medianas por lenguaje. Para 3-SAT se agregan semillas y repeticiones; las demás filas corresponden a un caso repetido tres veces.", "",
         "| Caso | Python (MiB) | C++ (MiB) |",
         "|---|---:|---:|"]
for family,n,label in [('random3sat',36,'3-SAT aleatorio, n=36'),('chain_sat',10000,'Cadena SAT, n=10 000'),
                       ('separated_equalities',36,'Igualdades separadas, n=36'),('modular',4096,'Modular M=4096, q=144')]:
    py,cpp=at(family,n,'python'),at(family,n,'cpp')
    text.append(f"| {label} | {py['peak_rss_kib_median']/1024:.2f} | {cpp['peak_rss_kib_median']/1024:.2f} |")
text += ["", "## Corrección y alcance", "",
         "- 512 fórmulas: todos los subconjuntos de las nueve cláusulas no tautológicas posibles sobre dos variables, incluida la vacía.",
         "- 300 fórmulas aleatorias pequeñas contrastadas por tabla de verdad y DPLL; 14 casos de máscaras de más de 64 bits; ocho casos CNF de borde y un caso de parser multilínea.",
         "- 84 controles modulares, siete rechazos de DIMACS inválido, dos pruebas de límites con respuesta UNKNOWN y una prueba de longitudes de certificado.",
         "- En los CNF contrastados se comparan también certificados, ancho de cada capa, número de configuraciones y transiciones entre Python y C++.",
         "- Cada testigo SAT del benchmark se comprueba contra el CNF original. Los UNSAT aleatorios se contrastan con DPLL y los estructurados se conocen por construcción. No se emiten certificados DRAT/LRAT de UNSAT.",
         "- Son datos sintéticos y una muestra pequeña; no son una evaluación en SAT Competition ni una comparación con MiniSat, CaDiCaL o Glucose.",
         "- La máquina es virtualizada; tres repeticiones no eliminan toda variación del anfitrión. Los tiempos muy cortos son especialmente sensibles al ruido.", "",
         "El motor del paper funciona para el verificador que se le proporciona. El adaptador CNF aquí añadido puede requerir memoria creciente y exponencialmente muchos estados. Estos resultados no demuestran P=NP ni P≠NP; documentan una implementación ejecutable de la construcción y sus costes en verificadores concretos.", "",
         "## Archivos y reproducción", "",
         "```bash", "python3 validate.py", "python3 benchmark.py --cpu 0", "python3 make_report.py", "python3 plot_results.py", "```", "",
         "Los dos primeros pasos solo requieren Python y un compilador C++17. Matplotlib se usa únicamente en la figura opcional. Se incluyen todos los CNF, semillas, respuestas, hashes, entorno, observaciones y agregados en `instances/` y `results/`.", "",
         "![Tiempos y crecimiento de estados](benchmark.png)", ""]
(ROOT/'BENCHMARK.md').write_text('\n'.join(text),encoding='utf-8')
print('Wrote BENCHMARK.md')
