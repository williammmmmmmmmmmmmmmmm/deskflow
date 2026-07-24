# Ubuntu cross-device clipboard file transfer

> Experimental feature / 实验性功能

This document is bilingual. The Chinese guide appears first, followed by the
English guide.

本文档为中英双语版本，先中文，后英文。

---

## 中文

### 1. 功能范围

此实验性功能面向两台运行 Ubuntu 的电脑，让 Deskflow 在切换活动屏幕后同步：

- UTF-8 文本和现有 HTML 剪贴板内容；
- PNG/BMP 图片剪贴板内容；
- Nautilus 中复制的一个或多个普通文件；
- Nautilus 中复制的目录及其普通文件子项。

在源电脑的 Nautilus 中按 `Ctrl+C`，切换到另一台电脑后，可在接收端
Nautilus 中直接按 `Ctrl+V`。文件传输的是实际内容，而不是只有源电脑才能
访问的路径。

文件剪贴板目前以 Ubuntu 24.04 的 GNOME、Nautilus、Wayland 和 X11 为优先
目标。它不恢复或依赖旧的拖拽传输代码。

### 2. 架构

数据沿用 Deskflow 的剪贴板同步路径：

```text
Nautilus / application clipboard
        |
        | Wayland: xdg-desktop-portal + libei
        | X11: X selection converters
        v
Deskflow clipboard formats
        |
        | Read selected local files and build a DFCB v1 content bundle
        v
Existing chunked DCLP clipboard protocol
        |
        v
Validate and materialize files in a receiver-local cache
        |
        | Rebuild local file:// URIs and claim the local clipboard
        v
Receiver Nautilus Ctrl+V
```

源端解析文件 URI，递归读取普通目录和普通文件，将文件内容、相对路径和基本
权限写入版本化的内部文件包。该文件包通过现有的分块 `DCLP` 剪贴板消息传送。

接收端先验证文件包，再将内容写入
`QStandardPaths::CacheLocation/clipboard-files/<UUID>`，最后生成指向这些本地
文件的 URI，写回 Wayland Portal 或 X11 剪贴板。

内部格式编号追加在原有 Text、HTML、Bitmap 格式之后。旧版客户端会忽略无法
识别的追加格式，因此现有文本和图片剪贴板保持兼容；文件粘贴要求两端都使用
支持该功能的版本。

### 3. 支持的 MIME 类型

| MIME/内部格式 | 用途 |
| --- | --- |
| `text/uri-list` | 标准文件 URI 列表，支持一个或多个本地 `file://` URI |
| `x-special/gnome-copied-files` | GNOME/Nautilus 文件剪贴板；第一行是 `copy` 或 `cut`，后续为 URI |
| `image/png` | Wayland 和 X11 的主要图片交换格式 |
| `image/bmp` | X11 图片兼容格式 |
| `text/plain;charset=utf-8`、`text/plain`、`UTF8_STRING` | 文本剪贴板格式 |
| Deskflow `FileBundle` | 仅用于 Deskflow 协议内部，包含实际文件内容；不会作为系统 MIME 对外发布 |

当源端同时提供 `x-special/gnome-copied-files` 和 `text/uri-list` 时，优先使用
GNOME 格式以保留 `copy`/`cut` 动作。

### 4. Ubuntu 24.04 构建和安装

#### 4.1 会话支持

- **Wayland**：使用 libei 和 xdg-desktop-portal 的 Remote Desktop/Input
  Capture 剪贴板接口。首次运行时需要接受桌面显示的 Portal 权限请求。
- **X11**：使用 X selection 和 Deskflow 的 X11 MIME 转换器。
- Ubuntu 登录界面的齿轮菜单中，`Ubuntu` 通常是 Wayland，
  `Ubuntu on Xorg` 是 X11。

两种后端可同时编入一个二进制。不要手动修改 `DISPLAY` 或
`WAYLAND_DISPLAY`；应在目标桌面会话内启动 Deskflow。

#### 4.2 推荐：Flatpak 构建

Ubuntu 24.04 仓库中的部分 Qt、libei 或 libportal 版本可能低于本项目要求。
项目 Flatpak 清单固定了可用依赖，因此这是推荐的验证方式。

当前最低依赖为：

- CMake 3.24；
- Qt 6.7；
- libei 1.3；
- libportal 0.9.1；
- OpenSSL 3.0。

安装构建工具和用户级 KDE SDK：

```bash
sudo apt update
sudo apt install flatpak flatpak-builder
flatpak remote-add --user --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
```

只构建、不安装：

```bash
flatpak-builder --force-clean --build-only \
  flatpak-build deploy/linux/flatpak/org.deskflow.deskflow.yml
```

安装为用户级测试版本：

```bash
flatpak-builder --force-clean --user --install \
  flatpak-build deploy/linux/flatpak/org.deskflow.deskflow.yml
flatpak run --user org.deskflow.deskflow
```

`--user` 不会覆盖系统级 Flatpak。若系统中已有同 ID 的 Deskflow，用户级构建
会在当前用户下优先运行；回滚时删除用户级构建即可恢复使用系统版本。不要为
此测试使用 `--system`。

为了读取 Nautilus 复制的实际文件，源码中的 Flatpak 清单包含
`--filesystem=host:ro`。这是只读权限，但范围较广，可以用以下命令检查最终
权限：

```bash
flatpak info --user --show-permissions org.deskflow.deskflow
```

#### 4.3 原生构建

只有在系统或自定义前缀已经提供上述最低版本依赖时才建议原生构建。Ubuntu
24.04 的默认 APT 包不一定满足版本要求。

以下命令构建 Wayland 后端，同时显式保留 X11 后端，并安装到仓库内的独立
目录，避免修改系统安装：

```bash
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-native" \
  -DBUILD_X11_SUPPORT=ON \
  -DBUILD_TESTS=ON
cmake --build build-native --parallel
ctest --test-dir build-native --output-on-failure
cmake --install build-native
```

从 `install-native/bin/deskflow` 启动测试版本。更完整的通用构建说明见
[Building Deskflow](dev/build.md)。

### 5. 双机使用步骤

1. 在两台 Ubuntu 电脑上安装同一功能版本的 Deskflow。
2. 按常规方式将一台配置为 Deskflow Server，另一台配置为 Client，并完成
   屏幕布局、网络和身份验证设置。
3. 在 Server 配置中启用 **Enable clipboard sharing**，并确认
   **Limit to** 足以容纳要复制的文件。
4. 在 Wayland 会话中接受 Deskflow 的 Portal 权限请求。X11 不需要 Portal
   剪贴板权限。
5. 先验证文本：在一台电脑复制一段 UTF-8 文本，切换到另一台电脑后粘贴。
6. 验证图片：从图片应用或截图工具复制图片，切换后粘贴到支持图片的应用。
7. 验证文件：在 Nautilus 中选择一个或多个文件或目录并按 `Ctrl+C`。
8. 将鼠标移到另一台电脑，使其成为活动屏幕。在接收端 Nautilus 中打开目标
   目录并按 `Ctrl+V`。
9. 对重要文件使用 `sha256sum` 对比两端内容。

建议先用小型测试数据：

```bash
mkdir -p "$HOME/DeskflowClipboardTest/folder"
printf 'Deskflow UTF-8 test 中文\n' \
  > "$HOME/DeskflowClipboardTest/text.txt"
head -c 65536 /dev/urandom \
  > "$HOME/DeskflowClipboardTest/folder/random.bin"
sha256sum "$HOME/DeskflowClipboardTest/text.txt" \
  "$HOME/DeskflowClipboardTest/folder/random.bin"
```

### 6. 大小限制

- 默认最大剪贴板大小为 **3 MiB**。
- 限制作用于整个序列化剪贴板，包括文本、图片、文件元数据、文件内容和协议
  头，不只是单个文件。
- 多文件和目录的所有内容共用同一限制。
- 超限、源文件不可读或读取期间变短时，Deskflow 会放弃该文件剪贴板并写入
  警告日志；同时存在的普通文本或图片格式仍会保留。
- 大文件会在内存中形成完整文件包。网络传输虽然使用现有分块消息，但目前不
  是端到端的磁盘流式传输。

可在 Server 配置的 **Enable clipboard sharing** 旁调整 **Limit to**。
提高限制会同时增加内存、网络和接收端磁盘占用，应按实际需要设置。

### 7. 安全措施

- 只处理剪贴板明确列出的本地 `file://` URI。
- 拒绝非本地 URL、不存在的路径、符号链接、设备、FIFO、套接字等特殊文件。
- 接收端验证签名、版本、动作、数量、UTF-8、根目录、重复路径、绝对路径、
  `.`/`..` 路径穿越、声明长度、截断和尾随数据。
- 每次接收使用新的 UUID 缓存目录，文件通过 `QSaveFile` 原子写入；失败时
  删除该次未完成目录。
- 文件包缺失或验证失败时，不会把源电脑的远程路径发布到本地剪贴板。
- 现有剪贴板大小配置同时充当资源上限。
- Flatpak 构建只以只读方式访问宿主文件；接收文件写入应用自己的缓存区域。

文件包没有独立于 Deskflow 连接的第二层加密。传输安全性继承当前 Deskflow
连接配置，因此只应连接可信电脑，并启用项目提供的身份验证和加密设置。

### 8. 已知限制

- 这是实验性 Ubuntu-to-Ubuntu 功能，不保证其他桌面环境或操作系统支持文件
  粘贴。
- 两端都必须支持新增格式；旧客户端仍可同步已有文本/图片，但不能接收实际
  文件内容。
- 仅支持本地文件 URI。`smb://`、`sftp://`、Google Drive、GVfs 虚拟位置等
  非本地 URI 会被拒绝。
- 符号链接和特殊文件不传输。
- 仅保留普通文件、目录和基本 Unix 权限；不保留所有者、ACL、扩展属性、
  稀疏布局、硬链接关系和原始时间戳。
- `cut` 标记会在接收端保留，但跨电脑粘贴不会删除源电脑上的原文件；其效果
  更接近“把接收缓存移动到目标目录”。
- 接收缓存不会在粘贴后立即删除，因为 Nautilus 仍可能请求其中的文件。缓存
  会占用磁盘空间；仅在 Deskflow 已停止且剪贴板不再引用旧文件时清理。
- 源文件在复制和 Deskflow 读取之间被删除、缩短或变为不可读时，本次文件
  同步会失败。
- Wayland 支持依赖桌面 Portal 后端提供并允许剪贴板接口。
- Flatpak 的 `host:ro` 权限允许应用读取广泛的宿主文件。只运行可信构建。

### 9. 测试方法

运行全部单元测试：

```bash
cmake -S . -B build-test -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DBUILD_X11_SUPPORT=ON
cmake --build build-test --parallel
ctest --test-dir build-test --output-on-failure
```

只运行文件剪贴板测试：

```bash
ctest --test-dir build-test -R ClipboardFileTransferTests -V
```

该测试覆盖多文件、二进制内容、递归目录、本地 URI 重写、保留其他剪贴板
格式、缺少内容包时移除远程 URI、路径穿越、大小限制和新增格式编解码。

手工测试至少应覆盖：

- Wayland → Wayland；
- X11 → X11；
- Wayland → X11 和 X11 → Wayland；
- 文本、PNG 图片、单文件、多文件、带空格/中文文件名和嵌套目录；
- 超过大小限制、删除源文件、旧客户端连接等失败/兼容场景。

可以在复制后查看本地剪贴板声明的 MIME：

```bash
# Wayland；需要 wl-clipboard
wl-paste --list-types

# X11；需要 xclip
xclip -selection clipboard -target TARGETS -out
```

预期文件复制至少包含 `text/uri-list` 或
`x-special/gnome-copied-files`；Nautilus 通常同时提供两者。

### 10. 回滚

回滚用户级 Flatpak 测试构建：

```bash
flatpak uninstall --user org.deskflow.deskflow
```

如果系统中原本已有 Deskflow Flatpak，该命令不会删除系统级版本。随后直接
启动系统版本即可。

原生测试构建安装在仓库的 `install-native` 独立目录中。停止该二进制并改回
原有 Deskflow 即可；确认目录中没有需要保留的内容后再删除测试构建目录。

若要从 Git 历史撤销功能提交，优先创建可审计的反向提交：

```bash
git revert <ubuntu-clipboard-feature-commit>
```

回滚后重新构建并在两台电脑上部署相同版本。不要直接修改或覆盖系统安装目录
中的二进制。

---

## English

### 1. Scope

This experimental feature targets two Ubuntu computers and synchronizes the
following Deskflow clipboard data after the active screen changes:

- UTF-8 text and existing HTML clipboard data;
- PNG/BMP image clipboard data;
- one or more regular files copied in Nautilus;
- directories copied in Nautilus, including their regular-file contents.

Copy with `Ctrl+C` in Nautilus on the source computer, switch to the other
computer, and paste directly with `Ctrl+V` in Nautilus. Deskflow transfers the
actual file contents rather than paths that only the source computer can access.

Ubuntu 24.04 GNOME with Nautilus, Wayland, and X11 are the priority targets.
The implementation neither restores nor depends on the deprecated drag-and-drop
transfer code.

### 2. Architecture

The feature uses Deskflow's existing clipboard synchronization path:

```text
Nautilus / application clipboard
        |
        | Wayland: xdg-desktop-portal + libei
        | X11: X selection converters
        v
Deskflow clipboard formats
        |
        | Read selected local files and build a DFCB v1 content bundle
        v
Existing chunked DCLP clipboard protocol
        |
        v
Validate and materialize files in a receiver-local cache
        |
        | Rebuild local file:// URIs and claim the local clipboard
        v
Receiver Nautilus Ctrl+V
```

The source parses file URIs, recursively reads regular directories and files,
and records contents, relative paths, and basic permissions in a versioned
internal bundle. The existing chunked `DCLP` clipboard message transports that
bundle.

The receiver validates the bundle, writes it below
`QStandardPaths::CacheLocation/clipboard-files/<UUID>`, rebuilds URIs that point
to those local files, and publishes them through the Wayland Portal or X11
clipboard.

The new internal format IDs are appended after the original Text, HTML, and
Bitmap IDs. Older peers ignore unknown appended IDs and continue to process
existing text and image formats. File paste requires feature-capable builds at
both endpoints.

### 3. Supported MIME types

| MIME/internal format | Purpose |
| --- | --- |
| `text/uri-list` | Standard list containing one or more local `file://` URIs |
| `x-special/gnome-copied-files` | GNOME/Nautilus file clipboard; the first line is `copy` or `cut`, followed by URIs |
| `image/png` | Preferred Wayland and X11 image exchange format |
| `image/bmp` | X11 image compatibility format |
| `text/plain;charset=utf-8`, `text/plain`, `UTF8_STRING` | Text clipboard formats |
| Deskflow `FileBundle` | Deskflow-internal actual-content bundle; it is not advertised as a system MIME type |

When both file MIME types are present, Deskflow prefers
`x-special/gnome-copied-files` so that the `copy`/`cut` action is retained.

### 4. Building and installing on Ubuntu 24.04

#### 4.1 Session support

- **Wayland** uses libei and the xdg-desktop-portal Remote Desktop/Input Capture
  clipboard APIs. Accept the desktop Portal permission prompt on first use.
- **X11** uses X selections and Deskflow's X11 MIME converters.
- In the Ubuntu login screen's gear menu, `Ubuntu` normally selects Wayland and
  `Ubuntu on Xorg` selects X11.

Both backends can be compiled into one binary. Do not override `DISPLAY` or
`WAYLAND_DISPLAY`; start Deskflow from the desktop session being tested.

#### 4.2 Recommended: Flatpak build

Some Qt, libei, or libportal packages in the Ubuntu 24.04 repositories may be
older than this project requires. The project Flatpak manifest pins suitable
dependencies and is therefore the recommended validation path.

Current minimum dependencies are:

- CMake 3.24;
- Qt 6.7;
- libei 1.3;
- libportal 0.9.1;
- OpenSSL 3.0.

Install the build tools and the user-scoped KDE SDK:

```bash
sudo apt update
sudo apt install flatpak flatpak-builder
flatpak remote-add --user --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
```

Build without installing:

```bash
flatpak-builder --force-clean --build-only \
  flatpak-build deploy/linux/flatpak/org.deskflow.deskflow.yml
```

Install a user-scoped test build:

```bash
flatpak-builder --force-clean --user --install \
  flatpak-build deploy/linux/flatpak/org.deskflow.deskflow.yml
flatpak run --user org.deskflow.deskflow
```

The `--user` option does not overwrite a system Flatpak. If a system Deskflow
with the same ID is present, the user build takes precedence for that user.
Removing the user build reveals the system version again. Do not use `--system`
for this test.

The source manifest contains `--filesystem=host:ro` so that Deskflow can read
the actual files copied by Nautilus. This permission is read-only but broad.
Inspect the effective permissions with:

```bash
flatpak info --user --show-permissions org.deskflow.deskflow
```

#### 4.3 Native build

Use a native build only when the minimum dependency versions listed above are
already available from the system or a custom prefix. Ubuntu 24.04's default
APT packages may not satisfy every version requirement.

The following builds the Wayland backend, explicitly retains X11 support, and
installs into an isolated directory inside the checkout instead of modifying a
system installation:

```bash
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-native" \
  -DBUILD_X11_SUPPORT=ON \
  -DBUILD_TESTS=ON
cmake --build build-native --parallel
ctest --test-dir build-native --output-on-failure
cmake --install build-native
```

Run `install-native/bin/deskflow` for testing. See
[Building Deskflow](dev/build.md) for the general build guide.

### 5. Two-computer usage

1. Install the same feature-capable Deskflow build on both Ubuntu computers.
2. Configure one as the Deskflow Server and the other as a Client using the
   normal screen layout, network, and authentication workflow.
3. Enable **Enable clipboard sharing** in the Server configuration and set
   **Limit to** high enough for the files being tested.
4. In a Wayland session, accept Deskflow's Portal permission prompt. X11 does
   not require Portal clipboard permission.
5. Test text first: copy UTF-8 text on one computer, switch screens, and paste.
6. Test images: copy from an image application or screenshot tool, switch
   screens, and paste into an image-aware application.
7. In Nautilus, select one or more files or directories and press `Ctrl+C`.
8. Move the pointer to the other computer so it becomes active. Open the target
   directory in Nautilus and press `Ctrl+V`.
9. Compare important content on both machines with `sha256sum`.

Start with small test data:

```bash
mkdir -p "$HOME/DeskflowClipboardTest/folder"
printf 'Deskflow UTF-8 test 中文\n' \
  > "$HOME/DeskflowClipboardTest/text.txt"
head -c 65536 /dev/urandom \
  > "$HOME/DeskflowClipboardTest/folder/random.bin"
sha256sum "$HOME/DeskflowClipboardTest/text.txt" \
  "$HOME/DeskflowClipboardTest/folder/random.bin"
```

### 6. Size limit

- The default maximum clipboard size is **3 MiB**.
- The limit applies to the complete serialized clipboard: text, images, file
  metadata, file contents, and protocol headers, not only to one file.
- All files in a multi-selection or directory share the same limit.
- If the limit is exceeded, a source file is unreadable, or it becomes shorter
  while being read, Deskflow omits the file clipboard and logs a warning. Other
  text or image formats in the same clipboard are retained.
- The complete bundle is held in memory. Network transport uses the existing
  chunked messages, but this is not end-to-end disk streaming.

Adjust **Limit to** next to **Enable clipboard sharing** in the Server
configuration. A larger limit increases memory, network, and receiver disk use.

### 7. Security measures

- Only local `file://` URIs explicitly listed by the clipboard are processed.
- Non-local URLs, missing paths, symbolic links, devices, FIFOs, sockets, and
  other special files are rejected.
- The receiver validates the signature, version, action, counts, UTF-8, roots,
  duplicate paths, absolute paths, `.`/`..` traversal, declared lengths,
  truncation, and trailing data.
- Each receive uses a fresh UUID cache directory. `QSaveFile` provides atomic
  file writes, and an incomplete transfer directory is removed on failure.
- A missing or invalid content bundle never causes source-computer paths to be
  advertised in the receiver's clipboard.
- The existing clipboard-size setting also serves as a resource cap.
- The Flatpak build has read-only host access; received files are written to
  the application's own cache.

The bundle does not add a second encryption layer independent of the Deskflow
connection. Transport security inherits the current Deskflow connection
configuration. Connect only trusted computers and enable the authentication and
encryption options provided by the project.

### 8. Known limitations

- This is an experimental Ubuntu-to-Ubuntu feature. File paste is not guaranteed
  on other desktop environments or operating systems.
- Both endpoints need the new formats. Older peers retain existing text/image
  behavior but cannot receive actual file contents.
- Only local file URIs are supported. Non-local locations such as `smb://`,
  `sftp://`, Google Drive, and GVfs virtual locations are rejected.
- Symbolic links and special files are not transferred.
- Regular-file data, directories, and basic Unix permissions are retained.
  Ownership, ACLs, extended attributes, sparse layout, hard-link relationships,
  and original timestamps are not retained.
- A `cut` marker is preserved on the receiver, but a cross-computer paste does
  not delete the original on the source computer. It behaves more like moving
  the receiver's cache copy into the destination.
- Received cache data is not removed immediately after paste because Nautilus
  may still request it. It consumes disk space; clean it only after Deskflow is
  stopped and the clipboard no longer references the old files.
- The transfer fails if a source file is removed, shortened, or becomes
  unreadable between the copy action and Deskflow's read.
- Wayland support depends on the desktop Portal backend exposing and allowing
  the clipboard APIs.
- Flatpak `host:ro` allows the application to read a broad set of host files.
  Run only trusted builds.

### 9. Testing

Run all unit tests:

```bash
cmake -S . -B build-test -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DBUILD_X11_SUPPORT=ON
cmake --build build-test --parallel
ctest --test-dir build-test --output-on-failure
```

Run only the file clipboard tests:

```bash
ctest --test-dir build-test -R ClipboardFileTransferTests -V
```

These tests cover multiple files, binary contents, recursive directories,
receiver-local URI rewriting, preservation of other formats, removal of remote
URIs without a content bundle, traversal rejection, the size cap, and
marshalling of the appended formats.

Manual testing should cover at least:

- Wayland to Wayland;
- X11 to X11;
- Wayland to X11 and X11 to Wayland;
- text, PNG images, one file, multiple files, names containing spaces/non-ASCII
  characters, and nested directories;
- over-limit data, source-file deletion, and old-peer compatibility.

Inspect clipboard MIME declarations after copying:

```bash
# Wayland; requires wl-clipboard
wl-paste --list-types

# X11; requires xclip
xclip -selection clipboard -target TARGETS -out
```

A file copy should expose at least `text/uri-list` or
`x-special/gnome-copied-files`. Nautilus normally offers both.

### 10. Rollback

Remove the user-scoped Flatpak test build:

```bash
flatpak uninstall --user org.deskflow.deskflow
```

If a system Deskflow Flatpak was already installed, this does not remove it.
Launch the system version afterward.

The native test build is installed in the checkout's isolated `install-native`
directory. Stop that binary and return to the previous Deskflow installation.
Delete the test directory only after confirming that it contains nothing that
must be retained.

To undo the feature in Git history, prefer an auditable reverse commit:

```bash
git revert <ubuntu-clipboard-feature-commit>
```

Rebuild and deploy the same reverted version to both computers. Do not directly
replace binaries inside a system installation.
