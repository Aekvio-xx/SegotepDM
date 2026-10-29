# 构建

## 环境

目标为 Windows x64，使用 C++20、Win32、CMake、Ninja 和 MSVC，静态链接 CRT。工程不依赖 MFC，不构建插件或验证程序，不安装驱动。运行需要系统已有可用的 USB 串口驱动。

随包保存 nlohmann/json 头文件。工具链本身不随源码分发，固定来源、版本和 SHA256 在 tools/toolchain-lock.json。当前锁定 MSVC 工具目录 14.44.35207、Windows SDK 目录 10.0.26100.0、CMake 4.4.4、Ninja 1.13.2。

## 项目内工具链

在项目根目录的 PowerShell 执行：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Prepare-LocalToolchain.ps1
~~~

此步骤按锁定地址下载和提取到本项目 .tools，并校验 SHA256。它不运行全局安装器，不永久修改环境或执行策略；命令中的 Bypass 仅用于该进程。工具获取仍遵循各自上游条款。

工具准备不属于普通源码整理操作。已经存在完整 .tools 时可直接构建；正常构建和程序运行不下载依赖。

## Agent

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Build.ps1
~~~

脚本只使用 .tools 中固定工具，临时设置当前进程环境并在结束后恢复。编译输出为 out/agent-x64；默认复制便携产物到 dist/DM-Agent-x64。

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Build.ps1 -SkipInstall
~~~

-SkipInstall 只编译，不覆盖交付目录。旧程序正在运行时，可先使用此选项，待其退出后再更新。构建、复制安装均不启动 Agent、不打开串口、不发送指令，也不执行测试。

## 离线 HTML

源码包已经包含 viewer/dist/DM-Record-Viewer.html，可直接使用。修改 viewer/src 后重新组装：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\viewer\Build-Viewer.ps1
~~~

它只将本地 HTML 片段、CSS 与 JavaScript 合成为一个文件，同时生成使用说明和构建指纹，不运行页面。先组装 HTML，再构建 Agent，可让便携目录附带最新查看器。

已有 Agent 便携目录时，可显式复制查看器：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\viewer\Build-Viewer.ps1 -CopyToAgent
~~~

-CopyToAgent 只写本项目 dist/DM-Agent-x64/viewer。查看器源码、已组装 HTML 和第三方许可应随版本控制保存；.tools、out 和根 dist 是本地生成目录。

## GitHub Actions 与 Release

[Windows build and release](../.github/workflows/release.yml) 在 main 更新或手动触发时构建 Windows x64 便携包，在推送版本标签时构建并发布 GitHub Release。标签格式为 v0.1.0，必须与 CMakeLists.txt 版本一致，并提供对应的 docs/releases/v0.1.0.md 发布说明。

在线构建通过 [Build-HostedRelease.ps1](../tools/Build-HostedRelease.ps1) 使用 windows-2022 托管环境预装的 Visual Studio 2022 和 CMake，按 Release 配置编译并静态链接 CRT。此入口专用于 GitHub Actions；本地构建继续使用前述项目内工具链。托管环境随 GitHub 镜像维护更新，其镜像版本、源码提交和构建链接记录在便携包 BUILD-INFO.json。

流水线组装查看器、编译、安装并检查文件、可执行文件架构和版本，随后生成 SegotepDM-v0.1.0-windows-x64.zip 与 SHA256SUMS.txt。标签构建成功后，发布任务校验 ZIP 哈希并将文件附加到 Release。编译产物可在 Actions 中下载，保留 14 天；正式发布附件保存在 Releases。
