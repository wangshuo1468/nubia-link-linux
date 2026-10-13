#include "rfcomm_connector.h"
#include <QThread>
#include <QElapsedTimer>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <utility>
#include <cerrno>

int NubiaRfcomm::channel(const sdp_record_t *record) {
    sdp_list_t *protocols=nullptr;
    if(!record || sdp_get_access_protos(record,&protocols)<0) return 0;
    const auto port=sdp_get_proto_port(protocols,RFCOMM_UUID);
    for(auto list=protocols;list;list=list->next) sdp_list_free(static_cast<sdp_list_t *>(list->data),nullptr);
    sdp_list_free(protocols,nullptr);
    return port>0 && port<=30 ? port : 0;
}
struct RfcommConnector::Job {
    std::atomic_bool canceled=false;
    std::mutex mutex;
    int active=-1,result=-1;
    QString error;
    ~Job() { if(result>=0) ::close(result); }
    void cancel() {
        canceled=true; std::lock_guard lock(mutex);
        if(active>=0) ::shutdown(active,SHUT_RDWR);
    }
    void track(int fd) { std::lock_guard lock(mutex); active=fd; }
    void untrack() { std::lock_guard lock(mutex); active=-1; }
    bool waitConnected(int fd) {
        QElapsedTimer elapsed; elapsed.start();
        while(!canceled && elapsed.elapsed()<10000) {
            pollfd poller{fd,POLLOUT,0}; const int ready=::poll(&poller,1,200);
            if(ready<0 && errno==EINTR) continue;
            if(ready<0) return false;
            if(!ready) continue;
            int code=0; socklen_t size=sizeof(code);
            return ::getsockopt(fd,SOL_SOCKET,SO_ERROR,&code,&size)==0 && code==0;
        }
        return false;
    }
    void run(QString localAddress,QString remoteAddress) {
        bdaddr_t local{},remote{};
        if(str2ba(localAddress.toLatin1().constData(),&local) || str2ba(remoteAddress.toLatin1().constData(),&remote)) {
            error=QStringLiteral("无效的协同蓝牙地址"); return;
        }
        auto session=sdp_connect(&local,&remote,SDP_NON_BLOCKING);
        if(!session) { error=QStringLiteral("无法连接手机的蓝牙服务发现端口"); return; }
        const int fd=sdp_get_socket(session); track(fd);
        if(!waitConnected(fd) || canceled) {
            untrack(); sdp_close(session); error=QStringLiteral("手机蓝牙服务发现连接超时或断开"); return;
        }
        ::fcntl(fd,F_SETFL,::fcntl(fd,F_GETFL)&~O_NONBLOCK);
        timeval timeout{5,0}; ::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        ::setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        uuid_t uuid{};
        const uint8_t service[]={0x18,0x7f,0x80,0,0,0,0x10,0,0x80,0,0xce,0xb5,0x74,0x81,0x50,0};
        sdp_uuid128_create(&uuid,service);
        uint32_t range=0x0000ffff;
        auto search=sdp_list_append(nullptr,&uuid),attributes=sdp_list_append(nullptr,&range);
        sdp_list_t *records=nullptr;
        const auto status=sdp_service_search_attr_req(session,search,SDP_ATTR_REQ_RANGE,attributes,&records);
        int port=0;
        if(status==0) for(auto record=records;record && !port;record=record->next)
            port=NubiaRfcomm::channel(static_cast<sdp_record_t *>(record->data));
        for(auto record=records;record;record=record->next) sdp_record_free(static_cast<sdp_record_t *>(record->data));
        sdp_list_free(records,nullptr); sdp_list_free(search,nullptr); sdp_list_free(attributes,nullptr);
        untrack(); sdp_close(session);
        if(canceled) return;
        if(!port) { error=QStringLiteral("手机尚未公布原厂 RFCOMM 协同管理服务"); return; }
        const int stream=::socket(AF_BLUETOOTH,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,BTPROTO_RFCOMM);
        if(stream<0) { error=QStringLiteral("无法创建协同蓝牙管理连接"); return; }
        track(stream);
        sockaddr_rc source{}; source.rc_family=AF_BLUETOOTH; source.rc_bdaddr=local;
        sockaddr_rc destination{}; destination.rc_family=AF_BLUETOOTH; destination.rc_bdaddr=remote; destination.rc_channel=uint8_t(port);
        bt_security security{}; security.level=BT_SECURITY_LOW;
        ::setsockopt(stream,SOL_BLUETOOTH,BT_SECURITY,&security,sizeof(security));
        bool ok=::bind(stream,reinterpret_cast<sockaddr *>(&source),sizeof(source))==0;
        if(ok) {
            const auto result=::connect(stream,reinterpret_cast<sockaddr *>(&destination),sizeof(destination));
            ok=result==0 || (errno==EINPROGRESS && waitConnected(stream));
        }
        untrack();
        if(ok && !canceled) result=stream;
        else { ::close(stream); error=QStringLiteral("手机的原厂 RFCOMM 协同管理连接失败"); }
    }
};
RfcommConnector::RfcommConnector(QObject *parent):QObject(parent) {}
RfcommConnector::~RfcommConnector() { stop(); }
bool RfcommConnector::start(QString local,QString phone) {
    if(job_) return false;
    auto job=job_=std::make_shared<Job>();
    auto thread=QThread::create([job,local,phone] { job->run(local,phone); });
    connect(thread,&QThread::finished,this,[this,job] {
        if(job_!=job || job->canceled) return;
        job_.reset();
        if(job->result>=0) emit connected(std::exchange(job->result,-1));
        else emit error(job->error);
    });
    connect(thread,&QThread::finished,thread,&QObject::deleteLater);
    thread->start(); return true;
}
void RfcommConnector::stop() { if(job_) { job_->cancel(); job_.reset(); } }
