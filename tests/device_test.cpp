// Opt-in real phone verification. Not registered with unattended CTest runs.
// Opens Settings to verify input, then returns to the launcher.
#include "session.h"
#include "decoder.h"
#include "window.h"
#include "link_shell.h"
#include <QtTest>
#include <QApplication>
#include <QInputMethodEvent>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QXmlStreamReader>
#include <QtEndian>

class DeviceTest : public QObject {
    Q_OBJECT
    ProjectionSession session_;
    LanProjection lan_{session_};
    VideoDecoder decoder_;
    PhoneAudio audio_;
    PhoneView view_;
    QString serial_, lastStatus_;
    std::shared_ptr<VideoFrame> frame_;
    QTimer display_;
    quint64 audioBytes_ = 0, nonzeroAudioSamples_ = 0;
    QByteArray audioRemainder_;
    bool touchedSettings_ = false;
    bool lanMode_ = false;
    quint64 decodedFrames_ = 0;
    QString adb(const QStringList &args,int timeout=6000) {
        QProcess process; QStringList options{"-s",serial_}; options += args;
        process.start("adb",options);
        if (!process.waitForFinished(timeout)) { process.kill(); process.waitForFinished(1000); return {}; }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
        return QString::fromUtf8(process.readAllStandardOutput());
    }
    void home() { session_.send(Nubia::key(0,3)); session_.send(Nubia::key(1,3)); }
    QString focused() {
        const auto dump=adb({"shell","dumpsys","window"});
        QString out;
        for (const auto &line : dump.split('\n'))
            if (line.contains("mCurrentFocus=") || line.contains("mFocusedApp=")) out += line + '\n';
        return out;
    }
    bool openSettings() {
        touchedSettings_=true;
        const auto result=adb({"shell","am","start","--activity-clear-top","-n","com.android.settings/.homepage.SettingsHomepageActivity"});
        if (result.contains("Error:")) return false;
        for (int i=0;i<20;++i) { QTest::qWait(100); if (focused().contains("SettingsHomepageActivity")) return true; }
        return false;
    }
    QString ui() {
        adb({"shell","uiautomator","dump","/data/local/tmp/nubia-link-device-test.xml"},12000);
        return adb({"shell","cat","/data/local/tmp/nubia-link-device-test.xml"});
    }
private slots:
    void initTestCase() {
        serial_=qEnvironmentVariable("NUBIA_TEST_DEVICE");
        if (serial_.isEmpty()) QSKIP("Set NUBIA_TEST_DEVICE to an authorized ADB serial or endpoint");
        QVERIFY2(adb({"reverse","--list"}).trimmed().isEmpty(),"Close other projection sessions first");
        connect(&session_,&ProjectionSession::status,this,[this](const QString &text) { lastStatus_=text; });
        connect(&lan_,&LanProjection::status,this,[this](const QString &text) { lastStatus_=text; qInfo().noquote()<<text; });
        connect(&session_,&ProjectionSession::videoPacket,&decoder_,&VideoDecoder::submit);
        connect(&decoder_,&VideoDecoder::error,this,[this](const QString &text,quint64) { lastStatus_=text; });
        connect(&view_,&PhoneView::control,&session_,&ProjectionSession::send);
        connect(&session_,&ProjectionSession::audioData,this,[this](QByteArray bytes) {
            if (qEnvironmentVariableIsSet("NUBIA_TEST_AUDIO_OUTPUT")) audio_.append(bytes);
            audioBytes_+=quint64(bytes.size()); audioRemainder_+=bytes;
            const auto complete=audioRemainder_.size()/2*2;
            for (qsizetype i=0;i<complete;i+=2)
                if (qFromLittleEndian<qint16>(audioRemainder_.constData()+i)!=0) ++nonzeroAudioSamples_;
            audioRemainder_.remove(0,complete);
        });
        view_.resize(360,800); view_.show(); QVERIFY(QTest::qWaitForWindowExposed(&view_));
        display_.setInterval(16);
        connect(&display_,&QTimer::timeout,this,[this] {
            if (auto frame=decoder_.takeFrame()) { ++decodedFrames_; frame_=std::move(frame); view_.showFrame(frame_); }
        });
        display_.start();
        const auto phone=qEnvironmentVariable("NUBIA_TEST_LAN_PHONE"); lanMode_=!phone.isEmpty();
        if(qEnvironmentVariableIsSet("NUBIA_TEST_RECEIVE")) {
            lanMode_=true;
            const auto interface=LinkShell::receiverInterface();
            QVERIFY(lan_.listen(interface.address,interface.prefix,interface.name));
        } else if(lanMode_) QVERIFY(lan_.start(QHostAddress(qEnvironmentVariable("NUBIA_TEST_LAN_LOCAL")),QHostAddress(phone)));
        else session_.startAdb(serial_);
        if (qEnvironmentVariableIsSet("NUBIA_TEST_AUDIO_OUTPUT")) {
            connect(&audio_,&PhoneAudio::warning,this,[](const QString &warning) { qWarning().noquote()<<warning; });
            audio_.start();
        }
        QTRY_VERIFY_WITH_TIMEOUT(session_.connected(),60000);
        QTRY_VERIFY_WITH_TIMEOUT(bool(frame_),20000);
        qInfo("Real phone video: %d x %d",frame_->size.width(),frame_->size.height());
    }
    void videoAndKeyboardControl() {
        QVERIFY2(frame_ && frame_->size.width()>0 && frame_->size.height()>0,qPrintable(lastStatus_));
        QVERIFY(!view_.grabFramebuffer().isNull());
        QVERIFY(openSettings()); home();
        QTRY_VERIFY_WITH_TIMEOUT(focused().contains("com.zte.mifavor.launcher"),5000);
    }
    void streamSurvivesAdbDisconnect() {
        if(!lanMode_) QSKIP("This independence check applies only to the original LAN entry");
        QProcess control; control.start("adb",{"disconnect",serial_}); QVERIFY(control.waitForFinished(3000));
        const auto before=decodedFrames_; QTest::qWait(2500);
        // Restore the research/verification connection even if streaming failed.
        control.start("adb",{"connect",serial_}); QVERIFY(control.waitForFinished(5000));
        QVERIFY2(decodedFrames_>before,"No decoded LAN frames arrived while ADB was disconnected");
        QVERIFY(session_.connected()); qInfo("Decoded %llu frames with ADB disconnected",static_cast<unsigned long long>(decodedFrames_-before));
    }
    void touchAndChineseInput() {
        QVERIFY(openSettings()); const auto xml=ui(); QVERIFY(!xml.isEmpty());
        QXmlStreamReader reader(xml); QRect search;
        while (!reader.atEnd()) {
            reader.readNext(); if (!reader.isStartElement() || reader.name()!="node") continue;
            const auto attributes=reader.attributes();
            const auto text=attributes.value("text").toString();
            const auto description=attributes.value("content-desc").toString();
            const auto id=attributes.value("resource-id").toString();
            if (!text.contains(QStringLiteral("搜索")) && !description.contains(QStringLiteral("搜索")) && !id.contains("search_action_bar")) continue;
            const auto match=QRegularExpression("\\[(\\d+),(\\d+)\\]\\[(\\d+),(\\d+)\\]").match(attributes.value("bounds").toString());
            if (match.hasMatch()) { search=QRect(QPoint(match.captured(1).toInt(),match.captured(2).toInt()),QPoint(match.captured(3).toInt()-1,match.captured(4).toInt()-1)); break; }
        }
        QVERIFY2(search.isValid(),"Settings search widget was not found; adapt to this ROM before continuing");
        const auto size=QRegularExpression("Physical size: (\\d+)x(\\d+)").match(adb({"shell","wm","size"})); QVERIFY(size.hasMatch());
        const QPoint point(search.center().x()*view_.width()/size.captured(1).toInt(),search.center().y()*view_.height()/size.captured(2).toInt());
        QTest::mouseClick(&view_,Qt::LeftButton,Qt::NoModifier,point); QTest::qWait(500);
        const auto editable=ui(); QVERIFY2(editable.contains("android.widget.EditText"),"Touch did not open the Settings search editor");
        QInputMethodEvent input; input.setCommitString(QStringLiteral("努比亚测试")); QApplication::sendEvent(&view_,&input);
        QTest::qWait(300);
        QVERIFY2(ui().contains(QStringLiteral("努比亚测试")),"Original Unicode input command did not reach the focused editor");
        home(); QTRY_VERIFY_WITH_TIMEOUT(focused().contains("com.zte.mifavor.launcher"),5000);
    }
    void audioChannel() {
        const auto uri=qEnvironmentVariable("NUBIA_AUDIO_TEST_URI");
        if (!uri.isEmpty()) {
            touchedSettings_=true;
            const auto result=adb({"shell","am","start","-n","cn.zte.music/.activity.AudioPreviewActivity",
                                   "-a","android.intent.action.VIEW","-d",uri,"-t","audio/wav"});
            QVERIFY2(!result.contains("Error:"),qPrintable(result));
            QTest::qWait(10000);
            session_.send(Nubia::key(0,86)); session_.send(Nubia::key(1,86));
            home();
        }
        qInfo("PCM received: %llu bytes, %llu nonzero samples",static_cast<unsigned long long>(audioBytes_),static_cast<unsigned long long>(nonzeroAudioSamples_));
        if (!audioBytes_ && uri.isEmpty() && !qEnvironmentVariableIsSet("NUBIA_AUDIO_TEST_ACTIVE"))
            QSKIP("No PCM during this silent session; play a known audio source and set NUBIA_AUDIO_TEST_ACTIVE before validating audio");
        QVERIFY2(audioBytes_>0,"No raw PCM data received from the audio channel");
        if (!uri.isEmpty()) QVERIFY2(nonzeroAudioSamples_>0,"The known test source produced only zero PCM samples");
        // Nonzero PCM does not by itself verify audible desktop output.
    }
    void clipboardRead() {
        // Read only: never log the phone's clipboard or replace either device's
        // clipboard while verifying the original request/notification path.
        QSignalSpy messages(&session_,&ProjectionSession::deviceMessage);
        QVERIFY(session_.connected());
        session_.send(QByteArray(1,char(7)));
        QElapsedTimer elapsed; elapsed.start();
        while(elapsed.elapsed()<3000) {
            for(const auto &message:messages) {
                if(message.at(0).toUInt()==0) {
                    qInfo("Original clipboard text notification received (contents omitted)");
                    return;
                }
            }
            QTest::qWait(20);
        }
        QSKIP("No clipboard text notification; the original service does not reply to a null clipboard");
    }
    void manualInputProbe() {
        // Only use coordinates explicitly observed on this phone's Settings
        // screenshot. This is a visual probe, not an automated input pass.
        const auto coordinates=qEnvironmentVariable("NUBIA_SETTINGS_SEARCH_POINT").split(',');
        const auto snapshot=qEnvironmentVariable("NUBIA_INPUT_PROBE_SNAPSHOT");
        if (coordinates.size()!=2 || snapshot.isEmpty()) QSKIP("Set an observed Settings search point and output snapshot for manual verification");
        bool xOk=false,yOk=false;
        const int x=coordinates[0].toInt(&xOk),y=coordinates[1].toInt(&yOk);
        QVERIFY(xOk && yOk); QVERIFY(openSettings()); QTest::qWait(500);
        const auto size=QRegularExpression("Physical size: (\\d+)x(\\d+)").match(adb({"shell","wm","size"})); QVERIFY(size.hasMatch());
        const QPoint point(x*view_.width()/size.captured(1).toInt(),y*view_.height()/size.captured(2).toInt());
        QTest::mouseClick(&view_,Qt::LeftButton,Qt::NoModifier,point); QTest::qWait(800);
        qInfo().noquote()<<"Focus after original touch:"<<focused();
        QInputMethodEvent input; input.setCommitString(QStringLiteral("努比亚测试")); QApplication::sendEvent(&view_,&input);
        QTest::qWait(1200); QVERIFY(view_.grabFramebuffer().save(snapshot));
        qInfo().noquote()<<"Manual input evidence:"<<snapshot;
        QSKIP("Inspect the snapshot to verify touch and Chinese; UIAutomation is unavailable on this ROM");
    }
    void cleanupTestCase() {
        if (!serial_.isEmpty() && touchedSettings_) { home(); QTest::qWait(200); adb({"shell","rm","-f","/data/local/tmp/nubia-link-device-test.xml"}); }
        display_.stop(); audio_.stop(); lan_.stop();
        if (!serial_.isEmpty()) QVERIFY2(adb({"reverse","--list"}).trimmed().isEmpty(),"Owned reverse mapping was not removed");
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format; format.setVersion(2,1); format.setDepthBufferSize(0); format.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format); QApplication app(argc,argv); DeviceTest test; return QTest::qExec(&test,argc,argv);
}
#include "device_test.moc"
