# Building native programs for the Harmony 900 (QNX Neutrino 6.3.2, ARM)

The 900's programs were built by Logitech with GCC 3.3.5 (`qnx-nto`) in
the old ARM ABI (APCS-GNU, `e_flags` 0x2, soft float), load at 0x100000
and are started by `/usr/lib/ldqnx.so.2`. `qnxcc.py` produces the same
kind of executable with stock tools (`arm-none-eabi-gcc`, `ld.lld`), no
QNX SDK:

- **ABI**: `arm-none-eabi-gcc -mabi=apcs-gnu -mcpu=arm926ej-s -msoft-float`
  gives the old layout (`long long` 4-byte aligned, 4-byte enums,
  `long long` arguments in odd registers). Floating point is not used yet:
  the word order of soft-float doubles has not been checked against QNX.
- **Startup**: [`crt0.S`](crt0.S) does what QNX's crt1 does (disassembled
  from `hao`): the loader passes an exit function in `r0`; `_start` calls
  `_init_libc(argc, argv, envp, auxv, exitfunc)` and `exit(main(...))`.
- **Libraries**: the boot image's libraries have no section headers, so
  they cannot be linked against directly. `qnxcc.py` generates stub
  libraries with the same sonames that only define the symbols named with
  `-l soname:sym,sym`; `ldqnx.so.2` resolves them on the remote.
- **Header fixups**: SysV hash, no `PT_GNU_STACK`, no RELRO, GOT[0] =
  `_DYNAMIC` and `e_flags` 0x2, as in the remote's own programs.
- [`include/qnx63.h`](include/qnx63.h) declares the few functions used.
  The `env_*` functions are in `libcdf_client.so` (boot image); their
  signatures follow from the tolua wrappers in `libenv_svc.so`:
  `int env_x(args..., int *result)`, after
  `env_service_connect("127.0.0.1")`.
- [`dynsyms.py`](dynsyms.py) lists or checks the exported symbols of the
  remote's libraries from their program headers alone, e.g.
  `dynsyms.py backup/h900/ifs-app/usr/lib/libgf.so.1 gf_draw_rect`, so a
  stub symbol that the remote does not have is caught before writing.
