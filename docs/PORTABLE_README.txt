DM Agent（Windows x64）

运行 DmTelemetryAgent.exe，首次选择电源对应的USB串口。
HIMOS或串口助手应先释放同一端口。关闭主窗口隐藏到托盘，退出使用托盘菜单。
启动自动连接与Windows登录自启动是独立选项，后者默认关闭。

程序显示11项遥测和今日估算用电。电量按已有分钟平均输入功率和有效时长累计，
每天本地零点开始新日，未监测时段不补算。风扇设置、清灰和清除故障码不提供。

运行文件位于程序目录：
  config/  配置、设备信息和电量账本
  data/    按日CSV
  logs/    运行日志
  viewer/DM-Record-Viewer.html  离线记录查看器

用Edge或Chrome打开离线HTML，导入最新一天v2 CSV可看本月逐日和历史逐月电量，
CSV与日志通过页签分别查看，无需服务器或网络。

完整使用：docs/USAGE.md
电量和CSV：docs/ENERGY.md
本机接口：docs/IPC.md
协议与兼容边界：docs/PROTOCOL.md
许可：LICENSE、licenses/

Copyright (c) 2026 Aekvio-xx
原创代码使用MIT；第三方组件保留自己的版权与许可。
