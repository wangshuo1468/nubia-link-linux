#include "discovery.h"
#include "service_manager.h"
#include "dfs.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QUuid>
#include <QtEndian>

// Opt-in real-device DFS probe, excluded from CTest. Only the uniquely named
// roundtrip file is read/written/deleted; --list enumerates virtual root names.
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); auto args=app.arguments();
    const bool shared=args.removeAll("--shared")>0;
    if(args.size()<5 || args[3]!="--start") {
        QTextStream(stderr)<<"Usage: dfs-device-probe LOCAL_IPV4 PHONE_IPV4 --start PORT [--list | --roundtrip REMOTE_DIR]\n"; return 2;
    }
    bool portOk=false; const auto port=args[4].toUInt(&portOk);
    const bool listing=(args.size()==6 || (args.size()==7 && Dfs::validPath(args[6]))) && args[5]=="--list";
    const bool roundtrip=args.size()==7 && args[5]=="--roundtrip" && Dfs::validPath(args[6]);
    if(!portOk || !port || port>65535 || (args.size()!=5 && !listing && !roundtrip)) return 2;
    const QString uuid="57240e3f-8445-4d76-b84d-9c761ca565c8";
    PhoneDiscovery discovery; ServiceManager manager; DfsClient dfs;
    bool requested=false,acknowledged=false,finishing=false,uploaded=false;
    QTemporaryDir temp;
    const auto local=temp.filePath("source.bin"),received=temp.filePath("received.bin");
    const auto testName="nubia-link-核验-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".bin";
    const auto remote=(roundtrip && !args[6].isEmpty() ? args[6]+"/" : QString())+testName;
    QByteArray content(3*1024*1024+4097,Qt::Uninitialized);
    if(roundtrip) {
        for(qsizetype i=0;i<content.size();++i) content[i]=char((i*37+i/251)%256);
        QFile file(local); if(!temp.isValid() || !file.open(QIODevice::WriteOnly) || file.write(content)!=content.size()) return 1;
    }
    std::function<void(int)> finish;
    auto shutdown=[&](int code) {
        dfs.stop();
        if(requested && manager.ready()) {
            const bool sent=manager.sendProfile("distributebus.action.startservice",uuid,"server_can_die");
            QTextStream(stdout)<<"DFS Java cleanup request: "<<(sent ? "sent" : "failed")<<Qt::endl;
            QTimer::singleShot(7000,&app,[&,code] { manager.stop(); discovery.stop(); app.exit(code); });
        } else QTimer::singleShot(0,&app,[&,code] { manager.stop(); discovery.stop(); app.exit(code); });
    };
    finish=[&](int code) {
        if(finishing) return; finishing=true;
        if(uploaded && dfs.ready() && !dfs.busy()) {
            dfs.request(14,Dfs::string(((shared ? "/data/media/0/" : "/mnt/dfs/")+remote).toUtf8()),[&,code](const Dfs::Packet &p) {
                const bool removed=p.type==101 && p.payload.size()>=4 && qFromBigEndian<quint32>(p.payload.constData())==0;
                QTextStream(stdout)<<"Roundtrip phone test-file cleanup: "<<(removed ? "success" : "FAILED")<<Qt::endl;
                uploaded=!removed; shutdown(removed ? code : 1);
            });
        } else shutdown(code);
    };
    auto error=[&](QString reason) { QTextStream(stderr)<<reason<<Qt::endl; if(finishing) shutdown(1); else finish(1); };
    QObject::connect(&discovery,&PhoneDiscovery::error,&app,error);
    QObject::connect(&manager,&ServiceManager::error,&app,error);
    QObject::connect(&dfs,&DfsClient::error,&app,error);
    QObject::connect(&dfs,&DfsClient::operationFailed,&app,error);
    QObject::connect(&manager,&ServiceManager::disconnected,&app,[&] { if(!finishing) error("Manager disconnected"); });
    QObject::connect(&discovery,&PhoneDiscovery::found,&app,[&](QJsonObject device) {
        if(requested || manager.ready() || finishing) return;
        const QUrl endpoint(device.value("socketUri").toString());
        if(endpoint.scheme()!="socket" || QHostAddress(endpoint.host())!=QHostAddress(args[2]) ||
           endpoint.port()<=0 || endpoint.port()>65535 || !endpoint.userInfo().isEmpty()) { finish(1); return; }
        manager.start(QHostAddress(args[2]),quint16(endpoint.port()),device.value("deviceId").toString(),discovery.localId());
    });
    auto request=[&] {
        if(requested || finishing || !manager.ready()) return;
        bool available=false;
        for(const auto &service:manager.services()) if(service.value("serviceUuid")==uuid) {
            if(service.value("authType").toInt()!=0) { error("DFS requires additional authentication"); return; }
            available=service.value("enable").toBool();
        }
        if(!available) return;
        const QJsonObject publication{{"serviceName","dfsservice"},{"serviceUuid",uuid},
            {"serviceVersion",1},{"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}};
        if(!manager.send("distributebus.action.publishservice","1",publication)) { finish(1); return; }
        requested=manager.sendProfile("distributebus.action.startservice",uuid,"remote_ready_mount_me="+discovery.localId());
        QTextStream(stdout)<<"Official DFS startup request: "<<(requested ? "sent" : "failed")<<Qt::endl;
        if(!requested) finish(1);
    };
    QObject::connect(&manager,&ServiceManager::connected,&app,request);
    QObject::connect(&manager,&ServiceManager::servicePublished,&app,[&] { request(); });
    QObject::connect(&manager,&ServiceManager::message,&app,[&](QJsonObject rpc) {
        if(finishing || rpc.value("uuid")!=uuid) return;
        if(rpc.value("action")=="distributebus.action.startservice" && rpc.value("profile")=="dfs_is_ok" && !acknowledged) {
            acknowledged=true;
            QTimer::singleShot(1000,&app,[&] { if(!finishing) dfs.start(QHostAddress(args[2]),quint16(port),discovery.localId(),shared ? DfsClient::Root::SharedStorage : DfsClient::Root::Virtual); });
        }
    });
    QObject::connect(&dfs,&DfsClient::connected,&app,[&] {
        QTextStream(stdout)<<"Official DFS root handshake: success"<<Qt::endl;
        if(listing) dfs.list(args.size()==7 ? args[6] : QString());
        else if(roundtrip) dfs.upload(local,remote);
        else finish(0);
    });
    QObject::connect(&dfs,&DfsClient::listing,&app,[&](QString,QList<Dfs::Entry> entries) {
        for(const auto &entry:entries) if(entry.directory()) QTextStream(stdout)<<"Directory: "<<entry.name<<Qt::endl;
        finish(0);
    });
    QObject::connect(&dfs,&DfsClient::completed,&app,[&](QString) {
        if(!uploaded) { uploaded=true; QTextStream(stdout)<<"Upload: success ("<<content.size()<<" bytes)"<<Qt::endl; dfs.download(remote,received); }
        else {
            QFile file(received); const bool opened=file.open(QIODevice::ReadOnly);
            const auto receivedContent=file.readAll(); const bool matched=opened && receivedContent==content;
            QTextStream(stdout)<<"Download exact bytes: "<<(matched ? "MATCH" : "MISMATCH")<<"; SHA256="
                <<QCryptographicHash::hash(receivedContent,QCryptographicHash::Sha256).toHex()<<Qt::endl;
            finish(matched ? 0 : 1);
        }
    });
    QTimer::singleShot(120000,&app,[&] { if(!finishing) error("DFS probe timed out"); });
    if(!discovery.start(QHostAddress(args[1]),QHostAddress(args[2]))) return 1;
    return app.exec();
}
