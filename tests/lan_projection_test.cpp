#include "lan_projection.h"
#include <QtTest>
#include <QUdpSocket>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QUrl>
#include <QtEndian>

namespace {
const QString CastUuid="187f8000-0000-1000-8000-ceb574816100";
QString json(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
QJsonObject rpc(QString action,QString uuid,QJsonObject profile) { return {{"action",action},{"uuid",uuid},{"profile",json(profile)}}; }
QJsonObject profile(const QJsonObject &message) { return QJsonDocument::fromJson(message.value("profile").toString().toUtf8()).object(); }
struct Fixture {
    ProjectionSession media;
    LanProjection lan{media};
    QUdpSocket coap;
    QTcpServer manager;
    QTcpSocket *peer=nullptr;
    DistBus::RpcReader reader;
    QList<QJsonObject> requests;
    Fixture() { coap.setProxy(QNetworkProxy::NoProxy); manager.setProxy(QNetworkProxy::NoProxy); }
    void open() {
        QVERIFY(coap.bind(QHostAddress::LocalHost,0)); QVERIFY(manager.listen(QHostAddress::LocalHost));
        QVERIFY(lan.start(QHostAddress::LocalHost,QHostAddress::LocalHost,coap.localPort()));
        QTRY_VERIFY(coap.hasPendingDatagrams());
        QByteArray request(int(coap.pendingDatagramSize()),Qt::Uninitialized); coap.readDatagram(request.data(),request.size());
        // The message ID can contain 0xff; locate payload with libcoap rather
        // than mistaking arbitrary header bytes for the payload marker.
        std::unique_ptr<coap_pdu_t,decltype(&coap_delete_pdu)> pdu(
            coap_pdu_init(COAP_MESSAGE_NON,COAP_EMPTY_CODE,0,size_t(request.size())),&coap_delete_pdu);
        QVERIFY(pdu); QVERIFY(coap_pdu_parse(COAP_PROTO_UDP,reinterpret_cast<const uint8_t *>(request.constData()),size_t(request.size()),pdu.get()));
        size_t length=0; const uint8_t *data=nullptr; QVERIFY(coap_get_data(pdu.get(),&length,&data));
        const auto discover=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data),qsizetype(length))).object();
        QVERIFY(!discover.value("deviceId").toString().isEmpty());
        const QUrl callback(discover.value("coapUri").toString());
        QByteArray response=QByteArray::fromHex("50021234bd02"); response+="device_discover"; response+=char(255);
        response+=json({{"deviceId","phone"},{"protocol","ZLINK"},
            {"socketUri",QString("socket://127.0.0.1:%1").arg(manager.serverPort())}}).toUtf8();
        QVERIFY(coap.writeDatagram(response,QHostAddress::LocalHost,quint16(callback.port()))>0);
        QTRY_VERIFY(manager.hasPendingConnections()); peer=manager.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable()>6); QVERIFY(reader.append(peer->readAll(),requests));
        QCOMPARE(requests.size(),1); QCOMPARE(profile(requests[0]).value("a"),discover.value("deviceId")); requests.clear();
        const auto published=rpc("distributebus.action.publishservice","1",{{"serviceUuid",CastUuid},{"enable",true},{"authType",0}});
        const auto hello=rpc("connectDevice","requestService",{{"a","phone"},{"b","Phone"},{"c",4},{"p",0}});
        peer->write(DistBus::frame(published)+DistBus::frame(hello));
        QTRY_VERIFY(peer->bytesAvailable()>6);
        QTRY_VERIFY(([&] { if(!reader.append(peer->readAll(),requests)) return false; return requests.size()==2; })());
        QCOMPARE(requests[0].value("action").toString(),QString("distributebus.action.publishservice"));
        QCOMPARE(profile(requests[0]).value("serviceUuid").toString(),CastUuid);
        QCOMPARE(requests[1].value("action").toString(),QString("distributebus.action.startservice"));
        QCOMPARE(profile(requests[1]).value("connectType").toInt(),2);
        QCOMPARE(profile(requests[1]).value("wifiParam").toObject().value("port").toString(),QString::number(media.port()));
        requests.clear();
    }
};
}
class LanProjectionTest:public QObject {
    Q_OBJECT
private slots:
    void officialLanSequenceAndDisconnect() {
        Fixture f; f.open(); QVERIFY(f.peer);
        f.peer->write(DistBus::frame(rpc("distributebus.action.startservice","unrelated",{{"appversion","15.1"}})));
        QTest::qWait(30); QCOMPARE(f.peer->bytesAvailable(),qint64(0));
        f.peer->write(DistBus::frame(rpc("distributebus.action.startservice",CastUuid,{{"appversion","15.1"}})));
        QTRY_VERIFY(f.peer->bytesAvailable()>6); QVERIFY(f.reader.append(f.peer->readAll(),f.requests)); QCOMPARE(f.requests.size(),1);
        QCOMPARE(f.requests[0].value("action").toString(),QString("distributebus.action.onstartservicecallback"));
        const auto callback=profile(f.requests[0]); QVERIFY(callback.value("result").toBool());
        const auto parameters=QJsonDocument::fromJson(callback.value("CallBackServiceInParam").toString().toUtf8()).object();
        QCOMPARE(parameters.value("connectType").toInt(),2);
        QCOMPARE(parameters.value("wifiParam").toObject().value("ip").toString(),QString("127.0.0.1"));
        QSignalSpy video(&f.media,&ProjectionSession::videoPacket);
        QTcpSocket sockets[3];
        for(auto &socket:sockets) { socket.setProxy(QNetworkProxy::NoProxy); socket.connectToHost(QHostAddress::LocalHost,f.media.port()); QTRY_COMPARE(socket.state(),QAbstractSocket::ConnectedState); QTest::qWait(10); }
        QTRY_VERIFY(f.media.connected()); QVERIFY(f.lan.running());
        QByteArray packet(12,Qt::Uninitialized); qToBigEndian<qint64>(-1,packet.data()); qToBigEndian<quint32>(3,packet.data()+8); packet+="SPS";
        sockets[0].write(packet); QTRY_COMPARE(video.size(),1);
        const QByteArray audioStart=QByteArray::fromHex("0c0017")+"{\"set_sound_control\":2}";
        QTRY_COMPARE(sockets[2].bytesAvailable(),qint64(audioStart.size())); QCOMPARE(sockets[2].readAll(),audioStart);
        f.peer->disconnectFromHost(); QTRY_VERIFY(!f.lan.running()); QCOMPARE(f.media.port(),quint16(0));
        for(auto &socket:sockets) QTRY_COMPARE(socket.state(),QAbstractSocket::UnconnectedState);
    }
    void pinRequestIsNotAutoAccepted() {
        Fixture f; f.open(); QVERIFY(f.peer);
        f.peer->write(DistBus::frame(rpc("distributebus.action.startservice",CastUuid,{{"appversion","15.1"},{"PIN","123456"}})));
        QTRY_VERIFY(!f.lan.running()); QCOMPARE(f.media.port(),quint16(0));
        QTRY_COMPARE(f.peer->state(),QAbstractSocket::UnconnectedState);
        QCOMPARE(f.peer->bytesAvailable(),qint64(0));
    }
    void handoverKeepsManagerUntilExplicitStop() {
        Fixture f; f.lan.preserveControlForHandover(true); f.open(); QVERIFY(f.peer);
        const auto identity=f.lan.localId();
        QTcpSocket sockets[3];
        for(auto &socket:sockets) {
            socket.setProxy(QNetworkProxy::NoProxy);
            socket.connectToHost(QHostAddress::LocalHost,f.media.port());
            QTRY_COMPARE(socket.state(),QAbstractSocket::ConnectedState);
        }
        QTRY_VERIFY(f.media.connected());
        sockets[0].disconnectFromHost(); QTRY_VERIFY(!f.media.connected());
        QVERIFY(f.lan.running()); QVERIFY(f.lan.serviceManager().ready());
        f.peer->write(DistBus::frame(rpc("distributebus.action.stopservice",CastUuid,{})));
        QTRY_COMPARE(f.media.port(),quint16(0));
        QVERIFY(f.lan.running()); QCOMPARE(f.lan.localId(),identity);
        QVERIFY(f.lan.serviceManager().sendProfile("distributebus.action.startservice","successor","connect_check"));
        QTRY_VERIFY(f.peer->bytesAvailable()>6);
        QVERIFY(f.reader.append(f.peer->readAll(),f.requests)); QCOMPARE(f.requests.size(),1);
        QCOMPARE(f.requests[0].value("uuid").toString(),QString("successor"));
        f.lan.stop(); QTRY_COMPARE(f.peer->state(),QAbstractSocket::UnconnectedState);
    }
    void cancelReleasesListener() {
        Fixture f; f.open(); QVERIFY(f.peer); const auto port=f.media.port();
        f.lan.stop(); QVERIFY(!f.lan.running()); QCOMPARE(f.media.port(),quint16(0));
        QTcpServer probe; probe.setProxy(QNetworkProxy::NoProxy); QVERIFY(probe.listen(QHostAddress::LocalHost,port));
    }
    void rejectsOtherPeerAddress() {
        ProjectionSession media; QVERIFY(media.listenNetwork(QHostAddress::LocalHost,QHostAddress("127.0.0.2")));
        QTcpSocket wrong; wrong.setProxy(QNetworkProxy::NoProxy); wrong.connectToHost(QHostAddress::LocalHost,media.port());
        QTRY_COMPARE(wrong.state(),QAbstractSocket::UnconnectedState); QVERIFY(!media.connected());
    }
};
QTEST_GUILESS_MAIN(LanProjectionTest)
#include "lan_projection_test.moc"
