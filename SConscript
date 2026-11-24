# for module compiling
import os
Import('RTT_ROOT')
from building import *

cwd = GetCurrentDir()
objs = []
list = os.listdir(cwd)

skip_dirs = {
    'Edgi_Talk_M55_WIFISafety',
    'cm33_companion',
    'rt-thread',
    'libraries',
    'build',
    'Debug',
    'download',
    'figures',
    'resources',
    'tools',
    '__pycache__',
}

for d in list:
    if d.startswith('.') or d in skip_dirs:
        continue
    path = os.path.join(cwd, d)
    if os.path.isfile(os.path.join(path, 'SConscript')):
        objs = objs + SConscript(os.path.join(d, 'SConscript'))

Return('objs')
