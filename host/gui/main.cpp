#include "tray.h"
#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QTimer>
int main(int argc,char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QApplication app(argc,argv);app.setApplicationName(QStringLiteral("SecondaryScreen"));app.setOrganizationName(QStringLiteral("SecondaryScreen"));app.setQuitOnLastWindowClosed(false);
    qInstallMessageHandler([](QtMsgType type,const QMessageLogContext&,const QString& message){pds::Log(type==QtFatalMsg||type==QtCriticalMsg?pds::LogLevel::Error:pds::LogLevel::Debug,message.toStdString());});
    const auto args=app.arguments();const bool smoke=args.contains(QStringLiteral("--smoke-test"));
    const bool quit=args.contains(QStringLiteral("--quit"));
    QString instance=QStringLiteral("SecondaryScreen-")+qEnvironmentVariable("USERNAME");
    if(smoke)instance+=QStringLiteral("-smoke-%1").arg(GetCurrentProcessId());
    QLocalSocket client;client.connectToServer(instance);
    if(client.waitForConnected(500)){
        client.write(quit?"quit\n":"show\n");if(!client.waitForBytesWritten(500))return 1;
        // Uninstall waits for the owning instance to release workers and sockets.
        if(quit&&client.state()!=QLocalSocket::UnconnectedState&&!client.waitForDisconnected(15000))return 1;
        return 0;
    }
    if(quit)return 0;
    QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);
    if(!server.listen(instance)){LOG_ERROR("Single instance listen: ",server.errorString().toStdString());return 1;}
    try {
        pds::Tray tray(smoke);
        QObject::connect(&server,&QLocalServer::newConnection,&app,[&]{
            while(auto* socket=server.nextPendingConnection()) {
                const auto read=[socket,&tray]{
                    if(socket->bytesAvailable()>64){socket->abort();return;}
                    if(!socket->canReadLine())return;
                    const auto command=socket->readLine(65).trimmed();
                    if(command=="quit"){qApp->quit();return;}
                    socket->disconnectFromServer();
                    if(command=="show")QTimer::singleShot(0,&tray,[&tray]{tray.ShowSettings();});
                };
                QObject::connect(socket,&QLocalSocket::readyRead,socket,read);
                QObject::connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
                read();
            }
        });
        if(smoke){const int index=args.indexOf(QStringLiteral("--smoke-test"));if(index+1>=args.size())return 2;tray.Smoke(args[index+1]);}
        return app.exec();
    }catch(const std::exception& error){LOG_ERROR("GUI: ",error.what());if(!smoke)QMessageBox::critical(nullptr,QStringLiteral("SecondaryScreen"),QStringLiteral("无法启动，请检查配置目录权限：\n")+QString::fromUtf8(error.what()));return 1;}
}
