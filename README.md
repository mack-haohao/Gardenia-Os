# 🌸 Gardenia OS

> A tiny, hand-rolled, highly customized Linux distribution built completely from scratch.  
> 一个从零开始、纯手写、带自我学习能力的极简 Linux 发行版。

![Language](https://img.shields.io/badge/Language-C-blue.svg)    ![Kernel](https://img.shields.io/badge/Kernel-Linux%207.3.1-orange.svg)    ![Size](https://img.shields.io/badge/Initramfs-%3C%2015MB-green.svg)    ![License](https://img.shields.io/badge/License-MIT-purple.svg)

**Gardenia OS** 不是基于 Ubuntu、Debian 或 Arch 的衍生版。除了 Linux 内核和必要的 OpenSSL 库，它的整个用户空间——包括 `init` 进程、Shell、网络工具、核心命令、文本编辑器、语法高亮与补全系统——都是用纯 C 手写的，不依赖 Systemd，也不依赖 BusyBox

## ✨ 核心特性

- 🐚 **`gash`（Gardenia Shell）**  
  支持管道、重定向、逻辑短路、历史记录和自动补全，并内置基于 `pty` 的多面板会话管理，可以在单窗口内 `detach` / `attach`

- 🎨 **`gardeniaApi`（渲染与智能引擎）**  
  一个用纯 C 写的极简 HTTP 服务器。它基于 256 分桶算法提供高性能语法高亮，并内置带上下文权重的智能代码补全词库

- 📝 **`gte`（Gardenia Text Editor）**  
  全屏终端代码编辑器，支持自适应窗口布局、语法高亮、侧边栏补全(Tab)、查找替换，并会通过 API 学习你的编码习惯

- 🔧 **手写工具链**  
  从带彩色主题的 `ls`、`mount`，到支持正则的 `grep`、`sed`、`sort`，全部采用 BusyBox 风格的单一二进制多调用架构

- 🌐 **纯 `ioctl` 网络栈**  
  `ifup` 和 `netup` 直接与内核网络驱动交互，不依赖外部组件

- 💾 **极度轻量**  
  生成的 `initramfs` 只有十几 MB，可以打包成 `.iso` 写入 U 盘，并在物理机上启动

## 🏗️ 系统架构

```text
Linux Kernel (bzImage)
  └── /init (PID 1, Hand-rolled in C)
       ├── Mounts pseudo-filesystems (devtmpfs, proc, sysfs, devpts)
       ├── Spawns network services (/bin/ifup, /bin/netup)
       ├── Spawns Gardenia API daemon (/bin/gardeniaApi, listening on 127.0.0.1:1)
       └── Respawns the shell (/bin/gash)
            ├── Built-in tools: cd, echo, exit, process-panes
            ├── Coreutils: touch, mkdir, wc, head, tail, printf, env
            ├── textTools: grep, sed, sort
            ├── File & System: ls, cat, rm, mount, umount, tty
            ├── Network: curl (with SSL & HTML formatter)
            └── Editor: gte (Gardenia Text Editor)
```

## 🚀 快速开始

### 依赖项

建议在 Fedora / Arch Linux 上构建，并确保安装以下工具：

- `musl-gcc`（用于静态编译）
- `qemu-system-x86_64`
- `cpio`、`gzip`、`make`
- `xorriso`、`grub2-tools-extra`（可选，用于打包 ISO）
- `openssl-devel`（用于编译 `curl`）
- 编译内核所需：`flex`、`bison`、`ncurses-devel`、`openssl-devel`、`elfutils-libelf-devel`、`bc`

### 编译内核 (Compiling the Kernel)

Gardenia OS 依赖于一个预配置的内核（支持物理机的 EFI 帧缓冲和 USB 键盘驱动）。请按照以下步骤获取并编译它：

```bash
# 1. 下载并解压内核
mkdir -p kernel
cd kernel
wget https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.3.1.tar.xz
tar -xf linux-7.3.1.tar.xz
cd linux-7.3.1

# 2. 注入 Gardenia 所需的驱动配置
./scripts/config --enable CONFIG_FB
./scripts/config --enable CONFIG_FB_EFI
./scripts/config --enable CONFIG_FB_VESA
./scripts/config --enable CONFIG_FRAMEBUFFER_CONSOLE
./scripts/config --enable CONFIG_USB
./scripts/config --enable CONFIG_USB_XHCI_HCD
./scripts/config --enable CONFIG_USB_EHCI_HCD
./scripts/config --enable CONFIG_USB_HID
./scripts/config --enable CONFIG_HID_GENERIC
./scripts/config --enable CONFIG_INPUT
./scripts/config --enable CONFIG_INPUT_KEYBOARD
./scripts/config --enable CONFIG_KEYBOARD_ATKBD
./scripts/config --enable CONFIG_SERIAL_8250
./scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
make olddefconfig

# 3. 编译内核
make -j$(nproc) bzImage
```

编译完成后，`bzImage` 会自动生成在 `arch/x86/boot/bzImage`

### 编译与运行 Gardenia OS

```bash
# 回到 myos 项目根目录
cd ../../

# 1. 生成默认的高亮规则与智能补全词库
python3 gen_cfg.py

# 2. 一键编译、打包 initramfs 并启动 QEMU
make run
```

### 启动到物理机

```bash
# 编译内核并生成可引导的 ISO 镜像
make iso

# 写入 U 盘（注意把 /dev/sdX 换成你的 U 盘设备名）
sudo dd if=build/gardenia.iso of=/dev/sdX bs=4M status=progress
```

> 物理机启动时请在 BIOS 中关闭 Secure Boot，并根据主板情况开启 CSM 或选择 UEFI 模式

## 🧠 关于 Gardenia API

传统终端工具（不安装插件的前提下）的输出通常只是普通字符。Gardenia 引入了输出重定向与渲染拦截：通过 `librender.c`，外部命令（如 `ls`、`cat`、`curl`）的标准输出会被捕获，并通过 TCP 实时发送给 `gardeniaApi`。API 收到数据后，用自建的哈希桶算法快速着色，再把结果返回终端。使用 `gte` 编写 C 代码时，后台 API 也会记录补全习惯并调整权重

## 🛠️ 组件说明

- `src/init.c`：系统一号进程，负责挂载文件系统、启动服务。
- `src/gash/`（`shell.c`）：终端 Shell，支持管道、重定向和多面板。
- `src/gardeniaApi.c`：高亮引擎与智能补全后端。
- `src/gte.c`：终端代码编辑器。
- `src/librender.c`：渲染拦截库。
- `src/shellTools.c` / `src/textTools.c`：BusyBox 风格的小工具集。
- `src/mount.c`：支持自适应列宽的挂载工具。
- `config/`：语法高亮与代码补全的预置配置文件。

## 📜 许可

本项目采用 MIT 许可证。可以随意 Fork、修改、把玩，或者装进你的口袋

# English Version

**Gardenia OS** is not another Ubuntu or Arch derivative. Apart from the Linux kernel and the required OpenSSL library, its entire user space—including the `init` process, shell, network tools, core utilities, text editor, syntax highlighting engine, and completion system—is hand-written in pure C. It does not use Systemd and does not rely on BusyBox.

## ✨ Features

- 🐚 **`gash` (Gardenia Shell)**  
  Supports pipes, redirection, logic short-circuiting, history, and auto-completion. It also includes a built-in `pty`-based multi-pane session manager, so you can `detach` and `attach` within a single window.

- 🎨 **`gardeniaApi` (Rendering & Smart Engine)**  
  A minimalist HTTP server written in pure C. It provides fast syntax highlighting based on a 256-bucket algorithm, along with a built-in smart completion dictionary with context-aware weighting.

- 📝 **`gte` (Gardenia Text Editor)**  
  A full-screen terminal code editor with adaptive layout, syntax highlighting, sidebar completion, find-and-replace, and the ability to learn your coding habits through the API.

- 🔧 **Hand-rolled Toolchain**  
  From colorful `ls` and `mount` to regex-supported `grep`, `sed`, and `sort`, all tools use a BusyBox-style multi-call binary architecture.

- 🌐 **Pure `ioctl` Network Stack**  
  `ifup` and `netup` communicate directly with kernel network drivers and require no external dependencies.

- 💾 **Ultra-lightweight**  
  The generated `initramfs` is only a dozen or so MB. It can be packed into an `.iso`, burned to a USB drive, and booted on real hardware.

## 🏗️ Architecture

```text
Linux Kernel (bzImage)
  └── /init (PID 1, Hand-rolled in C)
       ├── Mounts pseudo-filesystems (devtmpfs, proc, sysfs, devpts)
       ├── Spawns network services (/bin/ifup, /bin/netup)
       ├── Spawns Gardenia API daemon (/bin/gardeniaApi, listening on 127.0.0.1:1)
       └── Respawns the shell (/bin/gash)
            ├── Built-in tools: cd, echo, exit, process-panes
            ├── Coreutils: touch, mkdir, wc, head, tail, printf, env
            ├── textTools: grep, sed, sort
            ├── File & System: ls, cat, rm, mount, umount, tty
            ├── Network: curl (with SSL & HTML formatter)
            └── Editor: gte (Gardenia Text Editor)
```

## 🚀 Getting Started

### Prerequisites

Fedora or Arch Linux is recommended. Make sure the following tools are installed:

- `musl-gcc` for static compilation
- `qemu-system-x86_64`
- `cpio`, `gzip`, `make`
- `xorriso`, `grub2-tools-extra` (optional, for building the ISO)
- `openssl-devel` for compiling `curl`
- For kernel compilation: `flex`, `bison`, `ncurses-devel`, `openssl-devel`, `elfutils-libelf-devel`, `bc`

### Compiling the Kernel

Gardenia OS relies on a pre-configured kernel (with EFI framebuffer and USB keyboard drivers for physical hardware). Follow these steps to fetch and compile it:

```bash
# 1. Download and extract the kernel
mkdir -p kernel
cd kernel
wget https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.3.1.tar.xz
tar -xf linux-7.3.1.tar.xz
cd linux-7.3.1

# 2. Inject Gardenia's required driver configurations
./scripts/config --enable CONFIG_FB
./scripts/config --enable CONFIG_FB_EFI
./scripts/config --enable CONFIG_FB_VESA
./scripts/config --enable CONFIG_FRAMEBUFFER_CONSOLE
./scripts/config --enable CONFIG_USB
./scripts/config --enable CONFIG_USB_XHCI_HCD
./scripts/config --enable CONFIG_USB_EHCI_HCD
./scripts/config --enable CONFIG_USB_HID
./scripts/config --enable CONFIG_HID_GENERIC
./scripts/config --enable CONFIG_INPUT
./scripts/config --enable CONFIG_INPUT_KEYBOARD
./scripts/config --enable CONFIG_KEYBOARD_ATKBD
./scripts/config --enable CONFIG_SERIAL_8250
./scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
make olddefconfig

# 3. Compile the kernel
make -j$(nproc) bzImage
```

Once compiled, the `bzImage` will be located at `arch/x86/boot/bzImage`.

### Build & Run Gardenia OS

```bash
# Return to the myos project root
cd ../../

# 1. Generate default syntax highlighting rules and completion dictionaries
python3 gen_cfg.py

# 2. Build, pack initramfs, and launch QEMU
make run
```

### Boot on Hardware

```bash
# Compile the kernel and generate a bootable ISO image
make iso

# Write to a USB drive (replace /dev/sdX with your USB device name:use lsblk)
sudo dd if=build/gardenia.iso of=/dev/sdX bs=4M status=progress
```

> Disable Secure Boot in the BIOS when booting on hardware. Depending on your motherboard, enable CSM or select UEFI mode.

## 🧠 About Gardenia API

Traditional terminal tools mostly print plain characters. Gardenia introduces output redirection and rendering interception: through `librender.c`, the stdout of external commands such as `ls`, `cat`, and `curl` is captured and sent to `gardeniaApi` over TCP in real time. The API then colorizes the data with a custom hash-bucket algorithm and sends it back to the terminal. When you write C code in `gte`, the API also records completion habits and adjusts weights in the background.

## 🛠️ Components

- `src/init.c`: PID 1, responsible for mounting filesystems and starting services.
- `src/gash/` (`shell.c`): Terminal shell with pipes, redirection, and multi-pane support.
- `src/gardeniaApi.c`: Highlighting engine and smart completion backend.
- `src/gte.c`: Terminal code editor.
- `src/librender.c`: Rendering interception library.
- `src/shellTools.c` / `src/textTools.c`: BusyBox-style utility set.
- `src/mount.c`: Mount utility with adaptive column width.
- `config/`: Preset syntax highlighting and completion configuration files.
### About GardeniaAPI:
#### Running at:
	`localhost:1`
##### API Endpoint

| Endpoint         | method   | parameter                   | action                                                               |
| ---------------- | -------- | --------------------------- | -------------------------------------------------------------------- |
| `/render`        | POST     | `body`                      | return ANSI data                                                     |
| `/autoconf/data` | POST/GET | `lang`、`param`、`line`       | `param≠exist`：`commands[param]++`  `lines[line][param]++``return200` |
| `/autoconf/data` | POST/GET | `lang`、`param=exist`、`line` | be same to`/autoconf/get` ,return 404 when file not exist            |
| `/autoconf/get`  | POST/GET | `lang`、`param`、`line`       | sort as `score = commands[cmd] + lines[line][cmd]` use`param`        |


## 📜 License

This project is licensed under the MIT License. Feel free to fork, modify, play with it, or put it in your pocket.

---Donate---

  █▀▀▀▀▀█ ▄█ ▄▄▄▄██ ▄▀▄▀ ▀▄ █▀▀▀▀▀█
  █ ███ █ ▄█▀█▄▄▄▀▄▄▀ ▀▀█   █ ███ █
  █ ▀▀▀ █ ▄ █ █▄▄█▀▄█ ██▄▀  █ ▀▀▀ █
  ▀▀▀▀▀▀▀ ▀ ▀▄█ █▄█ ▀ ▀▄▀ █ ▀▀▀▀▀▀▀
  ▀▀██▀▄▀█▀█▀▄▀█▀██▀▄██▄▄▀▀▀ ▀ █ ▀▄
  ▀▄▀  ▄▀▄██▄    ▀  ▀  █▀▄▀ ▀█ ▀▄ ▄
  █▀  ██▀██▀▄▄▄▀█ ▄▀▀▀█▄█▀▀▀   ▄ ▄█
  █▄▄█▄█▀▀ ▀  ▄▀ █▀ ▄ ▄█▀▄  ██▄█▄
    ▀▄██▀ ▄█▄ ▀▀▀██▀ ▄▀▄█▄ ▄▄▀ ▄▄▄▄
   ▀██▀█▀ ▄█▀▄▄▀▄██▀ ▄▀▄ ▄▄█ ██ ▄▀
  ▄█▀ ▀▄▀█▄██▄ ▀▀▀██▀▄▀███▀█ ▀ ▀▄▄█
  █ ▄▀ ▀▀▀█▀    ▄██ █▄ ▄ ▄▀ ▀▀▀▄▄█▄
  ▀ ▀▀ ▀▀ ████▀█▀█▄█▄▄▀█▀▀█▀▀▀█▄▄█▄
  █▀▀▀▀▀█ ▀█▄ ▀ █▀█▀▄▄██▄██ ▀ █ ▄▀▄
  █ ███ █ █ ▀▄▀█▀▀█▄ ▄▄██ █▀▀▀█▀▄ ▀
  █ ▀▀▀ █ █ █▄▀▀█▀█ █ ▄▄▀▄▀▀▀  ▄█▀
  ▀▀▀▀▀▀▀ ▀   ▀ ▀▀ ▀    ▀ ▀ ▀    ▀
