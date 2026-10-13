#include "protocol.h"
#include "session.h"
#include "decoder.h"
#include "audio.h"
#include <QtTest>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QStandardPaths>
#include <QtEndian>

namespace {
QByteArray videoPacket(qint64 timestamp, const QByteArray &bytes) {
    QByteArray out(12,0);
    qToBigEndian<quint64>(quint64(timestamp),out.data()); qToBigEndian<quint32>(bytes.size(),out.data()+8);
    return out + bytes;
}
QByteArray h264Fixture() {
    QTemporaryDir dir;
    QProcess process;
    process.start(QStandardPaths::findExecutable("ffmpeg"), {"-v","error","-f","lavfi","-i","color=c=red:s=64x48:r=1",
                  "-frames:v","1","-pix_fmt","yuv420p","-c:v","libx264","-preset","ultrafast","-tune","zerolatency",
                  "-f","h264",dir.filePath("frame.h264")});
    if (!process.waitForFinished(10000) || process.exitCode() != 0) return {};
    QFile file(dir.filePath("frame.h264")); if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
qsizetype firstSlice(const QByteArray &bytes) {
    for (qsizetype i = 0; i+4 < bytes.size(); ++i) {
        int prefix = bytes.mid(i,4) == QByteArray::fromHex("00000001") ? 4
                   : bytes.mid(i,3) == QByteArray::fromHex("000001") ? 3 : 0;
        if (prefix && ((quint8(bytes[i+prefix]) & 31) == 5 || (quint8(bytes[i+prefix]) & 31) == 1)) return i;
    }
    return -1;
}
struct FakeAdb {
    QTemporaryDir dir;
    QByteArray originalPath = qgetenv("PATH");
    FakeAdb() {
        if (!dir.isValid()) qFatal("Cannot create isolated ADB test directory");
        QFile script(dir.filePath("adb"));
        if (!script.open(QIODevice::WriteOnly)) qFatal("Cannot create ADB test executable");
        script.write(R"PY(#!/usr/bin/python3
import pathlib, sys, time
root = pathlib.Path(__file__).parent
args = sys.argv[1:]
with (root/'calls').open('a') as log:
    log.write(' '.join(args)+'\n')
if args[:1] == ['-s']:
    args = args[2:]
mapping = root/'mapping'
if args == ['reverse', '--list']:
    if mapping.exists():
        print('usb localabstract:scrcpy '+mapping.read_text())
elif args == ['reverse', '--remove', 'localabstract:scrcpy']:
    mapping.unlink(missing_ok=True)
elif args[:2] == ['reverse', '--no-rebind']:
    mapping.write_text(args[-1])
    if (root/'delay').exists():
        time.sleep(10)
elif args[:3] == ['shell', 'am', 'start']:
    print('Starting: Intent { test only }')
else:
    sys.exit(1)
)PY");
        script.close(); script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
        qputenv("PATH",dir.path().toUtf8()+":"+originalPath);
    }
    ~FakeAdb() { qputenv("PATH",originalPath); }
    void write(const QString &name,const QByteArray &bytes) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Cannot write ADB test state");
    }
    QByteArray read(const QString &name) {
        QFile file(dir.filePath(name)); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
    }
    bool exists(const QString &name) { return QFile::exists(dir.filePath(name)); }
};
}
class ProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void controlGoldenBytes() {
        QCOMPARE(Nubia::key(0,4),QByteArray::fromHex("00000000000400000000"));
        QCOMPARE(Nubia::key(1,29,4096),QByteArray::fromHex("00010000001d00001000"));
        QCOMPARE(Nubia::text(11,QStringLiteral("中文")),QByteArray::fromHex("0b0006e4b8ade69687"));
        QCOMPARE(Nubia::clipboard(QStringLiteral("A"),true),QByteArray::fromHex("0801000141"));
        QCOMPARE(Nubia::touch(0,{100,200},{1080,2400}),QByteArray::fromHex("0200ffffffffffffffff00000064000000c804380960ffff00000001"));
        QCOMPARE(Nubia::touch(1,{100,200},{1080,2400},0),QByteArray::fromHex("0201ffffffffffffffff00000064000000c804380960000000000000"));
        QCOMPARE(Nubia::scroll({100,200},{1080,2400},-1,2),QByteArray::fromHex("0300000064000000c804380960ffffffff00000002"));
        QCOMPARE(Nubia::text(13,"1"),QByteArray::fromHex("0d000131"));
    }
    void unicodeTruncation() {
        QCOMPARE(Nubia::text(11,QStringLiteral("A中文"),4),QByteArray::fromHex("0b000441e4b8ad"));
        QCOMPARE(Nubia::text(11,QStringLiteral("A中文"),3),QByteArray::fromHex("0b000141"));
        QCOMPARE(Nubia::text(11,QString::fromUtf8("😀"),3),QByteArray::fromHex("0b0000"));
        QCOMPARE(Nubia::text(11,"x",-1),QByteArray::fromHex("0b0000"));
    }
    void fragmentedVideo() {
        VideoFramer framer; QSignalSpy packets(&framer,&VideoFramer::packet);
        const auto bytes = videoPacket(-1,"config") + videoPacket(123,"picture");
        for (char byte : bytes) framer.feed(QByteArray(1,byte));
        QCOMPARE(packets.count(),2); QCOMPARE(packets[0][0].toByteArray(),QByteArray("config"));
        QCOMPARE(packets[0][1].toLongLong(),qint64(-1)); QCOMPARE(packets[1][1].toLongLong(),qint64(123));
        framer.feed(bytes); QCOMPARE(packets.count(),4);
    }
    void invalidVideo() {
        VideoFramer framer; QSignalSpy errors(&framer,&VideoFramer::error), packets(&framer,&VideoFramer::packet);
        framer.feed(videoPacket(0,{})); QCOMPARE(errors.count(),1);
        framer.feed(videoPacket(0,"valid")); QCOMPARE(packets.count(),0);
        framer.reset(); framer.feed(videoPacket(0,"valid")); QCOMPARE(packets.count(),1);
        QByteArray bad(12,0); qToBigEndian<quint32>(Nubia::MaxVideoPacket+1,bad.data()+8);
        framer.feed(bad); QCOMPARE(errors.count(),2);
    }
    void resetDuringCallback() {
        VideoFramer framer; QSignalSpy packets(&framer,&VideoFramer::packet);
        connect(&framer,&VideoFramer::packet,&framer,[&] { framer.reset(); });
        framer.feed(videoPacket(0,"first") + videoPacket(1,"stale")); QCOMPARE(packets.count(),1);
    }
    void notifications() {
        MessageFramer framer; QSignalSpy messages(&framer,&MessageFramer::message), errors(&framer,&MessageFramer::error);
        const auto bytes = Nubia::text(0,QStringLiteral("手机剪贴板")) + Nubia::text(6,"15.1.17");
        for (char byte : bytes) framer.feed(QByteArray(1,byte));
        QCOMPARE(messages.count(),2); QCOMPARE(messages[0][1].toString(),QStringLiteral("手机剪贴板"));
        framer.feed(QByteArray::fromHex("080000")); QCOMPARE(errors.count(),1);
        framer.reset(); framer.feed(QByteArray::fromHex("001000")); QCOMPARE(errors.count(),2);
    }
    void threeChannels() {
        ProjectionSession session; QVERIFY(session.listenLocal());
        QSignalSpy video(&session,&ProjectionSession::videoPacket), audio(&session,&ProjectionSession::audioData),
            messages(&session,&ProjectionSession::deviceMessage), active(&session,&ProjectionSession::activeChanged);
        QTcpSocket clients[3];
        for (int i=0;i<3;++i) {
            clients[i].connectToHost(QHostAddress::LocalHost,session.port());
            QVERIFY(clients[i].waitForConnected(2000));
            QTest::qWait(10);
        }
        QTRY_VERIFY(session.connected()); QCOMPARE(active.count(),1); QVERIFY(active[0][0].toBool());
        clients[0].write(videoPacket(-1,"SPS") + videoPacket(1,"frame"));
        clients[1].write(QByteArray::fromHex("0001000200030004"));
        clients[2].write(Nubia::text(0,QStringLiteral("同步")));
        QTRY_COMPARE(video.count(),2); QTRY_COMPARE(messages.count(),1); QTRY_VERIFY(audio.count() > 0);
        QByteArray pcm; for (const auto &item : audio) pcm += item[0].toByteArray(); QCOMPARE(pcm,QByteArray::fromHex("0001000200030004"));
        const auto key = Nubia::key(0,3); session.send(key);
        QTRY_VERIFY(clients[2].bytesAvailable() >= key.size()); QCOMPARE(clients[2].read(key.size()),key);
        QTcpSocket extra; extra.connectToHost(QHostAddress::LocalHost,session.port());
        QVERIFY(extra.waitForConnected(2000)); QTRY_COMPARE(extra.state(),QAbstractSocket::UnconnectedState);
        clients[1].abort(); QTRY_VERIFY(!session.connected());
        QTRY_COMPARE(clients[0].state(),QAbstractSocket::UnconnectedState);
        QTRY_COMPARE(clients[2].state(),QAbstractSocket::UnconnectedState);
        QVERIFY(!active.last()[0].toBool()); QCOMPARE(session.port(),quint16(0));
        QVERIFY(session.listenLocal()); session.stop(); QCOMPARE(session.port(),quint16(0));
    }
    void malformedChannelClosesSession() {
        ProjectionSession session; QVERIFY(session.listenLocal());
        QTcpSocket video; video.connectToHost(QHostAddress::LocalHost,session.port()); QVERIFY(video.waitForConnected(2000));
        video.write(videoPacket(0,{})); QTRY_COMPARE(session.port(),quint16(0));
        QTRY_COMPARE(video.state(),QAbstractSocket::UnconnectedState);
    }
    void adbLaunchAndOwnedCleanup() {
        FakeAdb fake; ProjectionSession session;
        session.startAdb("test-serial"); const auto port = session.port(); QVERIFY(port);
        QTRY_VERIFY(fake.read("calls").contains("shell am start"));
        QCOMPARE(fake.read("mapping"),QStringLiteral("tcp:%1").arg(port).toUtf8());
        const auto calls = fake.read("calls");
        QVERIFY(calls.contains("cn.nubia.touping/.WiredTouPingMainActivity"));
        QVERIFY(calls.contains("--ei connet_port "+QByteArray::number(port)));
        QVERIFY(!calls.contains("push"));
        session.stop(); QVERIFY(!fake.exists("mapping"));
        QVERIFY(fake.read("calls").contains("reverse --remove localabstract:scrcpy"));
    }
    void adbDoesNotStealExistingMapping() {
        FakeAdb fake; fake.write("mapping","tcp:11111"); ProjectionSession session;
        session.startAdb("test-serial"); QTRY_COMPARE(session.port(),quint16(0));
        QCOMPARE(fake.read("mapping"),QByteArray("tcp:11111"));
        const auto calls = fake.read("calls");
        QVERIFY(!calls.contains("--no-rebind")); QVERIFY(!calls.contains("--remove")); QVERIFY(!calls.contains("am start"));
    }
    void adbCancellationAfterMappingCreation() {
        FakeAdb fake; fake.write("delay","1"); ProjectionSession session;
        session.startAdb("test-serial"); QTRY_VERIFY(fake.exists("mapping"));
        // No success callback yet: the external ADB process is still waiting.
        session.stop(); QVERIFY(!fake.exists("mapping"));
        QVERIFY(!fake.read("calls").contains("am start"));
    }
    void adbCleanupPreservesReplacedMapping() {
        FakeAdb fake; ProjectionSession session; session.startAdb("test-serial");
        QTRY_VERIFY(fake.read("calls").contains("am start"));
        fake.write("mapping","tcp:11111"); session.stop();
        QCOMPARE(fake.read("mapping"),QByteArray("tcp:11111")); QVERIFY(!fake.read("calls").contains("--remove"));
    }
    void pcmFragmentsAndLatencyBound() {
        PcmBuffer pcm;
        pcm.append(QByteArray::fromHex("010203")); QVERIFY(pcm.read(4).isEmpty());
        pcm.append(QByteArray::fromHex("0405060708")); QCOMPARE(pcm.read(4),QByteArray::fromHex("01020304"));
        QCOMPARE(pcm.read(4),QByteArray::fromHex("05060708"));
        QByteArray sample;
        for (int i=0;i<PcmBuffer::Capacity/4+10;++i) sample += QByteArray::fromHex("01020304");
        pcm.append(sample); QCOMPARE(pcm.bytesAvailable(),qint64(PcmBuffer::Capacity));
        QCOMPARE(pcm.read(4),QByteArray::fromHex("01020304"));
        pcm.clear(); QCOMPARE(pcm.bytesAvailable(),qint64(0));
    }
    void realH264Decode() {
        const auto bytes = h264Fixture(); QVERIFY2(!bytes.isEmpty(),"ffmpeg with libx264 is required for decoder validation");
        const auto split = firstSlice(bytes); QVERIFY(split > 0);
        H264Decoder decoder; QString error; std::shared_ptr<VideoFrame> frame;
        auto output = [&](std::shared_ptr<VideoFrame> value) { frame = std::move(value); };
        QVERIFY2(decoder.decode(bytes.left(split),-1,output,error),qPrintable(error)); QVERIFY(!frame);
        QVERIFY2(decoder.decode(bytes.mid(split),0,output,error),qPrintable(error)); QVERIFY(frame);
        QCOMPARE(frame->size,QSize(64,48)); QCOMPARE(frame->y.size(),64*48); QCOMPARE(frame->u.size(),32*24);
        QVERIFY(qAbs(quint8(frame->y[0])-81) < 3); QVERIFY(qAbs(quint8(frame->u[0])-90) < 3);
        QVERIFY(qAbs(quint8(frame->v[0])-240) < 3);
        VideoDecoder threaded; threaded.submit(bytes,0);
        std::shared_ptr<VideoFrame> asyncFrame;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { if (!asyncFrame) asyncFrame = threaded.takeFrame(); return bool(asyncFrame); })(),3000);
        QCOMPARE(asyncFrame->size,QSize(64,48));
        threaded.reset(); QVERIFY(!threaded.takeFrame());
    }
};
QTEST_GUILESS_MAIN(ProtocolTest)
#include "protocol_test.moc"
