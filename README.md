# pieload

Userland ELF loader. It runs non-PIE (`ET_EXEC`) Linux binaries on an unrooted
Android device, at full native speed.

Android blocks `execve()` on files in app data. Termux works around this by
asking `/system/bin/linker64` to run the file instead, and that linker only
accepts `ET_DYN`. So a normal Linux binary built as `ET_EXEC` cannot start,
and you get:

```
error: "./program" has unexpected e_type: 2
```

pieload does the kernel's job in user space. `mmap()` with `PROT_EXEC` is still
allowed on app data, because that is how libraries load. pieload uses it to map
the program itself.

## How it works

1. Read the program headers of the target binary.
2. Reserve its fixed address range with `MAP_FIXED_NOREPLACE`, then map each
   `PT_LOAD` at its own `p_vaddr` with `MAP_FIXED`. Zero the partial bss page.
3. Map the dynamic loader (`ET_DYN`) into an anonymous reservation and keep its
   base address.
4. Build a fresh stack: `argc`, `argv`, `envp`, then an auxiliary vector.
   `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_EXECFN` and
   `AT_FLAGS` describe the new program. Everything else is inherited from
   `/proc/self/auxv`, so `AT_HWCAP`, `AT_RANDOM`, `AT_PAGESZ` and
   `AT_SYSINFO_EHDR` stay correct.
5. Install a `SIGSYS` handler for syscalls that Android's seccomp filter
   rejects.
6. Set `sp` and branch to the loader entry point.

`LD_PRELOAD` is dropped from the child environment. On Termux it points to
`libtermux-exec.so`, which is built against Bionic and fails to relocate inside
a musl or glibc process.

## Build

```sh
make
make install
```

Requires clang and aarch64. Tested on Android 13.

## Use

```sh
pieload <dynamic-loader> <program> [args...]
```

Example with a musl build:

```sh
export LD_LIBRARY_PATH=$HOME/sysroot/lib
pieload $HOME/sysroot/lib/ld-musl-aarch64.so.1 ./program --help
```

Set `PIELOAD_VERBOSE=1` to print the mapping layout.

## Seccomp

Android installs a seccomp filter on app processes. Syscalls outside the
allowlist raise `SIGSYS` instead of returning `ENOSYS`, which kills the
process. Recent runtimes hit this often, because the filter is older than the
syscalls they use.

pieload answers `SIGSYS` by writing a result into the saved register context and
returning. `faccessat2` (439) is rewritten to `faccessat` (48). Anything else
gets `-ENOSYS`, which is what a program expects from an old kernel, so its own
fallback path takes over.

`ENOSYS` works better here than real emulation. Many runtimes install their own
`SIGSYS` crash handler during start up and replace this one. By then the program
has already chosen its fallback, so it never raises `SIGSYS` again.

`seccomp-probe` tells you which syscall numbers the filter rejects:

```sh
make seccomp-probe
./seccomp-probe 22 441 439
```

```
22 reaches the kernel
441 blocked by seccomp (SIGSYS)
439 blocked by seccomp (SIGSYS)
```

## Debugging

Install Termux's `strace`. The one in `/system/bin` fails with
`Unexpected wait status`. Many runtimes install a `SIGSEGV` handler, so the
crash produces no tombstone and `strace -f` is the only reliable view.

## Resolver config

musl reads `/etc/resolv.conf` and `/etc/hosts` from fixed paths. On Android
`/etc` is a symlink to the read only `/system/etc`, and binding UDP port 53
needs a privilege an app does not have. When musl cannot open
`/etc/resolv.conf` it falls back to `127.0.0.1`, so DNS times out with
`EAI_AGAIN`.

Patch the paths inside a copy of the loader. The new path has to be the same
length or shorter, padded with NUL. `/sdcard` is usually the only writable
place with a short path:

```sh
cp ld-musl-aarch64.so.1 ld-musl-patched.so.1
printf 'nameserver 1.1.1.1\n' > /sdcard/r.conf
python3 - <<'EOF'
p = "ld-musl-patched.so.1"
b = bytearray(open(p, "rb").read())
off = b.find(b"/etc/resolv.conf")
b[off:off + 16] = b"/sdcard/r.conf".ljust(16, b"\0")
open(p, "wb").write(bytes(b))
EOF
```

## Limits

Works on aarch64 Linux only. The fixed address range of the target binary has
to be free in the loader's own process, which pieload checks before mapping.
Each run keeps one Bionic process image mapped alongside the target, so memory
use is a little higher than a real `execve`.

## License

MIT
