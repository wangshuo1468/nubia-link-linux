#include "service_manager.h"
#include <QtTest>
#include <QTcpServer>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QtEndian>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
QString json(const QJsonObject &value) { return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)); }
QJsonObject hello(QString id="phone",int encryption=0) {
    return {{"action","connectDevice"},{"uuid","requestService"},
            {"profile",json({{"a",id},{"b",QStringLiteral("努比亚")},{"c",4},{"d",""},{"e",0},{"p",encryption}})}};
}
QJsonObject publication() {
    return {{"action","distributebus.action.publishservice"},
            {"profile",json({{"serviceUuid","187f8000-0000-1000-8000-ceb574815101"},
                             {"serviceName","MultiScrService"},{"enable",true},{"authType",0}})}};
}
}
class ServiceManagerTest:public QObject {
    Q_OBJECT
private slots:
    void outgoingNativeManagementStream() {
        int pair[2]; QCOMPARE(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair),0);
        ServiceManager manager; QSignalSpy errors(&manager,&ServiceManager::error);
        QVERIFY(manager.acceptRfcomm(pair[0],"phone","0123456789ab",QHostAddress("192.0.2.2"),"02:00:00:00:00:01",false));
        char bytes[4096]; const auto size=::recv(pair[1],bytes,sizeof(bytes),0); QVERIFY(size>0);
        DistBus::RpcReader reader; QList<QJsonObject> messages; QVERIFY(reader.append(QByteArray(bytes,size),messages));
        QCOMPARE(messages.size(),1); QCOMPARE(messages[0].value("action").toString(),QString("connectDevice"));
        QVERIFY(!manager.ready());
        const auto packet=DistBus::frame(hello());
        QCOMPARE(::send(pair[1],packet.constData(),size_t(packet.size()),MSG_NOSIGNAL),ssize_t(packet.size()));
        QTRY_VERIFY(manager.ready());
        const auto replySize=::recv(pair[1],bytes,sizeof(bytes),0); QVERIFY(replySize>0); messages.clear();
        QVERIFY(reader.append(QByteArray(bytes,replySize),messages));
        QCOMPARE(messages.size(),1); QCOMPARE(messages[0].value("action").toString(),QString("cmdSendIp"));
        QVERIFY(errors.isEmpty()); manager.stop(); ::close(pair[1]);
    }
    void nativeManagementStream() {
        int pair[2]; QCOMPARE(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair),0);
        ServiceManager manager; QSignalSpy ready(&manager,&ServiceManager::connected),errors(&manager,&ServiceManager::error),ended(&manager,&ServiceManager::disconnected);
        QVERIFY(manager.acceptRfcomm(pair[0],"phone","0123456789ab",QHostAddress("192.0.2.2"),"02:00:00:00:00:01"));
        const auto packet=DistBus::frame(hello());
        QCOMPARE(::send(pair[1],packet.constData(),3,MSG_NOSIGNAL),ssize_t(3)); QTest::qWait(10); QVERIFY(!manager.ready());
        QCOMPARE(::send(pair[1],packet.constData()+3,size_t(packet.size()-3),MSG_NOSIGNAL),ssize_t(packet.size()-3));
        QTRY_COMPARE(ready.count(),1);
        char bytes[4096]; const auto size=::recv(pair[1],bytes,sizeof(bytes),0); QVERIFY(size>0);
        DistBus::RpcReader reader; QList<QJsonObject> messages; QVERIFY(reader.append(QByteArray(bytes,size),messages));
        QCOMPARE(messages.size(),2); QCOMPARE(messages[0].value("action").toString(),QString("connectDevice"));
        const auto info=QJsonDocument::fromJson(messages[0].value("profile").toString().toUtf8()).object();
        QCOMPARE(info.value("c").toInt(),7); QCOMPARE(info.value("f").toString(),QString("02:00:00:00:00:01"));
        QCOMPARE(messages[1].value("action").toString(),QString("cmdSendIp"));
        QVERIFY(manager.sendProfile("custom-rpc","service",QStringLiteral("中文")));
        const auto rpcSize=::recv(pair[1],bytes,sizeof(bytes),0); QVERIFY(rpcSize>0); messages.clear();
        QVERIFY(reader.append(QByteArray(bytes,rpcSize),messages)); QCOMPARE(messages.size(),1);
        QCOMPARE(messages[0].value("profile").toString(),QStringLiteral("中文"));
        ::close(pair[1]); QTRY_COMPARE(ended.count(),1); QVERIFY(errors.isEmpty()); QVERIFY(!manager.ready());
    }
    void fragmentedAndCoalescedFrames() {
        // Literal official wire header and JSON, independent of frame().
        const QByteArray fixture="{\"action\":\"connectDevice\",\"uuid\":\"requestService\",\"profile\":\"{}\"}";
        QCOMPARE(fixture.size(),qsizetype(65));
        const auto packet=QByteArray::fromHex("000100000041")+fixture;
        DistBus::RpcReader reader; QList<QJsonObject> parsed;
        for(char byte:packet) QVERIFY(reader.append(QByteArray(1,byte),parsed));
        QCOMPARE(parsed.size(),1); QCOMPARE(parsed.front().value("action").toString(),QString("connectDevice"));
        const auto encoded=DistBus::frame(parsed.front());
        QCOMPARE(encoded.first(6),packet.first(6));
        QCOMPARE(QJsonDocument::fromJson(encoded.mid(6)),QJsonDocument::fromJson(fixture));
        parsed.clear();
        QByteArray many; for(int i=0;i<5000;++i) many+=packet;
        QVERIFY(many.size()>DistBus::MaxRpc);
        QVERIFY(reader.append(many,parsed)); QCOMPARE(parsed.size(),5000);
    }
    void invalidFrames_data() {
        QTest::addColumn<QByteArray>("wire");
        QTest::newRow("version")<<QByteArray::fromHex("000200000001");
        QTest::newRow("empty")<<QByteArray::fromHex("000100000000");
        QTest::newRow("oversize")<<QByteArray::fromHex("000100040001");
        QTest::newRow("json-array")<<(QByteArray::fromHex("000100000002")+"[]");
        QTest::newRow("missing-action")<<(QByteArray::fromHex("000100000002")+"{}");
        QTest::newRow("profile-object")<<DistBus::frame({{"action","test"},{"profile",QJsonObject{}}});
    }
    void invalidFrames() {
        QFETCH(QByteArray,wire); DistBus::RpcReader reader; QList<QJsonObject> messages;
        QVERIFY(!reader.append(wire,messages)); QVERIFY(!reader.error().isEmpty());
        QVERIFY(messages.isEmpty()); QVERIFY(!reader.append(DistBus::frame(hello()),messages));
        reader.reset(); QVERIFY(reader.append(DistBus::frame(hello()),messages)); QCOMPARE(messages.size(),1);
    }
    void officialHandshakeAndRpc() {
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        ServiceManager manager; QSignalSpy connected(&manager,&ServiceManager::connected);
        QSignalSpy published(&manager,&ServiceManager::servicePublished), errors(&manager,&ServiceManager::error);
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux"));
        QVERIFY(!manager.send("distributebus.action.comm.msg","uuid",{}));
        QTRY_VERIFY(server.hasPendingConnections()); auto peer=server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable()>6);
        DistBus::RpcReader reader; QList<QJsonObject> requests; QVERIFY(reader.append(peer->readAll(),requests));
        QCOMPARE(requests.size(),1); QCOMPARE(requests[0].value("uuid").toString(),QString("requestService"));
        QVERIFY(!requests[0].contains("deviceId"));
        const auto info=QJsonDocument::fromJson(requests[0].value("profile").toString().toUtf8()).object();
        QCOMPARE(info.value("a").toString(),QString("linux")); QCOMPARE(info.value("p").toInt(),0);
        const auto wire=DistBus::frame(publication())+DistBus::frame(hello());
        peer->write(wire.first(5)); peer->flush(); QTest::qWait(30);
        QVERIFY(!manager.ready()); QVERIFY(published.isEmpty());
        peer->write(wire.mid(5));
        QTRY_COMPARE(connected.count(),1); QTRY_COMPARE(published.count(),1);
        QVERIFY(manager.ready()); QCOMPARE(manager.services().size(),1); QVERIFY(errors.isEmpty());
        QVERIFY(manager.send("distributebus.action.comm.msg","uuid",{{"type",1}}));
        QTRY_VERIFY(peer->bytesAvailable()>6); requests.clear(); QVERIFY(reader.append(peer->readAll(),requests));
        QCOMPARE(requests.size(),1); QVERIFY(!requests[0].contains("deviceId"));
        // Official DFS profile must survive without extra JSON quotation.
        const auto profile=QStringLiteral("remote_ready_mount_me=Linux设备");
        QVERIFY(manager.sendProfile("distributebus.action.startservice","dfs",profile));
        QTRY_VERIFY(peer->bytesAvailable()>6); requests.clear(); QVERIFY(reader.append(peer->readAll(),requests));
        QCOMPARE(requests.size(),1); QCOMPARE(requests[0].value("profile").toString(),profile);
        QVERIFY(!requests[0].contains("deviceId"));
        QVERIFY(!manager.sendProfile("connectDevice","dfs",profile));
        manager.stop(); QVERIFY(!manager.ready()); QVERIFY(manager.services().isEmpty());
        QVERIFY(!manager.sendProfile("distributebus.action.startservice","dfs",profile));
        QTRY_COMPARE(peer->state(),QAbstractSocket::UnconnectedState);
    }
    void rejectWrongIdentityOrEncryption_data() {
        QTest::addColumn<QString>("id"); QTest::addColumn<int>("encryption");
        QTest::newRow("wrong-id")<<QString("other-phone")<<0;
        QTest::newRow("unsupported-encryption")<<QString("phone")<<2;
    }
    void rejectWrongIdentityOrEncryption() {
        QFETCH(QString,id); QFETCH(int,encryption);
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        ServiceManager manager; QSignalSpy errors(&manager,&ServiceManager::error),connected(&manager,&ServiceManager::connected);
        QSignalSpy published(&manager,&ServiceManager::servicePublished);
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux"));
        QTRY_VERIFY(server.hasPendingConnections()); auto peer=server.nextPendingConnection();
        peer->write(DistBus::frame(publication())+DistBus::frame(hello(id,encryption)));
        QTRY_COMPARE(errors.count(),1); QVERIFY(connected.isEmpty()); QVERIFY(published.isEmpty());
        QVERIFY(!manager.ready()); QVERIFY(manager.services().isEmpty());
    }
    void disconnectAndRestart() {
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        ServiceManager manager; QSignalSpy disconnected(&manager,&ServiceManager::disconnected),connected(&manager,&ServiceManager::connected);
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux"));
        QTRY_VERIFY(server.hasPendingConnections()); auto peer=server.nextPendingConnection();
        peer->write(DistBus::frame(hello())); QTRY_COMPARE(connected.count(),1);
        peer->disconnectFromHost(); QTRY_COMPARE(disconnected.count(),1); QVERIFY(!manager.ready());
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone2","linux"));
        QTRY_VERIFY(server.hasPendingConnections()); auto second=server.nextPendingConnection();
        second->write(DistBus::frame(hello("phone2"))); QTRY_COMPARE(connected.count(),2);
        QVERIFY(manager.ready()); manager.stop();
    }
    void urgentHeartbeatDoesNotCorruptRpc() {
#ifndef Q_OS_LINUX
        QSKIP("Linux native TCP urgent-data regression");
#else
        QTcpServer server; server.setProxy(QNetworkProxy::NoProxy); QVERIFY(server.listen(QHostAddress::LocalHost));
        ServiceManager manager; QSignalSpy connected(&manager,&ServiceManager::connected);
        QSignalSpy errors(&manager,&ServiceManager::error),messages(&manager,&ServiceManager::message);
        QVERIFY(manager.start(QHostAddress::LocalHost,server.serverPort(),"phone","linux"));
        QTRY_VERIFY(server.hasPendingConnections()); auto peer=server.nextPendingConnection();
        peer->write(DistBus::frame(hello())); QTRY_COMPARE(connected.count(),1);
        // Android NetClient uses Socket.sendUrgentData(255) during an idle read
        // timeout. This byte belongs outside the RPC stream, even between two
        // fragments of a frame. An inline byte shifts all subsequent framing.
        const auto packet=DistBus::frame({{"action","heartbeat-test"},{"uuid","test"},{"profile",""}});
        for(int split : {0,3,9}) {
            if(split) { peer->write(packet.first(split)); peer->flush(); QTest::qWait(20); }
            const char heartbeat=char(255);
            QCOMPARE(::send(int(peer->socketDescriptor()),&heartbeat,1,MSG_OOB),ssize_t(1));
            peer->write(packet.mid(split)); peer->flush();
            QTRY_VERIFY_WITH_TIMEOUT(messages.count()>0 || errors.count()>0,1000);
            QVERIFY2(errors.isEmpty(),"Official urgent heartbeat leaked into the RPC byte stream");
            QCOMPARE(messages.takeFirst().first().toJsonObject().value("action").toString(),QString("heartbeat-test"));
            QVERIFY(manager.ready());
        }
        // More than one idle period may pass with no regular RPC between
        // heartbeats. The old urgent byte must not become ordinary data when
        // TCP advances its urgent pointer for the next heartbeat.
        const char heartbeat=char(255);
        for(int i=0;i<3;++i) {
            QCOMPARE(::send(int(peer->socketDescriptor()),&heartbeat,1,MSG_OOB),ssize_t(1));
            QTest::qWait(30);
        }
        peer->write(packet); peer->flush();
        QTRY_VERIFY_WITH_TIMEOUT(messages.count()>0 || errors.count()>0,1000);
        QVERIFY2(errors.isEmpty(),"Successive idle heartbeats corrupted RPC framing");
        QCOMPARE(messages.count(),1);
        manager.stop();
#endif
    }
};
QTEST_GUILESS_MAIN(ServiceManagerTest)
#include "service_manager_test.moc"
