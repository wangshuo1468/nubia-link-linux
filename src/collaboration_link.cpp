#include "collaboration_link.h"
#include <QNetworkProxy>
#include <QUrl>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <unistd.h>

CollaborationLink::CollaborationLink(QObject *parent):QObject(parent) {
    listener_.setProxy(QNetworkProxy::NoProxy); listener_.setMaxPendingConnections(4);
    connect(&discovery_,&PhoneDiscovery::error,this,&CollaborationLink::fail);
    connect(&receiver_,&ReceiverDiscovery::error,this,&CollaborationLink::fail);
    connect(&manager_,&ServiceManager::error,this,&CollaborationLink::fail);
    connect(&manager_,&ServiceManager::connected,this,[this](QString name) {
        peerName_=name.left(80); completeReady();
    });
    connect(&bluetooth_,&BleDiscovery::status,this,&CollaborationLink::status);
    connect(&bluetooth_,&BleDiscovery::error,this,&CollaborationLink::fail);
    connect(&bluetooth_,&BleDiscovery::peerScanned,this,[this](QString id,QString,QString path) {
        if(running_ && blePeers_.size()<16) blePeers_[path]=id;
    });
    connect(&bluetooth_,&BleDiscovery::managementSocket,this,[this](int fd,QString path) {
        auto id=blePeers_.value(path);
        // Android advertises a private LE address but opens RFCOMM with its
        // classic address. With exactly one discovered candidate, its hello
        // must still match that candidate before any service is started.
        if(id.isEmpty() && blePeers_.size()==1) id=blePeers_.first();
        if(!running_ || managerStarted_ || id.isEmpty() || (!incoming_ && id!=blePeerId_)) { ::close(fd); return; }
        blePath_=path; blePeerId_=id;
        if(incoming_) phone_=receiver_.addressForId(id);
        managerStarted_=true;
        if(!manager_.acceptRfcomm(fd,id,receiver_.localId(),local_,bluetooth_.bluetoothAddress(),incoming_)) managerStarted_=false;
    });
    connect(&bluetooth_,&BleDiscovery::managementDisconnected,this,[this](QString path) {
        if(running_ && path==blePath_) { stop(); emit disconnected(); }
    });
    connect(&receiver_,&ReceiverDiscovery::peerFound,this,[this](QHostAddress ip,QJsonObject metadata) {
        if(!incoming_ && !local_.isLoopback() && ip==phone_ && blePeerId_.isEmpty()) {
            blePeerId_=metadata.value("deviceId").toString();
            if(!bluetooth_.connectPeer(blePeerId_)) fail(QStringLiteral("无法选择手机的协同蓝牙身份"));
        }
        if(!blePeerId_.isEmpty() && metadata.value("deviceId").toString()==blePeerId_) { phone_=ip; completeReady(); }
    });
    connect(&manager_,&ServiceManager::message,this,[this](QJsonObject rpc) {
        if(blePeerId_.isEmpty() || rpc.value("action")!="cmdSendIp") return;
        const auto profile=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
        const auto addresses=profile.value("srcIps").toString().split(',');
        if(addresses.size()>16) return;
        for(const auto &value:addresses) {
            QHostAddress ip(value);
            if(ip.protocol()==QAbstractSocket::IPv4Protocol && ip!=local_ && ip.isInSubnet(local_,prefix_) && (incoming_ || ip==phone_)) {
                phone_=ip; completeReady(); break;
            }
        }
    });
    connect(&manager_,&ServiceManager::disconnected,this,[this] {
        if(!running_) return;
        stop(); emit disconnected();
    });
    connect(&discovery_,&PhoneDiscovery::found,this,[this](QJsonObject device) {
        if(!running_ || incoming_ || managerStarted_) return;
        const QUrl endpoint(device.value("socketUri").toString());
        if(endpoint.scheme()!="socket" || QHostAddress(endpoint.host())!=phone_ ||
           endpoint.port()<1 || endpoint.port()>65535 || !endpoint.userInfo().isEmpty()) {
            fail(QStringLiteral("手机返回的协同服务管理地址无效")); return;
        }
        managerStarted_=true;
        // Retain discovery for the SDK's physical-connection identity lookup.
        manager_.start(phone_,quint16(endpoint.port()),device.value("deviceId").toString(),discovery_.localId(),DesktopRole::Collaboration);
    });
    connect(&listener_,&QTcpServer::newConnection,this,[this] {
        while(listener_.hasPendingConnections()) {
            auto socket=listener_.nextPendingConnection();
            const auto peer=receiver_.peer(socket->peerAddress());
            if(!running_ || !incoming_ || managerStarted_ || peer.isEmpty()) {
                socket->abort(); socket->deleteLater(); continue;
            }
            phone_=socket->peerAddress(); managerStarted_=true;
            if(!manager_.accept(socket,peer.value("deviceId").toString(),receiver_.localId(),DesktopRole::Collaboration)) {
                managerStarted_=false;
            }
        }
    });
}
CollaborationLink::~CollaborationLink() { stop(); }
bool CollaborationLink::start(QHostAddress local,QHostAddress phone,quint16 discoveryPort) {
    if(!local.isLoopback()) {
        for(const auto &interface:QNetworkInterface::allInterfaces()) for(const auto &entry:interface.addressEntries()) {
            if(entry.ip()!=local || entry.prefixLength()<1 || !phone.isInSubnet(local,entry.prefixLength())) continue;
            if(!listen(local,entry.prefixLength(),interface.name(),discoveryPort)) return false;
            incoming_=false; phone_=phone;
            emit status(QStringLiteral("电脑正在发现手机的独立多屏协同服务")); return true;
        }
        emit error(QStringLiteral("请选择手机所在的局域网接口")); return false;
    }
    // Local protocol fixtures exercise the supported legacy TCP transport.
    stop(); local_=local; phone_=phone; running_=true;
    if(!discovery_.start(local,phone,discoveryPort,DesktopRole::Collaboration)) { stop(); return false; }
    return true;
}
bool CollaborationLink::listen(QHostAddress local,int prefix,QString interfaceName,quint16 discoveryPort) {
    stop(); local_=local; prefix_=prefix;
    NubiaBle::desktopId();
    if(!listener_.listen(local,0)) { emit error(QStringLiteral("无法监听协同连接：")+listener_.errorString()); return false; }
    running_=true; incoming_=true;
    const QJsonArray services{
        QJsonObject{{"serviceName","MultiScrService"},{"serviceUUid","187f8000-0000-1000-8000-ceb574815100"},{"version",9},{"authType",0},{"custData","{}"}},
        QJsonObject{{"serviceName","RELAY_SERVICE"},{"serviceUUid","187f8000-0000-1000-8000-ceb574815500"},{"version",22},{"authType",0},{"custData","{}"}}
    };
    if(!receiver_.start(local,prefix,interfaceName,listener_.serverPort(),services,discoveryPort,DesktopRole::Collaboration)) { stop(); return false; }
    // Loopback fixtures exercise incoming RPCs and framing without radios.
    if(!local.isLoopback() && !bluetooth_.start(receiver_.localId(),services)) { stop(); return false; }
    return true;
}
void CollaborationLink::completeReady() {
    if(!running_ || !manager_.ready() || readyEmitted_ || phone_.isNull()) return;
    readyEmitted_=true; emit ready(local_,phone_,peerName_);
}
void CollaborationLink::stop() {
    running_=incoming_=managerStarted_=false;
    listener_.close(); receiver_.stop(); discovery_.stop(); manager_.stop(); bluetooth_.stop();
    blePeers_.clear(); blePath_.clear(); blePeerId_.clear(); peerName_.clear(); phone_.clear(); readyEmitted_=false;
}
void CollaborationLink::fail(QString reason) { stop(); emit error(reason); }
