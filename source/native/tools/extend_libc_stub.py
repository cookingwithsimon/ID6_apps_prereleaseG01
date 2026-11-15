"""Build a link-only libc.so.3 that adds symbols the bmw-tools import list omits.

The head-unit binary records SONAME libc.so.3, so the car's own libc resolves
the calls. The udf bodies are never copied onto the car and never executed.
"""
import subprocess
import sys


def dyn_symbols(readelf, libc):
    text = subprocess.check_output([readelf, "--dyn-syms", "-W", libc], text=True)
    found = []
    for line in text.splitlines():
        parts = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(parts) < 8 or parts[0][-1] != ":":
            continue
        size, kind, bind, name = parts[2], parts[3], parts[4], parts[7]
        if bind != "GLOBAL" or kind not in ("FUNC", "OBJECT"):
            continue
        found.append((name, kind, int(size)))
    return found


def main():
    if len(sys.argv) != 7:
        sys.exit("usage: extend_libc_stub.py CLANG READELF LD libc.so.3 output.so output.s")
    clang, readelf, ld, libc, output, assembly = sys.argv[1:]
    symbols = dyn_symbols(readelf, libc)
    names = {name for name, _, _ in symbols}
    for extra in ("getppid", "lstat", "rmdir"):
        if extra not in names:
            symbols.append((extra, "FUNC", 0))
    lines = [
        "/* generated link stub, not a car payload */",
        ".text",
    ]
    for name, kind, size in symbols:
        lines.append(".global %s" % name)
        if kind == "FUNC":
            lines.append(".type %s, %%function" % name)
            lines.append("%s:" % name)
            lines.append("    udf #0")
            lines.append(".size %s, .-%s" % (name, name))
        else:
            lines.append(".type %s, %%object" % name)
            lines.append(".bss")
            lines.append(".balign 4")
            lines.append("%s:" % name)
            lines.append("    .zero %d" % max(size, 4))
            lines.append(".size %s, %d" % (name, max(size, 4)))
            lines.append(".text")
    open(assembly, "w").write("\n".join(lines) + "\n")
    obj = output + ".o"
    subprocess.check_call([
        clang, "--target=armv7-none-eabi", "-march=armv7-a", "-c", assembly, "-o", obj
    ])
    subprocess.check_call([
        ld, "-shared", "-m", "armelf", "--soname=libc.so.3", obj, "-o", output
    ])


if __name__ == "__main__":
    main()
