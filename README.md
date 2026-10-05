# pieload

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Language: C](https://img.shields.io/badge/language-C-555555.svg)
![Arch: aarch64](https://img.shields.io/badge/arch-aarch64-orange.svg)
![Termux](https://img.shields.io/badge/Termux-Android%2013-3DDC84.svg?logo=android&logoColor=white)
![Claude Code](https://img.shields.io/badge/Claude%20Code-2.1.289-D97757.svg)
Run non-PIE aarch64 Linux binaries on Android from userland, at native speed.
Built to run Claude Code with Opus 5.5 on Termux.

Page: https://davutkaraoglan.github.io/pieload/ · For agents: [llms.txt](llms.txt)

Newer Claude Code releases, the ones that support Opus 5.5, ship only as
non-PIE Linux binaries. Android refuses to run those: SELinux blocks `execve`
on app data, and Bionic's linker only accepts `ET_DYN`. qemu works, but one
prompt takes about 85 seconds.

pieload loads the binary itself. It maps the segments with `mmap`, builds the
stack and auxiliary vector by hand, and jumps into the musl loader. The same
prompt takes 6 to 7 seconds.

## ⚙️ What it does

1. Reads the program headers of the target.
2. Reserves its fixed address range with `MAP_FIXED_NOREPLACE`, then maps each
   `PT_LOAD` at its own `p_vaddr`. Zeroes the partial bss page.
3. Maps the dynamic loader into an anonymous reservation and keeps its base.
4. Builds a fresh stack with `argc`, `argv`, `envp` and an auxiliary vector.
   `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_EXECFN` and
   `AT_FLAGS` describe the new program. The rest is copied from
   `/proc/self/auxv`, which keeps `AT_HWCAP`, `AT_RANDOM`, `AT_PAGESZ` and
   `AT_SYSINFO_EHDR` correct.
5. Installs a `SIGSYS` handler, see below.
6. Sets `sp` and branches to the loader entry.

It also drops `LD_PRELOAD` from the child environment. On Termux that points
at `libtermux-exec.so`, which is built against Bionic and fails to relocate
inside a musl process. If `PIELOAD_PRELOAD` is set, its value becomes the
child's `LD_PRELOAD`, which is how `execshim.so` gets in.

## 🔨 Build

```sh
make
make install
```

clang, aarch64. Tested on Android 13.

##  Use

```sh
pieload <dynamic-loader> <program> [args...]
```

`PIELOAD_VERBOSE=1` prints the mapping layout.

##  Running Claude Code with it

`cc5` in this repo is the wrapper. The setup it expects:

```sh
mkdir -p ~/ccmusl && cd ~/ccmusl
```

Get the musl build. `npm` refuses it, because it reports `os: android` and the
package asks for linux, so fetch the tarball directly:

```sh
V=2.1.289
curl -sL "https://registry.npmjs.org/@anthropic-ai/claude-code-linux-arm64-musl/-/claude-code-linux-arm64-musl-$V.tgz" | tar xz
```

Take `ld-musl-aarch64.so.1` from an Alpine `musl` package and put it in
`~/ccmusl/root/lib/`.

Then patch a copy of the loader for DNS, see the next section, and run:

```sh
cp cc5 $PREFIX/bin/ && cc5
```

Your existing `~/.claude` session is used as it is. The old `claude` install
stays where it is and keeps working.

## 🌐 DNS

musl reads `/etc/resolv.conf` and `/etc/hosts` from fixed paths. On Android
`/etc` is a symlink to `/system/etc`, which is read only, and binding UDP port
53 needs a privilege an app does not get. When musl cannot open
`/etc/resolv.conf` it falls back to `127.0.0.1`, and every lookup ends in
`EAI_AGAIN`.

The fix is to patch the paths inside a copy of the loader. The replacement has
to be the same length or shorter, padded with NUL. `/sdcard` is the only
writable place with a short enough path:

```sh
cd ~/ccmusl/root/lib
cp ld-musl-aarch64.so.1 ld-musl-native.so.1
printf 'nameserver 1.1.1.1\n' > /sdcard/r.conf
python3 - <<'EOF'
p = "ld-musl-native.so.1"
b = bytearray(open(p, "rb").read())
off = b.find(b"/etc/resolv.conf")
b[off:off + 16] = b"/sdcard/r.conf".ljust(16, b"\0")
open(p, "wb").write(bytes(b))
EOF
```

The original loader is left alone, so the qemu path still works if you want to
compare.

## Seccomp

This part is the expensive one to diagnose. Android installs a seccomp
filter on app processes. Syscalls outside the allowlist raise `SIGSYS`, which
kills the process, instead of returning `ENOSYS`. The filter is older than the
syscalls Bun uses, so Bun walks straight into it.

The symptom is a `SIGSEGV` at `0x300` after start up, with no tombstone,
because Bun installs its own `SIGSEGV` handler. `strace -f` showed the real
cause one line above the crash:

```
--- SIGSYS {si_code=SYS_SECCOMP, si_syscall=__NR_epoll_pwait2} ---
--- SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x300} ---
```

pieload answers `SIGSYS` by writing a result into the saved register context
and returning. `faccessat2` (439) becomes `faccessat` (48). Everything else
gets `-ENOSYS`.

Returning `ENOSYS` works better than emulating the call. Translating
`epoll_pwait2` into `epoll_pwait` properly gets 25 times further and then dies
anyway: Bun installs its own `SIGSYS` crash handler late in start up, replaces
this one, and that handler resets the signal to `SIG_DFL` and kills itself.
With `ENOSYS` the program picks its fallback once, early, and never raises
`SIGSYS` again.

`seccomp-probe` lists what the filter rejects on your device:

```sh
make seccomp-probe
./seccomp-probe 22 441 439
```

```
22 reaches the kernel
441 blocked by seccomp (SIGSYS)
439 blocked by seccomp (SIGSYS)
```

## Commands

Claude Code runs its shell commands by starting `bash` from Termux. That
`execve` comes from the musl process, where `libtermux-exec.so` cannot load,
so SELinux rejects it with `EACCES` and every Bash tool call fails.

`execshim.so` does what `libtermux-exec.so` does, built for musl. pieload loads
it into the musl process. It wraps `execve`, `execv`, `execvp`, `execvpe`,
`posix_spawn` and `posix_spawnp`:

- Programs under `/system`, `/apex` and `/vendor` start directly.
- Everything else starts through `/system/bin/linker64`.
- `#!` scripts start their interpreter the same way. `/bin/` and `/usr/bin/`
  map to `$PREFIX/bin/`.
- The child gets Termux's own `LD_PRELOAD` back, so commands it runs work
  as usual.

`make install` puts it in `~/ccmusl/root/lib/`, and `cc5` turns it on when it
finds it there.


## Speed

Same prompt, `-p "reply with exactly: ok"`, same model:

| path | wall |
| --- | --- |
| pieload | 6 to 7 s |
| qemu-aarch64 | 85 s |

CPU time under pieload is about 3.5 s, so most of what is left is the API
round trip. Tool use with a local file read came in at 10.5 s.

## Debugging

Use Termux's `strace`. The one in `/system/bin` quits with
`Unexpected wait status`. Since Bun handles `SIGSEGV` itself you get no
tombstone, so `strace -f` is the only honest view of what happened.

## ?Limits

aarch64 Linux targets only. The target's fixed address range has to be free in
pieload's own process, which it checks before mapping. One Bionic process image
stays mapped next to the target, so memory use is a bit above a real `execve`.

Dead ends, in case you were about to try them: flipping `e_type` to `ET_DYN`
and calling the musl loader by hand, repointing `PT_INTERP`,
`glibc-runner`, `getauxval` shims through `LD_PRELOAD`, and adding `PT_PHDR` to
glibc's `ld.so`. The last one gets past Bionic's check and then segfaults on
double relocation. proot and UserLAnd do not help either, since they are
ptrace based and the restriction is in the kernel, not in user space.

## 📄 License

MIT
