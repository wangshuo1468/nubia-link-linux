#include "lan_projection.h"
#include <QJsonDocument>
#include <QUrl>
#include <QJsonArray>
#include <QNetworkProxy>

namespace {
const QString CastUuid=QStringLiteral("187f8000-0000-1000-8000-ceb574816100");
QString json(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
}
LanProjection::LanProjection(ProjectionSession &media,QObject *parent):QObject(parent),media_(media) {
    deadline_.setSingleShot(true); deadline_.setInterval(120000);
    audioStart_.setSingleShot(true); audioStart_.setInterval(500);
    connect(&audioStart_,&QTimer::timeout,this,[this] {
        if(running_ && media_.connected()) media_.send(Nubia::text(12,QStringLiteral("{\"set_sound_control\":2}")));
    });
    connect(&deadline_,&QTimer::timeout,this,[this] { fail(QStringLiteral("局域网投屏超时，请检查手机上的连接和投屏提示")); });
    connect(&discovery_,&PhoneDiscovery::error,this,&LanProjection::fail);
    listener_.setProxy(QNetworkProxy::NoProxy);
    listener_.setMaxPendingConnections(4);
    connect(&receiver_,&ReceiverDiscovery::error,this,&LanProjection::fail);
    connect(&listener_,&QTcpServer::newConnection,this,[this] {
        while(listener_.hasPendingConnections()) {
            auto socket=listener_.nextPendingConnection(); const auto ip=socket->peerAddress();
            const auto peer=receiver_.peer(ip);
            if(!running_ || !incoming_ || managerStarted_ || peer.isEmpty()) { socket->abort(); socket->deleteLater(); continue; }
            phone_=ip; peerName_=peer.value("deviceName").toString().left(80); managerStarted_=true;
            if(!manager_.accept(socket,peer.value("deviceId").toString(),receiver_.localId())) managerStarted_=false;
        }
    });
    connect(&manager_,&ServiceManager::error,this,&LanProjection::fail);
    connect(&manager_,&ServiceManager::disconnected,this,[this] { if(running_) fail(QStringLiteral("官方协同连接已断开")); });
    connect(&discovery_,&PhoneDiscovery::found,this,[this](const QJsonObject &device) {
        if(!running_ || managerStarted_) return;
        const QUrl endpoint(device.value("socketUri").toString());
        if(endpoint.scheme()!="socket" || QHostAddress(endpoint.host())!=phone_ ||
           endpoint.port()<=0 || endpoint.port()>65535 || !endpoint.userInfo().isEmpty()) {
            fail(QStringLiteral("手机返回的官方服务管理地址无效")); return;
        }
        // Keep the CoAP callback endpoint alive for this session: the phone's
        // physical-connection lookup refers to the discovered local identity.
        managerStarted_=true;
        manager_.start(phone_,quint16(endpoint.port()),device.value("deviceId").toString(),discovery_.localId());
    });
    connect(&manager_,&ServiceManager::connected,this,[this](QString name) {
        peerName_=name.left(80); request();
    });
    connect(&manager_,&ServiceManager::servicePublished,this,[this](const QJsonObject &service) {
        if(service.value("serviceUuid").toString()==CastUuid) request();
    });
    connect(&manager_,&ServiceManager::message,this,&LanProjection::receive);
    connect(&media_,&ProjectionSession::activeChanged,this,[this](bool active) {
        if(!active && running_ && replied_ && !preserveControl_) stop();
        else if(!active && running_) emit status(QStringLiteral("原厂画面已停止，保留协同管理连接等待切换"));
        else if(active && running_) emit status(QStringLiteral("投屏通道已连接，请在手机上确认共享屏幕"));
    });
    connect(&media_,&ProjectionSession::videoPacket,this,[this] {
        if(!running_) return;
        deadline_.stop();
        // Collaboration mode deliberately leaves audio stopped until the PC
        // selects a source. Mode 2 captures system sound, not the microphone.
        if(!audioRequested_) { audioRequested_=true; audioStart_.start(); }
    });
}
LanProjection::~LanProjection() {
    QObject::disconnect(&media_,nullptr,this,nullptr);
    if(running_) stop();
}
bool LanProjection::start(QHostAddress local,QHostAddress phone,quint16 discoveryPort) {
    stop(); local_=local; phone_=phone;
    if(!media_.listenNetwork(local,phone)) return false;
    running_=true; deadline_.start();
    emit status(QStringLiteral("正在发现手机原厂局域网投屏服务"));
    return discovery_.start(local,phone,discoveryPort);
}
bool LanProjection::listen(QHostAddress local,int prefix,QString interfaceName,quint16 discoveryPort) {
    stop(); local_=local;
    if(!listener_.listen(local,0)) { emit status(QStringLiteral("无法监听手机连接：")+listener_.errorString()); return false; }
    incoming_=true; running_=true;
    // This endpoint only advertises smart cast. Collaboration has its own
    // discovery identity and management listener in CollaborationLink.
    const QJsonArray services{QJsonObject{{"serviceName","RMCastService"},{"serviceUUid",CastUuid},{"version",1},{"authType",0},{"custData","{}"}}};
    if(!receiver_.start(local,prefix,interfaceName,listener_.serverPort(),services,discoveryPort)) { stop(); return false; }
    emit status(QStringLiteral("接收已开启，请在手机上选择 Nubia Link Linux")); return true;
}
void LanProjection::stop() {
    running_=false; managerStarted_=false; requested_=false; replied_=false; deadline_.stop();
    audioStart_.stop(); audioRequested_=false;
    incoming_=false; peerName_.clear();
    listener_.close(); receiver_.stop(); discovery_.stop(); manager_.stop(); media_.stop();
}
void LanProjection::fail(QString reason) { stop(); emit status(reason); }
void LanProjection::request() {
    if(!running_ || requested_ || !manager_.ready()) return;
    if(incoming_) {
        requested_=true;
        manager_.send("distributebus.action.publishservice","1",
            {{"serviceName","RMCastService"},{"serviceUuid",CastUuid},{"serviceVersion",1},{"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}});
        return;
    }
    bool available=false;
    for(const auto &service:manager_.services()) {
        if(service.value("serviceUuid").toString()!=CastUuid) continue;
        if(service.value("authType").toInt()!=0) {
            fail(QStringLiteral("手机投屏服务要求额外认证，当前局域网入口尚未适配")); return;
        }
        available=service.value("enable").toBool();
    }
    if(!available) return;
    const QJsonObject publication{{"serviceName","RMCastService"},{"serviceUuid",CastUuid},
        {"serviceVersion",1},{"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}};
    const QJsonObject wifi{{"ip",local_.toString()},{"port",QString::number(media_.port())},
        {"deviceName","Nubia Link Linux"},{"maxFps",60}};
    // InitParam.CONNECTION_NET=2 in the original DistBus API.
    requested_=true;
    if(!manager_.send("distributebus.action.publishservice","1",publication) ||
       !manager_.send("distributebus.action.startservice",CastUuid,{{"connectType",2},{"wifiParam",wifi}})) {
        fail(QStringLiteral("无法发送官方局域网投屏请求")); return;
    }
    emit status(QStringLiteral("已请求原厂局域网投屏，请查看手机上的连接提示"));
}
void LanProjection::receive(const QJsonObject &rpc) {
    if(!running_ || !requested_ || rpc.value("uuid").toString()!=CastUuid) return;
    const auto action=rpc.value("action").toString();
    if(action=="distributebus.action.startservice" && !replied_) {
        const auto doc=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8());
        const auto profile=doc.object();
        if(!doc.isObject() || !profile.contains("appversion")) { fail(QStringLiteral("原厂投屏返回的会话参数无效")); return; }
        if(!profile.value("PIN").isNull() && !profile.value("PIN").isUndefined() && profile.value("PIN")!="") {
            fail(QStringLiteral("手机请求了 PIN 配对，当前入口尚未适配该流程")); return;
        }
        if(incoming_) {
            if(!media_.listenNetwork(local_,phone_)) { fail(QStringLiteral("无法准备投屏媒体监听")); return; }
            emit incomingPeerChanged(phone_);
            deadline_.start();
        }
        const QJsonObject wifi{{"ip",local_.toString()},{"port",QString::number(media_.port())},
            {"deviceName","Nubia Link Linux"},{"maxFps",60}};
        const QJsonObject callback{{"result",true},{"CallBackServiceInParam",json({{"connectType",2},{"wifiParam",wifi}})}};
        replied_=true;
        if(!manager_.send("distributebus.action.onstartservicecallback",CastUuid,callback)) {
            fail(QStringLiteral("无法回应原厂投屏会话")); return;
        }
        emit status(QStringLiteral("原厂投屏会话已回应，等待手机连接"));
    } else if(action=="distributebus.action.stopservice") {
        if(preserveControl_) {
            audioStart_.stop(); media_.stop();
            emit status(QStringLiteral("手机已结束原厂投屏，保留协同管理连接"));
        } else { stop(); emit status(QStringLiteral("手机已停止局域网投屏")); }
    }
}
