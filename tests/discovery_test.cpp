#include "discovery.h"
#include <QtTest>
#include <QSignalSpy>
#include <QProcess>
#include <QNetworkProxy>
#include <QUdpSocket>
#include <QJsonDocument>
#include <QJsonArray>

class DiscoveryTest:public QObject {
    Q_OBJECT
private slots:
    void invalidAddressAndBindFailure() {
        PhoneDiscovery discovery; QSignalSpy errors(&discovery,&PhoneDiscovery::error);
        QVERIFY(!discovery.start(QHostAddress::AnyIPv4,QHostAddress::LocalHost));
        QVERIFY(!discovery.start(QHostAddress::LocalHost,QHostAddress("::1")));
        QVERIFY(!discovery.start(QHostAddress("192.0.2.1"),QHostAddress::LocalHost));
        QCOMPARE(errors.count(),3); QCOMPARE(discovery.port(),quint16(0));
    }
    void largeOfficialCallbackAndCleanup() {
        PhoneDiscovery discovery; QSignalSpy found(&discovery,&PhoneDiscovery::found);
        QVERIFY(discovery.start(QHostAddress::LocalHost,QHostAddress::LocalHost,65533));
        const auto port=discovery.port(); QVERIFY(port);
        const QJsonObject device{{"deviceId","fixture"},{"deviceName","NX721J"},{"protocol","ZLINK"},
                                {"socketUri","socket://127.0.0.1:12345"},{"custData",QJsonArray{}},
                                {"padding",QString(2200,'x')}};
        QProcess sender;
        sender.start("coap-client-notls",{"-m","post","-e",QString::fromUtf8(QJsonDocument(device).toJson(QJsonDocument::Compact)),
                                       "-B","2",QString("coap://127.0.0.1:%1/device_discover").arg(port)});
        QVERIFY(sender.waitForStarted(2000));
        QTRY_COMPARE_WITH_TIMEOUT(found.count(),1,4000);
        QCOMPARE(found[0][0].toJsonObject(),device);
        QTRY_VERIFY_WITH_TIMEOUT(sender.state()==QProcess::NotRunning,3000); QCOMPARE(sender.exitCode(),0);
        discovery.stop();
        QUdpSocket probe; probe.setProxy(QNetworkProxy::NoProxy); QVERIFY(probe.bind(QHostAddress::LocalHost,port));
    }
    void largeUnblockedPhoneDatagram() {
        PhoneDiscovery discovery; QSignalSpy found(&discovery,&PhoneDiscovery::found);
        QVERIFY(discovery.start(QHostAddress::LocalHost,QHostAddress::LocalHost,65533));
        const QJsonObject device{{"deviceId","fixture"},{"deviceName",QStringLiteral("努比亚")},{"protocol","ZLINK"},
                                {"socketUri","socket://127.0.0.1:12345"},{"padding",QString(2200,'x')}};
        // Golden NON POST /device_discover with no token and no Block1 option,
        // matching the phone's single large UDP message (not block transfer).
        QByteArray datagram=QByteArray::fromHex("50021234bd02"); datagram+="device_discover"; datagram+=char(255);
        datagram+=QJsonDocument(device).toJson(QJsonDocument::Compact);
        QVERIFY(datagram.size()>1472);
        QUdpSocket sender; sender.setProxy(QNetworkProxy::NoProxy);
        QCOMPARE(sender.writeDatagram(datagram,QHostAddress::LocalHost,discovery.port()),qint64(datagram.size()));
        QTRY_COMPARE_WITH_TIMEOUT(found.count(),1,2000); QCOMPARE(found[0][0].toJsonObject(),device);
    }
};
QTEST_GUILESS_MAIN(DiscoveryTest)
#include "discovery_test.moc"
