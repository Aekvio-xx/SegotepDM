# 离线记录查看器

[打开DM-Record-Viewer.html](dist/DM-Record-Viewer.html)，用本机Edge／Chrome选择或拖入CSV、日志。单个HTML包含全部样式、解析器与图表，不需要安装、服务器、网络或Agent在线。

CSV数据趋势和运行日志通过页签切换，各自保留文件、筛选、通知和明细。切换不重新初始化，刷新或关闭后重新导入。查看器不自动连接设备、不修改源文件、不使用浏览器持久存储。

CSV页支持11项趋势、功率分布、状态摘要、全字段明细，以及日／月／年估算用电、自然月逐日与自然年逐月柱图。最新v2每日CSV自带历史，普通分钟和电量汇总分开处理；日志页提供时间分布、类别、连接摘要、规则归类和原文。详细使用见 [USAGE.md](USAGE.md)。

## 源码与组装

- src/index.html：共用外框。
- src/csv.html、src/logs.html：带DM_PART标记的HTML片段。
- src/csv.js、src/logs.js：各自的解析、状态与展示。
- src/common.js、src/styles.css：安全文本、日期、导出、Canvas图表与样式。
- src/navigation.js：页签、键盘与URL片段导航。

在项目根目录执行：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\viewer\Build-Viewer.ps1
~~~

输出仍是dist/DM-Record-Viewer.html；脚本只拼接文本，不运行页面。-CopyToAgent可显式复制到本项目dist/DM-Agent-x64/viewer。构建指纹使用相对路径和SHA256。已生成HTML应随源代码保留，不被根dist忽略规则覆盖。

## 数据语义

兼容v1的76列与v2的92列。普通趋势只使用telemetry；完整日／月快照先验证封口、条数和修订，累计值不再次相加。新旧格式、设备范围、有效时间与恢复限制见 [电量说明](../docs/ENERGY.md)。

文件导入、解析行数、来源数、图表点数和DOM分页都有上限。缺失不填零，真实零电量有基线标记；小电量纵轴自适应精度。导入内容通过文本节点展示；页面CSP禁止外部资源和连接。

源码与语法静态检查不等于浏览器运行验证。开发方式与验证状态见[开发说明](../docs/DEVELOPMENT.md)。
