# 本机只读快照接口

实现见 [shared_memory.hpp](../src/common/shared_memory.hpp)、[shared_memory.cpp](../src/common/shared_memory.cpp) 与 [controller.cpp](../src/agent/controller.cpp)。对象及固定头部为v1，JSON为1.1。

## 对象和权限

映射名为 Local\SegotepDM.Telemetry.v1.<当前用户SID>，代理单实例对象为 Local\SegotepDM.Agent.v1.<当前用户SID>。SID在运行时获得，不是固定账户常量。Local命名空间限制当前Windows会话，DACL只向创建账户开放；同账户进程不是强隔离边界。

代理创建65536字节的页文件支持映射，不建立磁盘快照文件或网络监听。客户端使用 OpenFileMappingW(FILE_MAP_READ) 和只读 MapViewOfFile，不新建同名对象。客户端不存在或读取慢时，代理覆盖同一份最新数据，不维护逐客户端队列。

映射可能因客户端句柄仍在而留存，因此“能打开映射”不代表代理在线。

## 固定头部

Windows小端、64字节、8字节对齐，禁止跨进程传递指针或运行时对象。

| 偏移 | 字节 | 字段 |
|---:|---:|---|
| 0 | 4 | magic，0x314D444D |
| 4 | 4 | 主版本，1 |
| 8 | 4 | 总容量，65536 |
| 12 | 4 | 32位发布序号，奇数表示写入中 |
| 16 | 4 | UTF-8 JSON长度，不含NUL |
| 20 | 44 | 保留并写零 |
| 64 | 最多65472 | JSON正文 |

写者先用Interlocked将序号设为奇数，屏障后写头部和正文，再经屏障提交偶数。读者按对齐32位加载序号，屏障后验证头部、复制正文，再屏障复查序号；仅前后相同且为偶数时接收。只读映射上不得使用会写内存的InterlockedCompareExchange。此约定面向Windows x86/x64内存语义，不是通用跨平台C++原子对象协议。

每次读取最多尝试两次，失败保留旧快照并继续计算年龄，不空转等待。客户端关闭视图和句柄即可退出。现有读取类在映射不存在时限制重开频率，避免忙循环。

## JSON 1.1

schema_version为{major:1,minor:1}。兼容字段扩展提升次版本，未知字段忽略；不兼容变更提升主版本并使用新对象名称。跨语言客户端需保留64位计数精度，不能把大整数记录标识无条件转成低精度数值。

| 分组 | 字段 |
|---|---|
| 实例与序号 | agent_instance_id、device_session_id、snapshot_sequence、status_sequence |
| 运行与时间 | running、published_at_utc、published_tick_ms、received_at_utc、received_tick_ms |
| 设备连接 | serial_enabled、connection_state、selected_port、actual_port、device_instance |
| 身份与规格 | device_id、metadata_id、device_info、serial_number、两类信息接收时间 |
| 指标 | metrics，包含下表11项 |
| 设备诊断 | fault_code_raw、fan_mode_raw_diagnostic、reserved_status_bytes、diagnostics |
| 操作与存储 | last_error、last_action、last_requested_interval_ms、observed_interval_ms、storage |
| 派生电量 | energy，见下文 |

device_id来自USB实例或已收到的电源序列号哈希，不直接以端口号作为持久身份。规格与序列号在新会话重新获取。observed_interval_ms为有限有效收包间隔均值，不是设备确认的设置值。设备采样时间未知，device_sample_time为null，主机接收时间不能冒充设备采样时间。

metrics每项提供value、raw_value、unit、source_field、evidence、received_at_utc、received_tick_ms、age_ms、validity、freshness和device_sample_time。当前validity主要为parseable/missing，invalid/unsupported为扩展状态；freshness独立为fresh/stale/offline。application_code说明换算依据，不表示测量精度已确认。

| 指标ID | 单位 |
|---|---|
| input_power_w | W，交流输入 |
| fan_speed_rpm | RPM |
| temperature_low_c | degC，低压侧 |
| input_voltage_v | V |
| rail_3v3_v、rail_5v_v、rail_12v_v、rail_5v_sb_v | V |
| input_frequency_hz | Hz |
| temperature_high_c、temperature_environment_c | degC |

connection_state包括disabled、connecting、waiting、connected、stale、disconnected、suspended、stopped，进程错误可为fatal_error。代理约每秒发布心跳；实时客户端须检查running、发布时间不晚于自身时钟且心跳年龄不超过五秒、连接状态和各指标接收年龄。不可只信任缓存的freshness字符串。

diagnostics为有界计数和原始候选诊断，status_frames仅计0x02；validated_frames也可能包含未知类型。原始诊断不是持续报文日志，也不是设备故障结论。

## energy v1

独立于metrics，version=1，basis=minute_mean。它包含scope_id、date_local、as_of_utc、revision，today_kwh、month_kwh、year_kwh、today_valid_ms，以及status、history_quality、recovery_error。

无有效覆盖时累计为null；有效0W可得到数值0。status为accumulating、paused、missing、recovering或error。as_of_utc是最后结算／恢复进度，不随心跳伪装成新读数。暂停可保留已累计电量，不能套用实时指标过期规则把历史清零；客户端仍需检查代理是否在线。

days最多为当前自然月31条，months最多为当前自然年12条。每项含period、kwh、valid_ms、has_data、finalized、quality、updated_utc、revision。finalized表示期间结束，覆盖完整性由其他字段独立说明。

更早年月由CSV汇总提供。电量是本地派生估算；详细字段、恢复封口及单位见 [ENERGY.md](ENERGY.md)。

## 接口边界

没有设备控制API，也未提供历史查询或命名管道服务。客户端需要磁盘历史时自行读取CSV，不能在宿主显示回调等待串口或磁盘。共享快照不因界面隐藏而裁剪；不同客户端的显示项可以不同。
