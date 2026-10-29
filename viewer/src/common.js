(function () {
'use strict';
const palette=['#16756f','#3f799d','#b47d35','#758499','#8b718f','#a56464'];
const el=id=>typeof id==='string'?document.getElementById(id):id;
const text=(target,value)=>{const node=el(target);if(node)node.textContent=value==null?'':String(value);return node;};
const numberPattern=/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?$/i;
function num(value){if(value==null || typeof value==='boolean')return null;const s=String(value).trim();if(!s || !numberPattern.test(s))return null;const n=Number(s);return Number.isFinite(n)?n:null;}
const formats=new Map();
function fmt(value,places=2){const n=num(value);if(n==null)return '—';const p=Math.max(0,Math.min(8,places|0)),magnitude=Math.abs(n);if(magnitude>=1e12||(magnitude>0&&magnitude<1e-6))return n.toExponential(Math.max(1,Math.min(4,p)));if(!formats.has(p))formats.set(p,new Intl.NumberFormat('zh-CN',{maximumFractionDigits:p}));return formats.get(p).format(n);}
function duration(ms){const n=num(ms);if(n==null || n<0)return '—';if(n<1000)return fmt(n,0)+' 毫秒';let s=Math.round(n/1000);const d=Math.floor(s/86400);s%=86400;const h=Math.floor(s/3600);s%=3600;const m=Math.floor(s/60);s%=60;return (d?d+' 天 ':'')+(h?h+' 小时 ':'')+(m?m+' 分 ':'')+(!d && !h && s?s+' 秒':'') || '0 秒';}
function bytes(n){if(!Number.isFinite(n))return '—';return n<1024?n+' B':n<1048576?fmt(n/1024,1)+' KiB':fmt(n/1048576,1)+' MiB';}
const pad=n=>String(n).padStart(2,'0');
function dateParts(ms,utc){const d=new Date(ms);if(!Number.isFinite(d.getTime()))return null;const get=name=>d[(utc?'getUTC':'get')+name]();return [get('FullYear'),pad(get('Month')+1),pad(get('Date')),pad(get('Hours')),pad(get('Minutes')),pad(get('Seconds'))];}
function time(ms,utc=false,seconds=true){if(ms==null)return '—';const p=dateParts(ms,utc);return p?p[0]+'-'+p[1]+'-'+p[2]+' '+p[3]+':'+p[4]+(seconds?':'+p[5]:''):'—';}
function tickTime(ms,utc=false,withDate=false){const p=dateParts(ms,utc);return p?(withDate?p[1]+'-'+p[2]+' ':'')+p[3]+':'+p[4]:'—';}
function inputTime(ms,utc=false){const p=dateParts(ms,utc);return p?p[0]+'-'+p[1]+'-'+p[2]+'T'+p[3]+':'+p[4]:'';}
function parseInput(value,utc=false){if(!value)return null;const m=/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(value);if(!m)return null;const a=m.slice(1).map(v=>Number(v||0));const d=utc?new Date(Date.UTC(a[0],a[1]-1,a[2],a[3],a[4],a[5])):new Date(a[0],a[1]-1,a[2],a[3],a[4],a[5]);const p=dateParts(d.getTime(),utc);if(!p || p[0]!==a[0] || Number(p[1])!==a[1] || Number(p[2])!==a[2] || Number(p[3])!==a[3] || Number(p[4])!==a[4])return null;return d.getTime();}
const nextFrame=()=>new Promise(resolve=>setTimeout(resolve,0));
function debounce(fn,ms=180){let timer;return function(){const args=arguments;clearTimeout(timer);timer=setTimeout(()=>fn.apply(this,args),ms);};}
async function readText(file){
 const buffer=await file.arrayBuffer();const b=new Uint8Array(buffer);let encoding='UTF-8',content;
 if(b[0]===0xff && b[1]===0xfe){encoding='UTF-16LE';content=new TextDecoder('utf-16le',{fatal:true}).decode(buffer);}
 else if(b[0]===0xfe && b[1]===0xff){encoding='UTF-16BE';content=new TextDecoder('utf-16be',{fatal:true}).decode(buffer);}
 else {try{content=new TextDecoder('utf-8',{fatal:true}).decode(buffer);}catch(error){encoding='GB18030（兼容读取）';content=new TextDecoder('gb18030',{fatal:true}).decode(buffer);}}
 content=content.replace(/^\uFEFF/,'');return {text:content,encoding,unterminated:content.length>0&&!/[\r\n]$/.test(content)};
}
async function parseCSV(content,options={}){
 const maxRows=options.maxRows==null?100000:options.maxRows;
 const maxColumns=options.maxColumns==null?256:options.maxColumns;
 const source=String(content).replace(/^\uFEFF/,'').replace(/\r\n/g,'\n').replace(/\r/g,'\n');
 const rows=[],issues=[];let headers=null,row=[],field='',quoted=false,after=false,started=false,bad='',line=1,rowLine=1,blankRows=0,issueCount=0;
 const issue=message=>{issueCount++;if(issues.length<24)issues.push(message);};
 const endField=()=>{row.push(field);field='';after=false;started=false;if(row.length>maxColumns)throw new Error('列数超过 '+maxColumns+'，请确认文件格式');};
 function endRow(){
  if(row.length===1 && row[0].trim()==='' && !bad)blankRows++;
  else if(!headers){
   if(bad)throw new Error('CSV 表头无效：'+bad);
   headers=row.map(v=>v.trim());
   if(headers.length<2 || headers.some(h=>!h))throw new Error('CSV 表头缺失或包含空列名');
   if(new Set(headers).size!==headers.length)throw new Error('CSV 表头包含重复列名');
   if(headers.some(h=>['__proto__','constructor','prototype'].includes(h)))throw new Error('CSV 包含不支持的保留列名');
  }else if(bad)issue('第 '+rowLine+' 行：'+bad+'，已跳过');
  else if(row.length!==headers.length)issue('第 '+rowLine+' 行有 '+row.length+' 列，预期 '+headers.length+' 列，已跳过');
  else{if(rows.length>=maxRows)throw new Error('记录数超过 '+fmt(maxRows,0)+'，请按日期分批导入');rows.push({values:row,line:rowLine});}
  row=[];field='';after=false;started=false;bad='';rowLine=line+1;
 }
 for(let i=0;i<source.length;i++){
  const ch=source[i];
  if(quoted){
   if(ch==='"'){if(source[i+1]==='"'){field+='"';i++;}else{quoted=false;after=true;}}
   else{field+=ch;if(ch==='\n')line++;}
  }else if(ch===','){endField();}
  else if(ch==='\n'){endField();endRow();line++;}
  else if(after){if(ch!==' '&&ch!=='\t'){bad='引号结束后存在非分隔字符';field+=ch;after=false;}}
  else if(ch==='"'){if(!started && field===''){quoted=true;started=true;}else{bad='未转义的引号';field+=ch;}}
  else{field+=ch;started=true;}
  if((i&262143)===262143){if(options.onProgress)options.onProgress(i/source.length);await nextFrame();}
 }
 if(quoted)issue('第 '+rowLine+' 行的引号未结束，末条记录未导入');
 else if(row.length || field!=='' || started || after){endField();endRow();}
 if(!headers)throw new Error('文件为空，未找到 CSV 表头');
 if(issueCount>issues.length)issues.push('另有 '+(issueCount-issues.length)+' 条格式问题未逐项列出');
 return {headers,rows,issues,issueCount,blankRows,unterminated:source.length>0&&!source.endsWith('\n')};
}
function fillSelect(target,items,selected){const node=el(target);if(!node)return;const previous=selected==null?node.value:String(selected);const fragment=document.createDocumentFragment();for(const item of items){const option=document.createElement('option');option.value=String(item.value);option.textContent=item.label;fragment.append(option);}node.replaceChildren(fragment);if(items.some(item=>String(item.value)===previous))node.value=previous;}
function notice(message,tone='info',target='notice'){const node=el(target);if(!node)return;node.className='notice '+(tone==='warn'||tone==='error'?tone:'info');node.textContent=message||'';node.hidden=!message;}
function download(content,filename,mime='text/plain;charset=utf-8'){const blob=content instanceof Blob?content:new Blob([content],{type:mime});const url=URL.createObjectURL(blob);const link=document.createElement('a');link.href=url;link.download=String(filename||'records.txt').replace(/[\\/:*?"<>|]/g,'_');link.hidden=true;document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),1500);}
function csvCell(value){let s=value==null?'':String(value);if(!numberPattern.test(s.trim()) && (/^[\s\u0000-\u001f]*[=+\-@]/.test(s)||/^[\t\r\n]/.test(s)))s="'"+s;return /[",\r\n]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s;}
function exportCSV(headers,records,filename){const lines=[headers.map(csvCell).join(',')];for(const record of records)lines.push(headers.map(h=>csvCell(record[h])).join(','));download('\uFEFF'+lines.join('\r\n')+'\r\n',filename,'text/csv;charset=utf-8');}
function finite(v){return typeof v==='number'&&Number.isFinite(v);}
function segments(points){
 const result=[];let part=[];
 for(const p of points||[]){if(!finite(p.x)||!finite(p.y)){if(part.length)result.push(part);part=[];continue;}if(p.breakBefore&&part.length){result.push(part);part=[];}part.push(p);}
 if(part.length)result.push(part);return result;
}
function reduceSegment(points,limit){
 if(points.length<=limit)return points;
 if(limit<2)return [points[0]];
 const output=[points[0]],count=Math.max(0,Math.floor((limit-2)/4)),span=count?(points.length-2)/count:0;
 for(let i=0;i<count;i++){
  const start=1+Math.floor(i*span),end=Math.min(points.length-1,1+Math.floor((i+1)*span));
  let lowY=null,highY=null,lowBand=null,highBand=null;
  for(let j=start;j<end;j++){
   const p=points[j];
   if(!lowY||p.y<lowY.y)lowY=p;if(!highY||p.y>highY.y)highY=p;
   if(!lowBand||(finite(p.low)?p.low:p.y)<(finite(lowBand.low)?lowBand.low:lowBand.y))lowBand=p;
   if(!highBand||(finite(p.high)?p.high:p.y)>(finite(highBand.high)?highBand.high:highBand.y))highBand=p;
  }
  const chosen=Array.from(new Set([lowY,highY,lowBand,highBand].filter(Boolean))).sort((a,b)=>a.x-b.x);
  output.push(...chosen);
 }
 output.push(points[points.length-1]);return output;
}
function preparedSeries(series,budget){
 const output=[];
 for(let i=0;i<series.length;i++){
  const item=series[i],allParts=segments(item.points);
  let parts=allParts;
  const maxParts=Math.max(1,Math.floor(budget/3));
  if(parts.length>maxParts)parts=Array.from({length:maxParts},(_,index)=>allParts[Math.floor(index*(allParts.length-1)/(maxParts-1))]);
  const total=parts.reduce((n,p)=>n+p.length,0);let remaining=budget;
  const sampled=[];
  for(let index=0;index<parts.length;index++){
   const part=parts[index],reserved=2*(parts.length-index-1);
   const allowance=Math.max(2,Math.min(remaining-reserved,Math.round(budget*part.length/Math.max(1,total))));
   const selected=reduceSegment(part,allowance);sampled.push(selected);remaining-=selected.length;
  }
  output.push(Object.assign({},item,{color:item.color||palette[i%palette.length],parts:sampled,valid:(item.points||[]).filter(p=>finite(p.x)&&finite(p.y)),sampled:allParts.length!==parts.length||total>budget}));
 }
 return output;
}
function label(ctx,value,x,y,align='left',color='#71808b'){ctx.fillStyle=color;ctx.textAlign=align;ctx.fillText(value,x,y);}
function shortLabel(ctx,value,width){const s=String(value);if(ctx.measureText(s).width<=width)return s;let lo=0,hi=Math.min(s.length,256);while(lo<hi){const mid=Math.ceil((lo+hi)/2);if(ctx.measureText(s.slice(0,mid)+'…').width<=width)lo=mid;else hi=mid-1;}return s.slice(0,lo)+'…';}
class Chart {
 constructor(target){
  this.host=el(target);if(!this.host)throw new Error('未找到图表容器 '+target);
  this.legend=document.createElement('div');this.legend.className='chart-legend';
  this.wrap=document.createElement('div');this.wrap.className='plot-wrap';
  this.canvas=document.createElement('canvas');this.canvas.setAttribute('role','img');this.canvas.tabIndex=0;
  this.tooltip=document.createElement('div');this.tooltip.className='plot-tooltip';this.tooltip.hidden=true;this.tooltip.setAttribute('role','tooltip');
  this.wrap.append(this.canvas,this.tooltip);this.host.replaceChildren(this.legend,this.wrap);
  this.base=document.createElement('canvas');this.spec={type:'line',series:[]};this.series=[];this.frame=0;this.hits=[];
  this.onMove=event=>this.hover(event);this.onLeave=()=>{this.tooltip.hidden=true;this.restore();};
  this.canvas.addEventListener('pointermove',this.onMove);this.canvas.addEventListener('pointerleave',this.onLeave);this.canvas.addEventListener('blur',this.onLeave);
  this.onResize=()=>{cancelAnimationFrame(this.frame);this.frame=requestAnimationFrame(()=>this.render());};
  if(typeof ResizeObserver!=='undefined'){this.observer=new ResizeObserver(this.onResize);this.observer.observe(this.wrap);}else window.addEventListener('resize',this.onResize);
 }
 set(spec){
  this.spec=Object.assign({type:'line',series:[],bars:[],intervals:[],utc:false,emptyText:'导入文件后显示图表'},spec);
  this.host.classList.toggle('is-timeline',this.spec.type==='timeline');
  this.series=preparedSeries(this.spec.series||[],2600);this.hits=[];
  this.canvas.setAttribute('aria-label',(this.spec.title||'数据图表')+'；精确数据可在记录明细查看');
  this.legend.replaceChildren();
  let items=this.spec.type==='line'?this.series.map(v=>({name:v.name,color:v.color})):this.spec.type==='timeline'?Array.from(new Map((this.spec.intervals||[]).map(v=>[v.label,{name:v.label,color:v.color||'#aab6bf'}])).values()):[];
  for(const item of items.slice(0,12)){const badge=document.createElement('span');const dot=document.createElement('i');dot.style.backgroundColor=item.color;badge.append(dot,document.createTextNode(item.name));this.legend.append(badge);}
  if(this.series.some(v=>v.sampled)){const note=document.createElement('span');note.className='muted';note.textContent='绘图已抽稀，统计保留全部记录';this.legend.append(note);}
  this.tooltip.hidden=true;this.onResize();
 }
 restore(){const ctx=this.canvas.getContext('2d');if(ctx&&this.base.width) {ctx.setTransform(1,0,0,1,0,0);ctx.clearRect(0,0,this.canvas.width,this.canvas.height);ctx.drawImage(this.base,0,0);}}
 render(){
  const w=Math.max(260,Math.floor(this.wrap.clientWidth||600)),h=Math.max(90,Math.floor(this.wrap.clientHeight||230)),dpr=Math.min(window.devicePixelRatio||1,2);
  this.width=w;this.height=h;this.dpr=dpr;
  this.canvas.width=this.base.width=Math.round(w*dpr);this.canvas.height=this.base.height=Math.round(h*dpr);
  this.canvas.style.width=w+'px';this.canvas.style.height=h+'px';
  const ctx=this.base.getContext('2d');if(!ctx)return;
  ctx.setTransform(dpr,0,0,dpr,0,0);ctx.fillStyle='#ffffff';ctx.fillRect(0,0,w,h);ctx.font='11px "Segoe UI","Microsoft YaHei",sans-serif';
  this.hits=[];
  const spec=this.spec,kind=spec.type;
  const has=kind==='line'?this.series.some(s=>s.valid.length):kind==='timeline'?(spec.intervals||[]).some(s=>finite(s.start)&&finite(s.end)&&s.end>s.start):(spec.bars||[]).some(s=>!s.missing&&(finite(s.value)||(s.parts||[]).some(p=>finite(p.value))));
  if(!has){label(ctx,spec.emptyText||'没有可显示的数据',w/2,h/2,'center','#94a0aa');this.plot=null;this.restore();return;}
  if(kind==='bars')this.drawBars(ctx,w,h);
  else if(kind==='histogram')this.drawHistogram(ctx,w,h);
  else if(kind==='timeline')this.drawTimeline(ctx,w,h);
  else this.drawLine(ctx,w,h);
  this.restore();
 }
 domain(){
  const spec=this.spec;let min=Infinity,max=-Infinity;
  if(spec.type==='timeline')for(const s of spec.intervals||[]){if(finite(s.start))min=Math.min(min,s.start);if(finite(s.end))max=Math.max(max,s.end);}
  else for(const s of this.series)for(const p of s.valid){min=Math.min(min,p.x);max=Math.max(max,p.x);}
  if(finite(spec.xmin))min=spec.xmin;if(finite(spec.xmax))max=spec.xmax;
  if(!finite(min)||!finite(max))return [0,1];if(max<=min)max=min+60000;return [min,max];
 }
 axis(ctx,plot,ymin,ymax,unit){
  const {x,y,w,h}=plot;ctx.strokeStyle='#e9eef1';ctx.lineWidth=1;
  const step=Math.abs(ymax-ymin)/4,places=step>0&&step<1?Math.max(2,Math.min(8,Math.ceil(-Math.log10(step))+1)):2;
  for(let i=0;i<=4;i++){
   const py=y+h-h*i/4,value=ymin+(ymax-ymin)*(i/4);
   ctx.beginPath();ctx.moveTo(x,py+.5);ctx.lineTo(x+w,py+.5);ctx.stroke();
   let caption=fmt(value,places);if(value!==0&&ctx.measureText(caption).width>x-14)caption=value.toExponential(1);
   label(ctx,caption,x-9,py+4,'right');
  }
  if(unit)label(ctx,unit,x,y-10,'left','#70808a');
 }
 timeAxis(ctx,plot,min,max){
  const ticks=this.width<480?3:5,span=max-min;
  for(let i=0;i<ticks;i++){const v=min+span*(i/(ticks-1));label(ctx,tickTime(v,this.spec.utc,span>=86400000),plot.x+plot.w*i/(ticks-1),plot.y+plot.h+23,i===0?'left':i===ticks-1?'right':'center');}
 }
 rangeError(ctx,w,h){this.plot=null;this.hits=[];label(ctx,'数值范围超出绘图能力，请查看记录明细',w/2,h/2,'center','#94a0aa');}
 drawLine(ctx,w,h){
  const [min,max]=this.domain();let ymin=Infinity,ymax=-Infinity;
  for(const s of this.series)for(const p of s.valid){ymin=Math.min(ymin,p.y);ymax=Math.max(ymax,p.y);if(s.band){if(finite(p.low))ymin=Math.min(ymin,p.low);if(finite(p.high))ymax=Math.max(ymax,p.high);}}
  if(this.spec.zero){ymin=Math.min(0,ymin);ymax=Math.max(0,ymax);}
  if(ymax===ymin){const pad=Math.max(1,Math.abs(ymin)*.03);ymin-=pad;ymax+=pad;}
  else{const pad=(ymax-ymin)*.09;ymin-=pad;ymax+=pad;}
  if(finite(this.spec.ymin))ymin=this.spec.ymin;if(finite(this.spec.ymax))ymax=this.spec.ymax;
  if(ymax<=ymin)ymax=ymin+1;
  if(![min,max,ymin,ymax,max-min,ymax-ymin].every(finite)||max<=min||ymax<=ymin){this.rangeError(ctx,w,h);return;}
  const plot={x:62,y:24,w:Math.max(80,w-78),h:Math.max(30,h-58),min,max,ymin,ymax};this.plot=plot;
  const X=v=>plot.x+(v-min)/(max-min)*plot.w,Y=v=>plot.y+plot.h-(v-ymin)/(ymax-ymin)*plot.h;
  this.axis(ctx,plot,ymin,ymax,this.spec.unit||'');this.timeAxis(ctx,plot,min,max);
  ctx.save();ctx.beginPath();ctx.rect(plot.x,plot.y,plot.w,plot.h);ctx.clip();
  for(const s of this.series){
   if(s.band)for(const part of s.parts){let run=[];const draw=()=>{if(!run.length)return;ctx.beginPath();run.forEach((p,i)=>i?ctx.lineTo(X(p.x),Y(p.high)):ctx.moveTo(X(p.x),Y(p.high)));for(let i=run.length-1;i>=0;i--)ctx.lineTo(X(run[i].x),Y(run[i].low));ctx.closePath();ctx.globalAlpha=.12;ctx.fillStyle=s.color;ctx.fill();ctx.globalAlpha=1;run=[];};for(const p of part){if(finite(p.low)&&finite(p.high))run.push(p);else draw();}draw();}
   ctx.strokeStyle=s.color;ctx.lineWidth=1.8;ctx.lineJoin='round';
   for(const part of s.parts){if(!part.length)continue;ctx.beginPath();for(let i=0;i<part.length;i++){const p=part[i];if(!i)ctx.moveTo(X(p.x),Y(p.y));else{if(s.step)ctx.lineTo(X(p.x),Y(part[i-1].y));ctx.lineTo(X(p.x),Y(p.y));}}ctx.stroke();if(part.length===1){ctx.fillStyle=s.color;ctx.beginPath();ctx.arc(X(part[0].x),Y(part[0].y),2.3,0,Math.PI*2);ctx.fill();}}
  }
  ctx.restore();
 }
 barTotal(bar){if(bar.missing)return 0;return Array.isArray(bar.parts)&&bar.parts.length?bar.parts.reduce((sum,p)=>sum+(finite(p.value)?Math.max(0,p.value):0),0):finite(bar.value)?Math.max(0,bar.value):0;}
 drawHistogram(ctx,w,h){
  const bars=(this.spec.bars||[]).slice(0,200),largest=Math.max(0,...bars.map(b=>this.barTotal(b))),max=(largest>0?largest:1)*1.12;
  if(!finite(max)){this.rangeError(ctx,w,h);return;}
  const plot={x:56,y:24,w:w-72,h:h-64};this.plot=plot;this.axis(ctx,plot,0,max,this.spec.unit||'');
  const step=plot.w/Math.max(1,bars.length),bw=Math.max(1,step-5),every=Math.max(1,Math.ceil(bars.length/7));
  bars.forEach((bar,i)=>{
   const x=plot.x+i*step+2;
   if(i%every===0||i===bars.length-1)label(ctx,shortLabel(ctx,bar.label,Math.max(44,step*every-8)),x+bw/2,plot.y+plot.h+22,'center');
   if(bar.missing||!(finite(bar.value)||(bar.parts||[]).some(part=>finite(part.value))))return;
   let total=0;
   const parts=bar.parts&&bar.parts.length?bar.parts:[{value:this.barTotal(bar),color:bar.color||palette[0]}];
   for(const part of parts){const value=finite(part.value)?Math.max(0,part.value):0;const bh=value/max*plot.h;ctx.fillStyle=part.color||palette[0];ctx.fillRect(x,plot.y+plot.h-(total+value)/max*plot.h,bw,bh);total+=value;}
   if(total===0){ctx.fillStyle=bar.color||(parts[0]&&parts[0].color)||palette[0];ctx.beginPath();ctx.arc(x+bw/2,plot.y+plot.h-2,2,0,Math.PI*2);ctx.fill();}
   this.hits.push({x,y:plot.y,w:bw,h:plot.h,bar});
  });
 }
 drawBars(ctx,w,h){
  const bars=(this.spec.bars||[]).slice(0,14),max=Math.max(1,...bars.map(b=>this.barTotal(b)))*1.15;
  if(!finite(max)){this.rangeError(ctx,w,h);return;}
  const left=Math.min(155,Math.max(90,w*.30)),plot={x:left,y:12,w:Math.max(60,w-left-36),h:h-28};this.plot=plot;
  const step=plot.h/Math.max(1,bars.length),bh=Math.min(21,step*.65);
  bars.forEach((bar,i)=>{const cy=plot.y+(i+.5)*step;label(ctx,shortLabel(ctx,bar.label,left-18),left-12,cy+4,'right','#4f616d');const parts=bar.parts&&bar.parts.length?bar.parts:[{value:this.barTotal(bar),color:bar.color||palette[i%palette.length]}];let total=0;for(const part of parts){const value=finite(part.value)?Math.max(0,part.value):0;ctx.fillStyle=part.color||palette[0];ctx.fillRect(plot.x+total/max*plot.w,cy-bh/2,value/max*plot.w,bh);total+=value;}label(ctx,fmt(total,2),plot.x+total/max*plot.w+7,cy+4,'left','#425660');this.hits.push({x:0,y:cy-step/2,w,h:step,bar});});
 }
 drawTimeline(ctx,w,h){
  const [min,max]=this.domain();if(!finite(max-min)||max<=min){this.rangeError(ctx,w,h);return;}const plot={x:12,y:16,w:w-24,h:28,min,max};this.plot=plot;
  ctx.fillStyle='#eef1f4';ctx.fillRect(plot.x,plot.y,plot.w,plot.h);
  const intervals=(this.spec.intervals||[]).filter(s=>finite(s.start)&&finite(s.end)&&s.end>s.start);
  for(const item of intervals){const x=plot.x+(Math.max(min,item.start)-min)/(max-min)*plot.w,xe=plot.x+(Math.min(max,item.end)-min)/(max-min)*plot.w;if(xe<x||xe<plot.x||x>plot.x+plot.w)continue;const width=Math.max(.8,xe-x);ctx.fillStyle=item.color||'#aab6bf';ctx.fillRect(x,plot.y,width,plot.h);if(width>100)label(ctx,shortLabel(ctx,item.label,width-12),x+width/2,plot.y+18,'center','#ffffff');this.hits.push({x,y:plot.y,w:width,h:plot.h,item});}
  this.timeAxis(ctx,plot,min,max);
 }
 tooltipLines(lines,event){
  this.tooltip.replaceChildren();
  for(let i=0;i<lines.length;i++){const node=document.createElement(i?'div':'strong');node.textContent=lines[i];this.tooltip.append(node);}
  this.tooltip.hidden=false;const rect=this.wrap.getBoundingClientRect(),x=event.clientX-rect.left,y=event.clientY-rect.top;
  const tw=this.tooltip.offsetWidth,th=this.tooltip.offsetHeight;
  this.tooltip.style.left=Math.max(4,Math.min(this.width-tw-4,x+14))+'px';this.tooltip.style.top=Math.max(4,Math.min(this.height-th-4,y+14))+'px';
 }
 hover(event){
  if(!this.plot)return;
  const rect=this.canvas.getBoundingClientRect(),x=(event.clientX-rect.left)*this.width/rect.width,y=(event.clientY-rect.top)*this.height/rect.height;
  this.restore();const spec=this.spec;
  if(spec.type==='line'){
   const plot=this.plot;if(x<plot.x||x>plot.x+plot.w||y<plot.y||y>plot.y+plot.h){this.tooltip.hidden=true;return;}
   const target=plot.min+(x-plot.x)/plot.w*(plot.max-plot.min);let nearest=null,chosen=[];
   for(const series of this.series){const a=series.valid;if(!a.length)continue;let lo=0,hi=a.length;while(lo<hi){const mid=(lo+hi)>>1;if(a[mid].x<target)lo=mid+1;else hi=mid;}const c=lo===0?a[0]:lo===a.length?a[a.length-1]:target-a[lo-1].x<=a[lo].x-target?a[lo-1]:a[lo];chosen.push({series,point:c});if(!nearest||Math.abs(c.x-target)<Math.abs(nearest.x-target))nearest=c;}
   if(!nearest){this.tooltip.hidden=true;return;}const lines=['最近记录 · '+time(nearest.x,spec.utc,true)];
   for(const item of chosen){const p=item.point;if(Math.abs(p.x-nearest.x)>1)continue;lines.push(item.series.name+'  '+fmt(p.y,3)+(item.series.unit||spec.unit?' '+(item.series.unit||spec.unit):''));if(item.series.band&&finite(p.low)&&finite(p.high))lines.push('观测范围 '+fmt(p.low,3)+' ～ '+fmt(p.high,3));}
   if(nearest.meta)lines.push(nearest.meta);
   const ctx=this.canvas.getContext('2d');ctx.setTransform(this.dpr,0,0,this.dpr,0,0);const cx=plot.x+(nearest.x-plot.min)/(plot.max-plot.min)*plot.w;ctx.strokeStyle='#94a4af';ctx.setLineDash([3,3]);ctx.beginPath();ctx.moveTo(cx,plot.y);ctx.lineTo(cx,plot.y+plot.h);ctx.stroke();ctx.setLineDash([]);this.tooltipLines(lines,event);
  }else{
   const hit=this.hits.findLast?this.hits.findLast(h=>x>=h.x&&x<=h.x+h.w&&y>=h.y&&y<=h.y+h.h):this.hits.slice().reverse().find(h=>x>=h.x&&x<=h.x+h.w&&y>=h.y&&y<=h.y+h.h);
   if(!hit){this.tooltip.hidden=true;return;}
   if(hit.item){const item=hit.item;this.tooltipLines([item.label,time(item.start,spec.utc)+' ～ '+time(item.end,spec.utc),item.meta||'记录状态摘要，不代表片段内的精确连通情况'],event);}
   else{const bar=hit.bar,lines=[bar.label,fmt(this.barTotal(bar),3)+(spec.unit?' '+spec.unit:'')];if(bar.parts)for(const part of bar.parts)lines.push(part.name+'  '+fmt(part.value,2));if(bar.note)lines.push(bar.note);this.tooltipLines(lines,event);}
  }
 }
 destroy(){cancelAnimationFrame(this.frame);if(this.observer)this.observer.disconnect();else window.removeEventListener('resize',this.onResize);this.canvas.removeEventListener('pointermove',this.onMove);this.canvas.removeEventListener('pointerleave',this.onLeave);this.canvas.removeEventListener('blur',this.onLeave);this.host.replaceChildren();}
}
window.DM=Object.freeze({el,text,num,fmt,integer:v=>fmt(v,0),duration,bytes,time,tickTime,inputTime,parseInput,readText,parseCSV,nextFrame,debounce,fillSelect,notice,download,exportCSV,Chart,palette});
})();
