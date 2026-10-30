"""Makes the symbol table the exception log uses (core/diag/guard.cpp) from a DLL or EXE built with DWARF (-g):
for every function its start, and inside our own sources every place where the source line changes.

  python -I tools/build/make_syms.py <llvm-objdump> <module with DWARF> <out.syms>

Format (UTF-8 text, one record per line):
  EMPEROR-SYMS 1 <build id: the module's .buildid section in hex>
  F <file index> <source path>
  N <name index> <function name (demangled)>
  L <rva hex> <name index> <file index or -1> <line or 0>
Rows are sorted by RVA; an address belongs to the last row at or before it. The build id ties the table to one link:
the exception log ignores a table whose id differs from the loaded module's .buildid."""
import re
import struct
import subprocess
import sys

OURS = ('src\\', 'proxy\\')  # our own sources, repository-relative (DWARF is only built for them anyway)


def build_id(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    for i in range(n):
        o = pe + 24 + opt + i * 40
        name = d[o:o + 8].rstrip(b'\0')
        vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
        if name == b'.buildid':
            # Bytes 24-27 are the debug directory's PointerToRawData, a file offset: llvm-strip changes it in
            # ddraw.dll (made from ddraw.dbg.dll, which this reads), so it is left out on both sides (0).
            b = bytearray(d[ra:ra + min(vs, rs)])
            b[24:28] = bytes(len(b[24:28]))
            return base, b.hex()
    raise SystemExit('%s: no .buildid section' % path)


def main():
    objdump, module, out = sys.argv[1:4]
    base, bid = build_id(module)
    text = subprocess.run([objdump, '-d', '-l', '-C', '--no-show-raw-insn', module], capture_output=True, text=True,
                          errors='replace', check=True).stdout
    sym = re.compile(r'^([0-9a-f]{8}) <(.*)>:$')
    src = re.compile(r'^; (.+):(\d+)$')
    insn = re.compile(r'^\s*([0-9a-f]+):')  # not padded: 6 digits in an exe at 0x400000
    files, names, rows = {}, {}, []
    func, pending, last = None, None, None
    for line in text.splitlines():
        m = sym.match(line)
        if m:
            func = names.setdefault(m.group(2), len(names))
            rows.append((int(m.group(1), 16) - base, func, -1, 0))
            pending, last = None, None
            continue
        m = src.match(line)
        if m:
            path = m.group(1).replace('/', '\\')
            for o in ('\\src\\', '\\proxy\\'):  # repository-relative: short, and no user folder in the log
                if o in path:
                    path = path[path.index(o) + 1:]
                    break
            pending = (files.setdefault(path, len(files)), int(m.group(2))) if path.startswith(OURS) else None
            continue
        m = insn.match(line)
        if m and pending is not None and func is not None:
            if pending != last:
                rows.append((int(m.group(1), 16) - base, func, pending[0], pending[1]))
                last = pending
            pending = None
    rows.sort()
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('EMPEROR-SYMS 1 %s\n' % bid)
        for path, i in sorted(files.items(), key=lambda kv: kv[1]):
            f.write('F %d %s\n' % (i, path))
        for name, i in sorted(names.items(), key=lambda kv: kv[1]):
            f.write('N %d %s\n' % (i, name))
        for rva, func, fi, ln in rows:
            f.write('L %x %d %d %d\n' % (rva, func, fi, ln))
    print('make_syms: %s: %d functions, %d source files, %d rows, build id %s' % (out, len(names), len(files),
                                                                                  len(rows), bid[:16]))


if __name__ == '__main__':
    main()
