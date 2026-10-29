#pragma once
#include "app/config.h"
#include "adb/adb_manager.h"
#include "util/log.h"
#include <QObject>
#include <QIcon>
#include <condition_variable>
#include <fstream>
#include <thread>
class QAction;
class QLabel;
class QMenu;
class QSystemTrayIcon;
class QWidget;
namespace pds {
enum class TrayState {Stopped,Waiting,Connected,Error};
QIcon StatusIcon(TrayState state);
class Tray : public QObject {
public:
    explicit Tray(bool smoke=false);
    ~Tray() override;
    void ShowSettings();
    void Smoke(const QString& directory);
private:
    void Monitor(std::stop_token stop);
    void OnLog(LogLevel level,const std::string& line);
    void SetState(TrayState state,const QString& message);
    void ToggleStats();
    void Request(bool enabled);
    AppConfig config_;
    std::filesystem::path config_path_,log_directory_;
    std::ofstream log_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread monitor_;
    bool enabled_=true,reconnect_=false,connected_=false,smoke_=false;
    std::vector<AdbDevice> devices_;
    QSystemTrayIcon* tray_;
    QMenu* menu_;
    QAction *status_,*toggle_;
    QWidget* stats_;
    QLabel* stats_text_;
    QString last_stats_,last_status_;
};
}
