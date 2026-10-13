#include "receiver_discovery.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QUrl>
#include <QUuid>

namespace {
coap_address_t address(QHostAddress ip,quint16 port) {
    coap_address_t result; coap_address_init(&result); result.size=sizeof(sockaddr_in);
    result.addr.sin.sin_family=AF_INET; result.addr.sin.sin_addr.s_addr=htonl(ip.toIPv4Address()); result.addr.sin.sin_port=htons(port); return result;
}
}
ReceiverDiscovery::ReceiverDiscovery(QObject *parent):QObject(parent) {
    static const bool started=[] { coap_startup(); return true; }(); Q_UNUSED(started);
    pump_.setInterval(10); announce_.setInterval(5000);
    connect(&pump_,&QTimer::timeout,this,[this] { if(context_) coap_io_process(context_,COAP_IO_NO_WAIT); });
    connect(&announce_,&QTimer::timeout,this,&ReceiverDiscovery::announce);
}
ReceiverDiscovery::~ReceiverDiscovery() { stop(); }
void ReceiverDiscovery::stop() {
    ++generation_; pump_.stop(); announce_.stop();
    for(auto session:clients_) coap_session_release(session);
    clients_.clear(); peers_.clear(); seen_.clear();
    if(context_) coap_free_context(context_);
    context_=nullptr; port_=0;
}
bool ReceiverDiscovery::start(QHostAddress local,int prefix,QString interfaceName,quint16 managerPort,QJsonArray services,quint16 discoveryPort,DesktopRole role) {
    stop();
    if(local.protocol()!=QAbstractSocket::IPv4Protocol || !local.toIPv4Address() || local.isMulticast() || local==QHostAddress::Broadcast || prefix<1 || prefix>32 || !managerPort || !discoveryPort) {
        emit error(QStringLiteral("接收需要有效的局域网接口")); return false;
    }
    local_=local; prefix_=prefix; port_=discoveryPort; managerPort_=managerPort; services_=std::move(services);
    role_=role;
    const auto identityKey=role==DesktopRole::Collaboration ? "collaboration/deviceId" : "receiver/deviceId";
    QSettings settings; id_=settings.value(identityKey).toString();
    if(id_.isEmpty()) { id_=QUuid::createUuid().toString(QUuid::WithoutBraces); settings.setValue(identityKey,id_); }
    context_=coap_new_context(nullptr);
    if(!context_) { emit error(QStringLiteral("无法创建接收发现环境")); return false; }
    const auto listen=address(QHostAddress::AnyIPv4,port_);
    auto endpoint=coap_new_endpoint(context_,&listen,COAP_PROTO_UDP);
    if(!endpoint) { stop(); emit error(QStringLiteral("发现端口被占用，请关闭另一个接收窗口")); return false; }
    coap_endpoint_set_default_mtu(endpoint,10240);
    if(coap_join_mcast_group_intf(context_,"239.255.255.250",interfaceName.isEmpty() ? nullptr : interfaceName.toUtf8().constData())<0) {
        stop(); emit error(QStringLiteral("无法加入原厂发现组播，请检查网络接口")); return false;
    }
    auto resource=coap_resource_init(coap_make_str_const("device_discover"),COAP_RESOURCE_FLAGS_HAS_MCAST_SUPPORT);
    if(!resource) { stop(); emit error(QStringLiteral("无法创建接收发现服务")); return false; }
    coap_resource_set_userdata(resource,this); coap_register_request_handler(resource,COAP_REQUEST_POST,&ReceiverDiscovery::receive); coap_add_resource(context_,resource);
    pump_.start(); announce_.start(); announce(); return true;
}
QJsonObject ReceiverDiscovery::metadata(bool multicast) const {
    QJsonObject object{{"vendor","Linux"},{"deviceName","Nubia Link Linux"},{"deviceSN",id_},{"deviceId",id_},
        {"version",1},{"accountHash",""},{"protocol","ZLINK"},{"devType",int(role_)},{"serviceTag",0},
        {"ipv4Addr",QJsonArray{local_.toString()}},{"ipv6Addr",QJsonArray{}}};
    if(multicast) object["coapUri"]=QString("coap://%1:%2/device_discover").arg(local_.toString()).arg(port_);
    else { object["socketUri"]=QString("socket://%1:%2").arg(local_.toString()).arg(managerPort_); object["custData"]=services_; }
    return object;
}
bool ReceiverDiscovery::send(QHostAddress ip,quint16 port,const QJsonObject &object) {
    const auto key=ip.toString()+":"+QString::number(port);
    auto session=clients_.value(key);
    if(!session) {
        if(clients_.size()>=32) return false;
        const auto destination=address(ip,port), source=address(local_,0);
        session=coap_new_client_session(context_,&source,&destination,COAP_PROTO_UDP);
        if(!session) return false;
        clients_[key]=session;
    }
    const auto body=QJsonDocument(object).toJson(QJsonDocument::Compact), path=QByteArrayLiteral("device_discover");
    auto pdu=coap_pdu_init(COAP_MESSAGE_NON,COAP_REQUEST_CODE_POST,coap_new_message_id(session),10240);
    if(!pdu) return false;
    if(!coap_add_option(pdu,COAP_OPTION_URI_PATH,size_t(path.size()),reinterpret_cast<const uint8_t *>(path.constData())) ||
       !coap_add_data(pdu,size_t(body.size()),reinterpret_cast<const uint8_t *>(body.constData()))) { coap_delete_pdu(pdu); return false; }
    return coap_send(session,pdu)!=COAP_INVALID_MID;
}
void ReceiverDiscovery::announce() { if(context_) send(QHostAddress("239.255.255.250"),25683,metadata(true)); }
QJsonObject ReceiverDiscovery::peer(QHostAddress address) const {
    const auto key=address.toString();
    return QDateTime::currentMSecsSinceEpoch()-seen_.value(key,0)<30000 ? peers_.value(key) : QJsonObject{};
}
QHostAddress ReceiverDiscovery::addressForId(QString id) const {
    for(auto i=peers_.cbegin();i!=peers_.cend();++i)
        if(i.value().value("deviceId").toString()==id && !peer(QHostAddress(i.key())).isEmpty()) return QHostAddress(i.key());
    return {};
}
void ReceiverDiscovery::receive(coap_resource_t *resource,coap_session_t *session,const coap_pdu_t *request,const coap_string_t *,coap_pdu_t *response) {
    auto self=static_cast<ReceiverDiscovery *>(coap_resource_get_userdata(resource));
    const auto remote=coap_session_get_addr_remote(session);
    if(!self || !remote || remote->addr.sa.sa_family!=AF_INET) return;
    const QHostAddress ip(ntohl(remote->addr.sin.sin_addr.s_addr));
    if(!ip.isInSubnet(self->local_,self->prefix_) || ip==self->local_) { coap_pdu_set_code(response,COAP_RESPONSE_CODE_FORBIDDEN); return; }
    size_t length=0,offset=0,total=0; const uint8_t *data=nullptr;
    if(!coap_get_data_large(request,&length,&data,&offset,&total) || !length || length>32768 || offset || total!=length) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST); return;
    }
    const auto doc=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data),qsizetype(length)));
    const auto object=doc.object(); const auto id=object.value("deviceId").toString();
    if(!doc.isObject() || id.isEmpty() || id.size()>256 || id==self->id_ || object.value("protocol")!="ZLINK") {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST); return;
    }
    const auto key=ip.toString();
    if(!self->peers_.contains(key) && self->peers_.size()>=16) { coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return; }
    const auto uri=QUrl(object.value("coapUri").toString());
    if(!uri.isEmpty() && (uri.scheme()!="coap" || QHostAddress(uri.host())!=ip || !uri.userInfo().isEmpty() || uri.path()!="/device_discover" || uri.port()<1 || uri.port()>65535)) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST); return;
    }
    coap_pdu_set_code(response,COAP_RESPONSE_CODE_CHANGED);
    const auto generation=self->generation_;
    const auto sourcePort=ntohs(remote->addr.sin.sin_port);
    QMetaObject::invokeMethod(self,[self,ip,key,object,uri,sourcePort,generation] {
        if(generation!=self->generation_) return;
        const bool first=!self->peers_.contains(key);
        self->peers_[key]=object; self->seen_[key]=QDateTime::currentMSecsSinceEpoch();
        if(!uri.isEmpty()) self->send(ip,quint16(uri.port()),self->metadata(false));
        // This ROM advertises itself in reply to our discovery request. Its
        // reply has no coapUri; exchange our receiver metadata once with the
        // responding CoAP endpoint so its device list can see our services.
        else if(first && !object.value("socketUri").toString().isEmpty()) self->send(ip,sourcePort,self->metadata(false));
        emit self->peerFound(ip,object);
    },Qt::QueuedConnection);
}
