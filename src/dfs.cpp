#include "dfs.h"
#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QStringDecoder>
#include <QtEndian>

namespace Dfs {
QByteArray u32(quint32 value) { QByteArray b(4,Qt::Uninitialized); qToBigEndian(value,b.data()); return b; }
QByteArray u64(quint64 value) { QByteArray b(8,Qt::Uninitialized); qToBigEndian(value,b.data()); return b; }
QByteArray string(QByteArrayView value) { return u32(quint32(value.size()))+value.toByteArray(); }
QByteArray frame(quint8 type,quint32 id,QByteArrayView payload) {
    if(payload.size()>MaxPayload) return {};
    return u32(quint32(payload.size()))+QByteArray(1,char(type))+u32(id)+payload.toByteArray();
}
bool attributes(QByteArrayView bytes,Entry &entry) {
    if(bytes.size()!=88) return false;
    entry.mode=qFromLittleEndian<quint32>(bytes.data()+16);
    entry.size=qFromLittleEndian<qint64>(bytes.data()+40);
    entry.modified=qFromLittleEndian<qint64>(bytes.data()+72);
    return entry.size>=0;
}
bool validPath(const QString &path) {
    if(path.isEmpty()) return true;
    if(path.toUtf8().size()>4096 || path.contains(QChar(0)) || path.startsWith('/') || path.contains('\\')) return false;
    for(const auto &part:path.split('/')) if(part.isEmpty() || part=="." || part=="..") return false;
    return true;
}
bool entries(QByteArrayView bytes,QList<Entry> &result) {
    result.clear();
    if(bytes.size()<4) return false;
    auto count=qFromBigEndian<quint32>(bytes.data()); bytes=bytes.sliced(4);
    if(count>quint32(bytes.size()/92)) return false;
    for(quint32 i=0;i<count;++i) {
        if(bytes.size()<4) return false;
        auto size=qFromBigEndian<quint32>(bytes.data()); bytes=bytes.sliced(4);
        if(size>4096 || bytes.size()<qsizetype(size)+88) return false;
        QStringDecoder decoder(QStringDecoder::Utf8);
        Entry entry; entry.name=decoder(bytes.first(size)); bytes=bytes.sliced(size);
        if(decoder.hasError() || entry.name.isEmpty() || entry.name.contains('/') || entry.name.contains('\\') || entry.name.contains(QChar(0))) return false;
        if(!attributes(bytes.first(88),entry)) return false;
        bytes=bytes.sliced(88);
        if(entry.name!="." && entry.name!="..") result.append(entry);
    }
    return bytes.isEmpty();
}
bool Reader::append(QByteArrayView bytes,QList<Packet> &packets) {
    if(failed_) return false;
    // Process incrementally, including several full packets in a single read.
    while(!bytes.isEmpty()) {
        qsizetype target=9;
        if(buffer_.size()>=9) {
            auto length=qFromBigEndian<quint32>(buffer_.constData());
            if(length>MaxPayload) { failed_=true; return false; }
            target+=length;
        }
        auto take=qMin(bytes.size(),target-buffer_.size());
        buffer_.append(bytes.data(),take); bytes=bytes.sliced(take);
        if(buffer_.size()<9) continue;
        auto length=qFromBigEndian<quint32>(buffer_.constData());
        if(length>MaxPayload) { failed_=true; return false; }
        if(buffer_.size()==9+qsizetype(length)) {
            packets.append({quint8(buffer_[4]),qFromBigEndian<quint32>(buffer_.constData()+5),buffer_.mid(9)});
            buffer_.clear();
        }
    }
    return true;
}
}
DfsClient::DfsClient(QObject *parent):QObject(parent) {
    socket_.setProxy(QNetworkProxy::NoProxy); socket_.setReadBufferSize(Dfs::MaxPayload+9);
    timeout_.setSingleShot(true); timeout_.setInterval(15000);
    connect(&timeout_,&QTimer::timeout,this,[this] { fail(QStringLiteral("原厂文件服务响应超时")); });
    connect(&socket_,&QTcpSocket::connected,this,[this] {
        const auto identity=socket_.localAddress().toString().toUtf8()+deviceId_.toUtf8();
        request(3,Dfs::string(root_.toUtf8())+Dfs::string("NubiaLinkLinux")+
                Dfs::string(QCryptographicHash::hash(identity,QCryptographicHash::Md5).toHex())+Dfs::u32(1024*1024),
                [this](const Dfs::Packet &packet) {
            Dfs::Entry root;
            if(packet.type!=105 || (packet.payload.size()!=88 && packet.payload.size()!=92) ||
               !Dfs::attributes(QByteArrayView(packet.payload).first(88),root) || !root.directory()) {
                fail(statusError(packet)); return;
            }
            // Older servers omit the optional negotiated block-size field.
            chunkSize_=64*1024;
            if(packet.payload.size()==92) {
                const auto block=qFromBigEndian<quint32>(packet.payload.constData()+88);
                if(!block) { fail(QStringLiteral("文件服务返回无效块大小")); return; }
                chunkSize_=qMin(chunkSize_,block);
            }
            ready_=true; emit connected();
        });
    });
    connect(&socket_,&QTcpSocket::readyRead,this,[this] {
        QList<Dfs::Packet> packets;
        if(!reader_.append(socket_.readAll(),packets)) { fail(QStringLiteral("文件服务返回超限数据包")); return; }
        for(const auto &packet:packets) {
            if(!running_) break;
            if(!pending_ || packet.id!=pendingId_) { fail(QStringLiteral("文件服务请求编号不匹配")); return; }
            timeout_.stop(); auto callback=std::move(pending_); pending_={}; callback(packet);
        }
    });
    connect(&socket_,&QTcpSocket::errorOccurred,this,[this] { if(running_) fail(QStringLiteral("文件连接失败：")+socket_.errorString()); });
    connect(&socket_,&QTcpSocket::disconnected,this,[this] { if(running_) fail(QStringLiteral("手机文件服务已断开")); });
}
DfsClient::~DfsClient() { QObject::disconnect(&socket_,nullptr,this,nullptr); stop(); }
void DfsClient::start(QHostAddress phone,quint16 port,QString localDeviceId,Root root) {
    stop(); deviceId_=localDeviceId; root_=root==Root::SharedStorage ? "/data/media/0/" : "/mnt/dfs/";
    nextId_=2; running_=true; timeout_.start(); socket_.connectToHost(phone,port);
}
void DfsClient::stop() {
    const bool wasRunning=running_; running_=false; ready_=false; pending_={}; timeout_.stop();
    if(socket_.state()==QAbstractSocket::ConnectedState) {
        socket_.write(Dfs::frame(31,nextId_,Dfs::string(socket_.localAddress().toString().toUtf8())));
        socket_.flush(); socket_.disconnectFromHost();
    } else socket_.abort();
    reader_.reset(); hasHandle_=false; input_.reset(); output_.reset(); created_=false; setBusy(false);
    if(wasRunning) emit disconnected();
}
void DfsClient::fail(QString reason) {
    if(created_) reason+=QStringLiteral("；手机可能保留未完成文件：")+remotePath_;
    const bool operation=busy_; stop();
    if(operation) emit operationFailed(reason);
    emit error(reason);
}
void DfsClient::setBusy(bool value) { if(busy_!=value) { busy_=value; emit busyChanged(value); } }
bool DfsClient::request(quint8 type,QByteArray payload,Reply callback) {
    if(!running_ || pending_ || socket_.state()!=QAbstractSocket::ConnectedState || payload.size()>Dfs::MaxPayload) return false;
    if(nextId_>0xfff9) nextId_=2;
    pendingId_=nextId_++; pending_=std::move(callback); timeout_.start();
    if(socket_.write(Dfs::frame(type,pendingId_,payload))<0) { fail(socket_.errorString()); return false; }
    return true;
}
QString DfsClient::statusError(const Dfs::Packet &p) const {
    if(p.type==101 && p.payload.size()>=4) return QStringLiteral("手机文件服务拒绝请求（状态 %1）").arg(qFromBigEndian<quint32>(p.payload.constData()));
    return QStringLiteral("文件服务回复格式无效（类型 %1）").arg(p.type);
}
bool DfsClient::expect(const Dfs::Packet &p,quint8 type) {
    if(p.type==type && (type!=101 || (p.payload.size()>=4 && qFromBigEndian<quint32>(p.payload.constData())==0))) return true;
    operationError(statusError(p)); return false;
}
QByteArray DfsClient::path(const QString &relative) const { return Dfs::string((root_+relative).toUtf8()); }
void DfsClient::closeHandle(std::function<void(bool)> next) {
    if(!hasHandle_) { next(true); return; }
    request(2,Dfs::u32(handle_),[this,next=std::move(next)](const Dfs::Packet &p) {
        hasHandle_=false;
        next(p.type==101 && p.payload.size()>=4 && qFromBigEndian<quint32>(p.payload.constData())==0);
    });
}
void DfsClient::operationError(QString reason) {
    input_.reset(); output_.reset();
    closeHandle([this,reason](bool) {
        if(created_) {
            request(14,path(remotePath_),[this,reason](const Dfs::Packet &p) {
                const bool removed=p.type==101 && p.payload.size()>=4 && qFromBigEndian<quint32>(p.payload.constData())==0;
                created_=false; setBusy(false);
                emit operationFailed(removed ? reason : reason+QStringLiteral("；手机可能保留未完成文件：")+remotePath_);
            });
        } else { setBusy(false); emit operationFailed(reason); }
    });
}
bool DfsClient::list(const QString &relative) {
    if(!ready_ || busy_ || !Dfs::validPath(relative)) return false;
    remotePath_=relative; listing_.clear(); setBusy(true);
    return request(5,path(relative),[this](const Dfs::Packet &p) {
        if(!expect(p,102)) return;
        if(p.payload.size()!=4) { operationError(QStringLiteral("无效目录句柄")); return; }
        handle_=qFromBigEndian<quint32>(p.payload.constData()); hasHandle_=true; readDirectory();
    });
}
void DfsClient::readDirectory() {
    request(6,Dfs::u32(handle_),[this](const Dfs::Packet &p) {
        if(p.type==101 && p.payload.size()>=4 && qFromBigEndian<quint32>(p.payload.constData())==1) {
            closeHandle([this](bool ok) {
                if(!ok) { operationError(QStringLiteral("关闭目录失败")); return; }
                const auto result=listing_; const auto folder=remotePath_; setBusy(false); emit listing(folder,result);
            }); return;
        }
        if(!expect(p,104)) return;
        QList<Dfs::Entry> page;
        if(!Dfs::entries(p.payload,page) || listing_.size()+page.size()>100000 || page.isEmpty()) {
            operationError(QStringLiteral("无效或超限目录列表")); return;
        }
        listing_.append(page); readDirectory();
    });
}
bool DfsClient::upload(const QString &localFile,const QString &relative) {
    if(!ready_ || busy_ || relative.isEmpty() || !Dfs::validPath(relative)) return false;
    input_=std::make_unique<QFile>(localFile);
    if(!input_->open(QIODevice::ReadOnly)) { const auto reason=input_->errorString(); input_.reset(); emit operationFailed(reason); return false; }
    total_=input_->size(); offset_=0; remotePath_=relative; uploading_=true; created_=false; setBusy(true);
    // Android O_WRONLY | O_CREAT | O_EXCL, mode 0600.
    return request(1,path(relative)+Dfs::u32(0xc1)+Dfs::u32(0600),[this](const Dfs::Packet &p) {
        if(!expect(p,102)) return;
        if(p.payload.size()!=4) { operationError(QStringLiteral("无效文件句柄")); return; }
        handle_=qFromBigEndian<quint32>(p.payload.constData()); hasHandle_=true; created_=true; writeChunk();
    });
}
void DfsClient::writeChunk() {
    if(offset_==total_) { request(24,Dfs::u32(handle_),[this](const Dfs::Packet &p) { if(expect(p,101)) completeTransfer(); }); return; }
    const auto chunk=input_->read(qMin<qint64>(chunkSize_,total_-offset_));
    if(chunk.isEmpty()) { operationError(QStringLiteral("读取电脑文件失败或文件已改变")); return; }
    request(8,Dfs::u32(1)+Dfs::u32(handle_)+Dfs::u64(quint64(offset_))+Dfs::u32(quint32(chunk.size()))+chunk,
            [this,size=chunk.size()](const Dfs::Packet &p) {
        if(!expect(p,101)) return;
        offset_+=size; emit progress(offset_,total_); writeChunk();
    });
}
bool DfsClient::download(const QString &relative,const QString &localFile) {
    if(!ready_ || busy_ || relative.isEmpty() || !Dfs::validPath(relative)) return false;
    output_=std::make_unique<QSaveFile>(localFile); output_->setDirectWriteFallback(false);
    if(!output_->open(QIODevice::WriteOnly)) { auto reason=output_->errorString(); output_.reset(); emit operationFailed(reason); return false; }
    offset_=0; total_=0; remotePath_=relative; uploading_=false; setBusy(true);
    return request(3,path(relative),[this](const Dfs::Packet &p) {
        if(!expect(p,105)) return;
        Dfs::Entry entry;
        if(!Dfs::attributes(p.payload,entry) || !entry.regular()) { operationError(QStringLiteral("请选择普通文件")); return; }
        total_=entry.size;
        request(1,path(remotePath_)+Dfs::u32(0)+Dfs::u32(0),[this](const Dfs::Packet &reply) {
            if(!expect(reply,102)) return;
            if(reply.payload.size()!=4) { operationError(QStringLiteral("无效文件句柄")); return; }
            handle_=qFromBigEndian<quint32>(reply.payload.constData()); hasHandle_=true; readChunk();
        });
    });
}
void DfsClient::readChunk() {
    if(offset_==total_) { completeTransfer(); return; }
    const auto requested=quint32(qMin<qint64>(chunkSize_,total_-offset_));
    request(7,Dfs::u32(handle_)+Dfs::u64(quint64(offset_))+Dfs::u32(requested),[this,requested](const Dfs::Packet &p) {
        if(!expect(p,103)) return;
        if(p.payload.size()<4) { operationError(QStringLiteral("无效文件数据")); return; }
        const auto size=qFromBigEndian<quint32>(p.payload.constData());
        if(!size || size>requested || p.payload.size()!=4+qsizetype(size)) { operationError(QStringLiteral("文件数据长度不匹配")); return; }
        if(output_->write(p.payload.constData()+4,size)!=qint64(size)) { operationError(output_->errorString()); return; }
        offset_+=size; emit progress(offset_,total_); readChunk();
    });
}
void DfsClient::completeTransfer() {
    closeHandle([this](bool ok) {
        if(!ok) { operationError(QStringLiteral("关闭文件失败")); return; }
        if(output_ && !output_->commit()) { operationError(output_->errorString()); return; }
        created_=false; input_.reset(); output_.reset(); const auto result=remotePath_; setBusy(false); emit completed(result);
    });
}
