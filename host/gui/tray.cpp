#include "tray.h"
#include "settings_dialog.h"
#include "app/host_service.h"
#include "app/process.h"
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpression>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
namespace pds {
QIcon StatusIcon(TrayState state) {
    const QColor colors[]{QColor("#8793a0"),QColor("#d9a323"),QColor("#24a66a"),QColor("#d64f5f")};
    QPixmap image(64,64);image.fill(Qt::transparent);QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colors[static_cast<unsigned>(state)],5));painter.setBrush(QColor("#f6f8fa"));
    painter.drawRoundedRect(QRectF(7,9,50,35),5,5);painter.drawLine(32,45,32,53);painter.drawLine(21,54,43,54);
    painter.setPen(Qt::NoPen);painter.setBrush(colors[static_cast<unsigned>(state)]);painter.drawEllipse(QRectF(24,18,16,16));return QIcon(image);
}
Tray::Tray(bool smoke):smoke_(smoke) {
    config_path_=smoke?ExecutableDirectory()/L"smoke-config"/L"config.json":ConfigPath();
    if(config_path_.empty())throw std::runtime_error("cannot locate config directory");
    config_=LoadConfig(config_path_);if(!smoke)config_.auto_start=AutoStartEnabled();log_directory_=config_path_.parent_path()/L"logs";
    std::filesystem::create_directories(log_directory_);log_.open(log_directory_/L"secondaryscreen.log",std::ios::app);
    if(!log_)throw std::runtime_error("cannot open log file");
    menu_=new QMenu;status_=menu_->addAction(QStringLiteral("未启动"));status_->setEnabled(false);menu_->addSeparator();
    toggle_=menu_->addAction(QStringLiteral("停止服务"));
    connect(toggle_,&QAction::triggered,this,[this]{bool enabled;{std::lock_guard lock(mutex_);enabled=!enabled_;}Request(enabled);});
    connect(menu_->addAction(QStringLiteral("重新连接")),&QAction::triggered,this,[this]{Request(true);});menu_->addSeparator();
    connect(menu_->addAction(QStringLiteral("设置…")),&QAction::triggered,this,[this]{ShowSettings();});
    connect(menu_->addAction(QStringLiteral("显示统计")),&QAction::triggered,this,[this]{ToggleStats();});
    connect(menu_->addAction(QStringLiteral("打开日志目录")),&QAction::triggered,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(log_directory_.wstring())));});
    menu_->addSeparator();connect(menu_->addAction(QStringLiteral("退出")),&QAction::triggered,qApp,&QApplication::quit);
    tray_=new QSystemTrayIcon(StatusIcon(TrayState::Stopped),this);tray_->setContextMenu(menu_);tray_->setToolTip(QStringLiteral("SecondaryScreen"));tray_->show();
    connect(tray_,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason reason){if(reason==QSystemTrayIcon::Trigger)ToggleStats();});
    stats_=new QWidget;stats_->setWindowTitle(QStringLiteral("SecondaryScreen 统计"));stats_->resize(500,240);
    auto* layout=new QVBoxLayout(stats_);stats_text_=new QLabel(QStringLiteral("等待连接\n编码数据将在连接后显示"),stats_);stats_text_->setWordWrap(true);stats_text_->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(stats_text_);
    SetLogSink([this](LogLevel level,const std::string& line){
        if(log_.tellp()>4*1024*1024){log_.close();std::error_code error;std::filesystem::remove(log_directory_/L"previous.log",error);std::filesystem::rename(log_directory_/L"secondaryscreen.log",log_directory_/L"previous.log",error);log_.open(log_directory_/L"secondaryscreen.log",std::ios::trunc);}
        constexpr const char* levels[]{"TRACE","DEBUG","INFO","WARN","ERROR"};
        log_<<'['<<levels[static_cast<unsigned>(level)]<<"] "<<line<<'\n';log_.flush();
        QMetaObject::invokeMethod(this,[this,level,line]{OnLog(level,line);},Qt::QueuedConnection);
    });
    if(!smoke_)monitor_=std::jthread([this](std::stop_token stop){Monitor(stop);});
}
Tray::~Tray(){if(monitor_.joinable()){monitor_.request_stop();wake_.notify_all();monitor_.join();}SetLogSink({});tray_->hide();delete menu_;delete stats_;}
void Tray::Request(bool enabled) {
    {std::lock_guard lock(mutex_);enabled_=enabled;reconnect_=true;}
    connected_=false;toggle_->setText(enabled?QStringLiteral("停止服务"):QStringLiteral("启动服务"));
    SetState(enabled?TrayState::Waiting:TrayState::Stopped,enabled?QStringLiteral("正在准备连接…"):QStringLiteral("正在停止服务…"));wake_.notify_all();
}
void Tray::Monitor(std::stop_token stop) {
    try {
        HostService service;std::unique_ptr<AdbManager> adb;AppConfig previous;bool have_previous=false;bool active=false;
        while(!stop.stop_requested()) {
            AppConfig config;bool enabled,reconnect;
            {std::lock_guard lock(mutex_);config=config_;enabled=enabled_;reconnect=std::exchange(reconnect_,false);}
            if(!enabled){service.Stop();adb.reset();active=false;QMetaObject::invokeMethod(this,[this]{SetState(TrayState::Stopped,QStringLiteral("服务已停止"));},Qt::QueuedConnection);}
            else {
                if(reconnect || (have_previous && config!=previous)){service.Stop();adb.reset();active=false;}
                previous=config;have_previous=true;
                if(!adb){const auto path=FindAdb(ExecutableDirectory());if(path)adb=std::make_unique<AdbManager>(*path);}
                AdbStatus state;
                if(!adb){state.error=true;state.message="未找到 adb，请重新安装含 platform-tools 的安装包";}
                else state=adb->Poll(config.port,config.device_serial,config.auto_reverse,config.foreground);
                if(!state.ready){service.Stop();active=false;}
                else {
                    if(state.changed){service.Stop();active=false;}
                    if(!active){service.Start(SessionFromConfig(config));active=true;}
                    else if(!service.Running()){state.error=true;state.ready=false;state.message="服务启动失败，请检查显示器、端口和日志";}
                }
                QMetaObject::invokeMethod(this,[this,state=std::move(state)]{
                    devices_=state.devices;
                    if(!state.ready)connected_=false;
                    if(state.error)SetState(TrayState::Error,QString::fromStdString(state.message));
                    else if(!connected_)SetState(TrayState::Waiting,QString::fromStdString(state.message));
                },Qt::QueuedConnection);
            }
            std::unique_lock lock(mutex_);wake_.wait_for(lock,stop,std::chrono::seconds(3),[this]{return reconnect_;});
        }
        service.Stop();adb.reset();
    }catch(const std::exception& error){LOG_ERROR("Tray monitor: ",error.what());QMetaObject::invokeMethod(this,[this]{SetState(TrayState::Error,QStringLiteral("后台服务异常，请查看日志"));},Qt::QueuedConnection);}
}
void Tray::SetState(TrayState state,const QString& message) {
    Q_ASSERT(QThread::currentThread()==qApp->thread());
    if(message!=last_status_)LOG_INFO("Tray state=",static_cast<unsigned>(state)," ",message.toStdString());
    tray_->setIcon(StatusIcon(state));status_->setText(message);tray_->setToolTip(QStringLiteral("SecondaryScreen · ")+message);
    if(state==TrayState::Error && message!=last_status_)tray_->showMessage(QStringLiteral("SecondaryScreen"),message,QSystemTrayIcon::Warning);
    last_status_=message;stats_text_->setText(message+QStringLiteral("\n\n")+last_stats_);
}
void Tray::OnLog(LogLevel,const std::string& line) {
    Q_ASSERT(QThread::currentThread()==qApp->thread());
    if(line.starts_with("HELLO_ACK ")&&!line.starts_with("HELLO_ACK ready:")){connected_=true;SetState(TrayState::Connected,QStringLiteral("已连接 · ")+QString::fromStdString(line.substr(10)));}
    if(line.find("client disconnected")!=std::string::npos){connected_=false;SetState(TrayState::Waiting,QStringLiteral("连接已断开，等待自动重连"));}
    if(line.starts_with("fps=")){
        const auto text=QString::fromStdString(line);
        const auto metric=[&](const QString& name){return QRegularExpression(QStringLiteral("\\b%1=([0-9.]+)").arg(name)).match(text).captured(1);};
        last_stats_=QStringLiteral("帧率：%1 fps\n编码（含转换与等待）：平均 %2 ms · P95 上界 %3 ms\n发送：%4 ms · 队列：%5 · 累计丢旧帧：%6")
            .arg(metric(QStringLiteral("fps")),metric(QStringLiteral("encode")),metric(QStringLiteral("encode_session_p95_upper")),metric(QStringLiteral("send")),metric(QStringLiteral("queue")),metric(QStringLiteral("drop")));
        stats_text_->setText(last_status_+QStringLiteral("\n\n")+last_stats_);
    }
}
void Tray::ToggleStats(){if(stats_->isVisible())stats_->hide();else{stats_->show();stats_->raise();stats_->activateWindow();}}
void Tray::ShowSettings() {
    if(auto* modal=QApplication::activeModalWidget()){modal->raise();modal->activateWindow();return;}
    AppConfig config;{std::lock_guard lock(mutex_);config=config_;}
    SettingsDialog dialog(config,devices_);
    if(dialog.exec()!=QDialog::Accepted)return;
    const auto next=dialog.Value();
    if(!SaveConfig(config_path_,next)){QMessageBox::warning(nullptr,QStringLiteral("SecondaryScreen"),QStringLiteral("配置保存失败，原设置未改变"));return;}
    if(next.auto_start!=config.auto_start&&!SetAutoStart(next.auto_start)) {
        SaveConfig(config_path_,config);QMessageBox::warning(nullptr,QStringLiteral("SecondaryScreen"),QStringLiteral("开机自启设置失败，已恢复原配置"));return;
    }
    {std::lock_guard lock(mutex_);config_=next;}Request(true);
}
void Tray::Smoke(const QString& directory) {
    QDir().mkpath(directory);
    auto* dialog=new SettingsDialog(AppConfig{},{{"example-tablet","device","平板示例"}},stats_);dialog->show();
    std::thread notifier([this]{QMetaObject::invokeMethod(this,[this]{SetState(TrayState::Waiting,QStringLiteral("界面演示：等待连接（非实机）"));},Qt::QueuedConnection);});notifier.join();
    QTimer::singleShot(500,this,[this,dialog,directory]{
        bool saved=dialog->grab().save(directory+QStringLiteral("/settings.png"));dialog->hide();
        for(unsigned i=0;i<4;++i)saved=StatusIcon(static_cast<TrayState>(i)).pixmap(64,64).save(directory+QStringLiteral("/state-%1.png").arg(i))&&saved;
        stats_->show();saved=stats_->grab().save(directory+QStringLiteral("/stats.png"))&&saved;
        menu_->popup(QPoint(80,80));saved=menu_->grab().save(directory+QStringLiteral("/menu.png"))&&saved;menu_->hide();
        LOG_INFO("GUI smoke: queued worker notification, four icons, settings/menu/stats rendered; system tray available=",QSystemTrayIcon::isSystemTrayAvailable());
        qApp->exit(saved?0:1);
    });
}
}
