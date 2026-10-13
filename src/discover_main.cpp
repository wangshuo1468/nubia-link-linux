#include "discovery.h"
#include "service_manager.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <QUrl>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    const bool handshake=args.size()==4 && args[3]=="--manager";
    if(args.size()!=3 && !handshake) { QTextStream(stderr)<<"Usage: nubia-discover LOCAL_IPV4 PHONE_IPV4 [--manager]\n"; return 2; }
    PhoneDiscovery discovery;
    ServiceManager manager;
    QObject::connect(&manager,&ServiceManager::error,&app,[&](const QString &error) { QTextStream(stderr)<<error<<'\n'; app.exit(1); });
    QObject::connect(&manager,&ServiceManager::disconnected,&app,[&] { QTextStream(stderr)<<"Service manager disconnected\n"; app.exit(1); });
    QObject::connect(&manager,&ServiceManager::connected,&app,[&](const QString &name) {
        QTextStream(stdout)<<"Official service-manager handshake: "<<name<<" (p=0; application pairing pending)\n";
        QTimer::singleShot(1000,&app,&QCoreApplication::quit);
    });
    QObject::connect(&manager,&ServiceManager::servicePublished,&app,[](const QJsonObject &service) {
        QTextStream(stdout)<<"Manager service: "<<service.value("serviceName").toString()<<" | "
            <<service.value("serviceUuid").toString()<<" | authType="<<service.value("authType").toInt()<<'\n';
    });
    QObject::connect(&discovery,&PhoneDiscovery::error,&app,[&](const QString &error) { QTextStream(stderr)<<error<<'\n'; app.exit(1); });
    QObject::connect(&discovery,&PhoneDiscovery::found,&app,[&](const QJsonObject &device) {
        QTextStream out(stdout); out<<"Official device: "<<device.value("deviceName").toString()<<'\n';
        out<<"Service-manager endpoint: "<<device.value("socketUri").toString()<<'\n';
        for(const auto &value:device.value("custData").toArray()) {
            const auto service=value.toObject();
            out<<service.value("serviceName").toString()<<" | "<<service.value("serviceUUid").toString()
               <<" | authType="<<service.value("authType").toInt()<<'\n';
        }
        if(!handshake) { app.quit(); return; }
        const QUrl endpoint(device.value("socketUri").toString());
        if(endpoint.scheme()!="socket" || QHostAddress(endpoint.host())!=QHostAddress(args[2]) ||
           endpoint.port()<=0 || endpoint.port()>65535 || !endpoint.userInfo().isEmpty()) {
            QTextStream(stderr)<<"Invalid official manager endpoint\n"; app.exit(1); return;
        }
        const auto localId=discovery.localId(); discovery.stop();
        manager.start(QHostAddress(endpoint.host()),quint16(endpoint.port()),device.value("deviceId").toString(),localId);
    });
    if(!discovery.start(QHostAddress(args[1]),QHostAddress(args[2]))) return 1;
    return app.exec();
}
