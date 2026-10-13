#include "lebo_receiver.h"
#include <QtTest>
#include <QNetworkProxy>
#include <QtEndian>
#include <QFile>
#include <cmath>

class LeboReceiverTest: public QObject {
    Q_OBJECT
    void open(LeboReceiver &receiver, QTcpSocket &control, QTcpSocket &media) {
        // Local wire tests do not announce a test service on the user's LAN.
        receiver.address_=QHostAddress::LocalHost; receiver.prefix_=8;
        QVERIFY(receiver.control_.listen(QHostAddress::LocalHost));
        QVERIFY(receiver.mirror_.listen(QHostAddress::LocalHost));
        control.setProxy(QNetworkProxy::NoProxy); media.setProxy(QNetworkProxy::NoProxy);
        control.connectToHost(QHostAddress::LocalHost,receiver.control_.serverPort());
        media.connectToHost(QHostAddress::LocalHost,receiver.mirror_.serverPort());
        QTRY_COMPARE(control.state(),QAbstractSocket::ConnectedState);
        QTRY_COMPARE(media.state(),QAbstractSocket::ConnectedState);
    }
    QByteArray framed(const QByteArray &data, quint16 type=0, bool keyframe=false) {
        QByteArray header(128,0);
        qToLittleEndian(quint32(data.size()),header.data());
        qToLittleEndian(type,header.data()+4); header[127]=keyframe ? 1 : 0;
        return header+data;
    }
    const QByteArray audioSdp="v=0\r\na=rtpmap:96 mpeg4-generic/44100/2\r\na=fmtp:96 mode=AAC-eld; constantDuration=480\r\n";
    QList<QByteArray> audioFixture() {
        QFile file(QStringLiteral(LEBO_AUDIO_FIXTURE));
        if(!file.open(QIODevice::ReadOnly)) return {};
        auto bytes=file.readAll(); QList<QByteArray> packets;
        while(bytes.size()>=4) {
            const auto n=qFromLittleEndian<quint32>(bytes.constData());
            if(n>4096 || n==0 || n>quint32(bytes.size()-4)) return {};
            packets.append(bytes.mid(4,n)); bytes.remove(0,4+n);
        }
        return bytes.isEmpty() ? packets : QList<QByteArray>();
    }
private slots:
    void realEldToneDecodeAndReset() {
        const auto packets=audioFixture(); QCOMPARE(packets.size(),40);
        EldAudio decoder;
        QVERIFY(!decoder.configure(QByteArray(audioSdp).replace("AAC-eld","AAC-hbr")));
        QVERIFY(decoder.configure(audioSdp)); QByteArray pcm;
        for(const auto &packet:packets) pcm+=decoder.decode(packet);
        QVERIFY(pcm.size()>80000 && pcm.size()<85000); QCOMPARE(pcm.size()%4,0);
        double energy=0,bestPower=0; int bestFrequency=0;
        const auto samples=pcm.size()/4;
        for(int i=0;i<samples;++i) { const double value=qFromLittleEndian<qint16>(pcm.constData()+i*4); energy+=value*value; }
        QVERIFY(std::sqrt(energy/samples)/32768>0.01);
        for(int frequency=300;frequency<=600;frequency+=10) {
            double real=0,imaginary=0;
            for(int i=0;i<samples;++i) {
                const double value=qFromLittleEndian<qint16>(pcm.constData()+i*4);
                const double phase=2*3.141592653589793*frequency*i/48000;
                real+=value*std::cos(phase); imaginary+=value*std::sin(phase);
            }
            const auto power=real*real+imaginary*imaginary;
            if(power>bestPower) { bestPower=power; bestFrequency=frequency; }
        }
        QCOMPARE(bestFrequency,440);
        QVERIFY(decoder.decode(QByteArray(4097,0)).isEmpty());
        decoder.reset(); QVERIFY(!decoder.ready()); QVERIFY(decoder.decode(packets.first()).isEmpty());
        QVERIFY(decoder.configure(audioSdp)); QVERIFY(!decoder.decode(packets.first()).isEmpty());
    }
    void audioHandshakeAndPeerSequenceIsolation() {
        LeboReceiver receiver; QTcpSocket control,media; open(receiver,control,media);
        QVERIFY(receiver.audio_.bind(QHostAddress::LocalHost,quint16(0)));
        QSignalSpy pcm(&receiver,&LeboReceiver::audioData);
        const auto announce="ANNOUNCE rtsp://phone/1 RTSP/1.0\r\nContent-Length: "+QByteArray::number(audioSdp.size())+"\r\n\r\n"+audioSdp;
        control.write(announce.left(announce.size()-2)); QTest::qWait(20); QVERIFY(!receiver.audioDecoder_.ready());
        control.write(announce.right(2)); QTRY_VERIFY(receiver.audioDecoder_.ready());
        QTRY_VERIFY(control.bytesAvailable()>0); QVERIFY(control.readAll().startsWith("RTSP/1.0 200"));
        receiver.active_=true;
        QUdpSocket phone,otherPort,otherPhone; phone.setProxy(QNetworkProxy::NoProxy); otherPort.setProxy(QNetworkProxy::NoProxy); otherPhone.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(phone.bind(QHostAddress::LocalHost,quint16(0))); QVERIFY(otherPort.bind(QHostAddress::LocalHost,quint16(0)));
        QVERIFY(otherPhone.bind(QHostAddress("127.0.0.2"),quint16(0)));
        const auto fixture=audioFixture(); QVERIFY(!fixture.isEmpty());
        auto send=[&](QUdpSocket &source,quint16 sequence,const QByteArray &payload,int type=96) {
            QByteArray header(12,0); header[0]=char(0x80); header[1]=char(type);
            qToBigEndian(sequence,header.data()+2); qToLittleEndian(quint64(1234),header.data()+4);
            source.writeDatagram(header+payload,QHostAddress::LocalHost,receiver.audio_.localPort());
        };
        send(otherPhone,65534,fixture[0]); QTest::qWait(20); QCOMPARE(pcm.size(),0);
        send(phone,65534,fixture[0]); QTRY_COMPARE(pcm.size(),1);
        send(phone,65535,fixture[1]); QTRY_COMPARE(pcm.size(),2);
        send(phone,0,fixture[2]); QTRY_COMPARE(pcm.size(),3);
        send(phone,0,fixture[2]); send(phone,65535,fixture[1]);
        send(otherPort,1,fixture[3]); send(phone,1,fixture[3],97); send(phone,1,QByteArray(5000,0));
        QTest::qWait(50); QCOMPARE(pcm.size(),3);
        send(phone,1,fixture[3]); QTRY_COMPARE(pcm.size(),4);
        receiver.disconnectPhone(); QVERIFY(!receiver.audioDecoder_.ready()); QVERIFY(!receiver.active());
        QVERIFY(receiver.audio_.isValid()); QVERIFY(receiver.control_.isListening());
        send(phone,2,fixture[4]); QTest::qWait(20); QCOMPARE(pcm.size(),4);
        // A new ANNOUNCE, rather than stale UDP, establishes the next session.
        QTcpSocket again; again.setProxy(QNetworkProxy::NoProxy);
        again.connectToHost(QHostAddress::LocalHost,receiver.control_.serverPort());
        QTRY_COMPARE(again.state(),QAbstractSocket::ConnectedState);
        again.write(announce); QTRY_VERIFY(receiver.audioDecoder_.ready()); receiver.active_=true;
        send(otherPort,42,fixture[0]); QTRY_COMPARE(pcm.size(),5);
    }
    void fragmentedLegacyHandshakeAndChainedKeyframes() {
        LeboReceiver receiver; QTcpSocket control,media; open(receiver,control,media);
        QSignalSpy packets(&receiver,&LeboReceiver::videoPacket);
        control.write("GET /stream.xml HTTP/1.1\r\nStream-Ti");
        QTest::qWait(20); QCOMPARE(control.bytesAvailable(),0);
        control.write("me: legacy-test-time\r\nContent-Length: 0\r\n\r\n");
        QTRY_VERIFY(control.bytesAvailable()>0); QVERIFY(control.readAll().startsWith("HTTP/1.1 200"));
        // POST carries metadata, not the IV negotiated on the control socket.
        media.write("POST /stream HTTP/1.1\r\nContent-Length: 4\r\n\r\nab");
        QTest::qWait(20); QCOMPARE(media.bytesAvailable(),0);
        media.write("cd"); QTRY_VERIFY(media.bytesAvailable()>0); media.readAll();
        const auto config=QByteArray::fromHex("0164001fffe100046764001f01000268ee");
        const auto packet=framed(config,1);
        media.write(packet.left(127)); QTest::qWait(20); QCOMPARE(packets.size(),0);
        media.write(packet.mid(127)); QTRY_COMPARE(packets.size(),1);
        QCOMPARE(packets.at(0).at(0).toByteArray(),QByteArray::fromHex("000000016764001f0000000168ee"));
        // Independent OpenSSL fixture: MD5/XOR keys, two consecutive CBC
        // chunks. The untouched tail and NAL byte remain outside encryption.
        const auto cipher=QByteArray::fromHex("8ecd6725907be381142be8acea5118d5fc552834d961f07a99f3241c143376139b4a2201aa3cf8911f5e07f08aa6df012eedb0ae4004d99d9c949aa9493cf54c");
        for(int i=0;i<2;++i) {
            QByteArray nal(65,0); nal[0]=0x65;
            for(int j=0;j<32;++j) nal[1+j]=char(1+i*32+j);
            for(int j=33;j<65;++j) nal[j]=char(0x40+j);
            QByteArray encrypted=nal; encrypted.replace(1,32,cipher.mid(i*32,32));
            QByteArray payload(4,0); qToBigEndian(quint32(nal.size()),payload.data()); payload+=encrypted;
            media.write(framed(payload,0,true)); QTRY_COMPARE(packets.size(),i+2);
            QCOMPARE(packets.at(i+1).at(0).toByteArray(),QByteArray("\0\0\0\1",4)+nal);
        }
        QVERIFY(receiver.active()); media.disconnectFromHost();
        QTRY_VERIFY(!receiver.active()); QTRY_VERIFY(receiver.peer_.isNull());
        QVERIFY(receiver.control_.isListening()); QVERIFY(receiver.mirror_.isListening());
    }
    void rejectsOversizedHeadersAndMediaWithoutLosingListener() {
        LeboReceiver receiver; QTcpSocket control,media; open(receiver,control,media);
        control.write(QByteArray(16385,'x'));
        QTRY_COMPARE(control.state(),QAbstractSocket::UnconnectedState);
        media.write("POST /stream HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
        QTRY_VERIFY(media.bytesAvailable()>0); media.readAll();
        QByteArray header(128,0); qToLittleEndian(quint32(2*1024*1024+1),header.data());
        media.write(header); QTRY_COMPARE(media.state(),QAbstractSocket::UnconnectedState);
        QVERIFY(receiver.mirror_.isListening()); QVERIFY(receiver.peer_.isNull());
        QTcpSocket again; again.setProxy(QNetworkProxy::NoProxy);
        again.connectToHost(QHostAddress::LocalHost,receiver.control_.serverPort());
        QTRY_COMPARE(again.state(),QAbstractSocket::ConnectedState);
        again.write("GET /server-info HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
        QTRY_VERIFY(again.bytesAvailable()>0); QVERIFY(again.readAll().startsWith("HTTP/1.1 200"));
    }
    void teardownReplyAndReconnect() {
        LeboReceiver receiver; QTcpSocket control,media; open(receiver,control,media);
        media.write("POST /stream HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
        QTRY_VERIFY(media.bytesAvailable()>0); media.readAll();
        control.write("TEARDOWN rtsp://phone/1 RTSP/1.0\r\nCSeq: 9\r\n\r\n");
        QTRY_COMPARE(media.state(),QAbstractSocket::UnconnectedState);
        QTRY_VERIFY(control.bytesAvailable()>0);
        const auto reply=control.readAll(); QVERIFY(reply.startsWith("RTSP/1.0 200")); QVERIFY(reply.contains("CSeq: 9"));
        QVERIFY(receiver.peer_.isNull()); QVERIFY(receiver.mirror_.isListening());
    }
};
QTEST_GUILESS_MAIN(LeboReceiverTest)
#include "lebo_receiver_test.moc"
