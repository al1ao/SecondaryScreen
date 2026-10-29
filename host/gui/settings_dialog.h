#pragma once
#include "app/config.h"
#include "adb/adb_manager.h"
#include <QDialog>
class QComboBox;
class QSpinBox;
class QCheckBox;
namespace pds {
class SettingsDialog : public QDialog {
public:
    SettingsDialog(const AppConfig& config,const std::vector<AdbDevice>& devices,QWidget* parent=nullptr);
    AppConfig Value() const;
private:
    QComboBox *monitor_, *device_;
    QSpinBox *width_, *height_, *fps_, *max_fps_, *bitrate_, *port_;
    QCheckBox *automatic_size_, *resize_, *reverse_, *startup_, *foreground_;
};
}
