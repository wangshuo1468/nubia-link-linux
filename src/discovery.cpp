#include "discovery.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUdpSocket>
#include <QNetworkProxy>
#include <QUuid>
#include <QSettings>
#include <cstring>

namespace {
coap_address_t address(QHostAddress ip, quint16 port) {
    coap_address_t result; coap_address_init(&result);
    result.size=sizeof(sockaddr_in); result.addr.sin.sin_family=AF_INET;
    result.addr.sin.sin_addr.s_addr=htonl(ip.toIPv4Address()); result.addr.sin.sin_port=htons(port);
    return result;
}
}
PhoneDiscovery::PhoneDiscovery(QObject *parent):QObject(parent) {
    static const bool started=[] { coap_startup(); return true; }(); Q_UNUSED(started);
    pump_.setInterval(10); deadline_.setSingleShot(true); deadline_.setInterval(8000);
    connect(&pump_,&QTimer::timeout,this,[this] { if(context_) coap_io_process(context_,COAP_IO_NO_WAIT); });
    connect(&deadline_,&QTimer::timeout,this,[this] {
        // Defer destruction until no libcoap callback is on the stack.
        stop(); emit error(QStringLiteral("官方协同发现超时，尚未收到手机设备信息"));
    });
}
PhoneDiscovery::~PhoneDiscovery() { stop(); }
void PhoneDiscovery::stop() {
    ++generation_;
    pump_.stop(); deadline_.stop();
    if(session_) { coap_session_release(session_); session_=nullptr; }
    if(context_) { coap_free_context(context_); context_=nullptr; }
    port_=0;
}
bool PhoneDiscovery::start(QHostAddress local,QHostAddress phone,quint16 phonePort,DesktopRole role) {
    stop();
    if(local.protocol()!=QAbstractSocket::IPv4Protocol || phone.protocol()!=QAbstractSocket::IPv4Protocol ||
       !local.toIPv4Address() || !phone.toIPv4Address() || local.isMulticast() || phone.isMulticast() ||
       local==QHostAddress::Broadcast || phone==QHostAddress::Broadcast || !phonePort) {
        emit error(QStringLiteral("协同发现需要有效的本机和手机 IPv4 地址")); return false;
    }
    phone_=phone; context_=coap_new_context(nullptr);
    if(!context_) { emit error(QStringLiteral("无法创建 CoAP 环境")); return false; }
    coap_context_set_block_mode(context_,COAP_BLOCK_USE_LIBCOAP|COAP_BLOCK_SINGLE_BODY);
    // libcoap does not expose a server endpoint's ephemeral port. Select one
    // through Qt, then bind it in libcoap; retry if another process claimed it.
    bool listening=false;
    for(int attempt=0;attempt<5 && !listening;++attempt) {
        QUdpSocket reservation;
        reservation.setProxy(QNetworkProxy::NoProxy);
        if(!reservation.bind(local,0)) {
            const auto message=reservation.errorString(); stop(); emit error(message); return false;
        }
        port_=reservation.localPort(); reservation.close();
        auto listen=address(local,port_);
        if(auto endpoint=coap_new_endpoint(context_,&listen,COAP_PROTO_UDP)) {
            // Official Californium peers permit 10 KiB discovery datagrams.
            coap_endpoint_set_default_mtu(endpoint,10240); listening=true;
        }
    }
    if(!listening) { stop(); emit error(QStringLiteral("无法监听官方协同发现回调，请核对本机 IP")); return false; }
    auto resource=coap_resource_init(coap_make_str_const("device_discover"),0);
    if(!resource) { stop(); emit error(QStringLiteral("无法创建官方发现回调资源")); return false; }
    coap_resource_set_userdata(resource,this);
    coap_register_request_handler(resource,COAP_REQUEST_POST,&PhoneDiscovery::receive);
    coap_add_resource(context_,resource);
    auto destination=address(phone,phonePort);
    session_=coap_new_client_session(context_,nullptr,&destination,COAP_PROTO_UDP);
    if(!session_) { stop(); emit error(QStringLiteral("无法创建手机发现请求")); return false; }
    localId_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(role==DesktopRole::Collaboration) {
        QSettings settings;
        localId_=settings.value("collaboration/deviceId").toString();
        if(localId_.isEmpty()) { localId_=QUuid::createUuid().toString(QUuid::WithoutBraces); settings.setValue("collaboration/deviceId",localId_); }
    }
    const auto id=localId_;
    // Discovery and the management hello must use the same desktop role.
    const QJsonObject info{{"vendor","Linux"},{"deviceName","Nubia Link Linux"},{"deviceSN",id},{"deviceId",id},
        {"version",1},{"accountHash",""},{"protocol","ZLINK"},{"devType",int(role)},{"serviceTag",0},
        {"ipv4Addr",QJsonArray{local.toString()}},{"ipv6Addr",QJsonArray{}},
        {"coapUri",QString("coap://%1:%2/device_discover").arg(local.toString()).arg(port_)}};
    const auto json=QJsonDocument(info).toJson(QJsonDocument::Compact);
    auto pdu=coap_pdu_init(COAP_MESSAGE_NON,COAP_REQUEST_CODE_POST,coap_new_message_id(session_),2048);
    const auto path=QByteArrayLiteral("device_discover");
    if(!pdu || !coap_add_option(pdu,COAP_OPTION_URI_PATH,size_t(path.size()),reinterpret_cast<const uint8_t *>(path.constData())) ||
       !coap_add_data(pdu,size_t(json.size()),reinterpret_cast<const uint8_t *>(json.constData()))) {
        if(pdu) coap_delete_pdu(pdu);
        stop(); emit error(QStringLiteral("无法编码官方发现请求")); return false;
    }
    if(coap_send(session_,pdu)==COAP_INVALID_MID) { stop(); emit error(QStringLiteral("发送官方发现请求失败")); return false; }
    pump_.start(); deadline_.start(); return true;
}
void PhoneDiscovery::receive(coap_resource_t *resource,coap_session_t *session,const coap_pdu_t *request,
                             const coap_string_t *,coap_pdu_t *response) {
    auto self=static_cast<PhoneDiscovery *>(coap_resource_get_userdata(resource));
    const auto remote=coap_session_get_addr_remote(session);
    size_t length=0,offset=0,total=0; const uint8_t *bytes=nullptr;
    if(!self || !remote || remote->addr.sa.sa_family!=AF_INET ||
       ntohl(remote->addr.sin.sin_addr.s_addr)!=self->phone_.toIPv4Address()) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_FORBIDDEN); return;
    }
    if(!coap_get_data_large(request,&length,&bytes,&offset,&total) || !length || length>32768 || offset || total!=length) {
        qWarning("Discovery body rejected: length=%zu offset=%zu total=%zu",length,offset,total);
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST); return;
    }
    QJsonParseError parseError;
    const auto doc=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(bytes),qsizetype(length)),&parseError);
    const auto object=doc.object();
    if(!doc.isObject() || object.value("deviceId").toString().isEmpty() || object.value("protocol")!="ZLINK") {
        qWarning("Discovery metadata rejected: object=%d deviceIdPresent=%d protocolPresent=%d",doc.isObject(),
                 !object.value("deviceId").toString().isEmpty(),object.value("protocol").isString());
        qWarning().noquote()<<"JSON parse error:"<<parseError.errorString()<<"offset:"<<parseError.offset<<"bytes:"<<length;
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST); return;
    }
    coap_pdu_set_code(response,COAP_RESPONSE_CODE_CHANGED);
    self->deadline_.stop();
    // Never log accountHash or device credentials. Deliver metadata to callers
    // asynchronously so they may safely stop discovery or destroy this object.
    const auto generation=self->generation_;
    QMetaObject::invokeMethod(self,[self,object,generation] {
        if(generation==self->generation_) emit self->found(object);
    },Qt::QueuedConnection);
}
