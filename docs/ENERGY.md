# 电量、CSV与恢复

电量复用已有分钟平均输入功率和有效覆盖时长，只在分钟或部分区间结算时计算增量。实现位于 [storage.cpp](../src/agent/storage.cpp) 和 [energy.cpp](../src/agent/energy.cpp)。

~~~text
区间电量（kWh）= input_power_w_mean × input_power_w_valid_ms ÷ 3,600,000,000
~~~

界面以kWh（度）显示三位小数，内部保留精度。无有效覆盖时是缺失，有效0W对应真实0值。断连、未运行、休眠和过期后的时间不补算；上一读数的有效延续沿用原有过期阈值，默认十秒。新功能不增加逐帧电量积分、额外查询或设备设置。

## 自然日、月和年

本地零点结算旧日并开始新日，已有分钟逻辑切分跨界区间；月用YYYY-MM，年用YYYY。连接变化、退出和时间调整可能产生不足一分钟的段。时间长度使用单调时钟，日期使用本地时间；不可因时钟回拨重复计时或跨缺口插值。

当天与当前月为进行中。finalized只表示自然期间已结束，不保证全天有监测。同一电源重连可续计；USB到已证明序列号的同会话贡献可迁移，无法证明身份的旧数据保持分开。

## CSV版本

新数据为data/telemetry_YYYY-MM-DD_v2.csv，schema_version=2。已有telemetry_YYYY-MM-DD.csv不改写，恢复和查看器兼容v1。

v1为76列：16个基础列、11项指标各5列、5个故障／质量／丢弃列。v2在其后追加16列，共92列；dropped_rows_total仍为第76列。精确列序由csv_header()产生。

基础列为schema_version、agent_instance_id、record_sequence、device_session_id、device_id、metadata_id、minute_start_local、period_start_utc、period_end_utc、last_received_utc、connection_state、rx_profile_validation、sample_count、rejected_candidates、partial、gap_reason。每项指标展开_mean、_min、_max、_last、_valid_ms。末五列为fault_code_last_u16、fault_nonzero_seen、invalid_fields、stale_fields、dropped_rows_total。

| v2追加列 | 含义 |
|---|---|
| record_type | telemetry、energy_day、energy_month或energy_snapshot |
| energy_scope_id | 电量设备统计范围 |
| energy_period | 日YYYY-MM-DD或月YYYY-MM |
| energy_kwh | telemetry本段增量；日／月汇总期间总量 |
| energy_valid_ms | 对应增量或总量的有效覆盖 |
| energy_has_data | 是否有有效覆盖；false时电量留空 |
| energy_finalized | 自然期间是否结束 |
| energy_quality | estimated_partial或recovery_incomplete |
| energy_updated_utc | 结算／汇总更新时间 |
| energy_revision | 汇总修订号 |
| energy_today_kwh | telemetry结算后当日累计 |
| energy_month_kwh | telemetry结算后当月累计 |
| energy_year_kwh | telemetry结算后当年累计 |
| energy_snapshot_id | 携带历史的汇总批次标识 |
| energy_snapshot_rows | 封口声明的日／月汇总行数 |
| energy_checkpoint | 封口中的恢复水位JSON，导出时保留 |

汇总行的无关遥测列留空，使用独立实例标识及序号，不计入样本、功率均值或普通趋势。采样主键按schema、代理实例和序号区分，不能用本地分钟去重；同一分钟的不同分段是有效独立记录。

## 最新一天文件携带的历史

每天CSV携带本月逐日汇总和已有各年月汇总。新日、重启或历史修正时可追加新批次；energy_snapshot是结束标记，声明前面同批次的汇总条数。普通分钟只追加进度，不每分钟重复复制整月历史。

最新telemetry中的当日／月／年累计更新当前进度。换月后日明细切换新月，旧月以月合计继续携带；要查看旧月逐日明细，另导入对应月份CSV。月份带年份，跨年不混淆。

读取必须先检查完整封口及行数，再按设备、期间、修订选择最新有效值。不完整尾批次不覆盖先前完整批次。energy_kwh在telemetry中是可累计的增量；各累计列和汇总总计不能重复相加。文件名、大小及修改时间不能证明内容重复。

## 本地保存

账本为config/energy_history.json。历史恢复、CSV和账本写盘均在存储线程，UI及显示客户端不读取磁盘。实时视图由已提交账本与有界待写段构成，仅在队列、账本、日期或质量变化时重建，不随每秒心跳重算。

默认五分钟批次写盘，跨日、休眠和正常退出触发提交。HTML使用已落盘快照，可能比界面晚一批。已完整写出的日期组先推进账本，账本原子保存失败可以从CSV恢复。schema、实例、设备会话、序号的设备内水位与完整封口避免重算。

账本缺失时，可使用CSV携带的完整汇总与checkpoint，再回放其后的有效记录。未知格式、坏行、无换行尾部、缺封口或容量超限会标记recovery_incomplete；不补造数据。设备身份冲突不盲目合并。

原始CSV保留清理不会顺带删除已保存月合计。存储失败、队列丢弃或异常退出仍可能丢失未提交数据。可能已部分落盘的重试项受保护，队列满时优先清理普通未写项或拒绝新项，并保留缺口标记。日志只记录变化及异常，不逐分钟记录正常电量。

## 上限

| 范围 | 限制 |
|---|---|
| 账本文件／设备范围 | 8MiB／16 |
| 高水位stream／已证明会话 | 8192／4096 |
| 显示别名／未归属会话 | 64／256 |
| 日、月及未归属贡献期间合计 | 8192 |
| 单scope汇总／checkpoint | 4096行／1MiB |
| CSV单次写出／尾部去重范围 | 4MiB／8MiB |
| 恢复文件 | 最多1024个，单个32MiB，合计512MiB |
| 单文件恢复记录／单行 | 250000行／2MiB |
| 本次新增唯一回放 | 200000条 |

内部保留当前与上一月日明细以接纳跨月迟到结算，CSV／接口按当前月份输出。历史月汇总保存到上述容量边界；超限明确标记，不无限增长或反复扫描。正常运行不每分钟扫描历史文件。

## 对外与查看器

[IPC.md](IPC.md)中的energy对象提供今日、当月、当年累计与有界日／月摘要，更早月份可读CSV。它是本地估算，不是设备回传的电表值。

离线查看器的耗电区域有独立设备和年月选择，不受普通分钟趋势筛选影响；展示截至时间、覆盖、进行中／已结算和恢复质量。缺失留空，有效零值保留标记。v1只能对已导入覆盖推算，不能补出未导入月份。查看器导出完整电量CSV时保留封口与checkpoint；普通分钟导出不携带完整历史。
