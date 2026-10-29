#include "agent/controller.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <dbt.h>
#include <algorithm>
#include <cmath>

namespace dm {
enum : int { PortBox=100,Rescan,TogglePort,Options,IntervalBox,ApplyInterval,OpenData,OpenLogs,IntervalLabel,
    AutoStart=200,FlushMinutes,StaleSeconds,RetentionDays,DataBits,Parity,StopBits,Dtr,Rts,SaveSettings,ConnectOnStartup };
constexpr UINT kTray=WM_APP+1;
struct App;
static App* app=nullptr;
static LRESULT CALLBACK window_proc(HWND,UINT,WPARAM,LPARAM);
static LRESULT CALLBACK settings_proc(HWND,UINT,WPARAM,LPARAM);
static int px(HWND h,int n) {return MulDiv(n,static_cast<int>(GetDpiForWindow(h)),96);}
static SIZE window_size(UINT dpi,int client_width,int client_height) {
    RECT r{0,0,MulDiv(client_width,static_cast<int>(dpi),96),MulDiv(client_height,static_cast<int>(dpi),96)};
    AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
    return {r.right-r.left,r.bottom-r.top};
}
static bool storage_has_error(const Json& snapshot) {
    auto storage=snapshot.value("storage",Json::object());
    for(auto name:{"csv_error","metadata_error","log_error","retention_error","energy_error"})
        if(!storage.value(name,std::string{}).empty())return true;
    return false;
}
static HWND child(HWND parent,LPCWSTR type,LPCWSTR text,DWORD style,int id) {
    return CreateWindowExW(std::wstring_view(type)==L"EDIT"?WS_EX_CLIENTEDGE:0,type,text,
        WS_CHILD|WS_VISIBLE|style,0,0,0,0,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
}
static void place(HWND parent,HWND h,int x,int y,int w,int height) {MoveWindow(h,px(parent,x),px(parent,y),px(parent,w),px(parent,height),TRUE);}
static void font_for(HWND h) {
    auto old=reinterpret_cast<HFONT>(GetPropW(h,L"DMFont"));
    auto font=CreateFontW(-MulDiv(10,static_cast<int>(GetDpiForWindow(h)),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    SetPropW(h,L"DMFont",font);
    EnumChildWindows(h,[](HWND c,LPARAM f)->BOOL{SendMessageW(c,WM_SETFONT,static_cast<WPARAM>(f),TRUE);return TRUE;},reinterpret_cast<LPARAM>(font));
    if(old)DeleteObject(old);
}
static void release_font(HWND h) {if(auto f=GetPropW(h,L"DMFont")) {RemovePropW(h,L"DMFont");DeleteObject(f);}}
static std::wstring text(HWND h) {int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
static void set_text_if_changed(HWND h,const std::wstring& value,std::wstring& cached) {
    if(value==cached)return;
    SetWindowTextW(h,value.c_str());cached=value;
}

static void set_list_cell_if_changed(HWND h,int row,int column,const std::wstring& value,std::wstring& cached) {
    if(value==cached)return;
    ListView_SetItemText(h,row,column,const_cast<wchar_t*>(value.c_str()));cached=value;
}

static void items(HWND h,std::initializer_list<const wchar_t*> values,int selected) {
    for(auto value:values)SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));
    SendMessageW(h,CB_SETCURSEL,selected,0);
}
static std::wstring state_text(std::string_view state) {
    if(state=="connected")return L"已连接";
    if(state=="waiting")return L"等待状态数据";
    if(state=="connecting")return L"正在连接";
    if(state=="disabled")return L"串口已关闭";
    if(state=="suspended")return L"休眠暂停";
    if(state=="stale")return L"数据已过期";
    if(state=="disconnected")return L"设备断开、被占用或不可用";
    if(state=="fatal_error")return L"代理发生错误，请查看日志后退出重启";
    return wide(state);
}
struct App {
    HINSTANCE instance{};
    HWND window{},settings{},status{},port{},interval{},list{};
    std::unique_ptr<Controller> controller;
    fs::path root;
    Config config;
    std::string config_error;
    std::vector<Port> ports;
    std::wstring status_text;
    std::wstring energy_value_cache,energy_state_cache;
    std::wstring tray_tip;
    UINT taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    HICON small_icon{},large_icon{};
    HDEVNOTIFY device_notification{};
    void icons() {
        tray_tip.clear();
        if(small_icon)DestroyIcon(small_icon);if(large_icon)DestroyIcon(large_icon);
        small_icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetricsForDpi(SM_CXSMICON,GetDpiForWindow(window)),GetSystemMetricsForDpi(SM_CYSMICON,GetDpiForWindow(window)),LR_DEFAULTCOLOR));
        large_icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetricsForDpi(SM_CXICON,GetDpiForWindow(window)),GetSystemMetricsForDpi(SM_CYICON,GetDpiForWindow(window)),LR_DEFAULTCOLOR));
        SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(small_icon));
        SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(large_icon));
    }
    void tray(DWORD operation) {
        if(operation==NIM_ADD)tray_tip.clear();
        NOTIFYICONDATAW data{sizeof(data)};data.hWnd=window;data.uID=1;data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
        data.uCallbackMessage=kTray;data.hIcon=small_icon;
        wcscpy_s(data.szTip,L"DM Agent · 本地遥测");
        if(operation==NIM_MODIFY && controller) {
            auto j=controller->snapshot();auto s=L"DM Agent · "+state_text(j.value("connection_state",std::string("disabled")));
            if(storage_has_error(j))s+=L" · 记录异常";if(!config_error.empty())s+=L" · 配置异常";
            if(tray_tip==s)return;
            tray_tip=s;wcsncpy_s(data.szTip,s.c_str(),_TRUNCATE);
        }
        Shell_NotifyIconW(operation,&data);
    }
    void show() {ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);}
    void scan_ports() {
        try {
            ports=enumerate_ports();SendMessageW(port,CB_RESETCONTENT,0,0);int selected=-1;
            for(std::size_t i=0;i<ports.size();++i) {
                auto label=wide(ports[i].name+"  "+ports[i].description);
                SendMessageW(port,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                if(!config.device_instance.empty()?ports[i].instance==config.device_instance:ports[i].name==config.port) selected=static_cast<int>(i);
            }
            if(selected<0 && !config.port.empty()) {
                ports.push_back({config.port,config.device_instance,"已保存设备，当前未枚举到"});
                auto label=wide(config.port+"  已保存设备，当前未枚举到");
                selected=static_cast<int>(ports.size()-1);SendMessageW(port,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(port,CB_SETCURSEL,selected,0);
        } catch(const std::exception& e) {config_error=e.what();}
    }
    bool apply(Config next,bool closing=false) {
        // 关闭动作先通知会话取消，配置写入失败也不重新开启串口。
        if(closing) {config=next;controller->configure(config);}
        std::string error;
        if(!save_config(root,next,error)) {
            config_error=error;
            auto notice=closing?"串口已关闭，但设置未保存；下次启动按已保存的自动连接设置处理。\n"+error:error;
            MessageBoxW(window,wide(notice).c_str(),L"设置未保存",MB_OK|MB_ICONERROR);
            return false;
        }
        config=next;config_error.clear();controller->configure(config);return true;
    }
    void select_port(bool connect) {
        int index=static_cast<int>(SendMessageW(port,CB_GETCURSEL,0,0));
        if(index<0 || index>=static_cast<int>(ports.size())) {MessageBoxW(window,L"请先选择电源对应的 COM 端口。",L"选择串口",MB_OK);return;}
        auto next=config;next.port=ports[index].name;next.device_instance=ports[index].instance;next.serial_enabled=connect;
        apply(next);
    }
    void open_serial() {select_port(true);}
    void close_serial() {auto next=config;next.serial_enabled=false;apply(next,true);}
    void apply_interval() {
        auto next=config;int index=static_cast<int>(SendMessageW(interval,CB_GETCURSEL,0,0));
        if(index<0)return;next.last_requested_interval_ms=1000+index*500;
        if(!apply(next))return;
        std::string error;
        if(!controller->request_interval(next.last_requested_interval_ms,error))MessageBoxW(window,wide(error).c_str(),L"设置未发送",MB_OK|MB_ICONINFORMATION);
    }
    void directory(const wchar_t* folder) {
        auto target=root/folder;
        std::error_code ec;fs::create_directories(target,ec);
        if(ec) {MessageBoxW(window,wide(ec.message()).c_str(),L"目录不可用",MB_OK|MB_ICONERROR);return;}
        ShellExecuteW(window,L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
    void menu() {
        HMENU m=CreatePopupMenu();
        AppendMenuW(m,MF_STRING,1,L"查看状态");
        AppendMenuW(m,MF_STRING|(config.serial_enabled?MF_GRAYED:0),2,L"开启串口");
        AppendMenuW(m,MF_STRING|(!config.serial_enabled?MF_GRAYED:0),3,L"关闭串口");
        AppendMenuW(m,MF_STRING,4,L"设置");
        AppendMenuW(m,MF_STRING,5,L"打开数据目录");
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);AppendMenuW(m,MF_STRING,6,L"退出");
        POINT p{};GetCursorPos(&p);SetForegroundWindow(window);
        int selected=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);
        DestroyMenu(m);
        if(selected==1)show();if(selected==2)open_serial();if(selected==3)close_serial();
        if(selected==4)show_settings();if(selected==5)directory(L"data");if(selected==6)DestroyWindow(window);
        PostMessageW(window,WM_NULL,0,0);
    }
    void initialize() {
        status=child(window,L"STATIC",L"DM Agent",0,0);
        port=child(window,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,PortBox);
        child(window,L"BUTTON",L"刷新端口",WS_TABSTOP,Rescan);
        child(window,L"BUTTON",config.serial_enabled?L"关闭串口":L"连接串口",WS_TABSTOP,TogglePort);
        child(window,L"BUTTON",L"设置",WS_TABSTOP,Options);
        child(window,L"STATIC",L"回传间隔",0,IntervalLabel);
        interval=child(window,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,IntervalBox);
        items(interval,{L"1000 ms",L"1500 ms",L"2000 ms",L"2500 ms",L"3000 ms"},(config.last_requested_interval_ms-1000)/500);
        child(window,L"BUTTON",L"应用间隔",WS_TABSTOP,ApplyInterval);
        list=child(window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|WS_BORDER,0);
        ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t* headings[]{L"监测项目",L"数值",L"单位",L"状态"};
        for(int i=0;i<4;++i) {LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<wchar_t*>(headings[i]);column.cx=px(window,i==0?230:i==3?180:130);ListView_InsertColumn(list,i,&column);}
        for(int i=0;i<static_cast<int>(kMetrics.size());++i) {LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=i;item.pszText=const_cast<wchar_t*>(kMetrics[i].label);ListView_InsertItem(list,&item);}
        LVITEMW energy_row{};energy_row.mask=LVIF_TEXT;energy_row.iItem=1;
        energy_row.pszText=const_cast<wchar_t*>(L"今日估算用电");ListView_InsertItem(list,&energy_row);
        ListView_SetItemText(list,1,2,const_cast<wchar_t*>(L"kWh（度）"));
        child(window,L"BUTTON",L"数据目录",WS_TABSTOP,OpenData);child(window,L"BUTTON",L"日志目录",WS_TABSTOP,OpenLogs);
        font_for(window);layout();icons();tray(NIM_ADD);scan_ports();
        DEV_BROADCAST_DEVICEINTERFACE_W filter{sizeof(filter),DBT_DEVTYP_DEVICEINTERFACE};
        filter.dbcc_classguid={0x86e0d1e0,0x8089,0x11d0,{0x9c,0xe4,0x08,0x00,0x3e,0x30,0x1f,0x73}};
        device_notification=RegisterDeviceNotificationW(window,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);
        SetTimer(window,1,1000,nullptr);
    }
    void layout() {
        if(!status)return;
        RECT r{};GetClientRect(window,&r);
        const int w=MulDiv(r.right,96,GetDpiForWindow(window)),h=MulDiv(r.bottom,96,GetDpiForWindow(window));
        place(window,status,12,12,w-126,22);
        place(window,GetDlgItem(window,Options),w-102,8,90,28);
        place(window,port,12,46,w-210,220);
        place(window,GetDlgItem(window,Rescan),w-190,46,78,28);
        place(window,GetDlgItem(window,TogglePort),w-104,46,92,28);
        place(window,GetDlgItem(window,IntervalLabel),12,88,68,22);
        place(window,interval,88,82,120,200);
        place(window,GetDlgItem(window,ApplyInterval),216,82,88,28);
        place(window,GetDlgItem(window,OpenData),w-196,82,88,28);
        place(window,GetDlgItem(window,OpenLogs),w-100,82,88,28);
        place(window,list,12,124,w-24,std::max(180,h-136));
        // 留出滚动条和边框，不让固定列宽在紧凑窗口中溢出。
        const int available=std::max(300,px(window,w-28)-GetSystemMetricsForDpi(SM_CXVSCROLL,GetDpiForWindow(window)));
        const int columns[]{available*43/100,available*23/100,available*14/100,available-available*43/100-available*23/100-available*14/100};
        for(int i=0;i<4;++i)ListView_SetColumnWidth(list,i,columns[i]);
    }
    void update() {
        if(!controller)return;
        auto j=controller->snapshot();auto state=j.value("connection_state",std::string("disabled"));
        auto message=config.port.empty()?std::wstring(L"请选择电源串口"):state_text(state);
        if(storage_has_error(j))message+=L" · 记录异常";
        if(!config_error.empty())message+=L" · 配置异常";
        set_text_if_changed(status,message,status_text);tray(NIM_MODIFY);
        EnableWindow(port,!config.serial_enabled);
        const auto connection_label=config.serial_enabled?L"关闭串口":L"连接串口";
        if(text(GetDlgItem(window,TogglePort))!=connection_label)
            SetDlgItemTextW(window,TogglePort,connection_label);
        EnableWindow(GetDlgItem(window,ApplyInterval),state=="connected");
        if(!IsWindowVisible(window))return;
        Json metrics=j.value("metrics",Json::object());
        auto fresh_metric=[&](const Json& m) {
            auto tick=m.value("received_tick_ms",Json(nullptr));auto now=GetTickCount64();
            auto published=j.value("published_tick_ms",std::uint64_t{0});
            return state=="connected" && j.value("running",false) && published<=now && now-published<=5000 &&
                m.value("freshness",std::string{})=="fresh" && tick.is_number_unsigned() && tick.get<std::uint64_t>()<=now &&
                now-tick.get<std::uint64_t>()<static_cast<unsigned>(j.value("stale_after_ms",10000));
        };
        for(int i=0;i<static_cast<int>(kMetrics.size());++i) {
            auto m=metrics.value(kMetrics[i].id,Json::object());bool fresh=fresh_metric(m);
            std::wstring result=L"--";
            if(fresh && m.contains("value") && m["value"].is_number()) {
                wchar_t buffer[64]{};swprintf_s(buffer,i==1?L"%.0f":L"%.2f",m["value"].get<double>());result=buffer;
            }
            auto unit=std::string_view(kMetrics[i].unit)=="degC"?std::wstring(L"℃"):wide(kMetrics[i].unit);
            auto freshness=fresh?L"正常":m.value("validity",std::string{})=="missing"?L"未收到":state=="stale"?L"已过期":L"已离线";
            const int row=i==0?0:i+1;
            ListView_SetItemText(list,row,1,result.data());ListView_SetItemText(list,row,2,unit.data());ListView_SetItemText(list,row,3,const_cast<wchar_t*>(freshness));
        }
        // Derived daily total is updated only by closed minute/partial records.
        auto energy=j.value("energy",Json::object());
        std::wstring energy_value=L"--",energy_state=L"未收到";
        if(energy.is_object()) {
            SYSTEMTIME local{};GetLocalTime(&local);char date[16]{};
            sprintf_s(date,"%04u-%02u-%02u",static_cast<unsigned>(local.wYear),static_cast<unsigned>(local.wMonth),static_cast<unsigned>(local.wDay));
            const bool current_day=energy.value("date_local",std::string{})==date;
            const auto total=energy.value("today_kwh",Json(nullptr));
            if(current_day && total.is_number()) {
                const auto amount=total.get<double>();
                if(std::isfinite(amount) && amount>=0) {wchar_t buffer[64]{};swprintf_s(buffer,amount<1e12?L"%.3f":L"%.3e",amount);energy_value=buffer;}
            }
            const auto phase=energy.value("status",std::string("missing"));
            const auto now=GetTickCount64(),published=j.value("published_tick_ms",std::uint64_t{0});
            const bool recent=j.value("running",false)&&published<=now&&now-published<=5000;
            if(phase=="recovering")energy_state=L"恢复中";
            else if(phase=="error")energy_state=L"记录异常";
            else if(!current_day)energy_state=L"等待结算";
            else if(energy_value==L"--")energy_state=recent && state=="connected"?L"待结算":L"未收到";
            else if(!energy.value("recovery_error",std::string{}).empty() ||
                energy.value("history_quality",std::string{})=="recovery_incomplete")energy_state=L"记录不全";
            else energy_state=recent && phase=="accumulating"?L"累计中":L"暂停";
        }
        set_list_cell_if_changed(list,1,1,energy_value,energy_value_cache);
        set_list_cell_if_changed(list,1,3,energy_state,energy_state_cache);
    }
    void show_settings() {
        if(settings) {ShowWindow(settings,SW_RESTORE);SetForegroundWindow(settings);return;}
        settings=CreateWindowExW(WS_EX_DLGMODALFRAME,L"DMSettings",L"DM Agent 设置",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,px(window,640),px(window,560),window,nullptr,instance,nullptr);
        settings_icons(settings);ShowWindow(settings,SW_SHOW);
    }
    void settings_icons(HWND h) {
        for(int kind:{ICON_SMALL,ICON_BIG}) {
            auto name=kind==ICON_SMALL?L"DMIconSmall":L"DMIconLarge";
            auto previous=reinterpret_cast<HICON>(GetPropW(h,name));
            UINT dpi=GetDpiForWindow(h);int icon_metric=kind==ICON_SMALL?SM_CXSMICON:SM_CXICON;
            auto icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetricsForDpi(icon_metric,dpi),GetSystemMetricsForDpi(icon_metric,dpi),0));
            SetPropW(h,name,icon);SendMessageW(h,WM_SETICON,kind,reinterpret_cast<LPARAM>(icon));if(previous)DestroyIcon(previous);
        }
    }
    void initialize_settings(HWND h) {
        child(h,L"BUTTON",L"用户登录后自动启动",BS_AUTOCHECKBOX|WS_TABSTOP,AutoStart);
        child(h,L"BUTTON",L"启动时自动连接串口",BS_AUTOCHECKBOX|WS_TABSTOP,ConnectOnStartup);
        SendDlgItemMessageW(h,ConnectOnStartup,BM_SETCHECK,config.connect_on_startup?BST_CHECKED:BST_UNCHECKED,0);
        SendDlgItemMessageW(h,AutoStart,BM_SETCHECK,config.autostart?BST_CHECKED:BST_UNCHECKED,0);
        const wchar_t* labels[]{L"CSV 批次写入间隔（分钟，1～60）",L"数据过期时间（秒，5～20）",L"CSV 保留天数（0 表示永久）",L"数据位 / 校验 / 停止位"};
        for(int i=0;i<4;++i) {auto label=child(h,L"STATIC",labels[i],0,300+i);place(h,label,20,65+i*48,350,26);}
        child(h,L"EDIT",std::to_wstring(config.csv_flush_seconds/60).c_str(),ES_NUMBER|WS_TABSTOP,FlushMinutes);
        child(h,L"EDIT",std::to_wstring(config.stale_after_ms/1000).c_str(),ES_NUMBER|WS_TABSTOP,StaleSeconds);
        child(h,L"EDIT",std::to_wstring(config.data_retention_days).c_str(),ES_NUMBER|WS_TABSTOP,RetentionDays);
        auto bits=child(h,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST,DataBits);items(bits,{L"8",L"7"},config.data_bits==8?0:1);
        auto parity=child(h,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST,Parity);items(parity,{L"N",L"E",L"O"},config.parity=="N"?0:config.parity=="E"?1:2);
        auto stops=child(h,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST,StopBits);items(stops,{L"1",L"2"},config.stop_bits==1?0:1);
        child(h,L"BUTTON",L"DTR（终端就绪）",BS_AUTOCHECKBOX,Dtr);child(h,L"BUTTON",L"RTS（请求发送）",BS_AUTOCHECKBOX,Rts);
        SendDlgItemMessageW(h,Dtr,BM_SETCHECK,config.dtr?BST_CHECKED:BST_UNCHECKED,0);SendDlgItemMessageW(h,Rts,BM_SETCHECK,config.rts?BST_CHECKED:BST_UNCHECKED,0);
        auto notice=child(h,L"STATIC",L"自动连接使用已保存的电源串口；首次选择一次即可。\r\n手动关闭仅停止本次采集；下次启动按自动连接设置处理。\r\nDTR/RTS 是串口控制信号，当前设备建议均关闭。\r\n修改串口参数前请关闭串口；回传设置不会在重连时恢复。\r\nCSV 每分钟汇总，落盘间隔独立；日志保留 30 天／100 MiB。\r\n风扇设置和清除故障记录功能不提供。",0,310);
        place(h,notice,20,300,580,132);
        child(h,L"BUTTON",L"保存设置",WS_TABSTOP,SaveSettings);
        place(h,GetDlgItem(h,AutoStart),20,20,250,28);
        place(h,GetDlgItem(h,ConnectOnStartup),300,20,280,28);
        for(int i=0;i<3;++i)place(h,GetDlgItem(h,FlushMinutes+i),390,62+i*48,170,28);
        place(h,bits,350,206,65,200);place(h,parity,425,206,65,200);place(h,stops,500,206,65,200);
        place(h,GetDlgItem(h,Dtr),20,255,210,28);place(h,GetDlgItem(h,Rts),260,255,220,28);
        place(h,GetDlgItem(h,SaveSettings),430,440,150,32);
        for(auto id:{DataBits,Parity,StopBits,Dtr,Rts})EnableWindow(GetDlgItem(h,id),!config.serial_enabled);
        font_for(h);
    }
    void layout_settings(HWND h) {
        place(h,GetDlgItem(h,AutoStart),20,20,250,28);
        place(h,GetDlgItem(h,ConnectOnStartup),300,20,280,28);
        for(int i=0;i<4;++i)place(h,GetDlgItem(h,300+i),20,65+i*48,350,26);
        for(int i=0;i<3;++i)place(h,GetDlgItem(h,FlushMinutes+i),390,62+i*48,170,28);
        place(h,GetDlgItem(h,DataBits),350,206,65,200);place(h,GetDlgItem(h,Parity),425,206,65,200);place(h,GetDlgItem(h,StopBits),500,206,65,200);
        place(h,GetDlgItem(h,Dtr),20,255,210,28);place(h,GetDlgItem(h,Rts),260,255,220,28);
        place(h,GetDlgItem(h,310),20,300,580,132);place(h,GetDlgItem(h,SaveSettings),430,440,150,32);
    }
    void save_settings(HWND h) {
        auto integer=[&](int id,int low,int high) {
            auto input=text(GetDlgItem(h,id));std::size_t used=0;int v=std::stoi(input,&used);
            if(used!=input.size() || v<low || v>high)throw std::runtime_error("设置值超出范围");
            return v;
        };
        try {
            auto next=config;
            next.csv_flush_seconds=integer(FlushMinutes,1,60)*60;next.stale_after_ms=integer(StaleSeconds,5,20)*1000;
            next.data_retention_days=integer(RetentionDays,0,36500);
            next.autostart=SendDlgItemMessageW(h,AutoStart,BM_GETCHECK,0,0)==BST_CHECKED;
            next.connect_on_startup=SendDlgItemMessageW(h,ConnectOnStartup,BM_GETCHECK,0,0)==BST_CHECKED;
            if(!config.serial_enabled) {
                next.data_bits=SendDlgItemMessageW(h,DataBits,CB_GETCURSEL,0,0)==0?8:7;
                int p=static_cast<int>(SendDlgItemMessageW(h,Parity,CB_GETCURSEL,0,0));next.parity=p==0?"N":p==1?"E":"O";
                next.stop_bits=SendDlgItemMessageW(h,StopBits,CB_GETCURSEL,0,0)==0?1:2;
                next.dtr=SendDlgItemMessageW(h,Dtr,BM_GETCHECK,0,0)==BST_CHECKED;next.rts=SendDlgItemMessageW(h,Rts,BM_GETCHECK,0,0)==BST_CHECKED;
            }
            std::string error;
            const bool previous_autostart=autostart_matches();
            if(!set_autostart(next.autostart,error))throw std::runtime_error(error);
            if(!apply(next)) {set_autostart(previous_autostart,error);return;}
            DestroyWindow(h);
        } catch(const std::exception& e) {MessageBoxW(h,wide(e.what()).c_str(),L"设置未保存",MB_OK|MB_ICONERROR);}
    }
};
static LRESULT CALLBACK window_proc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(!app)return DefWindowProcW(h,message,w,l);
    if(message==app->taskbar_created){app->tray(NIM_ADD);return 0;}
    switch(message) {
    case WM_CREATE:app->window=h;app->initialize();return 0;
    case WM_SIZE:app->layout();return 0;
    case WM_TIMER:try{app->update();}catch(const std::exception& e){app->config_error=e.what();}return 0;
    case WM_CLOSE:ShowWindow(h,SW_HIDE);return 0;
    case WM_COMMAND:
        switch(LOWORD(w)) {
        case Rescan:app->scan_ports();break;
        case TogglePort:if(app->config.serial_enabled)app->close_serial();else app->open_serial();break;
        case PortBox:
            if(HIWORD(w)==CBN_SELCHANGE && !app->config.serial_enabled)app->select_port(app->config.connect_on_startup);
            break;
        case Options:app->show_settings();break;case ApplyInterval:app->apply_interval();break;
        case OpenData:app->directory(L"data");break;case OpenLogs:app->directory(L"logs");break;
        }return 0;
    case kTray:if(l==WM_LBUTTONDBLCLK)app->show();if(l==WM_RBUTTONUP || l==WM_CONTEXTMENU)app->menu();return 0;
    case WM_DEVICECHANGE:app->controller->devices_changed();return TRUE;
    case WM_POWERBROADCAST:
        if(w==PBT_APMSUSPEND)app->controller->suspend(true);
        if(w==PBT_APMRESUMEAUTOMATIC || w==PBT_APMRESUMESUSPEND)app->controller->suspend(false);
        return TRUE;
    case WM_QUERYENDSESSION:return TRUE;
    case WM_ENDSESSION:if(w)DestroyWindow(h);return 0;
    case WM_DPICHANGED: {auto rect=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER);font_for(h);app->icons();app->layout();app->tray(NIM_MODIFY);return 0;}
    case WM_GETMINMAXINFO:{auto info=reinterpret_cast<MINMAXINFO*>(l);auto size=window_size(GetDpiForWindow(h),580,390);info->ptMinTrackSize={size.cx,size.cy};return 0;}
    case WM_DESTROY:
        KillTimer(h,1);app->controller->stop();app->tray(NIM_DELETE);
        if(app->device_notification)UnregisterDeviceNotification(app->device_notification);
        release_font(h);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,message,w,l);
}
static LRESULT CALLBACK settings_proc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CREATE){app->settings=h;app->initialize_settings(h);return 0;}
    if(message==WM_COMMAND && LOWORD(w)==SaveSettings){app->save_settings(h);return 0;}
    if(message==WM_DPICHANGED){auto r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);font_for(h);app->layout_settings(h);app->settings_icons(h);return 0;}
    if(message==WM_CLOSE){DestroyWindow(h);return 0;}
    if(message==WM_DESTROY){release_font(h);for(auto name:{L"DMIconSmall",L"DMIconLarge"})if(auto icon=GetPropW(h,name)){RemovePropW(h,name);DestroyIcon(static_cast<HICON>(icon));}app->settings=nullptr;return 0;}
    return DefWindowProcW(h,message,w,l);
}
} // namespace dm
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int) {
    using namespace dm;
    try {
        App state;app=&state;state.instance=instance;state.root=executable_dir();
        UserSecurity security;auto name=object_name(L"Agent");
        Handle singleton(CreateMutexW(security.get(),FALSE,name.c_str()));
        if(!singleton)throw std::runtime_error(error_text());
        bool autostart=std::wstring_view(command).find(L"--autostart")!=std::wstring_view::npos;
        auto ownership=WaitForSingleObject(singleton.get(),0);
        if(ownership==WAIT_TIMEOUT){if(!autostart)if(auto existing=FindWindowW(kWindowClass,nullptr)){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}return 0;}
        if(ownership!=WAIT_OBJECT_0 && ownership!=WAIT_ABANDONED)throw std::runtime_error(error_text());
        struct MutexOwnership {HANDLE handle;~MutexOwnership(){ReleaseMutex(handle);}} ownership_guard{singleton.get()};
        bool first=false;state.config=load_config(state.root,first,state.config_error);
        // 启动偏好和本次运行开关独立；损坏配置或未选定端口时不打开设备。
        state.config.serial_enabled=state.config_error.empty() && state.config.connect_on_startup && !state.config.port.empty();
        if(state.config.autostart && !autostart_matches())state.config_error+=" 自启动登记与当前路径不匹配，请在设置中重新保存。";
        state.controller=std::make_unique<Controller>(state.root,state.config);
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
        WNDCLASSW c{};c.hInstance=instance;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
        c.lpszClassName=kWindowClass;c.lpfnWndProc=window_proc;RegisterClassW(&c);
        c.lpszClassName=L"DMSettings";c.lpfnWndProc=settings_proc;RegisterClassW(&c);
        UINT dpi=GetDpiForSystem();
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        const auto size=window_size(dpi,620,420);
        const int width=std::min<int>(size.cx,static_cast<int>(work.right-work.left)),height=std::min<int>(size.cy,static_cast<int>(work.bottom-work.top));
        auto window=CreateWindowExW(0,kWindowClass,L"鑫谷 DM 本地遥测代理",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,width,height,nullptr,nullptr,instance,nullptr);
        if(!window)throw std::runtime_error(error_text());
        if(first && !autostart)state.show();
        MSG msg{};
        while(GetMessageW(&msg,nullptr,0,0)>0) {
            if(state.settings && IsDialogMessageW(state.settings,&msg))continue;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(state.small_icon)DestroyIcon(state.small_icon);if(state.large_icon)DestroyIcon(state.large_icon);
        app=nullptr;return 0;
    } catch(const std::exception& e) {MessageBoxW(nullptr,dm::wide(e.what()).c_str(),L"DM Agent 无法启动",MB_OK|MB_ICONERROR);return 1;}
}
