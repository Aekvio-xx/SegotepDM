(() => {
  'use strict';
  const D = window.DM;
  const showNotice = (message, tone) => D.notice(message, tone, 'csv-notice');
  const MAX_ROWS = 100000, MAX_ENERGY_ROWS = 50000, MAX_SNAPSHOT_ROWS = 4096, MAX_ENERGY_SCOPES = 128, MAX_FILE = 32 * 1024 * 1024, MAX_BYTES = 96 * 1024 * 1024;
  const METRICS = [
    {id:'input_power_w',label:'输入功率',unit:'W',color:'#16756f'},
    {id:'fan_speed_rpm',label:'风扇转速',unit:'RPM',color:'#3f799d'},
    {id:'temperature_low_c',label:'低压侧温度',unit:'℃',color:'#16756f'},
    {id:'input_voltage_v',label:'输入电压',unit:'V',color:'#3f799d'},
    {id:'rail_3v3_v',label:'+3.3V',unit:'V',nominal:3.3,color:'#16756f'},
    {id:'rail_5v_v',label:'+5V',unit:'V',nominal:5,color:'#3f799d'},
    {id:'rail_12v_v',label:'+12V',unit:'V',nominal:12,color:'#b47d35'},
    {id:'rail_5v_sb_v',label:'+5V SB',unit:'V',nominal:5,color:'#8b718f'},
    {id:'input_frequency_hz',label:'输入频率',unit:'Hz',color:'#3f799d'},
    {id:'temperature_high_c',label:'高压区温度',unit:'℃',color:'#b47d35'},
    {id:'temperature_environment_c',label:'环境温度',unit:'℃',color:'#758499'}
  ];
  const SUFFIXES = ['mean','min','max','last','valid_ms'];
  const STATE = {
    connected:{label:'已连接摘要',color:'#16756f'},
    waiting:{label:'等待数据',color:'#b2a375'},
    connecting:{label:'连接中',color:'#94a6b3'},
    disabled:{label:'采集关闭',color:'#a8afb7'},
    stale:{label:'区间末数据过期',color:'#b47d35'},
    disconnected:{label:'连接断开',color:'#b67869'},
    suspended:{label:'休眠暂停',color:'#8b8194'},
    stopped:{label:'已停止',color:'#a8afb7'},
    fatal_error:{label:'后台错误',color:'#b45d50'}
  };
  const ids = ['file-input','import-button','clear-button','drop-zone','import-title','file-summary','file-list',
    'import-diagnostics','diagnostics-title','diagnostics-list','device-filter','session-filter','timezone-filter',
    'range-start','range-end','apply-range','all-range','export-button','kpi-power','kpi-peak','kpi-energy',
    'kpi-duration','kpi-power-note','kpi-energy-note','kpi-duration-note','analysis-note','quality-summary',
    'rail-mode','mains-mode','table-summary','quality-filter','record-search','records-body','previous-page',
    'next-page','page-label','page-size','record-dialog','close-dialog','record-facts','metric-details','raw-details',
    'energy-scope','energy-month','energy-year','energy-export','energy-asof','energy-today','energy-today-note',
    'energy-month-total','energy-month-note','energy-year-total','energy-year-note','energy-note','energy-table-body'];
  const ui = Object.fromEntries(ids.map(id => [id,D.el(id)]));
  const charts = {
    power:new D.Chart('power-chart'),temperature:new D.Chart('temperature-chart'),fan:new D.Chart('fan-chart'),
    rails:new D.Chart('rails-chart'),mains:new D.Chart('mains-chart'),
    distribution:new D.Chart('distribution-chart'),state:new D.Chart('state-chart'),
    energyDays:new D.Chart('energy-days-chart'),energyMonths:new D.Chart('energy-months-chart')
  };
  const state = {
    rows:[], byKey:new Map(), files:new Map(), bytes:0, issues:[], issueOverflow:0,
    duplicates:0, conflicts:0, rejected:0, sourceSequence:0, busy:false, utc:false, rangeStart:null, rangeEnd:null,
    rangeMode:'all', page:1, pageSize:50, firstSelection:true, selectedRows:[], tableRows:[],
    energyRows:new Map(), energyModels:new Map(), energyWarnings:new Set(), energyAutoPeriod:true, energyScopePinned:false
  };

  function node(tag,text,className) {
    const element=document.createElement(tag);
    if(text !== undefined)element.textContent=String(text);
    if(className)element.className=className;
    return element;
  }
  function addIssue(message) {
    if(state.issues.length<200)state.issues.push(String(message));
    else state.issueOverflow++;
  }
  function get(row,name) {
    const index=row.columnMap.get(name);
    return index===undefined?'':row.values[index];
  }
  function value(row,metric,suffix=0) {
    const number=row.numbers[metric*5+suffix];
    return Number.isFinite(number)?number:null;
  }
  function hasMean(row,metric) {
    return value(row,metric,0)!==null && value(row,metric,4)!==null && value(row,metric,4)>0;
  }
  function short(text,max=16) {
    const string=String(text);
    return string.length>max?string.slice(0,max-1)+'…':string;
  }
  function identityLabel(id) {
    if(!id || id==='unselected')return '未归属身份';
    if(id.startsWith('psu:'))return 'PSU · '+short(id.slice(4),16);
    if(id.startsWith('usb:'))return 'USB · '+short(id.slice(4),16);
    return short(id,24);
  }
  function stateLabel(name) {return Object.hasOwn(STATE,name)?STATE[name].label:(name||'未知状态');}
  function numericCounter(raw) {
    const n=D.num(raw);
    return n!==null && n>=0?n:0;
  }
  function parseStamp(raw) {
    if(typeof raw!=='string')return null;
    const match=/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d+)?(Z|[+-](\d{2}):(\d{2}))$/i.exec(raw);
    if(!match)return null;
    const year=Number(match[1]),month=Number(match[2]),day=Number(match[3]);
    const days=[31,year%4===0&&(year%100!==0||year%400===0)?29:28,31,30,31,30,31,31,30,31,30,31];
    if(month<1||month>12||day<1||day>days[month-1]||Number(match[4])>23||Number(match[5])>59||Number(match[6])>59)return null;
    if(match[8]!==undefined&&(Number(match[8])>23||Number(match[9])>59))return null;
    const time=Date.parse(raw);
    return Number.isFinite(time)?time:null;
  }

  function periodDate(raw, monthOnly=false) {
    const s=String(raw||'').trim();
    if(monthOnly)return /^\d{4}-(0[1-9]|1[0-2])$/.test(s)?s:null;
    return /^\d{4}-\d{2}-\d{2}$/.test(s)&&parseStamp(s+'T00:00:00Z')!==null?s:null;
  }
  function revisionValue(raw) {
    const value=String(raw||'').trim().replace(/^0+(?=\d)/,'');
    if(!/^\d{1,20}$/.test(value)||(value.length===20&&value>'18446744073709551615'))throw new Error('电量修订号无效');
    return value;
  }
  function compareRevision(a,b) {return a.length-b.length||(a<b?-1:a>b?1:0);}
  function energyIssue(key,message) {
    if(state.energyWarnings.has(key))return;
    if(state.energyWarnings.size<500)state.energyWarnings.add(key);
    addIssue(message);
  }
  function parseEnergy(row) {
    const scope=get(row,'energy_scope_id').trim(),type=row.type;
    if(!scope&&type==='telemetry')return null;
    if(!scope||scope.length>512)throw new Error('电量设备标识缺失或过长');
    const period=periodDate(get(row,'energy_period'),type==='energy_month');
    if(!period)throw new Error('电量统计期间无效');
    const updated=parseStamp(get(row,'energy_updated_utc'));
    if(updated===null)throw new Error('电量汇总时间无效');
    const revision=revisionValue(get(row,'energy_revision'));
    const flag=type==='energy_snapshot'?'false':get(row,'energy_has_data').trim().toLowerCase();
    if(!['true','false'].includes(flag))throw new Error('电量有效性标记无效');
    const hasData=flag==='true',rawAmount=D.num(get(row,'energy_kwh'));
    if(hasData&&(rawAmount===null||rawAmount<0))throw new Error('有效电量缺少非负数值');
    const coverage=D.num(get(row,'energy_valid_ms')),validMs=type==='energy_snapshot'&&coverage===null?0:coverage;
    if(validMs===null||!Number.isSafeInteger(validMs)||validMs<0)throw new Error('电量有效覆盖时长无效');
    const running={};
    for(const field of ['today','month','year']) {
      const raw=get(row,'energy_'+field+'_kwh'),value=D.num(raw);
      if(raw.trim()&&(value===null||value<0))throw new Error('电量累计值无效：'+field);
      running[field]=value;
    }
    const snapshot=get(row,'energy_snapshot_id').trim();
    if(type!=='telemetry'&&(!snapshot||snapshot.length>512))throw new Error('电量快照标识缺失或过长');
    let expected=null;
    if(type==='energy_snapshot') {
      expected=D.num(get(row,'energy_snapshot_rows'));
      if(expected===null||!Number.isSafeInteger(expected)||expected<0||expected>MAX_SNAPSHOT_ROWS)throw new Error('电量快照汇总行数无效或超过 '+MAX_SNAPSHOT_ROWS);
    }
    return {scope,period,kwh:hasData?rawAmount:null,validMs,hasData,finalized:get(row,'energy_finalized').trim().toLowerCase()==='true',
      quality:get(row,'energy_quality').trim()||'estimated_partial',updated,updatedRaw:get(row,'energy_updated_utc'),
      revision,snapshot,expected,running};
  }

  function equivalent(first,second) {
    if(first.headers.length!==second.headers.length)return false;
    for(const h of first.headers)if(!second.columnMap.has(h)||get(first,h)!==get(second,h))return false;
    return true;
  }
  function normalized(parsed,source) {
    const headers=parsed.headers, columnMap=new Map(headers.map((h,i)=>[h,i]));
    const required=['schema_version','agent_instance_id','record_sequence','device_id','period_start_utc','period_end_utc',
      'input_power_w_mean','input_power_w_valid_ms'];
    const missing=required.filter(h=>!columnMap.has(h));
    if(missing.length)throw new Error('不是受支持的 DM Agent CSV，缺少列：'+missing.join('、'));
    const metricColumns=METRICS.map(m=>SUFFIXES.map(s=>columnMap.get(m.id+'_'+s)));
    const countMissing=metricColumns.flat().filter(i=>i===undefined).length;
    if(countMissing)addIssue(source.name+'：缺少 '+countMissing+' 个指标统计列，相关字段保留为空。');
    return {headers,columnMap,metricColumns};
  }
  function makeRecord(parsedRow,layout,source) {
    const row={values:parsedRow.values,headers:layout.headers,columnMap:layout.columnMap,source,line:parsedRow.line};
    row.schema=get(row,'schema_version').trim();
    if(!['1','2'].includes(row.schema))throw new Error('不支持的 CSV 版本 '+row.schema);
    row.type=row.schema==='1'?'telemetry':get(row,'record_type').trim();
    if(!['telemetry','energy_day','energy_month','energy_snapshot'].includes(row.type))throw new Error('未知记录类型 '+row.type);
    row.agent=get(row,'agent_instance_id').trim();
    row.sequence=get(row,'record_sequence').trim();
    if(!row.agent || !/^\d+$/.test(row.sequence))throw new Error('缺少运行实例或记录序号');
    row.device=get(row,'device_id').trim()||'unselected';
    row.session=get(row,'device_session_id').trim();
    row.sessionKey=JSON.stringify([row.agent,row.session]);
    row.key=JSON.stringify([row.schema,row.agent,row.sequence.replace(/^0+(?=\d)/,'')]);
    row.energy=null;
    if(row.type!=='telemetry') {row.energy=parseEnergy(row);return row;}
    if(row.schema==='2') {
      try {row.energy=parseEnergy(row);} catch(error) {addIssue(source.name+' 第 '+row.line+' 行：'+error.message+'；采样仍保留，电量扩展未使用。');}
    }
    row.start=parseStamp(get(row,'period_start_utc'));
    row.end=parseStamp(get(row,'period_end_utc'));
    if(row.start===null || row.end===null || row.end<row.start)throw new Error('实际区间时间无效或结束早于开始');
    row.state=get(row,'connection_state').trim();
    row.partial=get(row,'partial').trim().toLowerCase()==='true';
    row.reason=get(row,'gap_reason');
    row.sampleCount=numericCounter(get(row,'sample_count'));
    row.rejected=numericCounter(get(row,'rejected_candidates'));
    row.dropped=D.num(get(row,'dropped_rows_total'));
    row.fault=D.num(get(row,'fault_code_last_u16'));
    row.nonzeroFault=get(row,'fault_nonzero_seen').trim().toLowerCase()==='true'||(row.fault!==null&&row.fault!==0);
    row.numbers=new Float64Array(METRICS.length*5);
    row.numbers.fill(NaN);
    for(let m=0;m<METRICS.length;m++)for(let p=0;p<5;p++) {
      const column=layout.metricColumns[m][p];
      if(column===undefined)continue;
      const n=D.num(row.values[column]);
      if(n!==null)row.numbers[m*5+p]=n;
    }
    row.issue=row.state!=='connected'||!hasMean(row,0)||row.nonzeroFault||row.rejected>0;
    return row;
  }
  function setBusy(busy) {
    state.busy=busy;
    ui['import-button'].disabled=busy;
    ui['file-input'].disabled=busy;
    ui['clear-button'].disabled=busy||(state.files.size===0&&state.issues.length===0&&state.issueOverflow===0);
    ui['export-button'].disabled=busy||state.selectedRows.length===0;
    ui['energy-export'].disabled=busy||!state.energyModels.size;
    ui['drop-zone'].setAttribute('aria-busy',String(busy));
  }
  async function importFiles(fileList) {
    if(state.busy)return;
    const files=Array.from(fileList||[]);
    if(!files.length)return;
    setBusy(true);
    let acceptedFiles=0, acceptedRows=0, acceptedEnergy=0, failed=0;
    const beforeIssueCount=state.issues.length+state.issueOverflow;
    try {
      for(let fileIndex=0;fileIndex<files.length;fileIndex++) {
        const file=files[fileIndex];
        showNotice('正在读取 '+(fileIndex+1)+' / '+files.length+'：'+file.name,'info');
        await D.nextFrame();
        try {
          if(!/\.csv$/i.test(file.name))throw new Error('只接受 .csv 文件');
          if(file.size>MAX_FILE)throw new Error('单文件超过 32 MiB，请分批导出');
          // 文件元数据不能证明内容相同；每次读取后按记录主键去重。
          const content=await D.readText(file);
          const parsed=await D.parseCSV(content.text,{maxRows:MAX_ROWS+MAX_ENERGY_ROWS,maxColumns:256});
          const source={name:file.name,key:'csv-source-'+(++state.sourceSequence),size:file.size,encoding:content.encoding,accepted:0,duplicates:0,rejected:0};
          const layout=normalized(parsed,source);
          const staged=new Map(),stagedEnergy=new Map();
          let duplicates=0, conflicts=0, rejected=0;
          for(let i=0;i<parsed.rows.length;i++) {
            const raw=parsed.rows[i];
            try {
              const row=makeRecord(raw,layout,source);
              const existing=staged.get(row.key)||stagedEnergy.get(row.key)||state.byKey.get(row.key)||state.energyRows.get(row.key);
              if(existing) {
                duplicates++;
                if(!equivalent(existing,row)) {
                  conflicts++;
                  addIssue(source.name+' 第 '+raw.line+' 行：主键内容冲突，保留先导入记录。');
                }
              } else (row.type==='telemetry'?staged:stagedEnergy).set(row.key,row);
            } catch(error) {
              rejected++;
              addIssue(source.name+' 第 '+raw.line+' 行：'+error.message+'，此行未参与分析。');
            }
            if(i>0 && i%2000===0)await D.nextFrame();
          }
          if(state.byKey.size+staged.size>MAX_ROWS)throw new Error('采样记录超过 100,000 条，本文件未并入，请缩小文件批次');
          if(state.energyRows.size+stagedEnergy.size>MAX_ENERGY_ROWS)throw new Error('电量汇总记录超过 50,000 条，本文件未并入，请分批查看');
          for(const issue of parsed.issues||[])addIssue(source.name+'：'+issue);
          if(parsed.unterminated||content.unterminated)addIssue(source.name+'：末尾未以换行结束，可能正在写入；请核对导入诊断，必要时重新导入完整文件。');
          if(staged.size===0&&stagedEnergy.size===0) {
            state.duplicates+=duplicates;state.conflicts+=conflicts;state.rejected+=rejected;
            addIssue(source.name+'：没有可新增的记录；重复 '+D.integer(duplicates)+' 条、无效 '+D.integer(rejected)+' 条，未增加来源文件。');
            continue;
          }
          if(state.files.size>=128)throw new Error('本页最多保留 128 个来源文件，请清空后分批查看');
          if(state.bytes+file.size>MAX_BYTES)throw new Error('累计来源大小超过 96 MiB，请清空后分批查看');
          for(const [key,row] of staged)state.byKey.set(key,row);
          for(const [key,row] of stagedEnergy)state.energyRows.set(key,row);
          source.accepted=staged.size;source.summaries=stagedEnergy.size;source.duplicates=duplicates;source.rejected=rejected;
          state.files.set(source.key,source);
          state.bytes+=file.size;state.duplicates+=duplicates;state.conflicts+=conflicts;state.rejected+=rejected;
          acceptedFiles++;acceptedRows+=staged.size;acceptedEnergy+=stagedEnergy.size;
        } catch(error) {
          failed++;
          addIssue(file.name+'：'+(error&&error.message?error.message:String(error)));
        }
      }
      state.rows=Array.from(state.byKey.values()).sort((a,b)=>a.start-b.start||a.end-b.end||a.key.localeCompare(b.key));
      rebuildEnergyModels();rebuildEnergyOptions();
      rebuildOptions();
      renderAll(true);
      const issueCount=state.issues.length+state.issueOverflow-beforeIssueCount;
      showNotice('已并入 '+acceptedFiles+' 个文件、'+D.integer(acceptedRows)+' 条新采样、'+D.integer(acceptedEnergy)+' 条电量汇总；重复 '+D.integer(state.duplicates)+' 条已跳过。'+(failed?' '+failed+' 个文件未导入。':'')+(issueCount?' 详情见导入诊断。':''),failed||issueCount?'warn':'info');
    } catch(error) {
      showNotice('导入中断：'+(error&&error.message?error.message:String(error))+'。已成功并入的数据仍保留。','error');
      state.rows=Array.from(state.byKey.values()).sort((a,b)=>a.start-b.start||a.end-b.end);
      rebuildEnergyModels();rebuildEnergyOptions();rebuildOptions();renderAll(true);
    } finally {
      ui['file-input'].value='';
      setBusy(false);
    }
  }

  function energyModel(key,scope,mode) {
    let model=state.energyModels.get(key);
    if(model)return model;
    if(state.energyModels.size>=MAX_ENERGY_SCOPES) {
      energyIssue('scope-limit','电量来源超过 128 个，额外来源未建立图表，请缩小导入范围。');
      return null;
    }
    model={key,scope,mode,days:new Map(),months:new Map(),years:new Map(),groups:[],incomplete:[],telemetry:[],
      completeIds:new Set(),asof:null,latestComplete:null,blockedRevision:null,ambiguous:false,legacyRows:[]};
    state.energyModels.set(key,model);return model;
  }
  function pointOrder(a,b) {
    return compareRevision(a.revision,b.revision)||(a.updated-b.updated)||(a.period<b.period?-1:a.period>b.period?1:0);
  }
  function pointFrom(row,extra={}) {
    const e=row.energy;
    return {period:e.period,kwh:e.kwh,validMs:e.validMs,hasData:e.hasData,finalized:e.finalized,quality:e.quality,
      revision:e.revision,updated:e.updated,updatedRaw:e.updatedRaw,source:row.source.name,line:row.line,
      from:'snapshot',coverageExact:true,...extra};
  }
  function setEnergyPoint(map,key,point,model) {
    const previous=map.get(key);
    if(previous&&compareRevision(previous.revision,point.revision)===0&&
       (previous.hasData!==point.hasData||previous.kwh!==point.kwh||
       (previous.coverageExact&&point.coverageExact&&previous.validMs!==point.validMs))) {
      energyIssue('conflict:'+model.key+':'+key+':'+point.revision,
        '电量来源 '+identityLabel(model.scope)+' 的 '+key+' 在相同修订号下内容冲突，保留先导入值。');
      return;
    }
    if(!previous||pointOrder(point,previous)>0)map.set(key,point);
  }
  function markAsof(model,point) {if(!model.asof||pointOrder(point,model.asof)>0)model.asof=point;}
  function rebuildEnergyModels() {
    state.energyModels=new Map();
    const groups=new Map();
    for(const row of state.energyRows.values()) {
      const e=row.energy,key=JSON.stringify([e.scope,e.snapshot]);
      let group=groups.get(key);
      if(!group) {group={key,scope:e.scope,id:e.snapshot,parts:[],seals:[],rows:[]};groups.set(key,group);}
      group.rows.push(row);
      (row.type==='energy_snapshot'?group.seals:group.parts).push(row);
    }
    for(const group of groups.values()) {
      const model=energyModel('v2:'+group.scope,group.scope,'v2');if(!model)continue;
      const seal=group.seals[0],e=seal&&seal.energy;
      const distinct=new Set(group.parts.map(row=>row.type+':'+row.energy.period));
      let complete=!!seal&&group.parts.length===e.expected&&distinct.size===group.parts.length&&
        group.parts.length<=MAX_SNAPSHOT_ROWS&&group.seals.every(row=>row.energy.expected===e.expected&&
          row.energy.revision===e.revision&&row.energy.updated===e.updated&&row.energy.period===e.period)&&
        group.parts.every(row=>row.energy.revision===e.revision&&row.energy.updated===e.updated)&&
        group.parts.filter(row=>row.type==='energy_day').every(row=>row.energy.period.slice(0,7)===e.period.slice(0,7));
      if(!complete) {
        model.incomplete.push(group);
        energyIssue('snapshot:'+group.key,'电量快照 '+short(group.id,32)+' 不完整或内容不一致，未用于历史统计；请导入写入完成的 CSV。');
        continue;
      }
      group.seal=seal;model.groups.push(group);model.completeIds.add(group.id);
      const asof=pointFrom(seal);
      if(!model.latestComplete||pointOrder(asof,model.latestComplete)>0)model.latestComplete=asof;
      markAsof(model,asof);
      for(const row of group.parts)setEnergyPoint(row.type==='energy_day'?model.days:model.months,
        row.energy.period,pointFrom(row),model);
    }
    for(const row of state.rows)if(row.schema==='2'&&row.energy) {
      const model=energyModel('v2:'+row.energy.scope,row.energy.scope,'v2');
      if(model)model.telemetry.push(row);
    }
    for(const model of state.energyModels.values()) {
      for(const group of model.incomplete) {
        let revision=null;
        for(const row of group.rows)if(revision===null||compareRevision(row.energy.revision,revision)<0)revision=row.energy.revision;
        if(revision!==null&&(!model.latestComplete||compareRevision(revision,model.latestComplete.revision)>0)&&
           (model.blockedRevision===null||compareRevision(revision,model.blockedRevision)<0))model.blockedRevision=revision;
      }
      const baseDays=new Map(model.days),baseMonths=new Map(model.months),dayCoverage=new Map(),monthCoverage=new Map();
      const latestDays=new Map(),latestMonths=new Map();
      for(const row of model.telemetry) {
        const e=row.energy;
        if(model.blockedRevision!==null&&compareRevision(e.revision,model.blockedRevision)>=0)continue;
        const period=e.period,month=period.slice(0,7),year=period.slice(0,4);
        const baseDay=baseDays.get(period),baseMonth=baseMonths.get(month);
        if(e.hasData&&e.validMs>0) {
          if(!baseDay||compareRevision(e.revision,baseDay.revision)>0)dayCoverage.set(period,(dayCoverage.get(period)||0)+e.validMs);
          if(!baseMonth||compareRevision(e.revision,baseMonth.revision)>0)monthCoverage.set(month,(monthCoverage.get(month)||0)+e.validMs);
        }
        const latest=pointFrom(row,{from:'running',coverageExact:false});
        markAsof(model,latest);
        if(e.running.today!==null) {
          const point={...latest,kwh:e.running.today,hasData:true,finalized:false};
          setEnergyPoint(latestDays,period,point,model);
        }
        if(e.running.month!==null) {
          const point={...latest,period:month,kwh:e.running.month,hasData:true,finalized:false};
          setEnergyPoint(latestMonths,month,point,model);
        }
        if(e.running.year!==null)setEnergyPoint(model.years,year,{...latest,period:year,kwh:e.running.year,hasData:true,finalized:false},model);
      }
      for(const [period,point] of latestDays) {
        const base=baseDays.get(period);
        point.validMs=(base?base.validMs:0)+(dayCoverage.get(period)||0);
        if(!Number.isSafeInteger(point.validMs))point.validMs=null;
        setEnergyPoint(model.days,period,point,model);
      }
      for(const [period,point] of latestMonths) {
        const base=baseMonths.get(period);
        point.validMs=(base?base.validMs:0)+(monthCoverage.get(period)||0);
        if(!Number.isSafeInteger(point.validMs))point.validMs=null;
        setEnergyPoint(model.months,period,point,model);
      }
    }
    const legacyGroups=new Map();
    for(const row of state.rows)if(row.schema==='1') {
      if(!legacyGroups.has(row.device))legacyGroups.set(row.device,[]);
      legacyGroups.get(row.device).push(row);
    }
    for(const [scope,rows] of legacyGroups) {
      const model=energyModel('v1:'+scope,scope,'legacy');if(!model)continue;
      model.legacyRows=rows;
      const stats=analyze(rows);
      model.ambiguous=stats.ambiguous||stats.powerRows.some(row=>value(row,0,0)<0);
      for(const row of rows) {
        const local=get(row,'minute_start_local'),period=periodDate(local.slice(0,10));
        if(!period) {
          energyIssue('legacy-date:'+row.source.key,'旧版 CSV '+row.source.name+' 缺少可靠的本地日期，相关行未分配到日／月电量。');
          continue;
        }
        const asof={period,revision:'0',updated:row.end,updatedRaw:get(row,'period_end_utc'),source:row.source.name,line:row.line};
        markAsof(model,asof);
        if(model.ambiguous||!hasMean(row,0))continue;
        const amount=value(row,0,0)*value(row,0,4)/3600000000;
        if(!Number.isFinite(amount)||amount<0) {model.ambiguous=true;continue;}
        const previous=model.days.get(period),validMs=(previous?previous.validMs:0)+value(row,0,4);
        const kwh=(previous?previous.kwh:0)+amount;
        if(!Number.isFinite(kwh)||!Number.isSafeInteger(validMs)){model.ambiguous=true;continue;}
        model.days.set(period,{...asof,kwh,validMs,hasData:true,finalized:false,quality:'imported_only',from:'legacy',coverageExact:true});
      }
      if(model.ambiguous) {model.days.clear();continue;}
      for(const point of model.days.values()) {
        const month=point.period.slice(0,7),old=model.months.get(month);
        model.months.set(month,{...point,period:month,kwh:(old?old.kwh:0)+point.kwh,
          validMs:(old?old.validMs:0)+point.validMs});
      }
    }
  }
  function modelPeriods(model) {
    const months=new Set(model.months.keys());
    for(const period of model.days.keys())months.add(period.slice(0,7));
    if(model.asof)months.add(model.asof.period.slice(0,7));
    const monthList=Array.from(months).filter(month=>periodDate(month,true)).sort().reverse();
    const years=Array.from(new Set(monthList.map(month=>month.slice(0,4)))).sort().reverse();
    if(monthList.length>2400||years.length>200)energyIssue('period-limit:'+model.key,'电量选择器最多展示最近 200 年／2400 个月，请按来源分批查看更早汇总。');
    return {months:monthList.slice(0,2400),years:years.slice(0,200)};
  }
  function selectedEnergyModel() {return state.energyModels.get(ui['energy-scope'].value)||null;}
  function rebuildEnergyOptions() {
    const previous=ui['energy-scope'].value;
    const populated=model=>{
      for(const point of model.days.values())if(point.hasData)return true;
      for(const point of model.months.values())if(point.hasData)return true;
      return false;
    };
    const models=Array.from(state.energyModels.values()).map(model=>({model,known:populated(model),
      sampled:model.telemetry.some(row=>row.energy.hasData||row.energy.running.today!==null)}))
      .sort((a,b)=>(a.model.mode==='v2'?0:1)-(b.model.mode==='v2'?0:1)||Number(b.known)-Number(a.known)||
        Number(b.sampled)-Number(a.sampled)||(b.model.asof?b.model.asof.updated:-Infinity)-(a.model.asof?a.model.asof.updated:-Infinity)||
        Number(b.model.scope.startsWith('psu:'))-Number(a.model.scope.startsWith('psu:'))||a.model.scope.localeCompare(b.model.scope))
      .map(item=>item.model);
    const options=models.map(model=>({value:model.key,label:(model.mode==='legacy'?'旧版导入范围 · ':'电量账本 · ')+identityLabel(model.scope)}));
    if(!options.length)options.push({value:'',label:'尚无电量记录'});
    const choice=state.energyScopePinned&&options.some(item=>item.value===previous)?previous:options[0].value;
    if(choice!==previous)state.energyAutoPeriod=true;
    D.fillSelect(ui['energy-scope'],options,choice);
    const model=selectedEnergyModel();
    if(!model) {
      D.fillSelect(ui['energy-month'],[{value:'',label:'尚无月份'}],'');
      D.fillSelect(ui['energy-year'],[{value:'',label:'尚无年份'}],'');
    } else {
      const periods=modelPeriods(model),asof=model.asof&&model.asof.period;
      const month=state.energyAutoPeriod?(asof?asof.slice(0,7):periods.months[0]):ui['energy-month'].value;
      const year=state.energyAutoPeriod?(asof?asof.slice(0,4):periods.years[0]):ui['energy-year'].value;
      D.fillSelect(ui['energy-month'],periods.months.length?periods.months.map(value=>({value,label:value})):[{value:'',label:'尚无日历数据'}],periods.months.includes(month)?month:periods.months[0]);
      D.fillSelect(ui['energy-year'],periods.years.length?periods.years.map(value=>({value,label:value+' 年'})):[{value:'',label:'尚无日历数据'}],periods.years.includes(year)?year:periods.years[0]);
    }
    for(const id of ['energy-scope','energy-month','energy-year'])ui[id].disabled=!model;
    ui['energy-export'].disabled=state.busy||!model;
  }
  function energyStatus(point,period,model) {
    if(!point||!point.hasData) return model&&model.asof&&period>model.asof.period.slice(0,period.length)?'统计日之后':'未携带有效汇总';
    const asof=model&&model.asof?model.asof.period.slice(0,period.length):'';
    if(asof&&period>asof)return '晚于统计日，需核对时钟';
    const status=point.finalized||(asof&&period<asof)?'已结算':'累计中';
    return status+(point.quality==='recovery_incomplete'?' · 恢复不完整':point.from==='legacy'?' · 仅导入范围':' · 已监测估算');
  }
  function energyNote(point,period,model) {
    const lines=[period,energyStatus(point,period,model)];
    if(point&&point.hasData)lines.push((point.coverageExact?'有效覆盖 ':'已知覆盖至少 ')+D.duration(point.validMs),
      '截至 '+D.time(point.updated,state.utc)+'；'+point.source+' · 行 '+point.line);
    else lines.push('未填零，不代表没有耗电');
    return lines.join('；');
  }
  function energyBar(point,period,label,model) {
    const present=!!point&&point.hasData&&Number.isFinite(point.kwh);
    const accumulating=present&&!point.finalized&&model&&model.asof&&period===model.asof.period.slice(0,period.length);
    return {label,value:present?point.kwh:null,missing:!present,color:present?(point.quality==='recovery_incomplete'?'#b47d35':accumulating?'#16756f':'#3f799d'):'#dbe2e7',
      note:energyNote(point,period,model)};
  }
  function yearPoint(model,year) {
    if(!model||!year)return null;
    let sum=0,validMs=0,count=0,latest=null;
    for(const [period,point] of model.months)if(period.startsWith(year+'-')&&point.hasData&&point.kwh!==null) {
      sum+=point.kwh;validMs+=point.validMs||0;count++;
      if(!latest||pointOrder(point,latest)>0)latest=point;
    }
    const running=model.years.get(year);
    if(running&&(!latest||(compareRevision(running.revision,latest.revision)||(running.updated-latest.updated))>=0))return running;
    if(!count||!Number.isFinite(sum))return null;
    return {...latest,period:year,kwh:sum,validMs,hasData:true};
  }
  function renderEnergy() {
    const model=selectedEnergyModel(),asof=model&&model.asof,month=ui['energy-month'].value,year=ui['energy-year'].value;
    const day=asof&&model.days.get(asof.period),monthPoint=model&&model.months.get(month),yearTotal=yearPoint(model,year);
    D.text(ui['energy-today'],day&&day.hasData?D.fmt(day.kwh,3):'—');
    D.text(ui['energy-month-total'],monthPoint&&monthPoint.hasData?D.fmt(monthPoint.kwh,3):'—');
    D.text(ui['energy-year-total'],yearTotal&&yearTotal.hasData?D.fmt(yearTotal.kwh,3):'—');
    D.text(ui['energy-today-note'],asof?asof.period+' · '+energyStatus(day,asof.period,model):'导入最新 CSV 后显示');
    D.text(ui['energy-month-note'],month?month+' · 已记录的估算用电':'按本地自然月汇总');
    D.text(ui['energy-year-note'],year?year+' 年 · 已记录的估算用电':'按自然年各月累计');
    D.text(ui['energy-asof'],asof?'统计日 '+asof.period+' · 最后结算 '+D.time(asof.updated,state.utc)+' · '+asof.source:'尚无可用电量数据');
    let note='耗电统计使用独立设备和年月选择，不受下方分钟趋势的设备、会话或时间范围限制。';
    if(model) {
      note+=model.mode==='legacy'?' 当前为旧版 CSV 推算，仅覆盖已导入的有效分钟，不包含未导入历史。':' 当前使用文件内账本汇总和后续分钟累计，不重复相加累计列。';
      if(model.ambiguous)note+=' 旧记录存在时间重叠、时钟／数值歧义，已暂停电量推算；请导入单一且无重叠的记录。';
      if(model.incomplete.length)note+=' 有 '+D.integer(model.incomplete.length)+' 个不完整快照被忽略；保留此前完整汇总。';
      if(model.mode==='v2'&&!model.groups.length&&!model.incomplete.length)note+=' 未携带完整历史快照，累计值可见，但日／月拆分可能缺失。';
    }
    note+=' 所有数值只代表已监测覆盖；已结算不等于全天完整测量，空白与真实 0 分开。';
    D.text(ui['energy-note'],note);
    const days=[],months=[],table=[];
    if(model&&month&&periodDate(month,true)) {
      const y=Number(month.slice(0,4)),m=Number(month.slice(5,7)),length=[31,y%4===0&&(y%100!==0||y%400===0)?29:28,31,30,31,30,31,31,30,31,30,31][m-1];
      for(let i=1;i<=length;i++) {
        const period=month+'-'+String(i).padStart(2,'0'),point=model.days.get(period);
        days.push(energyBar(point,period,String(i)+'日',model));table.push({period,point});
      }
    }
    if(model&&year)for(let i=1;i<=12;i++) {
      const period=year+'-'+String(i).padStart(2,'0'),point=model.months.get(period);
      months.push(energyBar(point,period,i+'月',model));table.push({period,point});
    }
    charts.energyDays.set({type:'histogram',title:'自然月每日估算用电',unit:'kWh',bars:days,emptyText:month?'该月未携带有效日汇总；可导入对应月份 CSV':'导入 CSV 后显示逐日电量'});
    charts.energyMonths.set({type:'histogram',title:'自然年每月估算用电',unit:'kWh',bars:months,emptyText:'当前年份未携带有效月汇总'});
    ui['energy-table-body'].replaceChildren();
    for(const item of table) {
      const point=item.point,tr=node('tr');
      tr.append(cell(item.period),cell(point&&point.hasData?D.fmt(point.kwh,6):'—'),
        cell(point&&point.hasData?(point.coverageExact?'':'至少 ')+D.duration(point.validMs):'—'),
        cell(energyStatus(point,item.period,model)),cell(point?D.time(point.updated,state.utc)+' · '+point.source:'—'));
      ui['energy-table-body'].append(tr);
    }
    if(!table.length) {const tr=node('tr'),td=cell('导入 CSV 后查看日／月汇总');td.colSpan=5;tr.append(td);ui['energy-table-body'].append(tr);}
    ui['energy-export'].disabled=state.busy||!model;
  }
  function downloadRows(rows,filename) {
    const headers=[],seen=new Set();
    for(const row of rows)for(const header of row.headers)if(!seen.has(header)){headers.push(header);seen.add(header);}
    const output=rows.map(row=>{
      const result=Object.create(null);
      for(const header of headers)result[header]=get(row,header);
      return result;
    });
    D.exportCSV(headers,output,filename);
  }
  function exportEnergy() {
    const model=selectedEnergyModel();if(!model||state.busy)return;
    let rows;
    if(model.mode==='legacy')rows=model.legacyRows;
    else {
      const ids=model.completeIds;
      rows=Array.from(state.energyRows.values()).filter(row=>row.energy.scope===model.scope&&ids.has(row.energy.snapshot));
      rows=rows.concat(model.telemetry.filter(row=>model.blockedRevision===null||compareRevision(row.energy.revision,model.blockedRevision)<0));
    }
    if(!rows.length) {showNotice('当前没有可导出的完整电量来源。','warn');return;}
    downloadRows(rows,'dm-energy-'+(model.asof?model.asof.period:'records')+'.csv');
    showNotice(model.mode==='legacy'?'已导出该旧版来源的全部已导入分钟；没有补造历史汇总。':
      '已导出所选电量设备的完整快照、seal 与已导入后续累计；保留未知扩展列和恢复水位。不受下方分钟筛选影响。','info');
  }

  function baseRows() {
    const device=ui['device-filter'].value,session=ui['session-filter'].value;
    return state.rows.filter(row=>(device==='*'||row.device===device)&&(session==='*'||row.sessionKey===session));
  }
  function fillSessions(preferred='*') {
    const device=ui['device-filter'].value;
    const sessions=new Map();
    for(const row of state.rows)if(device==='*'||row.device===device) {
      const current=sessions.get(row.sessionKey)||{row,count:0};
      current.count++;sessions.set(row.sessionKey,current);
    }
    const options=[{value:'*',label:'所有会话'}];
    for(const [key,item] of sessions)options.push({value:key,label:'运行 '+short(item.row.agent.replace(/[{}]/g,''),8)+' / '+(item.row.session?short(item.row.session.replace(/[{}]/g,''),8):'未建立设备会话')+' · '+D.integer(item.count)+' 条'});
    D.fillSelect(ui['session-filter'],options,options.some(o=>o.value===preferred)?preferred:'*');
  }
  function resetRange() {
    const rows=baseRows();
    if(!rows.length) {state.rangeStart=null;state.rangeEnd=null;ui['range-start'].value='';ui['range-end'].value='';return;}
    state.rangeStart=Math.floor(rows[0].start/60000)*60000;
    state.rangeEnd=Math.floor(rows[rows.length-1].start/60000)*60000+60000;
    state.rangeMode='all';
    showRangeControls();
  }
  function showRangeControls() {
    ui['range-start'].value=state.rangeStart===null?'':D.inputTime(state.rangeStart,state.utc);
    ui['range-end'].value=state.rangeEnd===null?'':D.inputTime(state.rangeEnd-60000,state.utc);
  }
  function rebuildOptions() {
    const previous=ui['device-filter'].value, oldSession=ui['session-filter'].value;
    const groups=new Map();
    for(const row of state.rows) {
      const group=groups.get(row.device)||{count:0,weight:0};
      group.count++;if(hasMean(row,0))group.weight+=value(row,0,4);
      groups.set(row.device,group);
    }
    const sorted=Array.from(groups.entries()).sort((a,b)=>b[1].weight-a[1].weight||b[1].count-a[1].count);
    const options=[{value:'*',label:'所有身份（不合并统计）'}];
    for(const [id,group] of sorted)options.push({value:id,label:identityLabel(id)+' · '+D.integer(group.count)+' 条'});
    let choice=options.some(option=>option.value===previous)?previous:'*';
    if(state.firstSelection && sorted.length) {choice=sorted[0][0];state.firstSelection=false;}
    D.fillSelect(ui['device-filter'],options,choice);
    fillSessions(oldSession);
    if(state.rangeMode==='all')resetRange();
    const disabled=state.rows.length===0;
    for(const id of ['device-filter','session-filter','range-start','range-end','apply-range','all-range'])ui[id].disabled=disabled;
  }
  function selectedRows() {
    return baseRows().filter(row=>(state.rangeStart===null||row.start>=state.rangeStart)&&(state.rangeEnd===null||row.start<state.rangeEnd));
  }
  function renderSources() {
    D.text(ui['file-summary'],state.files.size?D.integer(state.files.size)+' 个文件 · '+D.integer(state.rows.length)+' 条采样 / '+D.integer(state.energyRows.size)+' 条汇总 · '+D.bytes(state.bytes):'尚未导入 CSV');
    ui['file-list'].replaceChildren();
    for(const source of state.files.values()) {
      const item=node('li',undefined,'file-item');
      const name=node('span',source.name);name.title=source.name;
      const detail=node('span',D.integer(source.accepted)+' 条采样 / '+D.integer(source.summaries||0)+' 条汇总 · '+D.bytes(source.size)+' · '+source.encoding,'csv-file-meta');
      item.append(name,detail);ui['file-list'].append(item);
    }
    D.text(ui['import-title'],state.files.size?'继续拖入 CSV，追加到当前记录':'拖入每日 CSV，或点击选择文件');
    const count=state.issues.length+state.issueOverflow;
    ui['import-diagnostics'].hidden=count===0;
    D.text(ui['diagnostics-title'],'导入诊断 · '+D.integer(count)+' 项（仅列前 200 项）');
    ui['diagnostics-list'].replaceChildren(...state.issues.map(issue=>node('li',issue)));
  }
  function analyze(rows) {
    const powerRows=rows.filter(row=>hasMean(row,0));
    const identities=new Set(powerRows.map(row=>row.device));
    let totalMs=0, weighted=0, peak=null, overlapping=false, timingAmbiguous=false,lastEnd=-Infinity;
    for(const row of rows) {
      const maximum=value(row,0,2);
      if(maximum!==null)peak=peak===null?maximum:Math.max(peak,maximum);
    }
    for(const row of powerRows) {
      const ms=value(row,0,4);
      totalMs+=ms;weighted+=value(row,0,0)*ms;
      if(row.start<lastEnd-1)overlapping=true;
      lastEnd=Math.max(lastEnd,row.end);
      if(row.end<=row.start||ms>row.end-row.start+2000)timingAmbiguous=true;
    }
    const arithmeticInvalid=!Number.isFinite(totalMs)||!Number.isFinite(weighted);
    const ambiguous=identities.size>1||overlapping||timingAmbiguous||arithmeticInvalid;
    const partial=rows.filter(row=>row.partial).length;
    const noPower=rows.length-powerRows.length;
    const fault=rows.filter(row=>row.nonzeroFault).length;
    const perAgent=new Map();
    let rejected=0;
    for(const row of rows) {
      rejected+=row.rejected;
      if(row.dropped!==null&&row.dropped>=0)perAgent.set(row.agent,Math.max(perAgent.get(row.agent)||0,row.dropped));
    }
    let dropped=0;for(const count of perAgent.values())dropped+=count;
    return {powerRows,identities,totalMs,weighted,peak,overlapping,timingAmbiguous,arithmeticInvalid,ambiguous,partial,noPower,fault,rejected,dropped};
  }
  function badge(text,kind,title) {
    const element=node('span',text,'badge '+(kind||''));
    if(title)element.title=title;
    return element;
  }
  function renderSummary(rows,stats) {
    const usable=stats.totalMs>0&&!stats.ambiguous;
    D.text(ui['kpi-power'],usable?D.fmt(stats.weighted/stats.totalMs,2):'—');
    D.text(ui['kpi-peak'],stats.peak===null?'—':D.fmt(stats.peak,2));
    D.text(ui['kpi-energy'],usable?D.fmt(stats.weighted/3600000,3):'—');
    D.text(ui['kpi-duration'],usable?D.duration(stats.totalMs):'—');
    const ambiguity=stats.identities.size>1?'选择了多个设备身份':stats.overlapping?'存在时间重叠的有效记录':stats.timingAmbiguous?'记录时钟与有效时长有歧义':stats.arithmeticInvalid?'数值超出可计算范围':'';
    D.text(ui['kpi-power-note'],ambiguity||'按各行功率有效毫秒数加权');
    D.text(ui['kpi-energy-note'],ambiguity?'请选择单一且无歧义的设备／会话':'仅积分已覆盖数据，不外推缺口');
    D.text(ui['kpi-duration-note'],ambiguity?'不将重叠记录相加为覆盖率':'功率积分使用的累计有效时长');
    let note=rows.length?'当前 '+D.integer(rows.length)+' 条完整记录，'+D.time(rows[0].start,state.utc)+' 至 '+D.time(bounds(rows).xmax,state.utc)+'。':'没有符合上方条件的记录。';
    if(ambiguity)note+=' '+ambiguity+'，暂停加权均值、电量和分布统计，避免重复积分。';
    else if(rows.length&&!stats.powerRows.length)note+=' 当前范围没有有效功率积分；空值不会补零。';
    else if(rows.length)note+=' 图线连接的是区间均值；缺口不连线，部分区间不按全天补齐。';
    D.text(ui['analysis-note'],note);
    ui['quality-summary'].replaceChildren();
    if(rows.length) {
      ui['quality-summary'].append(badge(D.integer(stats.partial)+' 条分段／部分记录','','分段或覆盖不足不等于设备故障'),
        badge(D.integer(stats.noPower)+' 条无功率覆盖',stats.noPower?'badge-warn':''),
        badge(D.integer(stats.fault)+' 条观察到非零故障码',stats.fault?'badge-warn':'','仅展示原始记录，不解释故障位'),
        badge(D.integer(stats.rejected)+' 个被拒候选',stats.rejected?'badge-warn':'','按各区间记录计数求和，不等同于物理坏帧数量'));
      if(stats.dropped>0)ui['quality-summary'].append(badge('实例报告丢弃累计 '+D.integer(stats.dropped),'badge-warn','每个运行实例取最大值后相加；不是当前筛选时段的实际丢失量'));
    }
  }
  function points(rows,metric,transform) {
    let previous=null, previousValid=false;
    const detailedHover=rows.length<=4000;
    return rows.map(row=>{
      const valid=hasMean(row,metric);
      const ms=value(row,metric,4),duration=row.end-row.start;
      const before=previous;
      let split=!before||!previousValid||row.device!==before.device||row.sessionKey!==before.sessionKey||
        row.start>before.end+1000||row.start<before.end-1||Boolean(row.reason);
      if(before && value(before,metric,4)!==null && value(before,metric,4)<before.end-before.start-2)split=true;
      if(ms!==null&&ms<duration-2)split=true;
      previous=row;previousValid=valid;
      const raw=valid?value(row,metric,0):null;
      const apply=n=>n===null?null:transform?transform(n):n;
      const point={x:row.start,y:apply(raw),breakBefore:split,
        meta:detailedHover?D.time(row.start,state.utc)+' → '+D.time(row.end,state.utc)+'\n'+
          METRICS[metric].label+'：'+D.fmt(raw,2)+' '+METRICS[metric].unit+
          '；有效 '+(ms===null?'—':D.fmt(ms/1000,3))+' 秒'+(row.partial?'；部分区间':'')+
          '\n'+row.source.name+' · 行 '+row.line:undefined};
      point.low=valid?apply(value(row,metric,1)):null;
      point.high=valid?apply(value(row,metric,2)):null;
      return point;
    });
  }
  function lineSeries(rows,index,options={}) {
    return {name:METRICS[index].label,color:METRICS[index].color,unit:options.unit||METRICS[index].unit,
      points:points(rows,index,options.transform),...options};
  }
  function bounds(rows) {
    if(!rows.length)return {};
    let maximum=rows[0].end;
    for(const row of rows)maximum=Math.max(maximum,row.end);
    return {xmin:rows[0].start,xmax:Math.max(maximum,rows[0].start+1000)};
  }
  function histogram(stats) {
    const emptyText=stats.ambiguous?'请选择单一、无时间重叠且数值可计算的设备／会话':'没有有效功率覆盖数据';
    if(stats.ambiguous||!stats.powerRows.length)return {bars:[],emptyText};
    let low=Infinity,high=-Infinity;
    for(const row of stats.powerRows){const p=value(row,0,0);low=Math.min(low,p);high=Math.max(high,p);}
    const count=low===high?1:Math.min(16,Math.max(5,Math.ceil(Math.sqrt(stats.powerRows.length))));
    const step=low===high?1:(high-low)/count;
    if(!Number.isFinite(low)||!Number.isFinite(high)||!Number.isFinite(step)||step<=0)return {bars:[],emptyText:'数值跨度超出分布图计算范围，请检查原始字段'};
    const bins=Array.from({length:count},(_,i)=>({label:count===1?D.fmt(low,2)+' W':D.fmt(low+i*step,1)+'–'+D.fmt(i===count-1?high:low+(i+1)*step,1)+' W',value:0,color:'#16756f',note:'按有效功率覆盖时长加权'}));
    for(const row of stats.powerRows) {
      const index=count===1?0:Math.min(count-1,Math.max(0,Math.floor((value(row,0,0)-low)/step)));
      bins[index].value+=value(row,0,4)/60000;
    }
    return {bars:bins,emptyText};
  }
  function renderCharts(rows,stats) {
    const base={...bounds(rows),utc:state.utc,emptyText:'导入并筛选 CSV 后显示图表'};
    charts.power.set({...base,type:'line',title:'输入功率趋势',unit:'W',zero:true,series:[lineSeries(rows,0,{band:true})]});
    charts.temperature.set({...base,type:'line',title:'三处温度',unit:'℃',series:[lineSeries(rows,2),lineSeries(rows,9),lineSeries(rows,10)]});
    charts.fan.set({...base,type:'line',title:'风扇转速',unit:'RPM',zero:true,series:[lineSeries(rows,1,{step:true})]});
    const percent=ui['rail-mode'].value==='percent';
    charts.rails.set({...base,type:'line',title:'四路电压',unit:percent?'%':'V',zero:percent,series:[4,5,6,7].map(index=>lineSeries(rows,index,percent?{unit:'%',transform:n=>(n/METRICS[index].nominal-1)*100}:{}))});
    const frequency=ui['mains-mode'].value==='frequency',mains=frequency?8:3;
    charts.mains.set({...base,type:'line',title:METRICS[mains].label,unit:METRICS[mains].unit,series:[lineSeries(rows,mains)]});
    charts.distribution.set({type:'histogram',title:'输入功率分布',unit:'有效分钟',...histogram(stats)});
    charts.state.set({...base,type:'timeline',title:'记录状态摘要',intervals:rows.filter(row=>row.end>row.start).map(row=>({
      start:row.start,end:row.end,label:stateLabel(row.state),color:Object.hasOwn(STATE,row.state)?STATE[row.state].color:'#9ba3aa',
      meta:rows.length<=4000?D.time(row.start,state.utc)+' → '+D.time(row.end,state.utc)+'\n'+stateLabel(row.state)+(row.partial?' · 部分区间':'')+'\n区间摘要不表示每一时刻的精确连接状态':undefined
    }))});
  }
  function filteredTableRows() {
    const quality=ui['quality-filter'].value,query=ui['record-search'].value.trim().toLowerCase();
    return state.selectedRows.filter(row=>{
      if(quality==='issue'&&!row.issue)return false;
      if(quality==='partial'&&!row.partial)return false;
      if(quality==='fault'&&!row.nonzeroFault)return false;
      if(query && !(row.device+' '+row.agent+' '+row.session+' '+row.reason+' '+row.source.name+' '+row.sequence).toLowerCase().includes(query))return false;
      return true;
    }).reverse();
  }
  function cell(text,title) {
    const element=node('td',text);
    if(title)element.title=title;
    return element;
  }
  function renderTable(resetPage=false) {
    if(resetPage)state.page=1;
    state.tableRows=filteredTableRows();
    const total=state.tableRows.length,pages=Math.ceil(total/state.pageSize);
    state.page=Math.max(1,Math.min(state.page,pages||1));
    const first=(state.page-1)*state.pageSize;
    ui['records-body'].replaceChildren();
    for(const row of state.tableRows.slice(first,first+state.pageSize)) {
      const tr=node('tr');
      tr.append(cell(D.time(row.start,state.utc),get(row,'period_start_utc')+' → '+get(row,'period_end_utc')));
      for(const metric of [0,1,2,3])tr.append(cell(hasMean(row,metric)?D.fmt(value(row,metric,0),metric===1?1:2):'—'));
      const valid=value(row,0,4);
      tr.append(cell(valid!==null?D.fmt(valid/1000,3):'—'));
      const flags=cell('');
      flags.append(badge(stateLabel(row.state),row.state==='connected'?'badge-ok':''));
      if(row.partial)flags.append(document.createTextNode(' '),badge('部分区间'));
      if(row.nonzeroFault)flags.append(document.createTextNode(' '),badge('非零故障','badge-warn'));
      tr.append(flags,cell(short(row.source.name,25)+' : '+row.line,row.source.name+' · 第 '+row.line+' 行'));
      const action=cell(''),button=node('button','详情','button quiet small');button.type='button';
      button.setAttribute('aria-label','查看 '+D.time(row.start,state.utc)+' 的记录详情');
      button.addEventListener('click',()=>showRecord(row));action.append(button);tr.append(action);
      ui['records-body'].append(tr);
    }
    if(!total) {const tr=node('tr'),td=cell('没有符合条件的记录');td.colSpan=9;td.className='csv-empty-cell';tr.append(td);ui['records-body'].append(tr);}
    D.text(ui['table-summary'],D.integer(total)+' 条明细 · 最新在前 · 上方范围共 '+D.integer(state.selectedRows.length)+' 条');
    D.text(ui['page-label'],'第 '+(pages?state.page:0)+' / '+pages+' 页');
    ui['previous-page'].disabled=state.page<=1||pages===0;
    ui['next-page'].disabled=state.page>=pages||pages===0;
  }
  function showRecord(row) {
    const facts=[
      ['实际开始',D.time(row.start,state.utc)],['实际结束',D.time(row.end,state.utc)],
      ['原始本地分钟标签',get(row,'minute_start_local')||'—'],['功率有效覆盖',D.duration(value(row,0,4))],
      ['设备身份',row.device],['运行实例／序号',row.agent+' / '+row.sequence],
      ['设备会话',row.session||'未建立'],['元信息版本',get(row,'metadata_id')||'—'],
      ['来源',row.source.name+' · 第 '+row.line+' 行'],['状态／分段原因',stateLabel(row.state)+' / '+(row.reason||'无')],
      ['原始故障码',row.fault===null?'—':String(row.fault)+' (0x'+Math.trunc(row.fault).toString(16).toUpperCase()+')'],
      ['协议观察状态',get(row,'rx_profile_validation')||'—']
    ];
    if(row.energy)facts.push(['电量设备',row.energy.scope],['本段估算电量',row.energy.hasData?D.fmt(row.energy.kwh,8)+' kWh':'—'],
      ['当日累计',D.fmt(row.energy.running.today,6)+' kWh'],['累计修订号',row.energy.revision]);
    ui['record-facts'].replaceChildren();
    for(const [label,text] of facts){const block=node('div');block.append(node('dt',label),node('dd',text));ui['record-facts'].append(block);}
    ui['metric-details'].replaceChildren();
    METRICS.forEach((metric,index)=>{
      const tr=node('tr');tr.append(cell(metric.label+' / '+metric.unit));
      for(let s=0;s<4;s++)tr.append(cell(D.fmt(value(row,index,s),index===1?1:3)));
      const ms=value(row,index,4);tr.append(cell(ms===null?'—':D.fmt(ms/1000,3)));ui['metric-details'].append(tr);
    });
    ui['raw-details'].replaceChildren();
    row.headers.forEach((header,index)=>{
      const tr=node('tr');tr.append(cell(header),cell(row.values[index]||'—'));ui['raw-details'].append(tr);
    });
    ui['record-dialog'].showModal();
  }
  function renderAll(resetPage=false) {
    renderSources();renderEnergy();
    state.selectedRows=selectedRows();
    const stats=analyze(state.selectedRows);
    renderSummary(state.selectedRows,stats);
    renderCharts(state.selectedRows,stats);
    renderTable(resetPage);
    ui['export-button'].disabled=state.busy||state.selectedRows.length===0;
    ui['clear-button'].disabled=state.busy||(state.files.size===0&&state.issues.length===0&&state.issueOverflow===0);
  }
  function applyRange() {
    const start=ui['range-start'].value?D.parseInput(ui['range-start'].value,state.utc):null;
    const end=ui['range-end'].value?D.parseInput(ui['range-end'].value,state.utc):null;
    if((ui['range-start'].value&&start===null)||(ui['range-end'].value&&end===null)){showNotice('时间格式无效，请重新选择。','error');return;}
    if(start!==null&&end!==null&&end<start){showNotice('结束分钟不能早于起始分钟。','error');return;}
    state.rangeStart=start;state.rangeEnd=end===null?null:end+60000;state.rangeMode='custom';
    showNotice('已按记录区间起点筛选；结束分钟内开始的记录均包含。','info');
    renderAll(true);
  }
  function exportRows() {
    const rows=state.selectedRows;if(!rows.length||state.busy)return;
    downloadRows(rows,'dm-telemetry-filtered.csv');
    showNotice('已导出下方设备、会话和时间范围的 '+D.integer(rows.length)+' 条分钟采样；此导出不含历史汇总，完整电量来源请用“导出电量 CSV”。源文件未改动。','info');
  }
  function clear() {
    if(state.busy)return;
    state.rows=[];state.byKey.clear();state.files.clear();state.bytes=0;state.issues=[];state.issueOverflow=0;
    state.energyRows.clear();state.energyModels.clear();state.energyWarnings.clear();state.energyAutoPeriod=true;state.energyScopePinned=false;
    state.duplicates=0;state.conflicts=0;state.rejected=0;state.sourceSequence=0;state.firstSelection=true;state.rangeMode='all';state.rangeStart=null;state.rangeEnd=null;
    state.page=1;ui['record-search'].value='';ui['quality-filter'].value='all';
    if(ui['record-dialog'].open)ui['record-dialog'].close();
    rebuildEnergyOptions();rebuildOptions();renderAll(true);showNotice('');
  }

  ui['import-button'].addEventListener('click',()=>ui['file-input'].click());
  ui['file-input'].addEventListener('change',()=>importFiles(ui['file-input'].files));
  ui['clear-button'].addEventListener('click',clear);
  ui['drop-zone'].addEventListener('click',()=>{if(!state.busy)ui['file-input'].click();});
  ui['drop-zone'].addEventListener('keydown',event=>{if((event.key==='Enter'||event.key===' ')&&!state.busy){event.preventDefault();ui['file-input'].click();}});
  for(const name of ['dragenter','dragover'])ui['drop-zone'].addEventListener(name,event=>{event.preventDefault();if(!state.busy)ui['drop-zone'].classList.add('is-dragover');});
  for(const name of ['dragleave','drop'])ui['drop-zone'].addEventListener(name,event=>{event.preventDefault();ui['drop-zone'].classList.remove('is-dragover');});
  ui['drop-zone'].addEventListener('drop',event=>importFiles(event.dataTransfer.files));
  ui['device-filter'].addEventListener('change',()=>{fillSessions();resetRange();renderAll(true);});
  ui['session-filter'].addEventListener('change',()=>{resetRange();renderAll(true);});
  ui['timezone-filter'].addEventListener('change',()=>{state.utc=ui['timezone-filter'].value==='utc';showRangeControls();renderAll(false);});
  ui['apply-range'].addEventListener('click',applyRange);
  ui['all-range'].addEventListener('click',()=>{resetRange();showNotice('已显示当前身份／会话的全部时间。','info');renderAll(true);});
  ui['export-button'].addEventListener('click',exportRows);
  ui['energy-export'].addEventListener('click',exportEnergy);
  ui['energy-scope'].addEventListener('change',()=>{state.energyScopePinned=true;state.energyAutoPeriod=true;rebuildEnergyOptions();renderEnergy();});
  for(const id of ['energy-month','energy-year'])ui[id].addEventListener('change',()=>{state.energyAutoPeriod=false;renderEnergy();});
  ui['quality-filter'].addEventListener('change',()=>renderTable(true));
  ui['record-search'].addEventListener('input',D.debounce(()=>renderTable(true),180));
  ui['page-size'].addEventListener('change',()=>{state.pageSize=Number(ui['page-size'].value)||50;renderTable(true);});
  ui['previous-page'].addEventListener('click',()=>{state.page--;renderTable(false);});
  ui['next-page'].addEventListener('click',()=>{state.page++;renderTable(false);});
  ui['rail-mode'].addEventListener('change',()=>renderCharts(state.selectedRows,analyze(state.selectedRows)));
  ui['mains-mode'].addEventListener('change',()=>renderCharts(state.selectedRows,analyze(state.selectedRows)));
  ui['close-dialog'].addEventListener('click',()=>ui['record-dialog'].close());
  // 防止拖入文件时浏览器直接导航离开；文件内容只交给显式导入入口。
  window.addEventListener('dragover',event=>{if(event.dataTransfer&&Array.from(event.dataTransfer.types).includes('Files'))event.preventDefault();});
  window.addEventListener('drop',event=>{if(event.dataTransfer&&event.dataTransfer.files.length)event.preventDefault();});
  rebuildEnergyOptions();rebuildOptions();renderAll(true);
})();
