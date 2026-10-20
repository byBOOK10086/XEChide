# NOTICE

## License

XEChide is licensed under the GNU General Public License v3.0 (see `LICENSE`).
The kernel part (`kernel/*.c`, `.h`, Kbuild fragments) is GPL-2.0-only, matching
the Linux kernel it is compiled into; each file's SPDX header says so.

## Independence

XEChide is a **standalone project**. It does not ship inside, import from, or
depend on any other KernelSU fork, module, app or design system. The only
external thing it relies on is the public KernelSU kernel interface (the
supercall table and the manager-authentication check), which is GPL-3.0 and is
attributed below.

## Built on

- **Linux kernel** — GPL-2.0-only. The hooks XEChide registers
  (`security_inode_permission`, `security_inode_getattr`, the directory actor,
  and a `binder_transaction` kprobe) are standard kernel APIs.
- **KernelSU** (`tiann/KernelSU`, GPL-3.0) — the supercall dispatch table and the
  "only the authenticated manager may call" permission flag. XEChide's ioctls
  are new numbers in a reserved range; no KernelSU source is copied here.
- **Zygisk** (part of Magisk, GPL-3.0) — the module form runs as a Zygisk
  module. The ABI structs in `module/src/zygisk_api.h` were written from the
  published interface documentation; no Magisk or Zygisk source is copied or
  vendored. The module uses the loader's public `pltHookRegister` API.

## No third-party code was copied

- **HMA-oss** (Hide My Apps): compared against in `docs/comparison-hma.md` from
  public documentation only — no code, assets or resource files were taken.
- **LSPosed / Shizuku**: Java hook frameworks rejected by design; not vendored.
- **TrickyStore** (ships without a licence) and **OhMyKeymint** (AGPL-3.0): not
  used, not referenced.

## Trademarks

Android and KernelSU are trademarks of their respective owners. This project is
not affiliated with Google or the KernelSU maintainers.
