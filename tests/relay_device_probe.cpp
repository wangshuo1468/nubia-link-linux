#include "discovery.h"
#include "service_manager.h"
#include "lan_projection.h"
#include "collaboration_wire.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QUdpSocket>
#include <QtEndian>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QImage>

// Explicit opt-in protocol research. Offer-only modes reject media; decode
// modes receive the selected application's picture. The second-app mode
// observes a new request while Settings is connected, then rejects that new
// offer. It does not test two accepted receivers or prove parallel support.
// A ready_to_connect response creates phone state; request its normal disconnect
// and wait for acknowledgement before closing our dedicated transport.
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()!=4 || (args[3]!="--associate-and-disconnect" && args[3]!="--virtual-offer-and-reject" && args[3]!="--cast-associate-and-offer" && args[3]!="--cast-transport-and-reject" && args[3]!="--cast-packet-probe" && args[3]!="--cast-decode-probe" && args[3]!="--cast-second-app-probe")) {
        QTextStream(stderr)<<"Usage: relay-device-probe LOCAL_IPV4 PHONE_IPV4 --associate-and-disconnect|--virtual-offer-and-reject|--cast-associate-and-offer|--cast-transport-and-reject|--cast-packet-probe|--cast-decode-probe|--cast-second-app-probe\n"; return 2;
    }
    const bool secondAppMode=args[3]=="--cast-second-app-probe";
    const bool decodeMode=secondAppMode || args[3]=="--cast-decode-probe";
    const bool packetMode=decodeMode || args[3]=="--cast-packet-probe";
    const bool transportMode=packetMode || args[3]=="--cast-transport-and-reject";
    const bool castMode=transportMode || args[3]=="--cast-associate-and-offer";
    const bool offerMode=castMode || args[3]=="--virtual-offer-and-reject";
    const QHostAddress local(args[1]),phone(args[2]);
    const QString uuid="187f8000-0000-1000-8000-ceb574815500";
    const QString sourceUuid="187f8000-0000-1000-8000-ceb574815101";
    const QString sinkUuid="187f8000-0000-1000-8000-ceb574815100";
    PhoneDiscovery discovery; ServiceManager standaloneManager;
    ProjectionSession media; LanProjection cast(media);
    cast.preserveControlForHandover(castMode);
    ServiceManager &manager=castMode ? cast.serviceManager() : standaloneManager;
    const auto localId=[&] { return castMode ? cast.localId() : discovery.localId(); };
    bool managerStarted=false,requested=false,associated=false,stopping=false,finished=false;
    bool appRequested=false,offerReceived=false,transportObserved=false;
    bool secondRequested=false,secondOffered=false;
    QTcpSocket control; control.setProxy(QNetworkProxy::NoProxy);
    Collaboration::MessageReader controlReader;
    QList<QUdpSocket *> udp;
    QMap<int,int> packetCounts;
    bool playSent=false;
    QTemporaryDir decodeFiles;
    QProcess decode;
    QByteArray decodedImage;
    bool decodeStarted=false,frameDecoded=false;
    int mediaPort=0;
    int resultCode=0;
    QTimer deadline; deadline.setSingleShot(true); deadline.setInterval(15000);
    auto finish=[&](int code) {
        if(finished) return;
        finished=true; deadline.stop();
        for(auto *socket:udp) socket->close();
        control.abort();
        if(decode.state()!=QProcess::NotRunning) { decode.kill(); decode.waitForFinished(1000); }
        if(packetMode) for(auto it=packetCounts.cbegin();it!=packetCounts.cend();++it)
            QTextStream(stdout)<<"Media port offset "<<it.key()<<": "<<it.value()<<" datagrams"<<Qt::endl;
        if(secondAppMode) QTextStream(stdout)<<"Second-app request="<<secondRequested<<", offer="<<secondOffered
            <<"; this probe does not validate parallel application windows"<<Qt::endl;
        if(castMode) cast.stop(); else { manager.stop(); discovery.stop(); }
        app.exit(code);
    };
    auto stop=[&] {
        if(stopping || finished || !associated) return;
        stopping=true;
        if(playSent && control.state()==QAbstractSocket::ConnectedState) {
            control.write(Collaboration::controlFrame("stop",{},2)); control.flush();
            QTextStream(stdout)<<"Official window stop command: sent"<<Qt::endl;
        }
        const bool sent=manager.sendProfile("distributebus.action.stopservice",uuid,"disconnect");
        manager.flush();
        QTextStream(stdout)<<"Official association disconnect: "<<(sent ? "sent" : "FAILED")<<Qt::endl;
        if(!sent) { finish(1); return; }
        deadline.start(8000);
    };
    auto error=[&](QString reason) {
        resultCode=1;
        QTextStream(stderr)<<reason<<Qt::endl;
        if(associated && !stopping && manager.ready()) stop(); else finish(1);
    };
    QObject::connect(&decode,&QProcess::readyReadStandardOutput,&app,[&] {
        decodedImage+=decode.readAllStandardOutput();
        if(decodedImage.size()>32*1024*1024) error("Decoded image exceeds diagnostic limit");
    });
    QObject::connect(&decode,&QProcess::readyReadStandardError,&app,[&] {
        // FFmpeg is limited to error logging; SDP only contains transport/codec data.
        const auto diagnostic=QString::fromUtf8(decode.readAllStandardError());
        if(!diagnostic.isEmpty()) QTextStream(stderr)<<diagnostic.left(2000)<<Qt::endl;
    });
    QObject::connect(&decode,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&app,[&](int code,QProcess::ExitStatus status) {
        if(finished) return;
        decodedImage+=decode.readAllStandardOutput();
        const auto image=QImage::fromData(decodedImage,"PNG");
        if(code==0 && status==QProcess::NormalExit && !image.isNull()) {
            QTemporaryFile snapshot("/tmp/nubia-independent-XXXXXX.png");
            if(!snapshot.open() || !image.save(&snapshot,"PNG")) { error("Cannot save decoded diagnostic frame"); return; }
            snapshot.setAutoRemove(false); frameDecoded=true;
            QTextStream(stdout)<<"FFmpeg decoded virtual-display frame: "<<image.width()<<" x "<<image.height()
                <<"; diagnostic image: "<<snapshot.fileName()<<Qt::endl;
        } else { error("FFmpeg did not decode a virtual-display frame"); }
    });
    QObject::connect(&control,&QTcpSocket::errorOccurred,&app,[&](QAbstractSocket::SocketError code) {
        if(!finished && !stopping && code!=QAbstractSocket::RemoteHostClosedError) error("Window control: "+control.errorString());
    });
    QObject::connect(&control,&QTcpSocket::readyRead,&app,[&] {
        QList<QByteArray> payloads;
        if(!controlReader.append(control.readAll(),payloads)) { error(controlReader.error()); return; }
        for(const auto &payload:payloads) {
            const auto command=QJsonDocument::fromJson(payload).object();
            const auto action=command.value("action").toString();
            if(!action.isEmpty()) QTextStream(stdout)<<"Window control message: "<<action.left(80)<<Qt::endl;
            if(decodeMode && action=="encode_sps_pps" && !decodeStarted) {
                const auto config=QJsonDocument::fromJson(command.value("extra").toString().toUtf8()).object();
                auto nal=[&](QString key) {
                    QByteArray bytes;
                    const auto array=config.value(key).toArray();
                    if(array.isEmpty() || array.size()>4096) return bytes;
                    for(const auto &value:array) {
                        if(!value.isDouble() || value.toDouble()!=value.toInt() || value.toInt()<-128 || value.toInt()>127) return QByteArray();
                        bytes.append(char(value.toInt()&255));
                    }
                    if(bytes.startsWith(QByteArray::fromHex("00000001"))) bytes.remove(0,4);
                    else if(bytes.startsWith(QByteArray::fromHex("000001"))) bytes.remove(0,3);
                    return bytes;
                };
                const auto sps=nal("sps"),pps=nal("pps");
                if(sps.isEmpty() || pps.isEmpty() || (quint8(sps[0])&31)!=7 || (quint8(pps[0])&31)!=8) {
                    error("Invalid H264 codec configuration"); return;
                }
                // Use FFmpeg's existing RTP demuxer, not a guessed ZIME depacketizer.
                udp[2]->close(); udp[3]->close();
                QFile sdp(decodeFiles.filePath("window.sdp"));
                if(!sdp.open(QIODevice::WriteOnly)) { error("Cannot create diagnostic SDP"); return; }
                const auto description=QString("v=0\no=- 0 0 IN IP4 %1\ns=Nubia virtual window\nc=IN IP4 %1\nt=0 0\n"
                    "m=video %2 RTP/AVP 114\na=rtpmap:114 H264/90000\na=fmtp:114 packetization-mode=1;sprop-parameter-sets=%3,%4\n"
                    "a=source-filter:incl IN IP4 %1 %5\n").arg(local.toString()).arg(mediaPort+2)
                    .arg(QString::fromLatin1(sps.toBase64()),QString::fromLatin1(pps.toBase64()),phone.toString());
                sdp.write(description.toUtf8()); sdp.close();
                decodeStarted=true;
                decode.start("ffmpeg",{"-hide_banner","-loglevel","error","-protocol_whitelist","file,udp,rtp",
                    "-analyzeduration","1000000","-probesize","32768","-i",sdp.fileName(),
                    "-frames:v","1","-f","image2pipe","-vcodec","png","pipe:1"});
                QTimer::singleShot(750,&app,[&] {
                    if(!finished && !stopping) { control.write(Collaboration::controlFrame("request_key_frame",{},3)); control.flush(); }
                });
                QTextStream(stdout)<<"FFmpeg RTP/H264 decoder started with phone-provided SPS/PPS"<<Qt::endl;
            }
        }
    });
    QObject::connect(&control,&QTcpSocket::connected,&app,[&] {
        const auto profile=QString::fromUtf8(QJsonDocument(QJsonObject{{"type",2},{"streamTransferProfile","0"}}).toJson(QJsonDocument::Compact));
        if(!manager.send("distributebus.action.sendnotification",sinkUuid,{{"command",5},{"profile",profile}})) {
            error("Cannot acknowledge experimental receiver"); return;
        }
        QTextStream(stdout)<<"Experimental UDP receiver listening; official control TCP connected"<<Qt::endl;
        QTimer::singleShot(500,&app,[&] {
            if(finished || stopping) return;
            const auto play=Collaboration::controlFrame("play",{{"width",1080},{"height",2400}},1);
            playSent=control.write(play)==play.size(); control.flush();
            if(!playSent) { error("Cannot send window play"); return; }
            QTextStream(stdout)<<"Official Settings window play: sent"<<Qt::endl;
            deadline.start(25000);
            if(secondAppMode) QTimer::singleShot(6000,&app,[&] {
                if(finished || stopping) return;
                if(!frameDecoded) { error("First application must decode before requesting a second"); return; }
                secondRequested=manager.send("distributebus.action.startservice",sourceUuid,
                    {{"castDisplayType","virtualDisplay"},{"pkg","com.android.calculator2"}});
                manager.flush();
                if(!secondRequested) { error("Cannot send second application request"); return; }
                QTextStream(stdout)<<"Second virtual-display request: com.android.calculator2; first control still connected="
                    <<(control.state()==QAbstractSocket::ConnectedState)<<Qt::endl;
            });
            QTimer::singleShot(12000,&app,[&] {
                if(packetCounts.isEmpty() || (decodeMode && !frameDecoded)) resultCode=1;
                if(secondAppMode && (!secondRequested || !secondOffered)) resultCode=1;
                stop();
            });
        });
    });
    QObject::connect(&discovery,&PhoneDiscovery::error,&app,error);
    QObject::connect(&manager,&ServiceManager::error,&app,error);
    QObject::connect(&manager,&ServiceManager::disconnected,&app,[&] {
        QTextStream(stdout)<<"Shared manager disconnected"<<Qt::endl;
        if(stopping) {
            QTextStream(stdout)<<"Phone closed association transport during cleanup; acknowledgement not observed"<<Qt::endl;
        }
        finish(1);
    });
    QObject::connect(&deadline,&QTimer::timeout,&app,[&] {
        if(associated && !stopping) { error("Association timed out; attempting cleanup"); return; }
        error(stopping ? "Association cleanup acknowledgement timed out" : "Association response timed out");
    });
    QObject::connect(&discovery,&PhoneDiscovery::found,&app,[&](QJsonObject device) {
        if(castMode) return;
        if(managerStarted || finished) return;
        const QUrl endpoint(device.value("socketUri").toString());
        if(endpoint.scheme()!="socket" || QHostAddress(endpoint.host())!=phone || endpoint.port()<=0 ||
           endpoint.port()>65535 || !endpoint.userInfo().isEmpty()) { error("Invalid manager endpoint"); return; }
        managerStarted=true;
        manager.start(phone,quint16(endpoint.port()),device.value("deviceId").toString(),discovery.localId());
    });
    auto request=[&] {
        if(requested || finished || !manager.ready()) return;
        bool available=false;
        for(const auto &service:manager.services()) if(service.value("serviceUuid")==uuid) {
            if(service.value("authType").toInt()!=0) { error("AppRelay requires additional authentication"); return; }
            available=service.value("enable").toBool();
        }
        if(!available) return;
        if(offerMode) {
            bool sourceAvailable=false;
            for(const auto &service:manager.services()) if(service.value("serviceUuid")==sourceUuid)
                sourceAvailable=service.value("enable").toBool() && service.value("authType").toInt()==0;
            if(!sourceAvailable) return;
            const QJsonObject sinkPublication{{"serviceName","ScreenCastSink"},{"serviceUuid",sinkUuid},
                {"serviceVersion",9},{"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}};
            if(!manager.send("distributebus.action.publishservice","1",sinkPublication)) {
                error("Cannot publish window sink"); return;
            }
        }
        const QJsonObject publication{{"serviceName","RELAY_SERVICE"},{"serviceUuid",uuid},{"serviceVersion",22},
            {"enable",true},{"authType",0},{"custData","{}"},{"serviceType","publish"}};
        if(!manager.send("distributebus.action.publishservice","1",publication)) { error("Cannot publish relay service"); return; }
        if(castMode) {
            requested=true;
            QTextStream(stdout)<<"Relay published using active casting identity; waiting for phone-initiated connect_check"<<Qt::endl;
            deadline.start(45000); return;
        }
        requested=manager.sendProfile("distributebus.action.startservice",uuid,
            "connect_check:::Nubia Link Linux:::"+localId());
        if(!requested) { error("Cannot send association request"); return; }
        QTextStream(stdout)<<"Official association requested with existing role 10 and empty account"<<Qt::endl;
        deadline.start();
    };
    QObject::connect(&manager,&ServiceManager::connected,&app,[&] { if(!castMode) request(); });
    bool videoObserved=false;
    QObject::connect(&manager,&ServiceManager::servicePublished,&app,[&] { if(!castMode || videoObserved) request(); });
    QObject::connect(&media,&ProjectionSession::videoPacket,&app,[&] {
        if(!castMode || videoObserved) return;
        videoObserved=true;
        QTextStream(stdout)<<"Normal RMCast video received; publishing collaboration services"<<Qt::endl;
        request();
    });
    QObject::connect(&cast,&LanProjection::status,&app,[&](QString message) {
        if(!castMode || finished) return;
        QTextStream(stdout)<<message<<Qt::endl;
        if(!cast.running()) error("Casting stopped before collaboration probe completed");
    });
    auto requestWindow=[&] {
        if(stopping || finished || appRequested) return;
        appRequested=manager.send("distributebus.action.startservice",sourceUuid,
            {{"castDisplayType","virtualDisplay"},{"pkg","com.android.settings"}});
        if(!appRequested) { error("Cannot request Settings window"); return; }
        QTextStream(stdout)<<"Official Settings virtual-display request: sent"<<Qt::endl;
        deadline.start(castMode ? 120000 : 30000);
    };
    QObject::connect(&manager,&ServiceManager::message,&app,[&](QJsonObject rpc) {
        if(finished || !requested) return;
        const auto action=rpc.value("action").toString();
        if(secondAppMode && secondRequested && rpc.value("uuid")==sinkUuid &&
           action=="distributebus.action.startservice") {
            const auto offer=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
            if((offer.value("pkg").isString() && offer.value("pkg")!="com.android.calculator2") ||
               offer.value("castDisplayType")!="virtualDisplay") {
                error("Unexpected second application offer"); return;
            }
            secondOffered=true;
            QTextStream(stdout)<<"Second application offered; first control connected="
                <<(control.state()==QAbstractSocket::ConnectedState)<<", package field present="
                <<offer.value("pkg").isString()<<Qt::endl;
            // Observe the phone's suspend/reuse behavior before rejecting the
            // new offer. Rejection itself cannot prove a parallel-session limit.
            QTimer::singleShot(5000,&app,[&] {
                if(finished || stopping) return;
                const QJsonObject rejection{{"resultCode",2},{"isCasting",true},{"castServiceVersion",9},
                    {"reason","Research probe: second receiver not accepted"}};
                manager.send("distributebus.action.onstartservicecallback",sinkUuid,
                    {{"result",false},{"CallBackServiceInParam",QString::fromUtf8(QJsonDocument(rejection).toJson(QJsonDocument::Compact))}});
                manager.flush();
                QTimer::singleShot(500,&app,stop);
            });
            return;
        }
        if(castMode && rpc.value("uuid")==sourceUuid) {
            QTextStream(stdout)<<"Source control action: "<<action<<Qt::endl;
        }
        if(transportMode && offerReceived && rpc.value("uuid")==sinkUuid &&
           action=="distributebus.action.sendnotification") {
            const auto notification=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
            const auto parameters=QJsonDocument::fromJson(notification.value("profile").toString().toUtf8()).object();
            const auto command=notification.value("command").toInt();
            QTextStream(stdout)<<"DistBus transport notification: command="<<command<<", type="<<parameters.value("type").toInt()<<Qt::endl;
            if(command==1) {
                if(parameters.value("type").toInt()!=10) { error("Unexpected transport combination"); return; }
                const auto acceptance=QString::fromUtf8(QJsonDocument(QJsonObject{{"v",2},{"fillWifiParam",1}}).toJson(QJsonDocument::Compact));
                if(!manager.send(action,sinkUuid,{{"command",2},{"profile",acceptance}})) {
                    error("Cannot acknowledge transport initiation"); return;
                }
                deadline.start(30000);
            } else if(command==3 && !transportObserved) {
                const auto endpoint=QJsonDocument::fromJson(parameters.value("msgTransferProfile").toString().toUtf8()).object();
                const auto stream=parameters.value("streamTransferProfile").toString();
                QTextStream(stdout)<<"Transport endpoints offered: control port="<<endpoint.value("port").toInt()
                    <<", stream profile format="<<(stream.startsWith("ip=") ? "IP/port query" : "legacy IP")<<Qt::endl;
                transportObserved=true;
                if(packetMode) {
                    const QUrlQuery query(stream);
                    bool validPort=false;
                    const int streamPort=query.queryItemValue("port").toInt(&validPort);
                    const int controlPort=endpoint.value("port").toInt();
                    if(QHostAddress(endpoint.value("ip").toString())!=phone ||
                       QHostAddress(query.queryItemValue("ip"))!=phone || !validPort ||
                       streamPort<1024 || streamPort>65526 || controlPort<1 || controlPort>65535 ||
                       query.hasQueryItem("tcp")) { error("Unexpected media/control endpoints"); return; }
                    for(int offset=0;offset<10;++offset) {
                        auto *socket=new QUdpSocket(&app); udp.append(socket);
                        socket->setProxy(QNetworkProxy::NoProxy);
                        if(!socket->bind(local,quint16(streamPort+offset),QUdpSocket::DontShareAddress)) {
                            error("Cannot bind offered media ports"); return;
                        }
                        QObject::connect(socket,&QUdpSocket::readyRead,&app,[&,socket,offset] {
                            while(socket->hasPendingDatagrams()) {
                                const auto datagram=socket->receiveDatagram();
                                if(datagram.senderAddress()!=phone) continue;
                                const auto bytes=datagram.data();
                                const int count=++packetCounts[offset];
                                if(count==1) {
                                    QTextStream output(stdout);
                                    output<<"Media packet: offset="<<offset<<", size="<<bytes.size();
                                    if(bytes.size()>=12 && (quint8(bytes[0])>>6)==2)
                                        output<<", RTP-v2 candidate type="<<(quint8(bytes[1])&127);
                                    output<<Qt::endl;
                                }
                            }
                        });
                    }
                    QTextStream(stdout)<<"Media base port="<<streamPort<<Qt::endl;
                    mediaPort=streamPort;
                    control.connectToHost(phone,quint16(controlPort));
                    return;
                }
                if(!manager.send(action,sinkUuid,{{"command",4},{"profile",""}})) { error("Cannot reject transport"); return; }
                manager.flush();
                QTextStream(stdout)<<"Official transport rejection sent; media decoding not implemented"<<Qt::endl;
                QTimer::singleShot(1500,&app,stop);
            }
            return;
        }
        if(offerMode && appRequested && rpc.value("uuid")==sinkUuid &&
           action=="distributebus.action.startservice" && !offerReceived) {
            const auto offer=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8());
            if(!offer.isObject() || offer.object().value("castDisplayType")!="virtualDisplay" ||
               (offer.object().value("pkg").isString() && offer.object().value("pkg")!="com.android.settings")) {
                error("Unexpected virtual window offer"); return;
            }
            offerReceived=true;
            const auto metadata=offer.object();
            // Only non-sensitive media metadata is printed. Never serialize an
            // offer: it may contain phone identifiers, account information/PIN.
            QTextStream(stdout)<<"Virtual-display offer following Settings request: "<<metadata.value("width").toInt()<<" x "
                <<metadata.value("height").toInt()<<", service version "<<metadata.value("csv").toInt()<<Qt::endl;
            const auto media=QJsonDocument::fromJson(metadata.value("profile").toString().toUtf8()).object();
            if(!media.isEmpty()) QTextStream(stdout)<<"Media capabilities: codec="<<media.value("type").toInt()
                <<", abilities="<<media.value("abilities").toInt()<<", fps="<<media.value("fps").toInt()
                <<", audio channels="<<media.value("chNum").toInt()<<Qt::endl;
            if(transportMode) {
                if((media.value("abilities").toInt()&2)==0) { error("Phone cannot encode H264"); return; }
                const auto capabilities=QString::fromUtf8(QJsonDocument(QJsonObject{{"type",2},{"abilities",2},
                    {"fps",30},{"chNum",2},{"isEncoder",false}}).toJson(QJsonDocument::Compact));
                const QJsonObject response{{"deviceName","Nubia Link Linux"},{"deviceModel","10"},
                    {"deviceWidth",1080},{"deviceHeight",2400},{"densityDpi",0},{"resultCode",0},
                    {"isCasting",false},{"castServiceVersion",9},{"pkg","com.android.settings"},{"profile",capabilities}};
                if(!manager.send("distributebus.action.onstartservicecallback",sinkUuid,
                    {{"result",true},{"CallBackServiceInParam",QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))}})) {
                    error("Cannot negotiate window capabilities"); return;
                }
                manager.flush(); deadline.start(30000);
                QTextStream(stdout)<<"H264/30fps capabilities acknowledged; waiting for DistBus transport initiation"<<Qt::endl;
                return;
            }
            const QJsonObject rejection{{"deviceName","Nubia Link Linux"},{"deviceModel","10"},
                {"deviceWidth",0},{"deviceHeight",0},{"resultCode",2},{"isCasting",false},
                {"castServiceVersion",9},{"reason","Linux media transport not implemented"}};
            const auto profile=QString::fromUtf8(QJsonDocument(rejection).toJson(QJsonDocument::Compact));
            if(!manager.send("distributebus.action.onstartservicecallback",sinkUuid,
                             {{"result",false},{"CallBackServiceInParam",profile}})) {
                error("Cannot reject media offer"); return;
            }
            manager.flush();
            QTextStream(stdout)<<"Official media offer rejection: sent (no media session accepted)"<<Qt::endl;
            QTimer::singleShot(1500,&app,stop);
            return;
        }
        if(rpc.value("uuid")!=uuid) return;
        if(castMode && action=="distributebus.action.startservice" && !associated) {
            const auto control=rpc.value("profile").toString();
            const auto fields=control.split(":::");
            if(fields.size()!=3 || fields[0]!="connect_check" || fields[2].isEmpty()) {
                QTextStream(stdout)<<"Other relay command received before association"<<Qt::endl; return;
            }
            if(!manager.send("distributebus.action.onstartservicecallback",uuid,
                             {{"result",true},{"CallBackServiceInParam","ready_to_connect"}})) {
                error("Cannot acknowledge phone-initiated association"); return;
            }
            associated=true; manager.flush();
            QTextStream(stdout)<<"Phone-initiated association check acknowledged; complete association still requires phone confirmation"<<Qt::endl;
            QTimer::singleShot(1500,&app,requestWindow);
            return;
        }
        const auto callback=QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object();
        const auto profile=callback.value("CallBackServiceInParam").toString();
        if(action=="distributebus.action.onstartservicecallback" && !associated) {
            if(callback.value("result").toBool() && profile=="ready_to_connect") {
                associated=true; deadline.start();
                QTextStream(stdout)<<"Official association: ready_to_connect (media/PIN authentication still unverified)"<<Qt::endl;
                if(!offerMode) QTimer::singleShot(500,&app,stop);
                else QTimer::singleShot(500,&app,[&] {
                    if(stopping || finished) return;
                    appRequested=manager.send("distributebus.action.startservice",sourceUuid,
                        {{"castDisplayType","virtualDisplay"},{"pkg","com.android.settings"}});
                    if(!appRequested) { error("Cannot request Settings window"); return; }
                    QTextStream(stdout)<<"Official Settings virtual-display request: sent"<<Qt::endl;
                    deadline.start(30000);
                });
            } else {
                QTextStream(stdout)<<"Association not created: "<<(profile=="connected" ? "phone busy or trust requirement unmet" : "unexpected/rejected response")<<Qt::endl;
                finish(1); // Never disconnect a peer's pre-existing association.
            }
        } else if(action=="distributebus.action.onstopservicecallback" && stopping &&
                  callback.value("result").toBool() && profile=="disconnect") {
            QTextStream(stdout)<<"Official association cleanup acknowledgement: success"<<Qt::endl;
            // Keep the socket briefly so the phone's normal reset callbacks run.
            QTimer::singleShot(500,&app,[&] { finish(resultCode || (offerMode && !offerReceived) ||
                (transportMode && !transportObserved) || (secondAppMode && (!secondRequested || !secondOffered)) ? 1 : 0); });
        }
    });
    deadline.start(castMode ? 120000 : 15000);
    if(castMode ? !cast.start(local,phone) : !discovery.start(local,phone)) return 1;
    return app.exec();
}
