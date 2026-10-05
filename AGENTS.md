# AGENTS.md

Notes for an agent editing this repo or reproducing the setup. Facts only. The
README is the prose version.

## Scope

`pieload.c` is an aarch64 Linux userland exec, about 300 lines, no
dependencies. It maps an `ET_EXEC` program and a dynamic loader into its own
address space, builds a stack with `argc`, `argv`, `envp` and an auxiliary
vector, then branches to the loader entry.

It exists for one job: starting a musl build of Claude Code on Termux, where
`execve()` on an app-data file is denied.

## Constraints that are easy to break

- aarch64 only. The register setup and the branch in `main()` are inline asm.
  There is no portable path.
- `ET_EXEC` targets carry a fixed address range. It is reserved with
  `MAP_FIXED_NOREPLACE` and the program exits if the range is taken. Do not
  relax this to `MAP_FIXED`, it would unmap pieload itself.
- The partial bss page must be zeroed by hand after the file-backed mapping.
  The kernel does it on a real `execve`, pieload does not get that.
- Most auxv entries are copied from `/proc/self/auxv`. Only `AT_PHDR`,
  `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_EXECFN` and `AT_FLAGS`
  are overridden. Rebuilding the whole vector loses `AT_HWCAP` and
  `AT_RANDOM`, and musl will abort.
- `LD_PRELOAD` is stripped from the child environment on purpose. On Termux it
  points at `libtermux-exec.so`, which is built against Bionic and fails to
  relocate inside a musl process.

## Seccomp

Android installs a seccomp filter on app processes. A syscall outside the
allowlist raises `SIGSYS`, which kills the process, instead of returning
`ENOSYS`. pieload installs a `SIGSYS` handler, writes a result into
`uc_mcontext.regs[0]` and returns. The syscall number is in `regs[8]`.

`faccessat2` (439) is translated to `faccessat` (48). Everything else gets
`-ENOSYS`.

Do not replace `-ENOSYS` with emulation. Translating `epoll_pwait2` into
`epoll_pwait` runs 25 times longer and then dies anyway: Bun installs its own
`SIGSYS` crash handler late in start up, which replaces this one, and that
handler resets the signal to `SIG_DFL` and kills the process. With `-ENOSYS`
the program picks a fallback once, early, and never raises `SIGSYS` again.

Check a device with:

```sh
make seccomp-probe && ./seccomp-probe 22 439 441
```

## DNS

musl reads `/etc/resolv.conf` and `/etc/hosts` from hardcoded paths. On Android
`/etc` is a symlink to read-only `/system/etc`, and when musl cannot open
`resolv.conf` it falls back to `127.0.0.1`, so every lookup returns
`EAI_AGAIN`.

The paths are patched inside a *copy* of the loader. The replacement string has
to be the same length or shorter, padded with NUL. `/sdcard` is the only
writable location with a short enough path. See the README for the exact
offsets and script.

## Debugging

- Bun installs its own `SIGSEGV` handler, so a crash leaves no tombstone in
  logcat. `strace -f` is the only reliable view.
- Use Termux's `strace` (`pkg install strace`). The one in `/system/bin` exits
  with `Unexpected wait status`.
- A `SIGSEGV` at `si_addr=0x300` with no tombstone is almost always a `SIGSYS`
  one line earlier.
- `PIELOAD_VERBOSE=1` prints the mapping layout.

## Dead ends

Already tried, all fail: flipping `e_type` to `ET_DYN` and calling the musl
loader by hand, repointing `PT_INTERP`, `glibc-runner`, `getauxval` shims
through `LD_PRELOAD`, adding `PT_PHDR` to glibc's `ld.so` (gets past Bionic's
check, then segfaults on double relocation), proot and UserLAnd (ptrace based,
and the restriction is in the kernel).

`qemu-aarch64` works but takes 85 s against 6 to 7 s for the same prompt.

## Style

No comments in the C source unless the reason is non-obvious. No long dashes in
prose. Keep lines under 80 columns.
