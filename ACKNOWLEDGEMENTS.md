# 致谢上游 / Acknowledgements

XEChide 的两种形态都建立在以下上游项目的**公开接口与生态**之上。代码没有复制；
许可证边界见 [`NOTICE.md`](NOTICE.md)，完整许可文本在 `LICENSES/`。

XEChide would not exist without the public interfaces and ecosystems below.
No implementation code is copied; see `NOTICE.md` for the licence boundaries.

| 上游 / Upstream | 用到的部分 / What we use | 许可证 / License | 说明 / Note |
|---|---|---|---|
| [KernelSU](https://github.com/tiann/KernelSU) | 内核端 patch 的基线（`0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2`）、supercall dispatch、`ksu_lsm_hook()`、manager 认证 | GPL-3.0（内核文件 GPL-2.0） | 只调用公开接口；没有复制 KernelSU 源码。感谢 KernelSU 作者与贡献者。 |
| [Magisk / Zygisk](https://github.com/topjohnwu/Magisk) | 模块端的加载器与 public Zygisk module API v4（`registerModule` / `connectCompanion` / `pltHookRegister` / `pltHookCommit`） | GPL-3.0 | `module/src/zygisk_api.h` 是独立编写的 ABI 声明，不是从 Magisk 复制；没有 vendor 任何实现代码。 |
| [LSPlt](https://github.com/LSPosed/LSPlt) | Zygisk 的 `pltHookRegister` 内部使用的 PLT hook 实现 | LGPL-3.0 | 只经 Zygisk API 间接使用；本仓库没有 vendor/链接/修改 LSPlt 代码。 |
| [Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext) | KernelSU/APatch 上的可选 Zygisk 运行时 | 以上游为准 | 外部运行时，本仓库不分发；感谢其兼容性工作。 |
| [KernelPatch](https://github.com/bmax121/KernelPatch) / [APatch](https://github.com/bmax121/APatch) | 原生 KPM：KPM 加载器、`hook_syscalln`/`hook_wrap`/`kallsyms_lookup_name` 运行时 API、`kpm.h` 与模块格式 | GPL-2.0 | 固定使用 KernelPatch 提交 `052c73f9…` 的公开头文件（构建时拉取，不 vendor）；`kpm/` 下的 `.kpm` 是真 KPM，不是脚本包装/LKM。感谢 bmax121 与 KernelPatch/APatch 社区。 |
| [Linux kernel](https://kernel.org/) | LSM / kprobe / binder UAPI 结构 | GPL-2.0 | 只使用标准内核接口。 |
| [Android Open Source Project](https://source.android.com/)、[AndroidX](https://developer.android.com/jetpack/androidx)、[Jetpack Compose](https://developer.android.com/jetpack/compose) | manager 的运行平台与 UI 依赖 | Apache-2.0 | 构建依赖，不 vendor 源码。 |
| [HMA-oss](https://github.com/Dr-TSNG/Hide-My-Applist) | 对比对象与问题定义 | 以上游为准 | 没有使用其代码/资源；`docs/comparison-hma.md` 只做行为对比。感谢 HMA 作者把这个问题做透。 |
| [Zig](https://ziglang.org/) | 编译 freestanding 模块 `.so` 与静态 helper 的编译器 | MIT | 仅构建工具，不进入产物逻辑。 |

## 特别感谢 / Special thanks

- KernelSU 与 Magisk 社区把 supercall、Zygisk 和模块生态的接口公开下来，才可能有
  "内核端 / 模块端同配置"的这种设计。
- HMA-oss 用 Java hook 把"应用隐藏"做成了可用产品；XEChide 的模块端是在它的
  问题空间里换了一条更靠底的实现路线（原生 parcel 过滤 + 文件层），不是抄它的实现。
- 所有提交 issue、做真机验证、指出检测面遗漏的人。

## 没有使用 / Not used

TrickyStore（无许可证）、OhMyKeymint（AGPL-3.0）以及任何无许可证的上游代码都没有
被编译、分发或引用。详见 `NOTICE.md`。
