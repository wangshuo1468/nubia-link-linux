#include "window.h"
#include "file_browser.h"
#include "collaboration_window.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QCommandLineParser>
extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char **argv) {
    av_log_set_level(AV_LOG_WARNING);
    QSurfaceFormat format; format.setVersion(2,1); format.setProfile(QSurfaceFormat::NoProfile);
    format.setDepthBufferSize(0); format.setStencilBufferSize(0); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);
    app.setApplicationName(QStringLiteral("NubiaLink")); app.setOrganizationName(QStringLiteral("NubiaLink"));
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("努比亚智慧投屏与多屏协同 Linux 原生预览"));
    parser.addHelpOption();
    QCommandLineOption device("device",QStringLiteral("连接已授权的 ADB 手机（序列号或 IP:端口）"),"device");
    QCommandLineOption lan("lan",QStringLiteral("通过官方局域网协议连接手机 IPv4"),"phone-ip");
    QCommandLineOption files("files",QStringLiteral("只打开原厂文件互传窗口，需指定 --local"),"phone-ip");
    QCommandLineOption local("local",QStringLiteral("指定局域网投屏的本机 IPv4，默认匹配同网段接口"),"local-ip");
    QCommandLineOption collaboration("collaboration",QStringLiteral("打开独立多屏协同入口并连接手机 IPv4"),"phone-ip");
    QCommandLineOption collaborationUi("collaboration-ui",QStringLiteral("打开多屏协同窗口，等待手机连接"));
    QCommandLineOption receive("receive",QStringLiteral("开启智慧投屏接收，等待手机发起"));
    QCommandLineOption receiveCollaboration("receive-collaboration",QStringLiteral("开启多屏协同接收，等待手机发起"));
    QCommandLineOption package("package",QStringLiteral("多屏协同应用包名，默认设置应用"),"package","com.android.settings");
    parser.addOption(device); parser.addOption(lan); parser.addOption(local); parser.addOption(files);
    parser.addOption(collaboration); parser.addOption(collaborationUi); parser.addOption(package);
    parser.addOption(receive); parser.addOption(receiveCollaboration); parser.process(app);
    // Receiver switches do not need a saved phone IP or ADB connection.
    if(parser.isSet(receive) || parser.isSet(receiveCollaboration)) {
        if((parser.isSet(receive) && parser.isSet(receiveCollaboration)) || parser.isSet(device) || parser.isSet(lan) ||
           parser.isSet(local) || parser.isSet(files) || parser.isSet(collaboration) || parser.isSet(collaborationUi) || parser.isSet(package)) parser.showHelp(2);
        if(parser.isSet(receiveCollaboration)) {
            MainWindow window; window.show(); QTimer::singleShot(0,&window,[&window] { window.openCollaboration({}); }); return app.exec();
        }
        MainWindow window; window.show(); QTimer::singleShot(0,&window,&MainWindow::receivePhone); return app.exec();
    }
    if(parser.isSet(collaboration) || parser.isSet(collaborationUi)) {
        if(parser.isSet(device) || parser.isSet(lan) || parser.isSet(files) ||
           (parser.isSet(collaboration) && parser.isSet(collaborationUi)) ||
           (parser.isSet(collaborationUi) && (parser.isSet(local) || parser.isSet(package)))) parser.showHelp(2);
        MainWindow window; window.show();
        if(parser.isSet(collaboration)) QTimer::singleShot(0,&window,[&] { window.openCollaboration(parser.value(collaboration),parser.value(local),true,parser.value(package)); });
        else QTimer::singleShot(0,&window,[&window] { window.openCollaboration({}); });
        return app.exec();
    }
    if(parser.isSet(package)) parser.showHelp(2);
    if(parser.isSet(files)) {
        if(parser.isSet(device) || parser.isSet(lan) || !parser.isSet(local)) parser.showHelp(2);
        const QHostAddress source(parser.value(local)),target(parser.value(files));
        if(source.protocol()!=QAbstractSocket::IPv4Protocol || target.protocol()!=QAbstractSocket::IPv4Protocol) parser.showHelp(2);
        FileBrowser browser(source,target); browser.show(); return app.exec();
    }
    if(parser.isSet(device) && parser.isSet(lan)) parser.showHelp(2);
    if(!parser.isSet(device) && !parser.isSet(lan)) {
        MainWindow window; window.openCollaboration({}); window.show();
        return app.exec();
    }
    MainWindow window; window.show();
    if (parser.isSet(device)) QTimer::singleShot(0,&window,[&window,serial=parser.value(device)] { window.connectDevice(serial); });
    if (parser.isSet(lan)) QTimer::singleShot(0,&window,[&window,phone=parser.value(lan),ip=parser.value(local)] { window.connectLan(phone,ip); });
    return app.exec();
}
