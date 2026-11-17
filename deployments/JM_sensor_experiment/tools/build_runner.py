#!/usr/bin/env python3
"""Link the experiment against the existing release build's OHQ/OHTwin objects."""
from pathlib import Path
import shlex
import subprocess

repo = Path(__file__).resolve().parents[3]
build = repo/'build-qmake'
values = {}
for line in (build/'Makefile').read_text().splitlines():
    if '=' in line:
        key, value = line.split('=', 1)
        if key.strip() in ('INCPATH','DEFINES','LIBS','LFLAGS'):
            values[key.strip()] = shlex.split(value.replace('$(SUBLIBS)', ''))
output = repo/'deployments/JM_sensor_experiment/outputs/experiment'
output.parent.mkdir(exist_ok=True)
objects = [str(p.relative_to(build)) for p in (build/'obj').glob('*.o') if p.name != 'main.o']
if not objects:
    raise SystemExit('Build the release runner first: make -C build-qmake -j4')
command = ['g++','-std=c++17','-fPIC','-fopenmp'] + values['DEFINES'] + values['INCPATH']
command += [str(Path(__file__).with_name('run_experiment.cpp'))] + objects + values['LFLAGS'] + values['LIBS'] + ['-o', str(output)]
subprocess.run(command,cwd=build,check=True)
print(output)
