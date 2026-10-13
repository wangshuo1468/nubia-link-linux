#include "file_session.h"
#include <QUrl>
namespace { const QString DfsUuid="57240e3f-8445-4d76-b84d-9c761ca565c8"; }
FileSession::FileSession(QObject *parent):QObject(parent) {
    deadline_.setSingleShot(true); deadline_.setInterval(20000);
    connectDelay_.setSingleShot(true); connectDelay_.setInterval(1000);
    cleanup_.setSingleShot(true); cleanup_.setInterval(7000);
    connect(&cleanup_,&QTimer::timeout,this,&FileSession::release);
    connect(&deadline_,&QTimer::timeout,this,[this] { fail(QStringLiteral("文件服务启动超时")); });
    connect(&connectDelay_,&QTimer::timeout,this,[this] {
        if(running_) client_.start(phone_,1500,discovery_.localId(),DfsClient::Root::SharedStorage);
    });
    connect(&discovery_,&PhoneDiscovery::error,this,&FileSession::fail);
    connect(&manager_,&ServiceManager::error,this,&FileSession::fail);
    connect(&manager_,&ServiceManager::disconnected,this,[this] { if(running_) fail(QStringLiteral("文件服务管理连接已断开")); });
    connect(&client_,&DfsClient::error,this,&FileSession::fail);
    connect(&client_,&DfsClient::connected,this,[this] {
        deadline_.stop(); emit available(true); emit status(QStringLiteral("已连接手机共享存储"));
    });
    connect(&discovery_,&PhoneDiscovery::found,this,[this](QJsonObject device) {
        if(!running_ || managerStarted_) return;
        const QUrl endpoint(device.value("socketUri").toString());
        if(endpoint.scheme()!="socket" || QHostAddress(endpoint.host())!=phone_ || endpoint.port()<=0 ||
           endpoint.port()>65535 || !endpoint.userInfo().isEmpty()) { fail(QStringLiteral("无效文件服务管理地址")); return; }
        managerStarted_=true;
        manager_.start(phone_,quint16(endpoint.port()),device.value("deviceId").toString(),discovery_.localId());
    });
    connect(&manager_,&ServiceManager::connected,this,[this] { request(); });
    connect(&manager_,&ServiceManager::servicePublished,this,[this] { request(); });
    connect(&manager_,&ServiceManager::message,this,[this](QJsonObject rpc) {
        if(!running_ || !requested_ || acknowledged_ || rpc.value("uuid")!=DfsUuid) return;
        if(rpc.value("action")=="distributebus.action.startservice" && rpc.value("profile")=="dfs_is_ok") {
            acknowledged_=true; connectDelay_.start();
        }
    });
}
FileSession::~FileSession() {
    QObject::disconnect(&client_,nullptr,this,nullptr); QObject::disconnect(&manager_,nullptr,this,nullptr);
    stop();
    // Closing the whole application cannot retain its event loop for 7 seconds.
    // Flush the normal cleanup profile before graceful manager disconnect.
    manager_.flush();
}
bool FileSession::start(QHostAddress local,QHostAddress phone) {
    if(running_ || cleanup_.isActive()) return false;
    phone_=phone; running_=true; deadline_.start();
    emit status(QStringLiteral("正在连接原厂文件服务"));
    return discovery_.start(local,phone);
}
void FileSession::request() {
    if(!running_ || requested_ || !manager_.ready()) return;
    bool available=false;
    for(const auto &service:manager_.services()) if(service.value("serviceUuid")==DfsUuid) {
        if(service.value("authType").toInt()!=0) { fail(QStringLiteral("手机文件服务要求额外认证")); return; }
        available=service.value("enable").toBool();
    }
    if(!available) return;
    const QJsonObject publication{{"serviceName","dfsservice"},{"serviceUuid",DfsUuid},{"serviceVersion",1},
        {"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}};
    if(!manager_.send("distributebus.action.publishservice","1",publication)) { fail(QStringLiteral("无法发布文件服务")); return; }
    requested_=manager_.sendProfile("distributebus.action.startservice",DfsUuid,"remote_ready_mount_me="+discovery_.localId());
    if(!requested_) fail(QStringLiteral("无法启动手机文件服务"));
}
void FileSession::stop() {
    if(!running_) return;
    running_=false; deadline_.stop(); connectDelay_.stop(); client_.stop(); emit available(false);
    if(requested_ && manager_.ready()) {
        manager_.sendProfile("distributebus.action.startservice",DfsUuid,"server_can_die");
        manager_.flush(); cleanup_.start();
    } else release();
}
void FileSession::release() {
    cleanup_.stop(); discovery_.stop(); manager_.stop(); requested_=false; acknowledged_=false; managerStarted_=false;
}
void FileSession::fail(QString reason) { stop(); emit status(reason); }
