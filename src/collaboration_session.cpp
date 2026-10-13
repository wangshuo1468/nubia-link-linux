#include "collaboration_session.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QUrlQuery>
#include <utility>

namespace {
const QString Relay="187f8000-0000-1000-8000-ceb574815500";
const QString Source="187f8000-0000-1000-8000-ceb574815101";
const QString Sink="187f8000-0000-1000-8000-ceb574815100";
QString json(QJsonObject object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
QByteArray nal(const QJsonObject &object,QString key) {
    QByteArray bytes;
    const auto array=object.value(key).toArray();
    if(array.isEmpty() || array.size()>4096) return {};
    for(const auto &value:array) {
        if(!value.isDouble() || value.toDouble()!=value.toInt() || value.toInt()<-128 || value.toInt()>127) return {};
        bytes.append(char(value.toInt()&255));
    }
    if(bytes.startsWith(QByteArray::fromHex("00000001"))) bytes.remove(0,4);
    else if(bytes.startsWith(QByteArray::fromHex("000001"))) bytes.remove(0,3);
    return bytes;
}
}
CollaborationSession::CollaborationSession(QObject *parent):QObject(parent) {
    for(auto channel:{&mouse_,&text_}) {
        connect(channel,&CollaborationInput::status,this,&CollaborationSession::status);
        connect(channel,&CollaborationInput::readyChanged,this,[this] { emit inputChanged(); });
        connect(channel,&CollaborationInput::editChanged,this,[this] { emit inputChanged(); });
    }
    control_.setProxy(QNetworkProxy::NoProxy);
    deadline_.setSingleShot(true); delayed_.setSingleShot(true); heartbeat_.setInterval(10000);
    audioPoll_.setInterval(10);
    connect(&audioPoll_,&QTimer::timeout,this,[this] { const auto pcm=receiver_.takeAudio(); if(!pcm.isEmpty()) audio_.append(pcm); });
    connect(&audio_,&PhoneAudio::warning,this,&CollaborationSession::status);
    connect(&receiver_,&RtpReceiver::audioWarning,this,[this](QString message,quint64 epoch) { if(epoch==receiver_.epoch() && state_==Playing) emit status(message); },Qt::QueuedConnection);
    connect(&deadline_,&QTimer::timeout,this,[this] {
        if(state_==Stopping) { emit status(QStringLiteral("断开已发送，未收到手机确认")); finish(); }
        else fail(QStringLiteral("多屏协同连接超时，请检查手机上的协同设备关联和授权"));
    });
    connect(&delayed_,&QTimer::timeout,this,&CollaborationSession::requestWindow);
    connect(&heartbeat_,&QTimer::timeout,this,[this] {
        if(control_.state()==QAbstractSocket::ConnectedState) {
            control_.write(QJsonDocument(QJsonObject{{"type",1},{"id",++messageId_},{"data",QJsonArray{}}}).toJson(QJsonDocument::Compact));
            control_.flush();
        }
    });
    auto &manager=link_.manager();
    connect(&manager,&ServiceManager::message,this,&CollaborationSession::receive);
    connect(&link_,&CollaborationLink::error,this,&CollaborationSession::fail);
    connect(&link_,&CollaborationLink::status,this,&CollaborationSession::status);
    connect(&link_,&CollaborationLink::ready,this,[this](QHostAddress local,QHostAddress phone,QString name) {
        if(state_!=Discovering) return;
        local_=local; phone_=phone; peerName_=name;
        if(!mouse_.prepare(local,phone) || !text_.prepare(local,phone)) { fail(QStringLiteral("无法准备协同输入端口")); return; }
        if(incoming_) emit incomingPeerChanged(local,phone);
        publish();
    });
    connect(&manager,&ServiceManager::servicePublished,this,[this] {
        if(state_==Associating && associated_ && !offered_ && !delayed_.isActive()) delayed_.start(1500);
    });
    connect(&link_,&CollaborationLink::disconnected,this,[this] {
        if(state_==Idle) return;
        if(state_!=Stopping) emit status(QStringLiteral("手机已断开多屏协同连接"));
        finish();
    });
    connect(&control_,&QTcpSocket::connected,this,[this] {
        if(state_!=Transport) return;
        if(!link_.manager().send("distributebus.action.sendnotification",Sink,
            {{"command",5},{"profile",json({{"type",2},{"streamTransferProfile","0"}})}})) {
            fail(QStringLiteral("无法确认多屏协同传输")); return;
        }
        heartbeat_.start();
        const auto generation=generation_;
        QTimer::singleShot(500,this,[this,generation] {
            if(generation!=generation_ || state_!=Transport) return;
            command("play",{{"width",1080},{"height",2400}});
            deadline_.start(30000);
            emit status(QStringLiteral("正在接收多屏协同画面"));
        });
    });
    connect(&control_,&QTcpSocket::readyRead,this,&CollaborationSession::readControl);
    connect(&control_,&QTcpSocket::disconnected,this,[this] {
        if(state_==Transport || state_==Playing) fail(QStringLiteral("多屏协同控制通道已断开"));
    });
    connect(&control_,&QTcpSocket::errorOccurred,this,[this](auto) {
        if(state_==Transport || state_==Playing) fail(QStringLiteral("多屏协同控制连接失败：")+control_.errorString());
    });
    connect(&receiver_,&RtpReceiver::listening,this,[this](quint64 epoch) {
        if(epoch!=receiver_.epoch() || state_!=Transport) return;
        state_=Playing; command("request_key_frame"); audio_.start(); audioPoll_.start();
        emit activeChanged(true); emit status(QStringLiteral("多屏协同已连接，等待首帧"));
    },Qt::QueuedConnection);
    connect(&receiver_,&RtpReceiver::firstFrame,this,[this](quint64 epoch) {
        if(epoch!=receiver_.epoch() || state_!=Playing) return;
        deadline_.stop();
        if(replacing_) {
            replacing_=false;
            // The first play creates the display asynchronously. Rebind the
            // existing native mouse/IME services once that display exists.
            command("play",{{"width",1080},{"height",2400}});
        }
    },Qt::QueuedConnection);
    connect(&receiver_,&RtpReceiver::keyFrameNeeded,this,[this](quint64 epoch) { if(epoch==receiver_.epoch() && state_==Playing) command("request_key_frame"); },Qt::QueuedConnection);
    connect(&receiver_,&RtpReceiver::error,this,[this](QString message,quint64 epoch) { if(epoch==receiver_.epoch() && state_!=Idle && state_!=Stopping) fail(message); },Qt::QueuedConnection);
}
CollaborationSession::~CollaborationSession() {
    QObject::disconnect(&link_,nullptr,this,nullptr);
    QObject::disconnect(&link_.manager(),nullptr,this,nullptr);
    QObject::disconnect(&control_,nullptr,this,nullptr);
    QObject::disconnect(&receiver_,nullptr,this,nullptr);
    stop(); finish();
}
bool CollaborationSession::start(QHostAddress local,QHostAddress phone,QString package) {
    if(running()) return false;
    static const QRegularExpression pattern("^[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+$");
    if(local.protocol()!=QAbstractSocket::IPv4Protocol || phone.protocol()!=QAbstractSocket::IPv4Protocol ||
       package.size()>255 || !pattern.match(package).hasMatch()) {
        emit status(QStringLiteral("需要有效的本机与手机 IPv4 地址，以及应用包名")); return false;
    }
    ++generation_; incoming_=false; local_=local; phone_=phone; package_=package; state_=Discovering;
    if(!mouse_.prepare(local,phone) || !text_.prepare(local,phone)) {
        finish(); emit status(QStringLiteral("无法准备多屏协同输入端口")); return false;
    }
    associated_=offered_=transportReceived_=false; port_=0; messageId_=0; reader_.reset();
    deadline_.start(120000);
    if(!link_.start(local,phone)) { finish(); return false; }
    return true;
}
bool CollaborationSession::listen(QHostAddress local,int prefix,QString interfaceName,quint16 discoveryPort) {
    if(running()) return false;
    ++generation_; incoming_=true; local_=local; package_="com.android.settings";
    associated_=offered_=transportReceived_=false; port_=0; messageId_=0; reader_.reset(); state_=Discovering;
    if(!link_.listen(local,prefix,interfaceName,discoveryPort)) { finish(); return false; }
    return true;
}
void CollaborationSession::publish() {
    auto &manager=link_.manager();
    state_=Associating;
    if(!mouse_.publish() || !text_.publish()) { fail(QStringLiteral("无法发布协同输入服务")); return; }
    for(const auto &entry:{qMakePair(Sink,9),qMakePair(Relay,22)}) {
        if(!manager.send("distributebus.action.publishservice","1",
            {{"serviceName",entry.first==Sink ? "MultiScrService" : "RELAY_SERVICE"},{"serviceUuid",entry.first},
             {"serviceVersion",entry.second},{"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}})) {
            fail(QStringLiteral("无法发布多屏协同接收服务")); return;
        }
    }
    if(!incoming_) deadline_.start(45000);
    emit status(QStringLiteral("协同服务已公布，等待手机建立协同设备关联"));
    // RFCOMM is ready before the phone's WLAN identity may arrive. The ROM
    // can send its first offer in that interval; preserve it until ports and
    // the WLAN identity are ready rather than losing the connection request.
    const auto early=std::exchange(earlyRequests_,{}); earlyBytes_=0;
    const auto generation=generation_;
    for(const auto &rpc:early) {
        if(generation!=generation_ || state_==Idle) break;
        receive(rpc);
    }
}
void CollaborationSession::requestWindow() {
    if(state_!=Associating || !associated_) return;
    bool available=false;
    for(const auto &service:link_.manager().services()) {
        if(service.value("serviceUuid")==Source)
            available=service.value("enable").toBool() && service.value("authType").toInt()==0;
    }
    if(!available) { emit status(QStringLiteral("等待手机启用协同应用服务")); return; }
    state_=Offering;
    if(!link_.manager().send("distributebus.action.startservice",Source,
        {{"castDisplayType","virtualDisplay"},{"pkg",package_}})) { fail(QStringLiteral("无法请求应用窗口")); return; }
    deadline_.start(120000);
    emit status(QStringLiteral("正在请求多屏协同应用画面，请查看手机授权提示"));
}
bool CollaborationSession::requestApplication(QString package) {
    static const QRegularExpression pattern("^[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+$");
    if(!applicationReady() || !link_.manager().ready() || package.size()>255 || !pattern.match(package).hasMatch()) return false;
    bool available=false;
    for(const auto &service:link_.manager().services()) if(service.value("serviceUuid")==Source)
        available=service.value("enable").toBool() && service.value("authType").toInt()==0;
    if(!available) return false;
    // Replace media within the existing collaboration identity. Do not end
    // AppRelay or start RMCast. Delayed work from the old transport expires.
    ++generation_; state_=Offering; offered_=false; replacing_=true;
    deadline_.stop(); delayed_.stop(); heartbeat_.stop(); audioPoll_.stop();
    audio_.stop();
    package_=std::move(package);
    emit activeChanged(false); emit inputChanged();
    if(!link_.manager().send("distributebus.action.startservice",Source,{{"castDisplayType","virtualDisplay"},{"pkg",package_}})) {
        fail(QStringLiteral("无法发送独立应用请求")); return false;
    }
    link_.manager().flush(); deadline_.start(30000);
    emit status(QStringLiteral("正在切换到独立应用画面")); return true;
}
void CollaborationSession::receive(const QJsonObject &rpc) {
    if(state_==Idle) return;
    if(state_==Discovering) {
        const auto bytes=QJsonDocument(rpc).toJson(QJsonDocument::Compact).size();
        if(!link_.manager().ready() || earlyRequests_.size()>=32 || earlyBytes_+bytes>1024*1024) {
            fail(QStringLiteral("协同在设备发现完成前发送了无效或过多消息")); return;
        }
        earlyRequests_.append(rpc); earlyBytes_+=bytes; return;
    }
    auto &manager=link_.manager();
    const auto action=rpc.value("action").toString(),uuid=rpc.value("uuid").toString();
    if(uuid==Relay || uuid==Sink || uuid==Source)
        qInfo().noquote()<<"Collaboration RPC"<<action<<uuid<<"state"<<int(state_);
    const auto text=rpc.value("profile").toString();
    const auto profile=QJsonDocument::fromJson(text.toUtf8()).object();
    if(uuid==Relay) {
        if(state_!=Stopping && action=="distributebus.action.startservice" && !associated_) {
            const auto fields=text.split(":::");
            if(fields.size()!=3 || fields[0]!="connect_check" || fields[2].isEmpty()) return;
            if(!manager.send("distributebus.action.onstartservicecallback",Relay,
                {{"result",true},{"CallBackServiceInParam","ready_to_connect"}})) { fail(QStringLiteral("无法回应手机的关联请求")); return; }
            associated_=true; manager.flush();
            emit inputChanged();
            if(!offered_) { delayed_.start(1500); deadline_.start(120000); }
        } else if(state_==Stopping && action=="distributebus.action.onstopservicecallback" &&
            profile.value("result").toBool() && profile.value("CallBackServiceInParam")=="disconnect") {
            emit status(QStringLiteral("多屏协同已正常断开"));
            const auto generation=generation_;
            QTimer::singleShot(500,this,[this,generation] { if(generation==generation_ && state_==Stopping) finish(); });
        } else if(action=="distributebus.action.stopservice" && text=="disconnect") {
            manager.send("distributebus.action.onstopservicecallback",Relay,{{"result",true},{"CallBackServiceInParam","disconnect"}});
            manager.flush(); associated_=false; stop();
        }
        return;
    }
    if(state_==Stopping) return;
    if(uuid==Source && action=="distributebus.action.onstartservicecallback" && !profile.value("result").toBool()) {
        fail(QStringLiteral("手机拒绝了应用窗口请求")); return;
    }
    if(uuid!=Sink) return;
    if(action=="distributebus.action.stopservice") {
        if(offered_) stop();
        return;
    }
    if((state_==Associating || state_==Offering) && action=="distributebus.action.startservice" && !offered_) {
        acceptOffer(rpc); return;
    }
    if(state_!=Transport || action!="distributebus.action.sendnotification") return;
    const int notification=profile.value("command").toInt();
    const auto parameters=QJsonDocument::fromJson(profile.value("profile").toString().toUtf8()).object();
    if(notification==1) {
        if(parameters.value("type").toInt()!=10) { fail(QStringLiteral("手机请求了尚未适配的传输类型")); return; }
        if(!manager.send(action,Sink,{{"command",2},{"profile",json({{"v",2},{"fillWifiParam",1}})}})) fail(QStringLiteral("无法回应传输请求"));
    } else if(notification==3 && !transportReceived_) {
        const auto endpoint=QJsonDocument::fromJson(parameters.value("msgTransferProfile").toString().toUtf8()).object();
        const QUrlQuery query(parameters.value("streamTransferProfile").toString());
        qInfo()<<"Collaboration transport profile"<<parameters.value("streamTransferProfile").toString()<<"control"<<endpoint.value("port");
        bool valid=false;
        const int port=query.queryItemValue("port").toInt(&valid),controlPort=endpoint.value("port").toInt();
        if(QHostAddress(endpoint.value("ip").toString())!=phone_ || QHostAddress(query.queryItemValue("ip"))!=phone_ ||
           !valid || port<1024 || port>65532 || controlPort<1 || controlPort>65535 || query.hasQueryItem("tcp")) {
            manager.send(action,Sink,{{"command",4},{"profile",""}}); fail(QStringLiteral("手机返回的媒体地址不受支持")); return;
        }
        port_=quint16(port); transportReceived_=true;
        // Reserve the verified four UDP ports before acknowledging ZIME.
        // Otherwise the first packet can generate ICMP port-unreachable and
        // cause the phone to terminate before it supplies SPS/PPS.
        for(int offset=0;offset<4;++offset) {
            auto socket=new QUdpSocket(this); socket->setProxy(QNetworkProxy::NoProxy); mediaSockets_.append(socket);
            if(!socket->bind(local_,port_+offset,QUdpSocket::DontShareAddress)) {
                fail(QStringLiteral("多屏协同媒体端口被占用：")+socket->errorString()); return;
            }
            connect(socket,&QUdpSocket::readyRead,this,[this,socket,offset,reported=false]() mutable {
                while(socket->hasPendingDatagrams()) {
                    const auto packet=socket->receiveDatagram(); const auto bytes=packet.data();
                    if(!reported && packet.senderAddress()==phone_ && bytes.size()>=12 && (quint8(bytes[0])>>6)==2) {
                        qInfo()<<"Collaboration UDP port offset"<<offset<<"RTP type"<<(quint8(bytes[1])&127)<<"size"<<bytes.size(); reported=true;
                    }
                }
            });
        }
        control_.connectToHost(phone_,quint16(controlPort));
    }
}
void CollaborationSession::acceptOffer(const QJsonObject &rpc) {
    if(offered_ || (state_!=Associating && state_!=Offering)) return;
    const auto profile=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
    const auto capabilities=QJsonDocument::fromJson(profile.value("profile").toString().toUtf8()).object();
    const bool requested=state_==Offering;
    if((profile.value("castDisplayType")!="virtualDisplay" && !(incoming_ && !requested && profile.value("castDisplayType")=="mirror")) ||
       ((requested || !incoming_) && profile.value("pkg").isString() && profile.value("pkg")!=package_) ||
       (capabilities.value("abilities").toInt()&2)==0) {
        link_.manager().send("distributebus.action.onstartservicecallback",Sink,{{"result",false},{"CallBackServiceInParam",json({{"resultCode",2}})}});
        fail(QStringLiteral("手机的协同媒体格式尚不支持")); return;
    }
    if(incoming_ && profile.value("pkg").isString()) package_=profile.value("pkg").toString().left(255);
    if(!replacing_) { control_.abort(); reader_.reset(); }
    else {
        receiver_.stop();
        for(int offset=0;offset<mediaSockets_.size();++offset) {
            if(!mediaSockets_[offset]->bind(local_,port_+offset,QUdpSocket::DontShareAddress)) {
                fail(QStringLiteral("无法保留应用切换的媒体端口")); return;
            }
        }
    }
    displayType_=profile.value("castDisplayType").toString();
    offered_=true; state_=Transport; delayed_.stop();
    const QJsonObject response{{"deviceName","Nubia Link Linux"},{"deviceModel",QString::number(int(DesktopRole::Collaboration))},
        {"deviceWidth",1080},{"deviceHeight",2400},{"densityDpi",0},{"resultCode",0},
        {"isCasting",replacing_},{"castServiceVersion",9},{"pkg",package_},
        {"profile",json({{"type",2},{"abilities",2},{"fps",30},{"chNum",2},{"isEncoder",false}})}};
    if(!link_.manager().send("distributebus.action.onstartservicecallback",Sink,{{"result",true},{"CallBackServiceInParam",json(response)}})) {
        fail(QStringLiteral("无法协商协同媒体能力")); return;
    }
    link_.manager().flush(); deadline_.start(30000);
    if(replacing_) {
        heartbeat_.start();
        const auto generation=generation_;
        QTimer::singleShot(500,this,[this,generation] {
            if(generation==generation_ && state_==Transport && replacing_)
                command("play",{{"width",1080},{"height",2400}});
        });
        emit status(QStringLiteral("正在复用协同传输接收独立应用"));
    }
}
void CollaborationSession::readControl() {
    if(state_==Idle) { control_.readAll(); return; }
    QList<QByteArray> payloads;
    if(!reader_.append(control_.readAll(),payloads)) { fail(reader_.error()); return; }
    for(const auto &payload:payloads) {
        const auto message=QJsonDocument::fromJson(payload).object();
        qInfo().noquote()<<"Collaboration control"<<message.value("action").toString().left(80);
        if(message.value("action")=="encode_sps_pps" && state_==Transport && transportReceived_) {
            const auto config=QJsonDocument::fromJson(message.value("extra").toString().toUtf8()).object();
            if(receiver_.isRunning()) continue;
            for(auto socket:mediaSockets_) socket->close();
            // This ROM's StreamParamConfig selects stereo 48kHz Opus. Actual
            // RTP payload 112 on base/base+1 was verified with a known tone.
            if(!receiver_.begin(local_,phone_,port_+2,nal(config,"sps"),nal(config,"pps"),112)) {
                fail(QStringLiteral("手机返回的 H.264 配置无效")); return;
            }
        }
    }
}
void CollaborationSession::command(QString action,QJsonObject extra) {
    if(control_.state()!=QAbstractSocket::ConnectedState || control_.bytesToWrite()>1024*1024) return;
    control_.write(Collaboration::controlFrame(action,extra,++messageId_)); control_.flush();
}
void CollaborationSession::key(int code) {
    if(state_!=Playing || code<0 || code>300) return;
    const auto payload=QJsonDocument(QJsonObject{{"action","send_key_code"},{"id",++messageId_},
        {"ackId",-1},{"extra",QString::number(code)}}).toJson(QJsonDocument::Compact);
    control_.write(Collaboration::payloadFrame(payload,messageId_)); control_.flush();
}
void CollaborationSession::pointer(int action,QPointF position,QSize display,int buttons) {
    if(!mouseReady() || action<0 || action>2 || display.isEmpty()) return;
    position.setX(qBound(0.0,position.x(),double(display.width()-1)));
    position.setY(qBound(0.0,position.y(),double(display.height()-1)));
    mouse_.send({{"type","MotionEventInfo"},{"action",action},{"pointerId",-1},
        {"sourceX",position.x()},{"sourceY",position.y()},{"displayW",display.width()},
        {"displayH",display.height()},{"pressure",action==1 ? 0.0 : 1.0},{"buttons",buttons}});
}
void CollaborationSession::scroll(QPointF position,QSize display,QPointF delta) {
    if(!mouseReady() || display.isEmpty()) return;
    mouse_.send({{"type","MotionEventScroll"},{"sourceX",position.x()},{"sourceY",position.y()},
        {"displayW",display.width()},{"displayH",display.height()},{"buttons",0},
        {"hScroll",delta.x()},{"vScroll",delta.y()}});
}
bool CollaborationSession::commitText(QString text) {
    return editable() && !text.isEmpty() && text.size()<=65536 && text_.send({{"type","InputTextInfo"},{"inputContent",text}});
}
void CollaborationSession::stop() {
    if(state_==Idle || state_==Stopping) return;
    command("stop"); state_=Stopping; delayed_.stop(); heartbeat_.stop(); deadline_.stop();
    mouse_.close(); text_.close(); emit inputChanged();
    audioPoll_.stop(); audio_.stop();
    receiver_.stop(); emit activeChanged(false);
    if(associated_ && link_.manager().ready() &&
       link_.manager().sendProfile("distributebus.action.stopservice",Relay,"disconnect")) {
        link_.manager().flush(); deadline_.start(8000); emit status(QStringLiteral("正在结束多屏协同"));
    } else finish();
}
void CollaborationSession::fail(QString message) { stop(); emit status(message); }
void CollaborationSession::finish() {
    if(state_==Idle) return;
    state_=Idle; ++generation_; deadline_.stop(); delayed_.stop(); heartbeat_.stop();
    control_.abort(); receiver_.stop(); link_.stop(); reader_.reset();
    audioPoll_.stop(); audio_.stop();
    mouse_.close(); text_.close();
    earlyRequests_.clear(); earlyBytes_=0;
    displayType_.clear();
    replacing_=false;
    qDeleteAll(mediaSockets_); mediaSockets_.clear();
    emit activeChanged(false); emit stopped();
}
