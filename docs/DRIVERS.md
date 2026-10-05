# 可加载内核驱动（.arco）

`.arco` 是 ArkOS 的可加载内核驱动映像：随系统分发的驱动都在内核里，而 `.arco` 允许在不重新编译内核的前提下加载、停用和移除一个内核模块。本文档描述格式、装载路径、隔离措施与已知限制；公开调用接口见 [API.md](API.md)，示例见 [sdk/driver_demo.c](../sdk/driver_demo.c)。

## 信任模型

驱动在 Ring 0 执行，因此"谁可以安装"与"安装的字节是否被改动"是两件都必须回答的事。

- **安装权限**：`ARK_SYS_DRIVER` 的 INSTALL 与 REMOVE 需要 SYSTEM 能力、活动会话，并且当前账户是管理员。普通账户即使能看到自己的文件也无法安装内核代码。
- **完整性**：已安装的 `.arco` 文件以 `@drv.<name>` blob 存放在 ArkFS 系统卷的内核命名空间（uid 1000，用户侧 LIST 不可见、`ARK_BLOB` 系统调用对 `@drv.` 名称直接拒绝）。受保护清单 `@drv.manifest`（`ARKD1` 记录）保存每个文件的 **整份 SHA-256**。启动时内核逐个比对，任何一个字节不同即拒绝该模块并继续启动其余模块。清单自身带 CRC-32，损坏时按"无已安装驱动"处理并记录日志。
- ARCO1 头里的 `image_sha256` 只覆盖载荷，用于传输完整性，不是发布者签名：改写镜像同样要改头，而头的 CRC-32 只能发现意外损坏。真正的信任锚是清单里的整份文件哈希。

这与 ArkPkg 包的信任模型一致：安装介质本身不被信任，校验和记录在系统卷里的哈希才是。

## ARCO1 格式

`include/ark_driver.h` 中的 `ArcoHeader`（`__attribute__((packed))`，256 字节）：

| 偏移 | 字段 | 含义 |
|---|---|---|
| 0 | `magic[8]` | `"ARCO1\0\0\0"` |
| 8 | `name[32]` | `[a-z0-9_.-]`，清单内唯一 |
| 40 | `api` | 必须等于 `ARK_DRIVER_API`（当前 1） |
| 44 | `version` | `major<<16 \| minor<<8 \| patch` |
| 48 | `image_size` | 头部之后的镜像字节数，1…1 MiB |
| 56 | `entry_off` | `arco_entry` 在镜像中的偏移，必须落在代码区 |
| 64 | `data_off` | 第一个可写段的偏移，必须 4 KiB 对齐 |
| 72 | `bss_size` | 额外清零的可写字节，4 KiB 的倍数，上限 256 KiB |
| 80 | `image_crc32` | 镜像字节的 CRC-32 |
| 84 | `header_crc32` | 头部自身 CRC-32（本字段置 0 后计算） |
| 88 | `image_sha256[32]` | 镜像字节 SHA-256 |
| 120 | `reserved[136]` | 必须全 0 |

头字段偏移由 `scripts/arco.py` 写入，并由 `tests/module_host_test.c` 用真实打包产物反查，任何一侧改动都会在主机测试中立即暴露。

## 打包

```sh
python3 scripts/arco.py sdk/driver_demo.c -o build/demo.arco --name demo --version 1.0.0
```

打包器以 `-ffreestanding -fno-stack-protector -fPIC -fno-plt -mno-red-zone -mgeneral-regs-only -mcmodel=small` 编译，用 `scripts/arco.ld` 链接到虚拟地址 0 的单一连续映像，并拒绝以下情况：

- 任何动态重定位或 `.rela/.dynamic/.interp` 段——`.arco` 映像逐字节装载，装载器不会打补丁；
- 入口不在只读可执行区；
- 数据边界不是 4 KiB 对齐，或有可写段落在只读区一侧（装载器按页给权限，不能有段跨页混用两种权限）；
- `.bss` 超过预算。

驱动代码里所有引用都必须已经是 RIP 相对形式。链接时使用 `-fvisibility=hidden`，只有 `arco_entry` 需要导出。

## 装载与隔离

`kernel/module.c` 在 `devices_init()` 之后装载，地址空间布局如下：

- **窗口**：内核 PML4 的第 1 项（虚拟地址 `0x80_0000_0000`，512 GiB 起）对所有地址空间可见。每个进程的新 PML4 都是内核 PML4 的完整 4096 字节拷贝，因此窗口内的映射对之后创建的每个进程同样存在，无需逐进程接线。该窗口只有 supervisor 权限，不设 `PTE_USER`，用户态无法访问。
- **权限**：每个槽位 2 MiB 步长；`[0, data_off)` 映射为可读可执行，`[data_off, image_size+bss)` 映射为可读写且 NX。代码页没有写权限，数据页不可执行。
- **栈**：每个模块有自己的 32 KiB 栈，位于槽位顶端，下方一页不映射，栈溢出会直接 #PF 而不是踩到模块数据。入口与 poll 回调都由 `kernel/module_asm.S` 的 `module_stack_call` 切换到该栈后再调用。
- **物理页**：来自内核页池（owner 255）的连续区段，经内核恒等映射拷入；装载器把同一批物理页映射进模块窗口。

诚实说明边界：内核恒等映射本身是 P/W 的大页，因此同一批物理页在恒等映射下仍然是可写的。窗口分离保证的是模块自身视图里的 W^X、以及模块误写自己代码页时会立刻 #PF；它不是针对内核模式攻击者的边界。驱动以 Ring 0 运行，本身就是受信任代码。

## 驱动可用能力

装载器把一张 `ArkDriverHost` 函数表交给 `arco_entry(host, op)`：`op` 为 `ARCO_OP_INIT`(1) 或 `ARCO_OP_DEINIT`(2)，返回负值拒绝装载。表内提供毫秒/滴答计数、限长日志、内核堆分配、PCI 总线枚举与配置空间读写、MMIO 别名（限定在 4 GiB 以内、64 MiB 以内）、以及设备模型的注册/置态/计数/通知/注册 poll。表外没有别的入口：驱动拿不到用户指针、进程状态或页表。

- `device_register_poll` 每个模块只能注册一次；`device_register_poll` 本身没有注销接口，所以每个槽位的桩回调在一次启动内只注册一次，重装时替换模块自己的 poll 指针，由分发层按状态跳过非 LOADED 槽位。
- `device_register` 在 INIT 期间登记到当前槽位，供 REMOVE 时把节点置为 ABSENT。
- `map_mmio` 返回恒等映射的 supervisor 别名，仅用于 4 GiB 以下的设备窗口。
- `irq_attach(irq, isr)` 绑定一条 ISA 中断线（0..15）：内核校验 `isr` 必须落在本模块 RX 代码窗口内，随后为该线分配投递——MADT 中有可用 IOAPIC 时写入对应 RTE（向量仍为 32+line，极性/触发遵循 Interrupt Source Override），否则解除 8259 PIC 屏蔽。中断到达时在模块栈上运行 `isr`，返回后发 EOI（IOAPIC 路径为 LAPIC EOI；PIC 路径为 8259 EOI）。ISR 必须短促——不许分配、阻塞、打日志或调用 host 服务，只应答设备并记录状态，具体工作交给 poll 或 net ops 消费。每条线只能有一个属主；定时器与 PS/2 占用的线（0、1、2、12）以及已被模块占用的线返回 -16；无 IOAPIC 引脚返回 -19。只能在 INIT/DEINIT 里调用；移除模块时内核先释放 RTE / 屏蔽线再运行 DEINIT，卸载再装载不会留下重复或残留条目。内核在串口打印 `[irq] IOAPIC id=N at ADDR gsi_base=B gsi B..E` 或 `[irq] legacy PIC only (原因)`，并在 `dev drivers` 的 detail 中报告绑定线与 ISR 计数（`irqN ... n=`）；未能 `irq_attach` 的驱动必须视为 poll-only，**不能**通过「中断走 IOAPIC、不靠轮询也能动」冒烟门。MSI/MSI-X 与 ACPI `_PRT` 尚未实现。

## 资源上限

| 项目 | 上限 |
|---|---|
| 同时加载的模块 | 8（`ARCO_MODULE_MAX`） |
| 单个镜像 | 1 MiB |
| 单个 `.bss` | 256 KiB |
| 模块栈 | 32 KiB |
| poll 回调 | 每模块 1 个，全系统 8 个（含内核自身注册） |
| ISA 中断线（IOAPIC RTE / PIC） | 每条线 1 个属主，0/1/2/12 归平台占用；仅 0..15，GSI 16+ / MSI 未开放 |
| 已注册设备节点 | 每模块 16 个，全系统 64 个 |

INSTALL 失败（校验不通过或 INIT 拒绝）不会写入清单或镜像；REMOVE 后槽位与映射页归还内核池，同名驱动可以重新安装。

## 用户态入口

```sh
dev                    # 设备清单
dev drivers            # 已安装的可加载驱动
dev query demo         # 名称、状态、字节数与整份文件 SHA-256
dev install /mnt/fat32/demo.arco   # 需要管理员
dev remove demo                    # 需要管理员
```

桌面"系统设置"的设备页只列出驱动状态与它注册的设备索引，不提供安装按钮：安装需要读取本地 `.arco` 文件并确认管理员身份，目前只在终端里完成。块设备原始读取面向应用而非 Shell：`ARK_SYS_DEVICE` 的 READ 需要应用自身获得 DEVICE 能力，Shell 没有这个能力，所以不提供 `dev read`。

## 已知限制

- 移除是逻辑移除：节点置为 ABSENT、清单与镜像删除、槽位归还。已装载过的驱动留下的常驻开销在本次启动内不会回到空闲池（页已归还池，但驱动自己 `ark_alloc` 的堆内存不回收）。
- 没有驱动热插拔、没有模块卸载时的页表 TLB 跨核广播（`invlpg` 在装载时逐页执行；卸载后任何残留项都会被清除，因为槽位 PT 项已置 0）。
- 驱动崩溃是内核 panic：没有模块级的异常隔离开。这是当前设计的有意取舍，不是缺陷。
- 一次启动中 `@drv.*` blob 随 uid 1000 的系统命名空间存在；该账户被删除时驱动记录一并消失。

## 中断投递：IOAPIC 与 8259 回退

启动时 `kernel/ioapic.c` 解析 ACPI MADT（与 SMP 共用 `include/acpi.h` 的表查找）：type 1 IOAPIC、type 2 ISA Interrupt Source Override。存在可响应的 IOAPIC 且 BSP LAPIC 可用（xAPIC MMIO 或 x2APIC MSR，APIC id ≤ 255）时：

- 所有 IOAPIC 引脚先清为“屏蔽 + 向量 0”；两片 8259 IMR 写 0xff 并保持全屏蔽，LAPIC LINT0（ExtINT）屏蔽。
- ISA 线 N 映射到其 GSI（有 override 用 override，否则恒等；被别的 override 占用的恒等 GSI 视为无引脚），向量 32+N，固定投递，物理目的地 = BSP。平台自身路由 0（PIT，QEMU/多数机器为 GSI 2）、1、12。
- `platform_irq_route/release` 是一对分配/释放：释放会把 RTE 写回“屏蔽 + 向量 0”。每次变更后串口打印完整活动 RTE 表与 `leftovers=`/`duplicates=` 计数，卸载→重装后应回到同一张表。
- IOAPIC 投递的 IRQ 以 LAPIC EOI 结束（电平触发同时清 Remote IRR）；不再对 8259 发 EOI，也不做 IRQ7/15 的 PIC 伪中断检查。

MADT 无 IOAPIC、IOAPIC 窗口无响应、CPU 无 LAPIC 或 BSP APIC id > 255 时打印 `[irq] legacy PIC only (...)`，维持原 8259 路径（IRQ0/1/2/12 解除屏蔽、PIC EOI）。

冒烟门可见的串口证据（QEMU q35 实测）：

```
[irq] IOAPIC id=0 at 0xfec00000 gsi_base=0 gsi 0..23 pins=24 ver=0x20
[irq] ISA override irq0 -> gsi2 flags=0x0 (edge/high)
[irq] IOAPIC mode: 1 IOAPIC(s), 5 ISA override(s), dest xAPIC id 0 physical; VT-d interrupt remapping not used (compat RTEs); MSI/MSI-X TODO
[irq] 8259 PIC fully masked: IMR master=0x...ff slave=0x...ff
[irq] RTE table (boot): gsi1=v33 gsi2=v32 gsi12=v44 | active=3 leftovers=0 duplicates=0
[irq] e1000 irq_attach(11) bound -> ioapic0 gsi11 vec43 level/high
```

已知限制：PCI INTx 只按配置空间 Interrupt Line（0..15）恒等映射到 GSI——QEMU 的 PIRQ 同时驱动该 IOAPIC 引脚，因此可用；真实机器（如 13700H）上固件常把 Interrupt Line 置为 0xFF 或依赖 `_PRT` 把 INTx 接到 GSI 16+，NVMe/xHCI 在实机上需要 MSI/MSI-X（或 `_PRT` + GSI 16+ 向量分配），本切片尚未实现。VT-d 中断重映射未启用。
