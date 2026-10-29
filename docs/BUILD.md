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
