# SegotepDM

DM Agent 是面向 Windows x64 的鑫谷 DM 数字电源本地监测程序，理论能与各不同型号电源联通通信，测试用电源型号 DM850G。实现独立管理 USB 串口、解析遥测、记录 CSV，并通过本机只读共享内存向其他程序提供数据。随源码附带一个离线记录查看器。


## 功能

- 托盘后台运行，支持自启动、连接已保存端口，以及连接／关闭串口操作。
- 展示输入功率、风扇转速、低压侧温度、输入电压、输入频率、输出电压、高压区与环境温度。
- 按日期追加 CSV，提供设备信息缓存、运行日志、有界重试与恢复。
- 共享快照提供 JSON 1.1 数据约定。


## 快速使用

源码包附带可直接打开的 [DM-Record-Viewer.html](viewer/dist/DM-Record-Viewer.html)。 Edge／Chrome 打开后，选择或拖入 CSV／日志。两个页签各自保留导入与筛选状态，文件只在浏览器内存中处理。

Agent 需要先构建。构建完成后运行 dist/DM-Agent-x64/DmTelemetryAgent.exe，首次选择电源对应端口。HIMOS等其它应用不能同时占用端口。关闭主窗口隐藏到托盘；退出请使用托盘菜单。

[使用说明](docs/USAGE.md)介绍连接、后台运行、数据目录和状态；[查看器说明](viewer/USAGE.md)介绍导入、筛选、图表与导出。

## 构建

工具在项目的 .tools 中准备，脚本不进行全局安装或永久修改环境。在项目根目录的 PowerShell 中执行：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Prepare-LocalToolchain.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\viewer\Build-Viewer.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Build.ps1
~~~

准备工具需要联网下载锁定包；正常构建使用本地依赖。Agent 输出到 out/agent-x64，便携交付目录为 dist/DM-Agent-x64。只编译而不复制交付文件可使用 Build.ps1 -SkipInstall。详见[构建说明](docs/BUILD.md)。

## 源码结构

| 目录 | 内容 |
|---|---|
| src/agent | 界面、会话控制、串口、协议、记录与电量 |
| src/common | 配置、时间、Windows 辅助函数与共享内存 |
| resources | 程序资源 |
| third_party/nlohmann | JSON 单头依赖及其原始许可 |
| tools | 项目内工具准备与构建脚本 |
| viewer/src | 单 HTML 查看器的模板、样式和脚本 |
| viewer/dist | 已组装的离线 HTML |
| docs | 协议、接口、电量、使用和开发说明 |

本仓库范围是 DM Agent 和离线查看器，不包含 TrafficMonitor 插件、原厂应用或协议分析运行时。显示客户端可按 [IPC 契约](docs/IPC.md)自行接入。

## 数据与兼容性

输入功率是交流输入功率，额定电流不是实时电流；没有来源的输出功率、效率和功率因数不填推算值。温度沿用协议区域标签，传感器物理位置和精度未确认。

代码定义、历史单机观察和逐固件兼容性分别记录。接收规则曾在一台 DM850G、固件 V34.26.06.23 上观察；不代表所有型号或固件都已验证。当前电量与离线查看器的交付依据是代码、编译与静态审阅，不能据此宣称运行、测量精度或性能验证通过。详见[协议](docs/PROTOCOL.md)和[开发说明](docs/DEVELOPMENT.md)。

电量只覆盖有效监测时段，关闭、休眠、未运行或数据过期时间不补算。异常退出仍可能丢失未提交缓冲。CSV v2、恢复边界和字段见[电量说明](docs/ENERGY.md)。

## 许可

项目原创代码和文档采用 [MIT License](LICENSE)，版权署名 Aekvio-xx。随包提供的 nlohmann/json 保留其原始版权与许可，详见[第三方依赖与许可](docs/THIRD_PARTY.md)。
