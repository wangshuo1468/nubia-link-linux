#include "lebo_receiver.h"
#include <QCryptographicHash>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QSettings>
#include <QtEndian>
extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

namespace {
constexpr qsizetype MaxPayload=2*1024*1024;
QByteArray derived(const QByteArray &text) {
    auto bytes=QCryptographicHash::hash(text,QCryptographicHash::Md5);
    for(auto &byte:bytes) byte=char(quint8(byte)^0x78);
    return bytes;
}
QByteArray plist(const QByteArray &dict) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict>"+dict+"</dict></plist>";
}
QByteArray annexB(const QByteArray &avcc, bool config) {
    QByteArray out; qsizetype offset=config ? 6 : 0;
    if(config && (avcc.size()<7 || quint8(avcc[0])!=1)) return {};
    int count=config ? (quint8(avcc[5])&31) : 0;
    for(int group=0;group<(config ? 2 : 1);++group) {
        if(config && group) { if(offset>=avcc.size()) return {}; count=quint8(avcc[offset++]); }
        for(int n=0;config ? n<count : offset<avcc.size();++n) {
            const int width=config ? 2 : 4;
            if(offset+width>avcc.size()) return {};
            const quint32 size=config ? qFromBigEndian<quint16>(avcc.constData()+offset) : qFromBigEndian<quint32>(avcc.constData()+offset);
            offset+=width;
            if(!size || size>quint32(avcc.size()-offset)) return {};
            out.append("\0\0\0\1",4); out+=avcc.mid(offset,size); offset+=size;
        }
    }
    return out;
}
}

LeboReceiver::LeboReceiver(QObject *parent):QObject(parent) {
    control_.setProxy(QNetworkProxy::NoProxy); mirror_.setProxy(QNetworkProxy::NoProxy); audio_.setProxy(QNetworkProxy::NoProxy);
    qDBusRegisterMetaType<QList<QByteArray>>();
    connect(&control_,&QTcpServer::newConnection,this,[this] { accept(control_,false); });
    connect(&mirror_,&QTcpServer::newConnection,this,[this] { accept(mirror_,true); });
    idle_.setInterval(15000); idle_.setSingleShot(true);
    connect(&idle_,&QTimer::timeout,this,[this] { emit status(QStringLiteral("智慧投屏媒体连接超时，等待重新连接")); endStream(); });
    audio_.setReadBufferSize(65536);
    connect(&audio_,&QUdpSocket::readyRead,this,&LeboReceiver::readAudio);
}
LeboReceiver::~LeboReceiver() { stop(); }
bool LeboReceiver::listen(QHostAddress address,int prefix,const QString &interfaceName) {
    stop(); address_=address; prefix_=prefix;
    const auto interface=QNetworkInterface::interfaceFromName(interfaceName);
    if(address.protocol()!=QAbstractSocket::IPv4Protocol || !interface.isValid() || prefix<1 || prefix>32) return false;
    // The phone remembers the lelinkport in its last-connected device row.
    // Reuse the announced control port across restarts; media/audio ports
    // remain negotiated per session. QSettings is ordinary user config.
    QSettings settings(QStringLiteral("NubiaLink"),QStringLiteral("NativeReceiver"));
    const auto key=QStringLiteral("lelinkControlPort/%1").arg(interfaceName);
    const auto saved=settings.value(key,0).toUInt();
    const quint16 preferred=saved>=1024 && saved<=65535 ? quint16(saved) : 0;
    if(!control_.listen(address,preferred) || !mirror_.listen(address,0) || !audio_.bind(address,quint16(0))) { emit status(QStringLiteral("智慧投屏接收端口无法开启：%1 / %2 / %3").arg(control_.errorString(),mirror_.errorString(),audio_.errorString())); stop(); return false; }
    QDBusInterface avahi("org.freedesktop.Avahi","/","org.freedesktop.Avahi.Server",QDBusConnection::systemBus());
    QDBusReply<QDBusObjectPath> group=avahi.call("EntryGroupNew");
    if(!group.isValid()) { emit status(QStringLiteral("无法公布智慧投屏服务：%1").arg(group.error().message())); stop(); return false; }
    groupPath_=group.value().path();
    QDBusConnection::systemBus().connect("org.freedesktop.Avahi",groupPath_,"org.freedesktop.Avahi.EntryGroup","StateChanged",this,SLOT(publicationStateChanged(int,QString)));
    QDBusInterface entry("org.freedesktop.Avahi",groupPath_,"org.freedesktop.Avahi.EntryGroup",QDBusConnection::systemBus());
    const auto port=QByteArray::number(control_.serverPort());
    QList<QByteArray> txt={"u="+interface.hardwareAddress().toUtf8(),"lelinkport="+port,"airplay="+port,"raop="+port,"channel=3.0","pt=pc","name=Nubia Link Linux"};
    auto added=entry.callWithArgumentList(QDBus::Block,"AddService",{interface.index(),0,quint32(0),QStringLiteral("Nubia Link Linux"),QStringLiteral("_leboremote._tcp"),QString(),QString(),QVariant::fromValue(control_.serverPort()),QVariant::fromValue(txt)});
    auto committed=added.type()==QDBusMessage::ErrorMessage ? added : entry.call("Commit");
    if(committed.type()==QDBusMessage::ErrorMessage) { emit status(QStringLiteral("智慧投屏服务公布失败：%1").arg(committed.errorMessage())); stop(); return false; }
    settings.setValue(key,control_.serverPort()); settings.sync();
    qInfo()<<"Lelink discovery control port"<<control_.serverPort();
    emit status(QStringLiteral("智慧投屏接收已开启，在手机“智慧投屏 → 搜索设备”选择 Nubia Link Linux"));
    return true;
}
void LeboReceiver::stop() {
    if(!groupPath_.isEmpty()) {
        QDBusConnection::systemBus().disconnect("org.freedesktop.Avahi",groupPath_,"org.freedesktop.Avahi.EntryGroup","StateChanged",this,SLOT(publicationStateChanged(int,QString)));
        QDBusInterface entry("org.freedesktop.Avahi",groupPath_,"org.freedesktop.Avahi.EntryGroup",QDBusConnection::systemBus());
        entry.call("Reset"); entry.call("Free"); groupPath_.clear();
    }
    control_.close(); mirror_.close(); audio_.close(); idle_.stop();
    const auto sockets=buffers_.keys(); buffers_.clear(); streaming_.clear();
    for(auto socket:sockets) { socket->disconnect(this); socket->abort(); socket->deleteLater(); }
    peer_.clear(); iv_.clear(); audioDecoder_.reset();
    audioPeerPort_=0; audioSequenceValid_=false; audioReceived_=false;
    if(active_) { active_=false; emit activeChanged(false); }
}
void LeboReceiver::publicationStateChanged(int state,QString error) {
    // Commit succeeds before Avahi checks for LAN name collisions.
    if(state==3 || state==4) {
        const auto message=QStringLiteral("智慧投屏服务公布失败：%1").arg(state==3 ? QStringLiteral("局域网内有同名接收器") : error);
        stop();
        emit status(message); emit publicationFailed(message);
    }
}
void LeboReceiver::endStream() {
    const auto sockets=buffers_.keys();
    for(auto socket:sockets) streaming_[socket]=false;
    for(auto socket:sockets) socket->abort();
    peer_.clear(); iv_.clear(); idle_.stop(); audioDecoder_.reset();
    audioPeerPort_=0; audioSequenceValid_=false; audioReceived_=false;
    if(active_) { active_=false; emit activeChanged(false); }
}
QByteArray LeboReceiver::streamInfo() const {
    return plist("<key>width</key><integer>1920</integer><key>height</key><integer>1080</integer><key>refreshRate</key><real>60</real><key>overscanned</key><false/><key>streams</key><array><dict><key>type</key><integer>110</integer><key>dataPort</key><integer>"+QByteArray::number(mirror_.serverPort())+"</integer></dict></array>");
}
void LeboReceiver::accept(QTcpServer &server,bool mirror) {
    while(server.hasPendingConnections()) {
        auto socket=server.nextPendingConnection();
        if(!socket->peerAddress().isInSubnet(address_,prefix_) || buffers_.size()>=8 || (!peer_.isNull() && socket->peerAddress()!=peer_)) { socket->abort(); socket->deleteLater(); continue; }
        buffers_.insert(socket,{}); streaming_.insert(socket,false); socket->setReadBufferSize(MaxPayload+65536);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket,mirror] { read(socket,mirror); });
        connect(socket,&QTcpSocket::disconnected,this,[this,socket] {
            const bool stream=streaming_.take(socket); buffers_.remove(socket); socket->deleteLater();
            if(stream) { emit status(QStringLiteral("手机已断开智慧投屏")); endStream(); }
        });
        auto timeout=new QTimer(socket); timeout->setInterval(15000); timeout->setSingleShot(true); timeout->start();
        connect(timeout,&QTimer::timeout,socket,&QTcpSocket::abort);
        connect(socket,&QTcpSocket::readyRead,timeout,qOverload<>(&QTimer::start));
    }
}
void LeboReceiver::read(QTcpSocket *socket,bool mirror) {
    auto &buffer=buffers_[socket]; buffer+=socket->readAll();
    if(buffer.size()>MaxPayload+65536) { socket->abort(); return; }
    for(;;) {
        if(streaming_.value(socket)) {
            if(buffer.size()<128) return;
            const auto length=qFromLittleEndian<quint32>(buffer.constData());
            if(length>MaxPayload) { emit status(QStringLiteral("智慧投屏媒体包超过支持大小")); socket->abort(); return; }
            if(buffer.size()<128+length) return;
            auto header=buffer.left(128),data=buffer.mid(128,length); buffer.remove(0,128+length);
            if(!frame(header,data)) { emit status(QStringLiteral("智慧投屏媒体格式不兼容")); socket->abort(); return; }
            idle_.start(); continue;
        }
        const auto end=buffer.indexOf("\r\n\r\n");
        if(end<0) { if(buffer.size()>16384) socket->abort(); return; }
        if(end>16384) { socket->abort(); return; }
        const auto lines=buffer.left(end).split('\n'); const auto request=lines.front().trimmed().split(' ');
        if(request.size()!=3) { socket->abort(); return; }
        QHash<QByteArray,QByteArray> headers;
        for(int i=1;i<lines.size();++i) { const auto colon=lines[i].indexOf(':'); if(colon>0) headers.insert(lines[i].left(colon).trimmed().toLower(),lines[i].mid(colon+1).trimmed()); }
        bool ok=true; const auto length=headers.value("content-length","0").toUInt(&ok);
        if(!ok || length>65536) { socket->abort(); return; }
        if(buffer.size()<end+4+length) return;
        const auto requestBody=buffer.mid(end+4,length);
        buffer.remove(0,end+4+length);
        const auto method=request[0],url=request[1];
        if(!peer_.isNull() && peer_!=socket->peerAddress()) { socket->abort(); return; }
        qInfo().noquote()<<"Lelink request"<<method<<url;
        // The legacy sender negotiates its CBC IV on GET /stream.xml,
        // then opens the media socket with a separate POST /stream.
        if(headers.contains("stream-time")) {
            if(!peer_.isNull() && peer_!=socket->peerAddress()) { socket->abort(); return; }
            peer_=socket->peerAddress(); iv_=derived(headers.value("stream-time")); idle_.start();
        }
        QByteArray body,extra; int code=200; bool ending=false;
        if(method=="GET" && url=="/server-info") body=plist("<key>deviceid</key><string>NubiaLinkLinux</string><key>features</key><integer>39</integer><key>model</key><string>NubiaLink</string><key>protovers</key><string>1.0</string><key>srcvers</key><string>150.33</string>");
        else if(method=="GET" && (url=="/stream.xml" || url=="/stream")) body=streamInfo();
        else if(mirror && method=="POST" && url=="/stream") {
            if(!peer_.isNull() && peer_!=socket->peerAddress()) code=453;
            else { peer_=socket->peerAddress(); body=streamInfo(); streaming_[socket]=true; emit peerChanged(peer_); idle_.start(); }
        } else if(method=="ANNOUNCE") {
            peer_=socket->peerAddress();
            audioPeerPort_=0; audioSequenceValid_=false; audioReceived_=false;
            if(!audioDecoder_.configure(requestBody)) {
                code=415; emit audioStatus(QStringLiteral("手机音频格式不受支持；当前支持 44.1 kHz 双声道 AAC-ELD"));
            }
        } else if(method=="SETUP") {
            extra="Session: 1\r\nTransport: "+headers.value("transport")+";server_port="+QByteArray::number(audio_.localPort())+";control_port="+QByteArray::number(audio_.localPort())+";timing_port="+QByteArray::number(audio_.localPort())+"\r\n";
        } else if(method=="GET_PARAMETER") body="volume: 0.000000\r\n";
        else if(method=="OPTIONS") extra="Public: ANNOUNCE, SETUP, RECORD, GET_PARAMETER, SET_PARAMETER, OPTIONS, TEARDOWN\r\n";
        else if(method=="TEARDOWN" || (method=="POST" && url=="/stop")) ending=true;
        else if(!(method=="ANNOUNCE" || method=="RECORD" || method=="SET_PARAMETER" || (method=="POST" && (url=="/feedback" || url=="/heartbat")))) code=404;
        const auto protocol=request[2].startsWith("RTSP/") ? QByteArray("RTSP/1.0") : QByteArray("HTTP/1.1");
        auto response=protocol+' '+QByteArray::number(code)+(code==200 ? " OK\r\n" : " Unsupported\r\n");
        if(headers.contains("cseq")) response+="CSeq: "+headers.value("cseq")+"\r\n";
        response+=extra+"Content-Length: "+QByteArray::number(body.size())+"\r\nContent-Type: text/x-apple-plist+xml\r\n\r\n"+body;
        socket->write(response);
        if(ending) {
            socket->flush(); endStream(); return;
        }
    }
}
void LeboReceiver::readAudio() {
    // Limit work per event-loop turn so a flooded UDP socket cannot stall UI.
    for(int n=0;n<64 && audio_.hasPendingDatagrams();++n) {
        const auto datagramSize=audio_.pendingDatagramSize();
        char bytes[4108]; QHostAddress sender; quint16 port=0;
        const auto size=audio_.readDatagram(bytes,sizeof(bytes),&sender,&port);
        if(!active_ || !audioDecoder_.ready() || peer_.isNull() || sender!=peer_ ||
           (audioPeerPort_ && port!=audioPeerPort_) || size<=12 || datagramSize>4108 ||
           quint8(bytes[0])!=0x80 || quint8(bytes[1])!=96) continue;
        const auto seq=qFromBigEndian<quint16>(bytes+2);
        const auto delta=quint16(seq-audioSequence_);
        if(audioSequenceValid_ && (delta==0 || delta>=0x8000)) continue;
        auto pcm=audioDecoder_.decode(QByteArray(bytes+12,size-12));
        if(pcm.isEmpty()) continue;
        audioPeerPort_=port; audioSequence_=seq; audioSequenceValid_=true;
        if(!audioReceived_) {
            audioReceived_=true;
            qInfo()<<"Lelink AAC-ELD decoded to 48kHz stereo PCM"<<pcm.size();
            emit audioStatus(QStringLiteral("手机声音已接通 · 电脑反控尚未实现"));
        }
        emit audioData(pcm);
    }
    if(audio_.hasPendingDatagrams()) QTimer::singleShot(0,this,&LeboReceiver::readAudio);
}
bool LeboReceiver::frame(QByteArray header,QByteArray data) {
    const auto type=qFromLittleEndian<quint16>(header.constData()+4);
    if(type>1) return true; // timing and metadata packets
    if(type==0 && quint8(header[127])==1) {
        if(iv_.size()!=16 || data.size()<5) return false;
        const int blocks=(data.size()-5)/32;
        if(blocks) {
            auto cipher=av_aes_alloc(); if(!cipher) return false;
            const auto key=derived("Happycast/1.0");
            av_aes_init(cipher,reinterpret_cast<const quint8 *>(key.constData()),128,1);
            av_aes_crypt(cipher,reinterpret_cast<quint8 *>(data.data()+5),reinterpret_cast<const quint8 *>(data.constData()+5),blocks,reinterpret_cast<quint8 *>(iv_.data()),1);
            av_free(cipher);
        }
    }
    auto packet=annexB(data,type==1); if(packet.isEmpty()) return false;
    if(!active_) { active_=true; emit activeChanged(true); }
    emit videoPacket(packet,type==1 ? -1 : qint64(qFromLittleEndian<quint64>(header.constData()+8)));
    return true;
}
