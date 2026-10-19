# XEChide — kernel source

Plain C sources that `../patches/*.patch` is generated from, kept readable so the
code can be reviewed without patch noise.

```
xechide.c    rule engine, RCU config, hot-path matcher, hook entry points
```

Not a fork of anything: the file depends only on the Linux kernel API plus the
KernelSU supercall table. Wiring and the claimed ioctl ABI live in
`../patches/README.md`.

Rules for changes here (enforced in review):

- never deny uid 0 / 1000 / init;
- reads only — writes always pass;
- deny with `-ENOENT`, never `-EACCES`;
- any malformed input passes through;
- `n_rules == 0` must be a strict no-op.
