#include "window.h"
#include "link_shell.h"
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <cmath>

// Authorized, opt-in real Lelink entrance. Never registered in CTest.
// Coordinates must be supplied from a current phone screenshot.
class SmartcastDeviceTest: public QObject {
    Q_OBJECT
    QString serial_,uri_;
    QString command(const QString &program,const QStringList &arguments) {
        QProcess process; process.start(program,arguments);
        if(!process.waitForFinished(6000)) { process.kill(); process.waitForFinished(1000); return {}; }
        return QString::fromUtf8(process.readAllStandardOutput());
    }
    QString adb(QStringList arguments) { return command("adb",QStringList{"-s",serial_}+arguments); }
    QByteArray monitor(const QString &path) {
        const auto source=command("pactl",{"get-default-sink"}).trimmed()+".monitor";
        QProcess process;
        process.start("ffmpeg",{"-v","error","-f","pulse","-i",source,"-t","3","-ar","48000","-ac","2","-f","s16le",path});
        QElapsedTimer timer; timer.start();
        while(process.state()!=QProcess::NotRunning && timer.elapsed()<6000) QTest::qWait(50);
        if(process.state()!=QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); return {}; }
        if(process.exitCode()!=0) return {};
        QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
    }
    double rms(const QByteArray &pcm) {
        double energy=0;
        for(int i=0;i+4<=pcm.size();i+=4) { const double value=qFromLittleEndian<qint16>(pcm.constData()+i); energy+=value*value; }
        return pcm.isEmpty() ? 0 : std::sqrt(energy/(pcm.size()/4))/32768;
    }
    int frequency(const QByteArray &pcm) {
        double best=0; int result=0;
        // Analyze one stable second, skipping startup of the pulse recorder.
        const int offset=48000*4, samples=48000;
        if(pcm.size()<offset+samples*4) return 0;
        for(int hz=350;hz<=550;hz+=10) {
            double real=0,imaginary=0;
            for(int i=0;i<samples;++i) {
                const double value=qFromLittleEndian<qint16>(pcm.constData()+offset+i*4);
                const double phase=2*3.141592653589793*hz*i/48000;
                real+=value*std::cos(phase); imaginary+=value*std::sin(phase);
            }
            const double power=real*real+imaginary*imaginary;
            if(power>best) { best=power; result=hz; }
        }
        return result;
    }
private slots:
    void nativeAudioMuteAndReconnect() {
        serial_=qEnvironmentVariable("NUBIA_TEST_DEVICE"); uri_=qEnvironmentVariable("NUBIA_AUDIO_TEST_URI");
        const auto row=qEnvironmentVariable("NUBIA_SMARTCAST_DEVICE_ROW").split(',');
        if(serial_.isEmpty() || uri_.isEmpty() || row.size()!=2) QSKIP("Explicit authorized phone, known tone URI and current search-row coordinates required");
        MainWindow window; window.show(); window.receivePhone();
        QVERIFY(window.receiverEnabled_); QVERIFY(window.lebo_.running());
        // Optional research recovery from a pre-fix cached port. This is
        // deliberately separate from the normal reconnect assertions below.
        if(qEnvironmentVariableIsSet("NUBIA_SMARTCAST_RESET_DISCOVERY")) {
            adb({"shell","am","force-stop","cn.nubia.touping"});
            adb({"shell","am","start","-W","-n","cn.nubia.touping/.HomeActivity"});
        }
        // Avahi probes the new record asynchronously. Let the phone receive
        // its updated port before selecting a cached last-connected row.
        QTest::qWait(5000);
        QVERIFY(window.lebo_.running());
        adb({"shell","input","tap",row[0],row[1]});
        QTRY_VERIFY_WITH_TIMEOUT(window.lebo_.active(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(window.frameSize_.isValid(),10000);
        qInfo()<<"Native Lelink rendered"<<window.frameSize_;
        QSignalSpy pcm(&window.lebo_,&LeboReceiver::audioData);
        adb({"shell","am","force-stop","cn.zte.music"});
        const auto started=adb({"shell","am","start","-W","-n","cn.zte.music/.activity.AudioPreviewActivity",
            "-a","android.intent.action.VIEW","-d",uri_,"-t","audio/wav"});
        QVERIFY2(started.contains("Status: ok"),qPrintable(started));
        QTRY_VERIFY_WITH_TIMEOUT(!pcm.isEmpty(),5000);
        QTemporaryDir files;
        auto sound=monitor(files.filePath("sound.pcm")); QVERIFY(sound.size()>48000*4*2);
        qInfo()<<"Desktop Lelink RMS"<<rms(sound)<<"frequency"<<frequency(sound);
        QVERIFY(rms(sound)>0.001); QCOMPARE(frequency(sound),440);
        QTest::mouseClick(window.mute_,Qt::LeftButton); QVERIFY(window.mute_->isChecked()); QTest::qWait(300);
        const auto muted=monitor(files.filePath("mute.pcm")); QVERIFY(muted.size()>48000*4*2);
        qInfo()<<"Muted desktop Lelink RMS"<<rms(muted); QVERIFY(rms(muted)<0.0001);
        QTest::mouseClick(window.mute_,Qt::LeftButton); QVERIFY(!window.mute_->isChecked()); QTest::qWait(300);
        sound=monitor(files.filePath("resumed.pcm")); QVERIFY(rms(sound)>0.001); QCOMPARE(frequency(sound),440);
        qInfo()<<"Resumed desktop Lelink RMS"<<rms(sound);
        const auto before=pcm.size(); QVERIFY(before>100);
        adb({"shell","am","force-stop","cn.zte.music"});
        adb({"shell","am","start","-W","-n","cn.nubia.touping/.HomeActivity"}); QTest::qWait(1000);
        QTest::mouseClick(window.shell_->receiveButton,Qt::LeftButton);
        QTRY_VERIFY(!window.lebo_.active()); QVERIFY(window.receiverEnabled_); QVERIFY(window.lebo_.running());
        QTRY_VERIFY(!window.frameSize_.isValid());
        QTest::qWait(2000);
        adb({"shell","input","tap",row[0],row[1]});
        QTRY_VERIFY_WITH_TIMEOUT(window.lebo_.active(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(window.frameSize_.isValid(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(pcm.size()>before,5000);
        qInfo()<<"Native Lelink desktop disconnect/reconnect rendered"<<window.frameSize_;
        window.lebo_.disconnectPhone(); QTRY_VERIFY(!window.lebo_.active());
        window.close();
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format; format.setVersion(2,1); format.setProfile(QSurfaceFormat::NoProfile);
    format.setDepthBufferSize(0); format.setStencilBufferSize(0); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv); SmartcastDeviceTest test; return QTest::qExec(&test,argc,argv);
}
#include "smartcast_device_test.moc"
