#include "service_manager.h"
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QtEndian>
extern "C" {
#include <libavutil/aes_ctr.h>
}
#include <QStringDecoder>
#include <utility>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace {
// Original DistBus AES-128/CTR profile codec. These are protocol constants,
// recovered from the phone SDK's AuthKeyUtil, not an account credential.
QByteArray cryptProfile(QByteArrayView input) {
    auto context=av_aes_ctr_alloc();
    if(!context) return {};
    const auto key=QByteArrayLiteral("7MJhgBmzxxs3gMCk"), iv=QByteArrayLiteral("66WywGXTNG4GA8EP");
    if(av_aes_ctr_init(context,reinterpret_cast<const uint8_t *>(key.constData()))<0) {
        av_aes_ctr_free(context); return {};
    }
    // Java creates a new cipher for each profile, resetting the full counter.
    av_aes_ctr_set_full_iv(context,reinterpret_cast<const uint8_t *>(iv.constData()));
    QByteArray result(input.size(),Qt::Uninitialized);
    av_aes_ctr_crypt(context,reinterpret_cast<uint8_t *>(result.data()),
        reinterpret_cast<const uint8_t *>(input.data()),int(input.size()));
    av_aes_ctr_free(context); return result;
}
}

QByteArray DistBus::frame(const QJsonObject &message) {
    const auto json=QJsonDocument(message).toJson(QJsonDocument::Compact);
    if(json.size()>MaxRpc) return {};
    QByteArray bytes(6,Qt::Uninitialized);
    qToBigEndian<quint16>(1,bytes.data());
    qToBigEndian<quint32>(quint32(json.size()),bytes.data()+2);
    return bytes+json;
}
bool DistBus::RpcReader::append(QByteArrayView bytes,QList<QJsonObject> &messages) {
    if(!error_.isEmpty()) return false;
    // Consume at most one bounded frame at a time, including for a large read
    // containing many complete frames. Never reserve an advertised length.
    while(!bytes.isEmpty()) {
        const qsizetype target=buffer_.size()<6 ? 6 : 6+qsizetype(qFromBigEndian<quint32>(buffer_.constData()+2));
        const auto take=qMin(bytes.size(),target-buffer_.size());
        buffer_.append(bytes.data(),take); bytes=bytes.sliced(take);
        if(buffer_.size()<6) continue;
        if(qFromBigEndian<quint16>(buffer_.constData())!=1) { error_="Unsupported service-manager frame version"; return false; }
        const auto length=qFromBigEndian<quint32>(buffer_.constData()+2);
        if(!length || length>MaxRpc) { error_="Invalid service-manager frame length"; return false; }
        if(buffer_.size()!=6+qsizetype(length)) continue;
        const auto doc=QJsonDocument::fromJson(buffer_.mid(6));
        const auto object=doc.object();
        if(!doc.isObject() || object.value("action").toString().isEmpty() ||
           (!object.value("profile").isUndefined() && !object.value("profile").isString())) {
            error_="Invalid service-manager RPC"; return false;
        }
        messages.append(object); buffer_.clear();
    }
    return true;
}

ServiceManager::ServiceManager(QObject *parent):QObject(parent) {
    timeout_.setSingleShot(true); timeout_.setInterval(8000);
    connect(&timeout_,&QTimer::timeout,this,[this] { fail(QStringLiteral("官方服务管理握手超时")); });
    socket_=nullptr; attach(new QTcpSocket(this));
}
bool ServiceManager::configureSocket() {
    socket_->setSocketOption(QAbstractSocket::KeepAliveOption,1);
#ifdef Q_OS_LINUX
    const int inlineUrgent=0;
    if(::setsockopt(int(socket_->socketDescriptor()),SOL_SOCKET,SO_OOBINLINE,&inlineUrgent,sizeof(inlineUrgent))!=0) {
        fail(QStringLiteral("无法配置官方服务管理带外心跳")); return false;
    }
#endif
    return true;
}
bool ServiceManager::hello(QString uuid) {
    QJsonObject profile{{"a",localId_},{"b","Nubia Link Linux"},{"c",int(role_)},{"d",""},{"e",rfcomm_>=0 ? 1 : 0},{"p",encrypted_ ? 1 : 0}};
    if(rfcomm_>=0) profile["f"]=bluetoothAddress_;
    return write({{"action","connectDevice"},{"uuid",uuid},
        {"profile",QString::fromUtf8(QJsonDocument(profile).toJson(QJsonDocument::Compact))}});
}
void ServiceManager::attach(QTcpSocket *socket) {
    if(socket_) { socket_->disconnect(this); socket_->abort(); socket_->deleteLater(); }
    socket_=socket; socket_->setParent(this);
    socket_->setProxy(QNetworkProxy::NoProxy); socket_->setReadBufferSize(DistBus::MaxRpc+6);
    connect(socket_,&QTcpSocket::connected,this,[this] {
        if(configureSocket() && !incoming_) hello("requestService");
    });
    connect(socket_,&QTcpSocket::readyRead,this,[this] {
        receiveBytes(socket_->readAll());
    });
    connect(socket_,&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError code) {
        // Qt reports graceful peer EOF as an error before disconnected().
        if(running_ && code!=QAbstractSocket::RemoteHostClosedError) fail(socket_->errorString());
    });
    connect(socket_,&QTcpSocket::disconnected,this,[this] {
        if(!running_) return;
        stop(); emit disconnected();
    });
}
void ServiceManager::receiveBytes(QByteArray bytes) {
    QList<QJsonObject> messages;
    const auto generation=generation_;
    if(!reader_.append(bytes,messages)) { fail(reader_.error()); return; }
    for(const auto &rpc:messages) {
        if(!running_ || generation!=generation_) break;
        receive(rpc);
    }
}
bool ServiceManager::acceptRfcomm(int descriptor,QString phoneId,QString localId,QHostAddress local,QString bluetoothAddress,bool incoming) {
    stop();
    if(descriptor<0 || phoneId.isEmpty() || phoneId.size()>256 || localId.isEmpty() || localId.size()>256 ||
       local.protocol()!=QAbstractSocket::IPv4Protocol || bluetoothAddress.isEmpty()) {
        if(descriptor>=0) ::close(descriptor);
        return false;
    }
    const int flags=::fcntl(descriptor,F_GETFL);
    if(flags<0 || ::fcntl(descriptor,F_SETFL,flags|O_NONBLOCK)<0) { ::close(descriptor); return false; }
    ::fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    rfcomm_=descriptor; bluetoothLocal_=local; bluetoothAddress_=std::move(bluetoothAddress);
    incoming_=incoming; role_=DesktopRole::Collaboration; phoneId_=std::move(phoneId); localId_=std::move(localId); running_=true;
    rfcommRead_=new QSocketNotifier(descriptor,QSocketNotifier::Read,this);
    rfcommWrite_=new QSocketNotifier(descriptor,QSocketNotifier::Write,this); rfcommWrite_->setEnabled(false);
    connect(rfcommRead_,&QSocketNotifier::activated,this,[this] { readRfcomm(); });
    connect(rfcommWrite_,&QSocketNotifier::activated,this,[this] { drainRfcomm(); });
    timeout_.start();
    if(!incoming_ && !hello("requestService")) { fail(QStringLiteral("无法发起协同蓝牙管理握手")); return false; }
    const auto generation=generation_;
    QTimer::singleShot(0,this,[this,generation] { if(generation==generation_) readRfcomm(); });
    return true;
}
void ServiceManager::readRfcomm() {
    char bytes[4096];
    while(running_ && rfcomm_>=0) {
        const auto count=::recv(rfcomm_,bytes,sizeof(bytes),0);
        if(count>0) { receiveBytes(QByteArray(bytes,count)); continue; }
        if(count<0 && errno==EINTR) continue;
        if(count<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return;
        if(count==0) { stop(); emit disconnected(); }
        else fail(QStringLiteral("协同蓝牙管理连接读取失败"));
        return;
    }
}
void ServiceManager::drainRfcomm() {
    while(running_ && rfcomm_>=0 && !rfcommQueue_.isEmpty()) {
        const auto count=::send(rfcomm_,rfcommQueue_.constData(),size_t(rfcommQueue_.size()),MSG_NOSIGNAL);
        if(count>0) { rfcommQueue_.remove(0,count); continue; }
        if(count<0 && errno==EINTR) continue;
        if(count<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) { rfcommWrite_->setEnabled(true); return; }
        fail(QStringLiteral("协同蓝牙管理连接发送失败")); return;
    }
    if(rfcommWrite_) rfcommWrite_->setEnabled(false);
}
void ServiceManager::flush() { if(rfcomm_>=0) drainRfcomm(); else socket_->flush(); }
bool ServiceManager::start(QHostAddress phone,quint16 port,QString phoneId,QString localId,DesktopRole role) {
    stop();
    if(phone.protocol()!=QAbstractSocket::IPv4Protocol || !phone.toIPv4Address() || phone.isMulticast() ||
       phone==QHostAddress::Broadcast || !port || phoneId.isEmpty() || localId.isEmpty() ||
       phoneId.size()>256 || localId.size()>256) {
        emit error(QStringLiteral("官方服务管理需要有效的发现地址和设备标识")); return false;
    }
    incoming_=false; role_=role; phoneId_=std::move(phoneId); localId_=std::move(localId); running_=true;
    timeout_.start(); socket_->connectToHost(phone,port); return true;
}
bool ServiceManager::accept(QTcpSocket *socket,QString phoneId,QString localId,DesktopRole role) {
    stop();
    if(!socket || socket->state()!=QAbstractSocket::ConnectedState || phoneId.isEmpty() || localId.isEmpty() || phoneId.size()>256 || localId.size()>256) {
        if(socket) { socket->abort(); socket->deleteLater(); }
        return false;
    }
    incoming_=true; role_=role; phoneId_=std::move(phoneId); localId_=std::move(localId); running_=true;
    attach(socket); timeout_.start(); if(!configureSocket()) return false;
    if(socket_->bytesAvailable()) QMetaObject::invokeMethod(socket_,"readyRead",Qt::QueuedConnection);
    return true;
}
ServiceManager::~ServiceManager() { stop(); }
void ServiceManager::stop() {
    ++generation_; running_=false; ready_=false; timeout_.stop(); socket_->abort();
    for(auto notifier:{rfcommRead_,rfcommWrite_}) if(notifier) { notifier->setEnabled(false); notifier->deleteLater(); }
    rfcommRead_=rfcommWrite_=nullptr;
    if(rfcomm_>=0) ::close(rfcomm_);
    rfcomm_=-1; rfcommQueue_.clear(); bluetoothLocal_.clear(); bluetoothAddress_.clear();
    services_.clear(); pendingMessages_.clear(); pendingBytes_=0; encrypted_=false; reader_.reset(); phoneId_.clear(); localId_.clear();
}
void ServiceManager::fail(QString reason) { stop(); emit error(reason); }
bool ServiceManager::write(const QJsonObject &rpc) {
    auto wire=rpc;
    if(encrypted_ && wire.value("action")!="connectDevice" && !wire.value("profile").toString().isEmpty()) {
        const auto plain=wire.value("profile").toString().toUtf8();
        if(plain.size()>DistBus::MaxRpc) return false;
        const auto cipher=cryptProfile(plain);
        if(cipher.isEmpty()) { fail(QStringLiteral("无法加密官方服务消息")); return false; }
        wire["profile"]=QString::fromLatin1(cipher.toBase64());
    }
    const auto bytes=DistBus::frame(wire);
    if(rfcomm_>=0) {
        if(bytes.isEmpty() || !running_) return false;
        if(rfcommQueue_.size()+bytes.size()>DistBus::MaxRpc*2) { fail(QStringLiteral("协同蓝牙管理发送队列已满")); return false; }
        rfcommQueue_+=bytes; drainRfcomm(); return running_;
    }
    if(bytes.isEmpty() || socket_->state()!=QAbstractSocket::ConnectedState) return false;
    if(socket_->bytesToWrite()+bytes.size()>DistBus::MaxRpc*2) { fail(QStringLiteral("官方服务管理发送队列已满")); return false; }
    if(socket_->write(bytes)!=bytes.size()) { fail(QStringLiteral("官方服务管理发送失败")); return false; }
    return true;
}
bool ServiceManager::send(QString action,QString uuid,const QJsonObject &profile) {
    return sendProfile(std::move(action),std::move(uuid),
        QString::fromUtf8(QJsonDocument(profile).toJson(QJsonDocument::Compact)));
}
bool ServiceManager::sendProfile(QString action,QString uuid,QString profile) {
    if(!ready_ || action.isEmpty() || uuid.isEmpty() || action=="connectDevice") return false;
    // Official NetClient omits deviceId on wire; the peer derives it from the
    // connected socket identity. Do not insert a target device ID here.
    return write({{"action",action},{"uuid",uuid},{"profile",profile}});
}
void ServiceManager::receive(const QJsonObject &received) {
    auto rpc=received;
    const auto action=rpc.value("action").toString();
    if(action=="connectDevice") {
        const auto doc=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8());
        const auto profile=doc.object();
        if(ready_ || !doc.isObject() || profile.value("a").toString()!=phoneId_ || !profile.value("c").isDouble()) {
            fail(QStringLiteral("官方服务管理返回的设备身份不匹配")); return;
        }
        if(!profile.value("p").isUndefined() && (!profile.value("p").isDouble() || (profile.value("p").toDouble()!=0 && profile.value("p").toDouble()!=1))) {
            fail(QStringLiteral("手机返回了不支持的服务加密版本")); return;
        }
        encrypted_=profile.value("p").toInt()==1;
        const auto generation=generation_;
        if(incoming_ && !hello("requestService")) { fail(QStringLiteral("无法回应手机服务管理握手")); return; }
        ready_=true; timeout_.stop(); emit connected(profile.value("b").toString());
        if(!running_ || generation!=generation_) return;
        if(rfcomm_>=0) write({{"action","cmdSendIp"},{"profile",QString::fromUtf8(QJsonDocument(QJsonObject{{"srcIps",bluetoothLocal_.toString()}}).toJson(QJsonDocument::Compact))}});
        // Some ROMs publish before hello. Decode only after negotiation.
        const auto queued=std::exchange(pendingMessages_,{}); pendingBytes_=0;
        for(const auto &message:queued) { if(!running_ || generation!=generation_) break; receive(message); }
        return;
    }
    if(!ready_) {
        if(action!="distributebus.action.publishservice" || pendingMessages_.size()>=64 ||
           (pendingBytes_+=rpc.value("profile").toString().toUtf8().size())>DistBus::MaxRpc) {
            fail(QStringLiteral("官方服务管理在握手前发送了无效或过多消息")); return;
        }
        pendingMessages_.append(rpc); return;
    }
    if(encrypted_ && !rpc.value("profile").toString().isEmpty()) {
        const auto decoded=QByteArray::fromBase64Encoding(rpc.value("profile").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!decoded || decoded.decoded.isEmpty()) { fail(QStringLiteral("官方加密服务消息无效")); return; }
        const auto plain=cryptProfile(decoded.decoded);
        QStringDecoder utf8(QStringDecoder::Utf8); const auto text=utf8(plain);
        if(plain.isEmpty() || utf8.hasError()) { fail(QStringLiteral("无法解密官方服务消息")); return; }
        rpc["profile"]=QString(text);
    }
    if(action=="distributebus.action.publishservice") {
        const auto doc=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8());
        const auto service=doc.object(); const auto uuid=service.value("serviceUuid").toString();
        if(!doc.isObject() || uuid.isEmpty() || uuid.size()>128 ||
           (!services_.contains(uuid) && services_.size()>=64)) { fail(QStringLiteral("官方服务描述无效或过多")); return; }
        services_[uuid]=service;
        if(ready_) emit servicePublished(service);
        return;
    }
    if(!ready_) { fail(QStringLiteral("官方服务管理在握手前发送了会话指令")); return; }
    emit message(rpc);
}
