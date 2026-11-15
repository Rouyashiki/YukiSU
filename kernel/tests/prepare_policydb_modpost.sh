#!/bin/bash
set -euo pipefail
task_src=/opt/ddk/src/$(basename "$KDIR")
task_fix=/probe/modpost-tool
mkdir -p "$task_fix/scripts" "$task_fix/include/linux"
cp -a "$task_src/scripts/mod" "$task_fix/scripts/"
cp -a "$task_src/scripts/include" "$task_fix/scripts/" 2>/dev/null || true
cp "$task_src/include/linux/"{mod_devicetable.h,license.h} "$task_fix/include/linux/"
if [ -f "$task_src/include/linux/module_symbol.h" ]; then
    cp "$task_src/include/linux/module_symbol.h" "$task_fix/include/linux/"
fi
cp "$KDIR/scripts/mod/"{elfconfig.h,devicetable-offsets.h} "$task_fix/scripts/mod/"
python3 - <<'PY'
from pathlib import Path
import re
p = Path('/probe/modpost-tool/scripts/mod/modpost.c')
text = p.read_text()
for statement in ('s->module = exp->module;', 'check_exports(mod);'):
    text, n = re.subn(r'^//([ \t]*' + re.escape(statement) + ')$', r'\1', text, flags=re.M)
    assert n == 1, (statement, n)
p.write_text(text)
PY
sources=("$task_fix/scripts/mod/modpost.c" "$task_fix/scripts/mod/file2alias.c" "$task_fix/scripts/mod/sumversion.c")
if [ -f "$task_fix/scripts/mod/symsearch.c" ]; then
    sources+=("$task_fix/scripts/mod/symsearch.c")
fi
clang -O2 -std=gnu11 -I "$task_fix/scripts/mod" -I "$task_fix/scripts/include" "${sources[@]}" -o "$task_fix/modpost"
