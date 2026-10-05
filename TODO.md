# ArkOS 交接 TODO（临时 · 2026-10-05）

后人从这里接着做。额度紧时优先 **#7 合入**，其余按序。

## 仓库与身份

| 项 | 值 |
|---|---|
| 上游 | https://github.com/ArkUI-Project/ArkOS |
| Fork | https://github.com/heDuke/ArkOS |
| 上游 `main` 头（交接时） | `687bd6da`（已含 #1–#5：ArkFS2、VFS、加密核心、`.arco` 驱动） |
| 本机/CI 身份 | `gh` → heDuke |

## 进行中 / 待合

### PR #7 — 安装 GPT + 加密安装 UI + 解锁卡（优先合）

- 链接：https://github.com/ArkUI-Project/ArkOS/pull/7
- 分支：`install-unlock-local` @ `91b9893`
- Reviewer：**已签字可合**（2026-10-05）
- Tester 机验（QEMU）：不加密安装 PASS；加密安装+解锁（含错口令）PASS；本 PR 三条修复已过
- CI：`91b9893` 已绿

合入后建议立刻：用合入后的 tip 打 ISO，UEFI 无盘启动再冒一次烟。

## 合 #7 之后、另开（不挡 #7）

1. **安装后账户迁到目标盘** — 当前首次无盘启动会走建账户（账户未迁盘，可接受为已知差异）
2. **legacy BIOS 无盘启动** — hybrid GPT 下 BIOS 串口空；真机/Limbo 请先用 **UEFI**
3. **设置/存储卡「加密 · 仅文件内容」角标** — Designer 定稿有；本轮机验未点开设置确认，需补一眼
4. **可选：把 `arco_vm_test.py`（BIOS+UEFI）挂进 CI** — 见已合的 #6 讨论；#6 若仍开着可一并处理

## 13700H 实机驱动轨（已停，交后人）

分支（已推 fork、**无 PR**）：
https://github.com/heDuke/ArkOS/tree/device/13700h-drivers

| Commit | 内容 | 状态 |
|---|---|---|
| `5c1e645` | IOAPIC：MADT 解析 + IRQ→IOAPIC 路由 | 半成品；主机测当时未完全绿就停了 |
| `8f01927` | NVMe 用的 `block_attach` 脚手架 | 半成品，未接真 NVMe |

用户说明：**IOAPIC 交给其他人处理**；Helper 已停手，本地改动已推 fork。

### 设备清单（用户机 · Raptor Lake-P / 13700H 类）

优先顺序（Reviewer 已锁）：

**P0**

1. NVMe `06:00.0` `1cc4:6a14`（Unionmemory AM6A1）— 真机安装落盘
2. xHCI `00:14.0` `8086:51ed` — 键盘鼠标真输入
3. 显示保底（现有 FB/简易 VGA）；**不做** Iris Xe `a7a0` 加速、**不做** NVIDIA `28e0`

**P1**

4. RTL8168 `07:00.0` `10ec:8168` — 对照 inbox `e1000.arco`

**P2 / 延后**

- RTL8852BE Wi‑Fi、cAVS 音频、NVIDIA HDMI 声卡
- DPTF / Crashlog / HECI / SMBus / SPI / Shared SRAM — 只记 PCI ID

### 已锁冒烟门禁（Tester）

1. 干净 ISO 能装、能进桌面；串口无 `[exception]`；已装 `.arco` 启动校验全过
2. `dev install` / `dev remove` 要管理员；坏镜像拒装、不留半装
3. 每驱动：设备页见节点；真输入或真输出；remove 后 ABSENT，同名可重装
4. INIT 失败不改持久状态；缺模块不能卡死在登录前
5. **IOAPIC（P0）**
   - 开机日志：MADT 解析出的 IOAPIC 地址、GSI 范围、重映射；legacy PIC **全部屏蔽**
   - NVMe / xHCI 各一次读写，**中断计数必涨**；轮询模式 **拒绝过门**
   - remove 再 install：IOAPIC 表项回收后重配，无残留/重复

### 宿主侧已有（勿重复造）

- `ArkDriverHost` / `ARK_SYS_DRIVER`、`.arco` 打包 `scripts/arco.py`
- 示例 `sdk/driver_demo.c`、内置 `e1000.arco`
- PR #5 合入后的设备模型与 `irq_attach`（当前偏 legacy PIC；实机要接到 IOAPIC）

## 存储 / 加密产品规则（已锁，勿改口径）

- 卷级加密/压缩，安装可选，**默认关**
- AES-128-GCM + 每块随机 nonce；元数据明文；自研小窗口 LZ
- 解锁在**登录前**玻璃卡；标题「磁盘已加密」；按钮「解锁」→「正在解锁…」
- 错口令「口令错误」；元数据坏「磁盘加密信息已损坏，无法解锁」
- 无 RDRAND：加密开关灰掉 +「此设备不支持加密」
- 安装文案：加密小字「开机需要输入口令；忘记口令无法恢复」；压缩「节省空间，读写会稍慢」
- 空口令「口令不能为空」；不一致「两次口令不一致」；此时禁用「开始安装」
- 失败文案：「写入分区失败，目标盘未完成」/「安装已取消，目标盘未完成」
- 目标盘：**自研写 GPT + hybrid MBR**；安装器在源 ISO 无 `EFI PART` 时**自建** GPT（勿依赖源盘碰巧有 GPT）
- 数据卷：MBR **第 4 项**类型 `0xDA`；GPT ArkFS GUID；`ARKFS2` 魔数在 `data_start`，不是 LBA 0

## 多分盘 / 软 RAID

**整轨押后**（当前存储轨完成后另开设计）：安装选分区/双盘、存储卡按卷、降级/重建验收。

## 建议接手顺序

1. 确认/催合 **#7** → 合入 tip 打 ISO（UEFI）冒烟
2. 开小 PR：加密角标一眼确认 +（可选）账户迁盘 / BIOS 说明文档
3. 从 `device/13700h-drivers` @ `8f01927` 接手：**先把 IOAPIC 主机测编绿并满足门禁 5**，再开 NVMe `.arco`
4. 实机 13700H：P0 NVMe + xHCI，再 RTL8168

## 相关链接速查

- #7 https://github.com/ArkUI-Project/ArkOS/pull/7
- 13700H 分支 https://github.com/heDuke/ArkOS/tree/device/13700h-drivers
- 可试包历史（可能过期）：安装解锁本地 zip 曾上传用户 Drive；以分支 tip 重打为准
