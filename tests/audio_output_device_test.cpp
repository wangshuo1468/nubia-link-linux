#include "audio.h"
#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <cmath>

// Opt-in only: plays a known, low-volume tone and records the desktop sink.
class AudioOutputDeviceTest:public QObject {
    Q_OBJECT
private slots:
    void delayedPcmReachesDesktop() {
        if(!qEnvironmentVariableIsSet("NUBIA_TEST_DESKTOP_AUDIO")) QSKIP("Desktop sound playback requires explicit opt-in");
        QProcess sink; sink.start("pactl",{"get-default-sink"}); QVERIFY(sink.waitForFinished(3000)); QCOMPARE(sink.exitCode(),0);
        const auto source=QString::fromUtf8(sink.readAllStandardOutput()).trimmed()+".monitor";
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path=directory.filePath("output.pcm");
        QProcess monitor; monitor.start("ffmpeg",{"-v","error","-f","pulse","-i",source,"-t","4","-ar","48000","-ac","2","-f","s16le",path});
        QVERIFY(monitor.waitForStarted(3000));
        PhoneAudio output; QSignalSpy warnings(&output,&PhoneAudio::warning); output.start();
        QTest::qWait(1000); // Reproduce starting the sink before network PCM arrives.
        QByteArray tone(3840,Qt::Uninitialized);
        for(int block=0;block<100;++block) {
            for(int i=0;i<960;++i) {
                const auto value=qint16(3000*std::sin(2*3.141592653589793*440*(block*960+i)/48000));
                qToLittleEndian(value,tone.data()+i*4); qToLittleEndian(value,tone.data()+i*4+2);
            }
            output.append(tone); QTest::qWait(20);
        }
        QTest::qWait(300); output.stop();
        QTRY_COMPARE_WITH_TIMEOUT(monitor.state(),QProcess::NotRunning,7000); QCOMPARE(monitor.exitCode(),0);
        QCOMPARE(warnings.size(),0); QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto pcm=file.readAll();
        double best=0,frequency=0;
        for(int offset=0;offset+19200<=pcm.size();offset+=19200) {
            double energy=0; int crossings=0;
            for(int i=offset+4;i<offset+19200;i+=4) {
                const auto previous=qFromLittleEndian<qint16>(pcm.constData()+i-4),value=qFromLittleEndian<qint16>(pcm.constData()+i);
                energy+=double(value)*value; if(previous<=0 && value>0) ++crossings;
            }
            const auto rms=std::sqrt(energy/4800)/32768;
            if(rms>best) { best=rms; frequency=crossings*10; }
        }
        qInfo()<<"Local desktop PCM RMS"<<best<<"frequency"<<frequency;
        QVERIFY(best>0.001); QVERIFY(std::abs(frequency-440)<20);
    }
};
QTEST_GUILESS_MAIN(AudioOutputDeviceTest)
#include "audio_output_device_test.moc"
