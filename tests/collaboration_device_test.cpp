#include "collaboration_window.h"
#include <QtTest>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QProcess>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <QRegularExpression>

// Explicit opt-in: exercises default incoming connection without a desktop prompt.
// No unattended CTest registration and no simulated multi-window claims.
class CollaborationDeviceTest:public QObject {
    Q_OBJECT
private slots:
    void independentApplicationAndPhoneHome() {
        if(!qEnvironmentVariableIsSet("NUBIA_TEST_INDEPENDENT")) QSKIP("Explicit independent application device test only");
        const auto serial=qEnvironmentVariable("NUBIA_TEST_DEVICE");
        QVERIFY(!serial.isEmpty());
        auto adb=[&](QStringList args) {
            QProcess process; process.start("adb",QStringList{"-s",serial}+args);
            if(!process.waitForFinished(5000) || process.exitCode()!=0) return QByteArray();
            return process.readAllStandardOutput();
        };
        CollaborationWindow window; window.show(); window.receivePhone();
        // The ROM may need another BLE scan cycle after reopening its page.
        QTest::qWait(20000);
        const auto row=qEnvironmentVariable("NUBIA_COLLABORATION_DEVICE_ROW").split(',');
        QVERIFY2(row.size()==2,"Screenshot-verified original phone row coordinates required");
        adb({"shell","input","tap",row[0],row[1]});
        QTRY_VERIFY_WITH_TIMEOUT(window.decodedFrames()>=3 || !window.running(),30000);
        QVERIFY(window.decodedFrames()>=3);
        QCOMPARE(window.castDisplayType(),QString("mirror"));
        auto application=window.findChild<QPushButton *>("collaborationApplication"); QVERIFY(application);
        QTRY_VERIFY_WITH_TIMEOUT(application->isEnabled(),10000);
        QTest::mouseClick(application,Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window.castDisplayType(),QString("virtualDisplay"),10000);
        QTRY_VERIFY_WITH_TIMEOUT(window.decodedFrames()>=3 && window.displayedSize()==QSize(2400,1080),30000);
        const auto tasks=QString::fromUtf8(adb({"shell","dumpsys","activity","activities"}));
        QRegularExpression separate("Display #(\\d+) \\(activities from top to bottom\\):(?:(?!\\nDisplay #)[\\s\\S])*?topResumedActivity=[^\\n]*com\\.android\\.settings/");
        const auto match=separate.match(tasks); QVERIFY2(match.hasMatch(),qPrintable(tasks.left(500)));
        const auto displayId=match.captured(1); QVERIFY(displayId!="0");
        if(const auto path=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_SNAPSHOT");!path.isEmpty()) QVERIFY(window.grab().save(path));
        adb({"shell","input","-d","0","keyevent","KEYCODE_HOME"});
        QTest::qWait(2000);
        auto current=QString::fromUtf8(adb({"shell","dumpsys","activity","activities"}));
        QFile diagnostic("/tmp/nubia-independent-tasks.txt");
        if(diagnostic.open(QIODevice::WriteOnly)) { diagnostic.write(current.toUtf8()); diagnostic.close(); }
        if(const auto path=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_SNAPSHOT");!path.isEmpty()) QVERIFY(window.grab().save(path+".home.png"));
        QVERIFY(current.contains("Display #"+displayId+" (activities"));
        const auto main=current.mid(current.indexOf("Display #0 (activities"));
        QVERIFY(main.section("topResumedActivity=",1,1).section('\n',0,0).contains("launcher"));
        QVERIFY2(separate.match(current).captured(1)==displayId,qPrintable(current.left(1200)));
        QTRY_VERIFY_WITH_TIMEOUT(window.mouseReady(),5000);
        auto view=window.findChild<QWidget *>("collaborationView"); QVERIFY(view);
        QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,QPoint(view->width()/2,view->height()*23/100));
        QTRY_VERIFY_WITH_TIMEOUT(window.editable(),10000);
        auto text=window.findChild<QLineEdit *>("collaborationText"); auto send=window.findChild<QPushButton *>("collaborationSendText");
        QVERIFY(text && send); text->setText(QStringLiteral("蓝牙")); QTest::mouseClick(send,Qt::LeftButton);
        QTest::qWait(2000);
        current=QString::fromUtf8(adb({"shell","dumpsys","activity","activities"}));
        if(diagnostic.open(QIODevice::WriteOnly)) { diagnostic.write(current.toUtf8()); diagnostic.close(); }
        if(const auto path=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_SNAPSHOT");!path.isEmpty()) QVERIFY(window.grab().save(path+".input.png"));
        // Settings search is a separate system package on NebulaAIOS. It must
        // stay on the original independent display, just like Settings itself.
        QRegularExpression searchDisplay("Display #(\\d+) \\(activities from top to bottom\\):(?:(?!\\nDisplay #)[\\s\\S])*?topResumedActivity=[^\\n]*com\\.android\\.settings(?:\\.intelligence)?/");
        QVERIFY2(searchDisplay.match(current).captured(1)==displayId,qPrintable(current.left(500)));
        QVERIFY(current.mid(current.indexOf("Display #0 (activities")).section("topResumedActivity=",1,1).section('\n',0,0).contains("launcher"));
        if(const auto path=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_SNAPSHOT");!path.isEmpty()) QVERIFY(window.grab().save(path));
        if(const auto inspection=qEnvironmentVariableIntValue("NUBIA_TEST_INSPECTION_MS");inspection>0) QTest::qWait(inspection);
        qInfo()<<"Independent Settings display"<<displayId<<"phone foreground is launcher";
        QTest::qWait(5000); QVERIFY(window.running());
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.running(),10000);
    }
    void sustainedWindowAndNormalClose() {
        if(qEnvironmentVariableIsSet("NUBIA_TEST_INDEPENDENT")) QSKIP("Separate independent application test selected");
        const auto phone=qEnvironmentVariable("NUBIA_TEST_LAN_PHONE"),local=qEnvironmentVariable("NUBIA_TEST_LAN_LOCAL");
        if(phone.isEmpty() || local.isEmpty()) QSKIP("Set NUBIA_TEST_LAN_PHONE and NUBIA_TEST_LAN_LOCAL for the authorized phone");
        QTemporaryDir snapshots;
        CollaborationWindow window; window.show();
        const auto package=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_PACKAGE","com.android.settings");
        if(qEnvironmentVariableIsSet("NUBIA_TEST_RECEIVE")) {
            window.receivePhone();
        } else window.connectPhone(phone,local,package);
        QElapsedTimer connectionWait; connectionWait.start();
        const auto connectionDeadline=qEnvironmentVariableIsSet("NUBIA_TEST_RECEIVE") ? 180000 : 120000;
        while(window.decodedFrames()<3 && window.running() && connectionWait.elapsed()<connectionDeadline)
            QTest::qWait(100);
        QVERIFY2(window.decodedFrames()>=3,"No collaboration frames within the connection deadline");
        const auto width=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_WIDTH","1080").toInt();
        const auto height=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_HEIGHT","2400").toInt();
        const QSize expectedSize(width,height);
        QTRY_COMPARE_WITH_TIMEOUT(window.displayedSize(),expectedSize,5000);
        QVERIFY(window.grab().save(snapshots.filePath("window.png")));
        if(const auto path=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_SNAPSHOT"); !path.isEmpty()) QVERIFY(window.grab().save(path));
        if(qEnvironmentVariableIsSet("NUBIA_TEST_COLLABORATION_AUDIO")) {
            const auto uri=qEnvironmentVariable("NUBIA_AUDIO_TEST_URI"),serial=qEnvironmentVariable("NUBIA_TEST_DEVICE");
            QVERIFY2(!uri.isEmpty() && !serial.isEmpty(),"Explicit known test tone URI and authorized ADB serial required");
            QProcess prepare; prepare.start("adb",{"-s",serial,"shell","am","start","--activity-clear-top","-n",
                "cn.zte.music/.activity.AudioPreviewActivity","-a","android.intent.action.VIEW","-d",uri,"-t","audio/wav"});
            QVERIFY(prepare.waitForFinished(5000)); QVERIFY(!prepare.readAll().contains("Error:"));
            QProcess sink; sink.start("pactl",{"get-default-sink"}); QVERIFY(sink.waitForFinished(3000)); QCOMPARE(sink.exitCode(),0);
            const auto source=QString::fromUtf8(sink.readAllStandardOutput()).trimmed()+".monitor";
            QProcess monitor; const auto pcmPath=snapshots.filePath("output.pcm");
            monitor.start("ffmpeg",{"-v","error","-f","pulse","-i",source,"-t","5","-ar","48000","-ac","2","-f","s16le",pcmPath});
            QTRY_VERIFY_WITH_TIMEOUT(window.decodedAudioBytes()>48000,10000);
            QTRY_COMPARE_WITH_TIMEOUT(monitor.state(),QProcess::NotRunning,10000); QCOMPARE(monitor.exitCode(),0);
            QFile pcmFile(pcmPath); QVERIFY(pcmFile.open(QIODevice::ReadOnly)); const auto pcm=pcmFile.readAll();
            QVERIFY(pcm.size()>19200); double bestRms=0,bestFrequency=0;
            for(int offset=0;offset+19200<=pcm.size();offset+=19200) {
                double energy=0; int crossings=0;
                for(int i=offset+4;i<offset+19200;i+=4) {
                    const auto previous=qFromLittleEndian<qint16>(pcm.constData()+i-4),value=qFromLittleEndian<qint16>(pcm.constData()+i);
                    energy+=double(value)*value; if(previous<=0 && value>0) ++crossings;
                }
                const double rms=std::sqrt(energy/4800)/32768;
                if(rms>bestRms) { bestRms=rms; bestFrequency=crossings*10; }
            }
            qInfo()<<"Desktop Opus audio monitor RMS"<<bestRms<<"frequency"<<bestFrequency;
            qInfo()<<"Phone Opus decoded bytes"<<window.decodedAudioBytes()<<"peak"<<window.decodedAudioPeak();
            QVERIFY2(bestRms>0.001,"Desktop output remained silent");
            QVERIFY2(std::abs(bestFrequency-440)<20,"Desktop output did not contain the known 440Hz tone");
        }
        const auto before=window.decodedFrames();
        qInfo()<<"Collaboration sustained test started at frame"<<before;
        const int idle=qEnvironmentVariableIntValue("NUBIA_TEST_COLLABORATION_IDLE_MS");
        QTest::qWait(idle>0 ? idle : 35000);
        QVERIFY(window.running()); QCOMPARE(window.displayedSize(),expectedSize);
        qInfo()<<"Collaboration decoded frames after idle:"<<window.decodedFrames();
        qInfo()<<"Collaboration decoded audio bytes:"<<window.decodedAudioBytes();
        if(qEnvironmentVariableIsSet("NUBIA_TEST_COLLABORATION_INPUT")) {
            if(qEnvironmentVariableIsSet("NUBIA_TEST_RECEIVE")) {
                // Phone-initiated mirror displays the real foreground task,
                // unlike the computer-requested Settings virtual display.
                const auto serial=qEnvironmentVariable("NUBIA_TEST_DEVICE");
                QVERIFY2(!serial.isEmpty(),"Authorized ADB serial required to prepare mirror input test");
                QProcess settings; settings.start("adb",{"-s",serial,"shell","am","start","--activity-clear-top","-n","com.android.settings/.Settings"});
                QVERIFY(settings.waitForFinished(5000)); QCOMPARE(settings.exitCode(),0);
                QVERIFY(!settings.readAll().contains("Error:")); QTest::qWait(2000);
            }
            QTRY_VERIFY_WITH_TIMEOUT(window.mouseReady(),10000);
            auto view=window.findChild<QWidget *>("collaborationView"); QVERIFY(view); QVERIFY(view->isVisible());
            const auto beforeClick=window.decodedFrames();
            QTest::mouseClick(view,Qt::LeftButton,Qt::NoModifier,QPoint(view->width()/2,view->height()*12/100));
            QTRY_VERIFY_WITH_TIMEOUT(window.editable(),10000);
            QTRY_VERIFY_WITH_TIMEOUT(window.decodedFrames()>beforeClick,5000);
            auto text=window.findChild<QLineEdit *>("collaborationText"); QVERIFY(text); QVERIFY(text->isEnabled());
            auto send=window.findChild<QPushButton *>("collaborationSendText"); QVERIFY(send); QVERIFY(send->isEnabled());
            const auto beforeText=window.decodedFrames();
            text->setText(QStringLiteral("蓝牙")); QTest::mouseClick(send,Qt::LeftButton);
            QCOMPARE(text->text(),QString()); QTRY_VERIFY_WITH_TIMEOUT(window.decodedFrames()>beforeText,5000);
            QTest::qWait(2000);
            if(const auto path=qEnvironmentVariable("NUBIA_TEST_COLLABORATION_SNAPSHOT"); !path.isEmpty()) QVERIFY(window.grab().save(path+".input.png"));
            qInfo()<<"Official input channels sent Settings search text; screenshot requires visual verification";
        }
        if(qEnvironmentVariableIsSet("NUBIA_TEST_COLLABORATION_BACK")) {
            auto back=window.findChild<QPushButton *>("collaborationBack"); QVERIFY(back); QVERIFY(back->isEnabled());
            QTest::mouseClick(back,Qt::LeftButton); QTest::qWait(1000);
            QTRY_VERIFY_WITH_TIMEOUT(window.decodedFrames()>before,5000);
        }
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(),10000);
        QTRY_VERIFY(!window.isVisible());
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format; format.setVersion(2,1); format.setProfile(QSurfaceFormat::NoProfile);
    format.setDepthBufferSize(0); format.setStencilBufferSize(0); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    CollaborationDeviceTest test; return QTest::qExec(&test,argc,argv);
}
#include "collaboration_device_test.moc"
