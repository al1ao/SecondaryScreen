#include "settings_dialog.h"
#include "display/monitor_enum.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
namespace pds {
SettingsDialog::SettingsDialog(const AppConfig& config,const std::vector<AdbDevice>& devices,QWidget* parent):QDialog(parent) {
    setWindowTitle(QStringLiteral("PadScreen 设置"));setMinimumWidth(440);
    auto* layout=new QVBoxLayout(this);auto* form=new QFormLayout;
    monitor_=new QComboBox(this);
    monitor_->addItem(QStringLiteral("自动选择虚拟显示器"),QString{});
    monitor_->addItem(QStringLiteral("主显示器（调试）"),QStringLiteral("primary"));
    for(const auto& monitor:EnumerateMonitors()) {
        const auto id=QString::fromStdWString(monitor.monitor_device_id);
        monitor_->addItem(QString::fromStdWString(monitor.friendly_name)+QStringLiteral(" · %1 × %2").arg(monitor.width).arg(monitor.height),id);
    }
    const auto wanted=config.primary?QStringLiteral("primary"):QString::fromStdString(config.monitor_id);
    int index=monitor_->findData(wanted);
    if(index<0){monitor_->addItem(QStringLiteral("上次选择的显示器（未连接）"),wanted);index=monitor_->count()-1;}
    monitor_->setCurrentIndex(index);form->addRow(QStringLiteral("采集显示器"),monitor_);
    auto spin=[&](int min,int max,int value){auto* item=new QSpinBox(this);item->setRange(min,max);item->setValue(value);return item;};
    width_=spin(2,16384,static_cast<int>(config.width));width_->setSingleStep(2);
    height_=spin(2,16384,static_cast<int>(config.height));height_->setSingleStep(2);
    fps_=spin(1,60,static_cast<int>(config.fps));bitrate_=spin(100,100000,static_cast<int>(config.bitrate_kbps));bitrate_->setSuffix(QStringLiteral(" kbps"));
    port_=spin(1,65535,config.port);
    form->addRow(QStringLiteral("输出宽度"),width_);form->addRow(QStringLiteral("输出高度"),height_);
    form->addRow(QStringLiteral("帧率"),fps_);form->addRow(QStringLiteral("码率"),bitrate_);form->addRow(QStringLiteral("端口"),port_);
    device_=new QComboBox(this);device_->addItem(QStringLiteral("自动（仅连接一台时）"),QString{});
    for(const auto& device:devices) device_->addItem(QString::fromStdString(device.model.empty()?device.serial:device.model+" · "+device.serial)+QStringLiteral(" [%1]").arg(QString::fromStdString(device.state)),QString::fromStdString(device.serial));
    index=device_->findData(QString::fromStdString(config.device_serial));
    if(index<0){device_->addItem(QString::fromStdString(config.device_serial)+QStringLiteral("（未连接）"),QString::fromStdString(config.device_serial));index=device_->count()-1;}
    device_->setCurrentIndex(index);form->addRow(QStringLiteral("USB 平板"),device_);layout->addLayout(form);
    auto check=[&](const QString& text,bool checked){auto* item=new QCheckBox(text,this);item->setChecked(checked);layout->addWidget(item);return item;};
    resize_=check(QStringLiteral("自动调整虚拟屏分辨率（会改变 Windows 显示设置）"),config.auto_resize);
    reverse_=check(QStringLiteral("自动配置 USB 连接"),config.auto_reverse);
    startup_=check(QStringLiteral("登录 Windows 时启动"),config.auto_start);
    foreground_=check(QStringLiteral("Android 前台保活（实验性，默认关闭）"),config.foreground);
    connect(monitor_,&QComboBox::currentIndexChanged,this,[this]{const bool primary=monitor_->currentData().toString()==QStringLiteral("primary");resize_->setEnabled(!primary);if(primary)resize_->setChecked(false);});
    if(config.primary)resize_->setEnabled(false);
    auto* note=new QLabel(QStringLiteral("保存后重新连接。编码器自动选择，硬件不可用时回退 JPEG。\nAndroid 保活尚待实机验证；切回 App 会自动连接。"),this);note->setWordWrap(true);layout->addWidget(note);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);layout->addWidget(buttons);
}
AppConfig SettingsDialog::Value() const {
    AppConfig value;
    const auto monitor=monitor_->currentData().toString();value.primary=monitor==QStringLiteral("primary");value.monitor_id=value.primary?"":monitor.toStdString();
    value.width=static_cast<uint32_t>(width_->value())&~1u;value.height=static_cast<uint32_t>(height_->value())&~1u;
    value.fps=static_cast<uint32_t>(fps_->value());value.bitrate_kbps=static_cast<uint32_t>(bitrate_->value());value.port=static_cast<uint16_t>(port_->value());
    value.device_serial=device_->currentData().toString().toStdString();value.auto_resize=resize_->isChecked()&&!value.primary;
    value.auto_reverse=reverse_->isChecked();value.auto_start=startup_->isChecked();value.foreground=foreground_->isChecked();return value;
}
}
