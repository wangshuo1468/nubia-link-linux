#include "lan_projection.h"
#include "collaboration_session.h"
#include <QtTest>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QUrl>
#include <QSettings>
#include <QTemporaryDir>
#include <QtEndian>

namespace {
const QHostAddress Local("127.0.0.1"), Phone("127.0.0.2");
const QString Cast="187f8000-0000-1000-8000-ceb574816100";
const QString Relay="187f8000-0000-1000-8000-ceb574815500";
QString json(QJsonObject value) { return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)); }
QJsonObject rpc(QString action,QString uuid,QString profile) { return {{"action",action},{"uuid",uuid},{"profile",profile}}; }
QJsonObject profile(QJsonObject rpc) { return QJsonDocument::fromJson(rpc.value("profile").toString().toUtf8()).object(); }
QByteArray discover(QJsonObject value) {
    QByteArray bytes=QByteArray::fromHex("50021234bd02"); bytes+="device_discover"; bytes+=char(255);
    return bytes+QJsonDocument(value).toJson(QJsonDocument::Compact);
}
quint16 availablePort() {
    QUdpSocket socket; socket.setProxy(QNetworkProxy::NoProxy);
    if(!socket.bind(Local,0)) return 0;
    return socket.localPort();
}
struct PhoneFixture {
    QUdpSocket discovery;
    QTcpSocket socket;
    DistBus::RpcReader reader;
    QList<QJsonObject> messages;
    QJsonObject metadata;
    PhoneFixture() { discovery.setProxy(QNetworkProxy::NoProxy); socket.setProxy(QNetworkProxy::NoProxy); }
    void connectTo(quint16 port,int role=10) {
        QVERIFY(discovery.bind(Phone,0));
        const QJsonObject identity{{"deviceId","phone-fixture"},{"deviceName","NX721J"},{"protocol","ZLINK"},{"devType",4},
            {"coapUri",QString("coap://127.0.0.2:%1/device_discover").arg(discovery.localPort())}};
        QVERIFY(discovery.writeDatagram(discover(identity),Local,port)>0);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            if(metadata.contains("socketUri")) return true;
            while(discovery.hasPendingDatagrams()) {
                const auto bytes=discovery.receiveDatagram().data();
                auto pdu=coap_pdu_init(COAP_MESSAGE_NON,COAP_EMPTY_CODE,0,size_t(bytes.size()));
                if(!pdu) return false;
                const bool parsed=coap_pdu_parse(COAP_PROTO_UDP,reinterpret_cast<const uint8_t *>(bytes.constData()),size_t(bytes.size()),pdu);
                size_t length=0; const uint8_t *data=nullptr;
                if(parsed && coap_get_data(pdu,&length,&data)) metadata=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data),qsizetype(length))).object();
                coap_delete_pdu(pdu);
                if(metadata.contains("socketUri")) return true;
            }
            return false;
        })(),2000);
        QCOMPARE(metadata.value("devType").toInt(),role); QVERIFY(!metadata.contains("coapUri"));
        QVERIFY(metadata.value("accountHash").toString().isEmpty());
        const QUrl endpoint(metadata.value("socketUri").toString());
        QVERIFY(socket.bind(Phone,0)); socket.connectToHost(Local,quint16(endpoint.port()));
        QTRY_COMPARE(socket.state(),QAbstractSocket::ConnectedState);
        socket.write(DistBus::frame(rpc("connectDevice","requestService",json({{"a","phone-fixture"},{"b","NX721J"},{"c",4},{"p",0}}))));
        QTRY_VERIFY(read()>0);
        QCOMPARE(messages.front().value("action").toString(),QString("connectDevice"));
        QCOMPARE(profile(messages.front()).value("a"),metadata.value("deviceId"));
        QCOMPARE(profile(messages.front()).value("c").toInt(),role);
    }
    int read() {
        if(!reader.append(socket.readAll(),messages)) return -1;
        return int(messages.size());
    }
    bool has(QString action,QString uuid) {
        read(); for(const auto &message:messages) if(message.value("action")==action && message.value("uuid")==uuid) return true;
        return false;
    }
};
}
class ReceiverTest:public QObject {
    Q_OBJECT
    QTemporaryDir configuration_;
private slots:
    void initTestCase() {
        QVERIFY(configuration_.isValid()); QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,configuration_.path());
        QCoreApplication::setOrganizationName("NubiaLinkTests");
    }
    void castAutomaticallyAcceptsPhone() {
        ProjectionSession media; LanProjection lan(media);
        const auto port=availablePort(); QVERIFY(port); QVERIFY(lan.listen(Local,8,"lo",port));
        PhoneFixture phone; phone.connectTo(port);
        QTRY_VERIFY(phone.has("distributebus.action.publishservice","1"));
        // Advertising and a manager handshake alone do not open media ports.
        QCOMPARE(media.port(),quint16(0)); phone.messages.clear();
        phone.socket.write(DistBus::frame(rpc("distributebus.action.startservice",Cast,json({{"appversion","15.1"}}))));
        QTRY_VERIFY(phone.has("distributebus.action.onstartservicecallback",Cast));
        QJsonObject callback; for(const auto message:phone.messages) if(message.value("uuid")==Cast) callback=profile(message);
        QVERIFY(callback.value("result").toBool()); QVERIFY(media.port());
        const auto parameters=QJsonDocument::fromJson(callback.value("CallBackServiceInParam").toString().toUtf8()).object();
        QCOMPARE(parameters.value("connectType").toInt(),2);
        QCOMPARE(parameters.value("wifiParam").toObject().value("port").toString(),QString::number(media.port()));
        lan.stop(); QCOMPARE(media.port(),quint16(0));
        QTRY_COMPARE(phone.socket.state(),QAbstractSocket::UnconnectedState);
        QUdpSocket probe; probe.setProxy(QNetworkProxy::NoProxy); QVERIFY(probe.bind(Local,port));
    }
    void collaborationAutomaticallyAcceptsPhone() {
        const QString sink="187f8000-0000-1000-8000-ceb574815100";
        CollaborationSession session;
        const auto port=availablePort(); QVERIFY(session.listen(Local,8,"lo",port));
        PhoneFixture phone; phone.connectTo(port,7);
        QTRY_VERIFY(phone.has("distributebus.action.publishservice","1"));
        bool hasSink=false,hasRelay=false;
        for(const auto value:phone.metadata.value("custData").toArray()) {
            const auto service=value.toObject(); const auto uuid=service.value("serviceUUid").toString();
            QVERIFY(uuid!=Cast);
            if(uuid==sink) { hasSink=true; QCOMPARE(service.value("serviceName").toString(),QString("MultiScrService")); }
            hasRelay|=uuid==Relay;
        }
        QVERIFY(hasSink); QVERIFY(hasRelay); hasSink=hasRelay=false;
        for(const auto message:phone.messages) {
            QVERIFY(message.value("uuid")!=Cast);
            if(message.value("action")!="distributebus.action.publishservice") continue;
            const auto service=profile(message); const auto uuid=service.value("serviceUuid").toString();
            QVERIFY(uuid!=Cast);
            if(uuid==sink) { hasSink=true; QCOMPARE(service.value("serviceName").toString(),QString("MultiScrService")); }
            hasRelay|=uuid==Relay;
        }
        QVERIFY(hasSink); QVERIFY(hasRelay); phone.messages.clear();
        // A direct modern offer connects without RMCast or desktop approval.
        phone.socket.write(DistBus::frame(rpc("distributebus.action.startservice",sink,
            json({{"castDisplayType","mirror"},{"profile",json({{"abilities",3},{"type",4}})}}))));
        QTRY_VERIFY(phone.has("distributebus.action.onstartservicecallback",sink));
        QJsonObject callback; for(const auto message:phone.messages) if(message.value("uuid")==sink) callback=profile(message);
        QVERIFY(callback.value("result").toBool());
        const auto answer=QJsonDocument::fromJson(callback.value("CallBackServiceInParam").toString().toUtf8()).object();
        QCOMPARE(answer.value("deviceModel").toString(),QString("7"));
        phone.messages.clear();
        phone.socket.write(DistBus::frame(rpc("distributebus.action.sendnotification",sink,
            json({{"command",1},{"profile",json({{"type",10}})}}))));
        QTRY_VERIFY(phone.has("distributebus.action.sendnotification",sink));
        phone.socket.write(DistBus::frame(rpc("distributebus.action.startservice",Relay,"connect_check:::NX721J:::phone-fixture")));
        QTRY_VERIFY(phone.has("distributebus.action.onstartservicecallback",Relay));
        phone.socket.disconnectFromHost(); QTRY_VERIFY(!session.running());
        QUdpSocket probe; probe.setProxy(QNetworkProxy::NoProxy); QVERIFY(probe.bind(Local,port));
    }
    void collaborationOverlappingAssociationAndOffer() {
        const QString sink="187f8000-0000-1000-8000-ceb574815100";
        CollaborationSession session;
        const auto port=availablePort(); QVERIFY(session.listen(Local,8,"lo",port));
        PhoneFixture phone; phone.connectTo(port,7); phone.messages.clear();
        phone.socket.write(DistBus::frame(rpc("distributebus.action.startservice",Relay,"connect_check:::NX721J:::phone-fixture")));
        phone.socket.write(DistBus::frame(rpc("distributebus.action.startservice",sink,
            json({{"castDisplayType","virtualDisplay"},{"pkg","com.android.settings"},{"profile",json({{"abilities",3},{"type",4}})}}))));
        QTRY_VERIFY(phone.has("distributebus.action.onstartservicecallback",Relay));
        QTRY_VERIFY(phone.has("distributebus.action.onstartservicecallback",sink));
        int relayReplies=0,sinkReplies=0;
        for(const auto message:phone.messages) {
            QVERIFY(message.value("uuid")!=Cast);
            if(message.value("action")!="distributebus.action.onstartservicecallback") continue;
            if(message.value("uuid")==Relay) ++relayReplies;
            if(message.value("uuid")==sink) ++sinkReplies;
        }
        QCOMPARE(relayReplies,1); QCOMPARE(sinkReplies,1);
        phone.socket.disconnectFromHost(); QTRY_VERIFY(!session.running());
    }
    void computerCollaborationDiscoveryAndHandshake() {
        QUdpSocket discovery; discovery.setProxy(QNetworkProxy::NoProxy); QVERIFY(discovery.bind(Phone,0));
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(Phone));
        CollaborationLink link; QSignalSpy ready(&link,&CollaborationLink::ready);
        QVERIFY(link.start(Local,Phone,discovery.localPort()));
        QTRY_VERIFY(discovery.hasPendingDatagrams());
        const auto bytes=discovery.receiveDatagram().data();
        auto pdu=coap_pdu_init(COAP_MESSAGE_NON,COAP_EMPTY_CODE,0,size_t(bytes.size())); QVERIFY(pdu);
        QVERIFY(coap_pdu_parse(COAP_PROTO_UDP,reinterpret_cast<const uint8_t *>(bytes.constData()),size_t(bytes.size()),pdu));
        size_t length=0; const uint8_t *data=nullptr; QVERIFY(coap_get_data(pdu,&length,&data));
        const auto identity=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data),qsizetype(length))).object();
        coap_delete_pdu(pdu);
        QCOMPARE(identity.value("devType").toInt(),7); QVERIFY(identity.value("accountHash").toString().isEmpty());
        QCOMPARE(identity.value("vendor").toString(),QString("Linux"));
        const auto id=identity.value("deviceId").toString(); QVERIFY(!id.isEmpty());
        QCOMPARE(id,QSettings().value("collaboration/deviceId").toString());
        const QUrl callback(identity.value("coapUri").toString());
        QVERIFY(discovery.writeDatagram(discover({{"deviceId","phone-fixture"},{"deviceName","NX721J"},{"protocol","ZLINK"},
            {"socketUri",QString("socket://127.0.0.2:%1").arg(server.serverPort())}}),Local,quint16(callback.port()))>0);
        QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
        QTRY_VERIFY(peer->bytesAvailable()>6);
        DistBus::RpcReader reader; QList<QJsonObject> messages; QVERIFY(reader.append(peer->readAll(),messages));
        QCOMPARE(messages.size(),1); QCOMPARE(messages.front().value("action").toString(),QString("connectDevice"));
        QCOMPARE(profile(messages.front()).value("c").toInt(),7); QCOMPARE(profile(messages.front()).value("a").toString(),id);
        QVERIFY(profile(messages.front()).value("d").toString().isEmpty());
        peer->write(DistBus::frame(rpc("connectDevice","requestService",json({{"a","phone-fixture"},{"b","NX721J"},{"c",4},{"p",0}}))));
        QTRY_COMPARE(ready.count(),1); QVERIFY(link.manager().ready());
        // Management connection alone starts neither RMCast nor a media session.
        QTest::qWait(50); QCOMPARE(peer->bytesAvailable(),qint64(0));
        link.stop(); QTRY_COMPARE(peer->state(),QAbstractSocket::UnconnectedState);
    }
    void discoveryRejectsForgedCallback() {
        ReceiverDiscovery receiver; const auto port=availablePort(); QSignalSpy found(&receiver,&ReceiverDiscovery::peerFound);
        QVERIFY(receiver.start(Local,8,"lo",30001,{},port));
        QUdpSocket phone; phone.setProxy(QNetworkProxy::NoProxy); QVERIFY(phone.bind(Phone,0));
        QVERIFY(phone.writeDatagram(discover({{"deviceId","phone-fixture"},{"protocol","ZLINK"},{"coapUri","coap://127.0.0.3:30000/device_discover"}}),Local,port)>0);
        QTest::qWait(100); QVERIFY(found.isEmpty()); QVERIFY(receiver.peer(Phone).isEmpty()); receiver.stop();
    }
    void incomingEncryptedHandshakeAndProfiles() {
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(Local));
        QTcpSocket phone; phone.setProxy(QNetworkProxy::NoProxy); phone.connectToHost(Local,server.serverPort());
        QTRY_VERIFY(server.hasPendingConnections()); ServiceManager manager;
        QSignalSpy errors(&manager,&ServiceManager::error),connected(&manager,&ServiceManager::connected);
        QSignalSpy published(&manager,&ServiceManager::servicePublished),messages(&manager,&ServiceManager::message);
        QVERIFY(manager.accept(server.nextPendingConnection(),"phone-fixture","linux",DesktopRole::Collaboration));
        // Independent Python cryptography AES/CTR vectors using the SDK constants.
        phone.write(DistBus::frame(rpc("distributebus.action.publishservice","1","SyWIjVFWFxmuNh6oB8Cm02Qs1W5in29CdRlCVE/dvPGhERkF")));
        phone.write(DistBus::frame(rpc("connectDevice","requestService",json({{"a","phone-fixture"},{"b","NX721J"},{"c",4},{"p",1}}))));
        QTRY_COMPARE(connected.count(),1); QTRY_COMPARE(published.count(),1); QVERIFY(manager.ready());
        QCOMPARE(manager.services().front().value("serviceUuid").toString(),QString("test"));
        DistBus::RpcReader reader; QList<QJsonObject> replies;
        QTRY_VERIFY(phone.bytesAvailable()>6); QVERIFY(reader.append(phone.readAll(),replies));
        QCOMPARE(profile(replies.front()).value("p").toInt(),1);
        QCOMPARE(profile(replies.front()).value("c").toInt(),7);
        phone.write(DistBus::frame(rpc("distributebus.action.startservice",Relay,"U2iVhkZDCiWoCw6iCNimy14RkShx+XcdIQhIV0Saq+O6HAgNsI4=")));
        QTRY_COMPARE(messages.count(),1);
        QCOMPARE(messages.front().front().toJsonObject().value("profile").toString(),QString("connect_check:::NX721J:::phone-fixture"));
        QVERIFY(manager.sendProfile("distributebus.action.startservice","dfs",QStringLiteral("remote_ready_mount_me=Linux设备")));
        replies.clear(); QTRY_VERIFY(phone.bytesAvailable()>6); QVERIFY(reader.append(phone.readAll(),replies));
        QCOMPARE(replies.front().value("profile").toString(),QString("QmKWh1dFIQiuAg+4PI/zhH49+XcljgFOdQ1Y0IRBYyFU"));
        QVERIFY(errors.isEmpty());
        phone.write(DistBus::frame(rpc("distributebus.action.startservice",Relay,"*invalid-base64*")));
        QTRY_COMPARE(errors.count(),1); QTRY_COMPARE(phone.state(),QAbstractSocket::UnconnectedState);
        QVERIFY(!manager.ready());
    }
};
QTEST_GUILESS_MAIN(ReceiverTest)
#include "receiver_test.moc"
