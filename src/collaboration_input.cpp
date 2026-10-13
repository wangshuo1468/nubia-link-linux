#include "collaboration_input.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkProxy>

namespace {
QString json(QJsonObject object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
}
CollaborationInput::CollaborationInput(ServiceManager &manager,Kind kind,QObject *parent)
    :QObject(parent),manager_(manager),kind_(kind) {
    server_.setProxy(QNetworkProxy::NoProxy);
    deadline_.setSingleShot(true); heartbeat_.setInterval(10000);
    connect(&manager_,&ServiceManager::message,this,&CollaborationInput::receive);
    connect(&deadline_,&QTimer::timeout,this,[this] {
        resetConnection(); emit status(kind_==Mouse ? QStringLiteral("协同鼠标连接超时") : QStringLiteral("协同输入法连接超时"));
    });
    connect(&heartbeat_,&QTimer::timeout,this,[this] {
        if(socket_) socket_->write(QJsonDocument(QJsonObject{{"type",1},{"id",++id_},{"data",QJsonArray{}}}).toJson(QJsonDocument::Compact));
    });
    connect(&server_,&QTcpServer::newConnection,this,[this] {
        while(server_.hasPendingConnections()) {
            auto socket=server_.nextPendingConnection();
            if(!prepared_ || !started_ || !accepted_ || socket_ || socket->peerAddress()!=phone_) {
                socket->abort(); socket->deleteLater(); continue;
            }
            socket_=socket; reader_.reset(); deadline_.stop(); heartbeat_.start();
            connect(socket_,&QTcpSocket::readyRead,this,&CollaborationInput::read);
            connect(socket_,&QTcpSocket::disconnected,this,[this] { resetConnection(); });
            connect(socket_,&QTcpSocket::errorOccurred,this,[this](auto) { resetConnection(); });
            // This acknowledgment is the original source service's first payload.
            socket_->write(Collaboration::payloadFrame(QJsonDocument(QJsonObject{{"type","MsgConnectAck"},{"result",true}}).toJson(QJsonDocument::Compact),++id_));
            socket_->flush();
            ready_=kind_==Text; emit readyChanged(ready_);
            emit status(kind_==Mouse ? QStringLiteral("协同鼠标通道已连接，等待窗口尺寸") : QStringLiteral("协同输入法通道已连接"));
            if(socket_->bytesAvailable()) read();
        }
    });
}
CollaborationInput::~CollaborationInput() { close(); }
QString CollaborationInput::uuid() const {
    return kind_==Mouse ? "187f8000-0000-1000-8000-ceb574815350" : "187f8000-0000-1000-8000-ceb574815300";
}
bool CollaborationInput::prepare(QHostAddress local,QHostAddress phone) {
    close(); local_=local; phone_=phone;
    if(local.protocol()!=QAbstractSocket::IPv4Protocol || phone.protocol()!=QAbstractSocket::IPv4Protocol) return false;
    prepared_=server_.listen(local,0); return prepared_;
}
bool CollaborationInput::publish() {
    if(!prepared_) return false;
    return manager_.send("distributebus.action.publishservice","1",{
        {"serviceName",kind_==Mouse ? "RemoteInputEventService" : "RemoteInputMethodService"},
        {"serviceUuid",uuid()},{"serviceVersion",3},{"enable",true},{"authType",0},
        {"custData","{}"},{"serviceType","publish"}});
}
void CollaborationInput::resetConnection() {
    ready_=editable_=started_=accepted_=false; deadline_.stop(); heartbeat_.stop(); reader_.reset();
    if(socket_) {
        auto socket=socket_; socket_=nullptr; socket->disconnect(this); socket->abort(); socket->deleteLater();
    }
    emit readyChanged(false); emit editChanged(false);
}
void CollaborationInput::close() { prepared_=false; resetConnection(); server_.close(); id_=0; }
void CollaborationInput::receive(QJsonObject rpc) {
    if(!prepared_ || rpc.value("uuid")!=uuid()) return;
    const auto action=rpc.value("action").toString();
    qInfo().noquote()<<"Collaboration input RPC"<<action<<uuid();
    auto callback=[this](QString action,QString type) {
        return manager_.send(action,uuid(),{{"result",true},{"CallBackServiceInParam",json({
            {"type",type},{"ackId",kind_==Mouse ? "RemoteInputEventService" : "RemoteInputMethodService"},{"result",true}})}});
    };
    if(action=="distributebus.action.startservice") {
        resetConnection(); started_=true;
        if(!callback("distributebus.action.onstartservicecallback","distributebus.action.startservicecallback") ||
            !manager_.send("distributebus.action.sendnotification",uuid(),{{"command",1},{"profile",json({{"type",8}})}})) {
            resetConnection(); return;
        }
        manager_.flush(); deadline_.start(30000);
    } else if(action=="distributebus.action.stopservice") {
        callback("distributebus.action.onstopservicecallback","distributebus.action.stopservicecallback");
        manager_.flush(); resetConnection();
    } else if(action=="distributebus.action.sendnotification" && started_) {
        const auto profile=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
        if(profile.value("command")==2 && !accepted_) {
            accepted_=true;
            if(!manager_.send(action,uuid(),{{"command",3},{"profile",json({{"type",8},
                {"msgTransferProfile",json({{"ip",local_.toString()},{"port",server_.serverPort()}})}})}})) resetConnection();
            manager_.flush();
        } else if(profile.value("command")==4) resetConnection();
    }
}
void CollaborationInput::read() {
    if(!socket_) return;
    QList<QByteArray> payloads;
    if(!reader_.append(socket_->readAll(),payloads)) { resetConnection(); emit status(QStringLiteral("协同输入通道数据无效")); return; }
    for(const auto &payload:payloads) {
        const auto message=QJsonDocument::fromJson(payload).object();
        const auto type=message.value("type").toString();
        if(kind_==Mouse && type=="ScreenInfo") {
            const auto point=message.value("point").toObject();
            const QSize size(point.value("x").toInt(),point.value("y").toInt());
            if(size.width()<=0 || size.height()<=0 || size.width()>16384 || size.height()>16384) continue;
            ready_=true; emit screenChanged(size); emit readyChanged(true); emit status(QStringLiteral("协同鼠标已就绪"));
        } else if(kind_==Text && type=="EditInfo") {
            const bool editable=message.value("canEdit").toBool();
            if(editable==editable_) continue;
            editable_=editable; emit editChanged(editable_);
            emit status(editable_ ? QStringLiteral("手机编辑框已就绪，可输入中文") : QStringLiteral("手机当前没有可编辑的输入框"));
        }
    }
}
bool CollaborationInput::send(QJsonObject payload) {
    if(!ready_ || !socket_ || socket_->state()!=QAbstractSocket::ConnectedState || socket_->bytesToWrite()>256*1024) return false;
    const auto frame=Collaboration::payloadFrame(QJsonDocument(payload).toJson(QJsonDocument::Compact),++id_);
    return !frame.isEmpty() && socket_->write(frame)==frame.size();
}
