#include "rtp_receiver.h"
#include <QtTest>
#include <QProcess>
#include <QStandardPaths>
#include <QUdpSocket>
#include <QtEndian>
#include <cmath>

namespace {
QList<QByteArray> parameterSets() {
    QProcess process;
    process.start(QStandardPaths::findExecutable("ffmpeg"),{"-v","error","-f","lavfi","-i","color=c=red:s=128x96:r=10",
        "-frames:v","1","-pix_fmt","yuv420p","-c:v","libx264","-preset","ultrafast","-tune","zerolatency",
        "-f","h264","pipe:1"});
    if(!process.waitForFinished(10000) || process.exitCode()!=0) return {};
    const auto bytes=process.readAllStandardOutput();
    QList<QByteArray> result;
    auto prefix=[&](int i) {
        if(bytes.mid(i,4)==QByteArray::fromHex("00000001")) return 4;
        if(bytes.mid(i,3)==QByteArray::fromHex("000001")) return 3;
        return 0;
    };
    for(int i=0;i<bytes.size()-4;) {
        int n=prefix(i);
        if(!n) { ++i; continue; }
        const int begin=i+n; int end=begin;
        while(end<bytes.size() && !prefix(end)) ++end;
        const int type=quint8(bytes[begin])&31;
        if(type==7 || type==8) result.append(bytes.mid(begin,end-begin));
        i=end;
    }
    return result;
}
quint16 freePair(bool audio=false) {
    // Bind/check both RTP and RTCP together; FFmpeg takes ownership next.
    for(int i=0;i<100;++i) {
        QUdpSocket video,control,voice,feedback;
        if(video.bind(QHostAddress::LocalHost,0) && video.localPort()>1026 && video.localPort()<65535 &&
           control.bind(QHostAddress::LocalHost,video.localPort()+1) &&
           (!audio || (voice.bind(QHostAddress::LocalHost,video.localPort()-2) && feedback.bind(QHostAddress::LocalHost,video.localPort()-1)))) return video.localPort();
    }
    return 0;
}
}
class RtpReceiverTest:public QObject {
    Q_OBJECT
private slots:
    void concurrentOpusAndH264() {
        const auto sets=parameterSets(); QCOMPARE(sets.size(),2); const auto port=freePair(true); QVERIFY(port);
        RtpReceiver receiver; QSignalSpy listening(&receiver,&RtpReceiver::listening),warnings(&receiver,&RtpReceiver::audioWarning);
        QVERIFY(receiver.begin(QHostAddress::LocalHost,QHostAddress::LocalHost,port,sets[0],sets[1],112));
        QTRY_COMPARE_WITH_TIMEOUT(listening.size(),1,3000);
        QProcess video,audio;
        const auto ffmpeg=QStandardPaths::findExecutable("ffmpeg");
        video.start(ffmpeg,{"-v","error","-re","-f","lavfi","-i","color=c=red:s=128x96:r=10","-t","5",
            "-pix_fmt","yuv420p","-c:v","libx264","-preset","ultrafast","-tune","zerolatency","-g","10",
            "-payload_type","114","-f","rtp",QString("rtp://127.0.0.1:%1?pkt_size=1200").arg(port)});
        audio.start(ffmpeg,{"-v","error","-re","-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=5",
            "-ac","2","-c:a","libopus","-application","lowdelay","-payload_type","112","-f","rtp",
            QString("rtp://127.0.0.1:%1?pkt_size=1200").arg(port-2)});
        QTRY_VERIFY_WITH_TIMEOUT(receiver.frameCount()>=10 && receiver.audioBytes()>=48000,5000);
        const auto pcm=receiver.takeAudio(); QVERIFY(pcm.size()>4000); QVERIFY(pcm.size()<=48000); QCOMPARE(pcm.size()%4,0);
        int crossings=0; double energy=0;
        for(int i=4;i<pcm.size();i+=4) {
            const auto previous=qFromLittleEndian<qint16>(pcm.constData()+i-4),value=qFromLittleEndian<qint16>(pcm.constData()+i);
            if(previous<=0 && value>0) ++crossings; energy+=double(value)*value;
        }
        const double frequency=crossings*48000.0/(pcm.size()/4);
        QVERIFY2(std::abs(frequency-440)<15,qPrintable(QString::number(frequency)));
        QVERIFY(energy/(pcm.size()/4)>100000); QCOMPARE(warnings.size(),0);
        video.kill(); audio.kill(); QVERIFY(video.waitForFinished(2000)); QVERIFY(audio.waitForFinished(2000));
        receiver.stop(); QVERIFY(receiver.takeAudio().isEmpty());
        for(int offset=-2;offset<=1;++offset) { QUdpSocket socket; QVERIFY(socket.bind(QHostAddress::LocalHost,port+offset)); }
    }
    void sustainedDecodeAndIdleStop() {
        const auto sets=parameterSets(); QCOMPARE(sets.size(),2);
        const auto port=freePair(); QVERIFY(port);
        RtpReceiver receiver;
        QSignalSpy listening(&receiver,&RtpReceiver::listening),errors(&receiver,&RtpReceiver::error);
        QVERIFY(receiver.begin(QHostAddress::LocalHost,QHostAddress::LocalHost,port,sets[0],sets[1]));
        QTRY_COMPARE_WITH_TIMEOUT(listening.size(),1,3000);
        QProcess sender;
        sender.start(QStandardPaths::findExecutable("ffmpeg"),{"-v","error","-re","-f","lavfi","-i","color=c=red:s=128x96:r=10",
            "-t","6","-pix_fmt","yuv420p","-c:v","libx264","-preset","ultrafast","-tune","zerolatency","-g","10",
            "-payload_type","114","-f","rtp",QString("rtp://127.0.0.1:%1?pkt_size=1200").arg(port)});
        QTRY_VERIFY_WITH_TIMEOUT(receiver.frameCount()>=30,7000);
        const auto frame=receiver.takeFrame(); QVERIFY(frame); QCOMPARE(frame->size,QSize(128,96));
        // Verify decoded content, not merely receipt of arbitrary datagrams.
        QVERIFY(quint8(frame->y[0])>70 && quint8(frame->y[0])<90);
        QVERIFY(quint8(frame->v[0])>220);
        sender.kill(); QVERIFY(sender.waitForFinished(2000));
        QTest::qWait(16000); QVERIFY(receiver.isRunning()); QCOMPARE(errors.size(),0);
        QElapsedTimer elapsed; elapsed.start(); receiver.stop(); QVERIFY(elapsed.elapsed()<2000);
        QCOMPARE(errors.size(),0);
        QUdpSocket video,control;
        QVERIFY(video.bind(QHostAddress::LocalHost,port)); QVERIFY(control.bind(QHostAddress::LocalHost,port+1));
        video.close(); control.close();
        // Restart with no sender, then interrupt a blocked receive immediately.
        QVERIFY(receiver.begin(QHostAddress::LocalHost,QHostAddress::LocalHost,port,sets[0],sets[1]));
        QTRY_COMPARE_WITH_TIMEOUT(listening.size(),2,3000);
        elapsed.restart(); receiver.stop(); QVERIFY(elapsed.elapsed()<2000);
        QCOMPARE(receiver.frameCount(),quint64(0)); QVERIFY(!receiver.takeFrame());
    }
};
QTEST_GUILESS_MAIN(RtpReceiverTest)
#include "rtp_receiver_test.moc"
