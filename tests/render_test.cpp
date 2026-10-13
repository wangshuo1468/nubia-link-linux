#include "window.h"
#include <QLineEdit>
#include "collaboration_window.h"
#include "link_shell.h"
#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QInputMethodEvent>
#include <QComboBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QNetworkProxy>
#include <QStackedWidget>
#include <QWindow>

// Keep navigation tests on loopback; target activation must not publish to a phone.
class ReceivingMainWindow : public MainWindow {
public:
    int receiveCalls=0;
    void receivePhone() override { ++receiveCalls; }
};
class ReceivingCollaborationWindow : public CollaborationWindow {
public:
    int receiveCalls=0;
    void receivePhone() override { ++receiveCalls; }
};

class RenderTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir configuration_;
    static quint16 discoveryPort() {
        QUdpSocket socket; socket.setProxy(QNetworkProxy::NoProxy);
        return socket.bind(QHostAddress::LocalHost,0) ? socket.localPort() : 0;
    }
    static std::shared_ptr<VideoFrame> redFrame(QSize size={65,49}) {
        auto frame = std::make_shared<VideoFrame>(); frame->size = size; frame->bt709 = false;
        const auto chroma=((size.width()+1)/2)*((size.height()+1)/2);
        frame->y = QByteArray(size.width()*size.height(),char(81)); frame->u = QByteArray(chroma,char(90)); frame->v = QByteArray(chroma,char(240));
        return frame;
    }
private slots:
    void initTestCase() {
        QVERIFY(configuration_.isValid()); QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,configuration_.path());
        QCoreApplication::setOrganizationName("NubiaLinkTests");
    }
    void actualOpenGLRender() {
        PhoneView view; view.resize(260,196); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.showFrame(redFrame()); QTest::qWait(100);
        QVERIFY(view.isValid()); const auto image = view.grabFramebuffer(); QVERIFY(!image.isNull());
        for (auto point : {QPoint(20,20),QPoint(image.width()/2,image.height()/2),QPoint(image.width()-20,image.height()-20)}) {
            const auto color = image.pixelColor(point);
            QVERIFY2(color.red() > 245 && color.green() < 10 && color.blue() < 10,qPrintable(color.name()));
        }
    }
    void inputAndChineseCommit() {
        PhoneView view; view.resize(260,196); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view)); view.showFrame(redFrame());
        QSignalSpy control(&view,&PhoneView::control);
        QTest::mousePress(&view,Qt::LeftButton,Qt::NoModifier,{104,80});
        QCOMPARE(control.last()[0].toByteArray(),Nubia::touch(0,{26,20},{65,49}));
        QTest::mouseRelease(&view,Qt::LeftButton,Qt::NoModifier,{104,80});
        QCOMPARE(control.last()[0].toByteArray(),Nubia::touch(1,{26,20},{65,49},0));
        control.clear(); QInputMethodEvent input; input.setCommitString(QStringLiteral("你好，努比亚"));
        QApplication::sendEvent(&view,&input); QCOMPARE(control.count(),1);
        QCOMPARE(control[0][0].toByteArray(),Nubia::text(11,QStringLiteral("你好，努比亚")));
        control.clear(); QTest::keyClick(&view,Qt::Key_A,Qt::ControlModifier);
        QCOMPARE(control.count(),2); QCOMPARE(control[0][0].toByteArray(),Nubia::key(0,29,4096));
        QCOMPARE(control[1][0].toByteArray(),Nubia::key(1,29,4096));
        control.clear(); QTest::keyPress(&view,Qt::Key_Left); view.releaseInput();
        QCOMPARE(control.count(),2); QCOMPARE(control.last()[0].toByteArray(),Nubia::key(1,21));
    }
    void encodedPaddingIsRemovedAndInputUsesSourceCoordinates() {
        PhoneView view; view.resize(96,240); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto frame=redFrame({320,180}); frame->contentRect=QRect(124,0,72,180);
        frame->y.fill(char(16)); frame->u.fill(char(128)); frame->v.fill(char(128));
        for(int row=0;row<180;++row) memset(frame->y.data()+row*320+124,81,72);
        for(int row=0;row<90;++row) {
            memset(frame->u.data()+row*160+62,90,36); memset(frame->v.data()+row*160+62,240,36);
        }
        view.showFrame(frame); QTest::qWait(100);
        const auto image=view.grabFramebuffer();
        for(auto point:{QPoint(0,0),image.rect().center(),QPoint(image.width()-1,image.height()-1)})
            QVERIFY2(image.pixelColor(point).red()>245,qPrintable(image.pixelColor(point).name()));
        QSignalSpy control(&view,&PhoneView::control);
        QTest::mousePress(&view,Qt::LeftButton,Qt::NoModifier,{48,120});
        QCOMPARE(control.last()[0].toByteArray(),Nubia::touch(0,{160,90},{320,180}));
        QTest::mouseRelease(&view,Qt::LeftButton,Qt::NoModifier,{48,120});
        frame->contentRect=QRect(-1,0,999,999); view.showFrame(frame); QTest::qWait(50);
        QVERIFY(view.grabFramebuffer().pixelColor(2,2).red()<10);
    }
    void nativeWindowOffline() {
        MainWindow window; window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window)); QTest::qWait(300);
        QVERIFY(window.windowTitle().contains(QStringLiteral("努比亚")));
        QVERIFY(window.findChildren<QComboBox *>().isEmpty());
        QVERIFY(!window.findChild<QLineEdit *>("phoneAddress"));
        for (auto button : window.findChildren<QPushButton *>()) {
            if (button->toolTip() == QStringLiteral("返回") || button->text() == QStringLiteral("剪贴板 → 手机")) QVERIFY(!button->isEnabled());
        }
        const auto snapshot = qEnvironmentVariable("NUBIA_GUI_SNAPSHOT");
        if (!snapshot.isEmpty()) QVERIFY(window.grab().save(snapshot));
        window.resize(760,620); QTest::qWait(50);
        const auto small=qEnvironmentVariable("NUBIA_GUI_SMALL_SNAPSHOT");
        if(!small.isEmpty()) QVERIFY(window.grab().save(small));
        for(auto button:window.findChildren<QPushButton *>()) {
            if(button->isVisible()) QVERIFY(window.rect().contains(QRect(button->mapTo(&window,QPoint()),button->size())));
        }
    }
    void connectionModesAndFocus() {
        LinkShell shell(false); shell.resize(1040,760); shell.show(); QVERIFY(QTest::qWaitForWindowExposed(&shell));
        auto focus=shell.findChild<QPushButton *>("focusMode"); QVERIFY(focus);
        QTest::mouseClick(focus,Qt::LeftButton); QVERIFY(!shell.sidebar->isVisible()); QVERIFY(!shell.isFullScreen());
        QVERIFY(shell.height()>shell.width()); QVERIFY(!shell.windowHandle()->flags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(QTest::qWaitForWindowActive(&shell));
        QTest::keyClick(&shell,Qt::Key_Escape); QVERIFY(shell.sidebar->isVisible()); QVERIFY(!shell.isFullScreen());
        QVERIFY(!shell.findChild<QComboBox *>("connectionDirection"));
        QVERIFY(!shell.findChild<QLineEdit *>("phoneAddress"));
        shell.setReceiving(true); QVERIFY(shell.receiveButton->isVisible());
        QVERIFY(!shell.findChild<QPushButton *>("acceptConnection"));
        QVERIFY(!shell.findChild<QPushButton *>("rejectConnection"));
        QSignalSpy mode(&shell,&LinkShell::modeRequested);
        QTest::mouseClick(shell.findChild<QPushButton *>("collaborationMode"),Qt::LeftButton);
        QCOMPARE(mode.count(),1); QVERIFY(shell.findChild<QPushButton *>("smartcastMode")->isChecked());
    }
    void collaborationWindowOffline() {
        MainWindow window;
        auto page=window.ensureCollaborationPage(); window.pages_->setCurrentWidget(page);
        window.setWindowTitle(QStringLiteral("努比亚连接 · 多屏协同"));
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(page->findChild<QPushButton *>("collaborationMode")->isChecked());
        QVERIFY(!page->findChild<QPushButton *>("collaborationBack")->isEnabled());
        const auto snapshot=qEnvironmentVariable("NUBIA_COLLABORATION_GUI_SNAPSHOT");
        if(!snapshot.isEmpty()) QVERIFY(window.grab().save(snapshot));
        window.resize(760,620); QTest::qWait(50);
        const auto small=qEnvironmentVariable("NUBIA_COLLABORATION_GUI_SMALL_SNAPSHOT");
        if(!small.isEmpty()) QVERIFY(window.grab().save(small));
        for(auto button:window.findChildren<QPushButton *>()) {
            if(button->isVisible()) QVERIFY(window.rect().contains(QRect(button->mapTo(&window,QPoint()),button->size())));
        }
        const auto stage=page->findChild<QWidget *>("phoneStage");
        QVERIFY(stage); QVERIFY(window.rect().contains(QRect(stage->mapTo(&window,QPoint()),stage->size())));
    }
    void focusShowsOnlyScreenAndRestoresWindow() {
        MainWindow main; auto collaboration=main.ensureCollaborationPage();
        main.resize(1040,760); main.move(70,90); main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        for(auto shell:{main.shell_,collaboration->shell_}) {
            main.pages_->setCurrentWidget(shell==main.shell_ ? static_cast<QWidget *>(shell) : collaboration);
            QYUVOpenGLWidget *view=shell==main.shell_ ? main.view_ : collaboration->view_;
            const auto setVideo=[&](QSize size) {
                const auto frame=redFrame(size);
                if(shell==main.shell_) {
                    main.frameSize_=size; main.fitView(); main.view_->showFrame(frame);
                } else {
                    collaboration->displayed_=frame; collaboration->fitView();
                    view->setFrameSize(size); view->setColorSpace(false,false);
                    view->updateTextures(reinterpret_cast<quint8 *>(frame->y.data()),reinterpret_cast<quint8 *>(frame->u.data()),
                        reinterpret_cast<quint8 *>(frame->v.data()),size.width(),(size.width()+1)/2,(size.width()+1)/2);
                }
            };
            setVideo({108,240});
            view->show(); shell->setWaitingVisible(false);
            for(bool maximized:{false,true}) {
                if(maximized) main.showMaximized(); else main.showNormal();
                QTest::qWait(50);
                const auto geometry=main.geometry(); const auto state=main.windowState();
                auto button=shell->findChild<QPushButton *>("focusMode");
                QTest::mouseClick(button,Qt::LeftButton);
                QVERIFY(!main.isFullScreen()); QVERIFY(!main.isMaximized()); QVERIFY(shell->focused());
                QVERIFY(!main.windowHandle()->flags().testFlag(Qt::FramelessWindowHint));
                QTRY_VERIFY(main.height()>main.width()); QVERIFY(main.height()<=720); QVERIFY(main.width()<=360);
                QVERIFY(qAbs(double(main.width())/main.height()-108.0/240)<0.01);
                QTRY_COMPARE(QRect(shell->stage->mapTo(&main,QPoint()),shell->stage->size()),main.rect());
                QVERIFY(QTest::qWaitForWindowActive(&main)); QTest::qWait(50); QVERIFY(view->isValid()); QVERIFY(view->isVisible());
                QTRY_VERIFY2(view->hasFocus(),qPrintable(QString("Focus=%1, policy=%2, active=%3, collaboration=%4, maximized=%5")
                    .arg(QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : "none")
                    .arg(view->focusPolicy()).arg(main.isActiveWindow()).arg(shell==collaboration->shell_).arg(maximized)));
                const auto pixels=view->grabFramebuffer(); QVERIFY(!pixels.isNull());
                QVERIFY(pixels.pixelColor(pixels.width()/2,pixels.height()/2).red()>245);
                QVERIFY(!shell->placeholder->isVisible());
                for(auto widget:shell->findChildren<QWidget *>()) {
                    if(widget->isVisible()) QVERIFY(widget==shell->stage || widget->isAncestorOf(shell->stage) || shell->stage->isAncestorOf(widget));
                }
                // Landscape uses the available desktop instead of a 640px cap.
                setVideo({240,108});
                const auto available=main.screen()->availableGeometry();
                const auto landscape=QSize(240,108).scaled(QSize(int(available.width()*0.72),int(available.height()*0.78)),Qt::KeepAspectRatio);
                QTRY_COMPARE(main.size(),landscape);
                QTRY_VERIFY((main.frameGeometry().center()-available.center()).manhattanLength()<=2);
                QTRY_VERIFY(view->width()>=landscape.width()-2);
                main.resize(landscape*0.8); QTest::qWait(50); const auto manual=main.size();
                QVERIFY(qAbs(main.width()-main.height()*(240.0/108))<=1);
                setVideo({480,216}); QTest::qWait(50); QCOMPARE(main.size(),manual);
                const auto manualPosition=main.pos();
                // Small aspect changes within landscape must not reset the
                // user's window size or pull it back to the desktop center.
                main.move(manualPosition+QPoint(30,20)); const auto moved=main.pos();
                setVideo({460,216}); QTest::qWait(50);
                QCOMPARE(main.size(),manual); QCOMPARE(main.pos(),moved);
                setVideo({480,216}); QTest::qWait(50);
                QCOMPARE(main.size(),manual); QCOMPARE(main.pos(),moved);
                main.resize(manual.width(),manual.height()+100);
                QTRY_VERIFY(qAbs(main.width()-main.height()*(240.0/108))<=1);
                QTRY_VERIFY(qAbs(view->width()-main.width())<=2 && qAbs(view->height()-main.height())<=2);
                const auto movePointer=[&](QPoint position) {
                    QMouseEvent move(QEvent::MouseMove,position,view->mapToGlobal(position),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                    QApplication::sendEvent(view,&move);
                };
                movePointer({1,1}); QCOMPARE(view->cursor().shape(),Qt::SizeFDiagCursor);
                movePointer(view->rect().center()); QCOMPARE(view->cursor().shape(),Qt::ArrowCursor);
                setVideo({108,240}); QTRY_VERIFY(main.height()>main.width());
                // Disconnect and responsive text layout must not bring UI back.
                shell->setWaitingVisible(true); shell->setMediaActive(false);
                shell->resize(shell->size());
                QVERIFY(!shell->placeholder->isVisible()); QVERIFY(!button->isVisible());
                QTest::keyClick(view,Qt::Key_Escape);
                QTRY_VERIFY(!main.isFullScreen()); QVERIFY(!shell->focused());
                QTRY_COMPARE(main.windowState(),state); QTRY_COMPARE(main.geometry(),geometry);
                QVERIFY(!main.windowHandle()->flags().testFlag(Qt::FramelessWindowHint));
                QVERIFY(view->isValid());
                QVERIFY(button->isVisible()); QVERIFY(shell->sidebar->isVisible()); QVERIFY(shell->placeholder->isVisible());
            }
        }
    }
    void switchFromListeningCollaborationWithoutManualPause() {
        ReceivingMainWindow main;
        main.resize(1040,760); main.move(70,90); main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        const auto nativeId=main.winId();
        auto collaboration=main.ensureCollaborationPage();
        QVERIFY(!collaboration->isWindow()); QCOMPARE(collaboration->window(),&main);
        for(int round=1;round<=3;++round) {
            if(round==2) { main.showMaximized(); QTRY_VERIFY(main.isMaximized()); }
            if(round==3) { main.showNormal(); QTRY_VERIFY(!main.isMaximized()); }
            QTest::qWait(50);
            const auto geometry=main.geometry(); const auto state=main.windowState();
            const auto active=main.isActiveWindow();
            main.pages_->setCurrentWidget(collaboration);
            QTest::qWait(30);
            QCOMPARE(main.winId(),nativeId); QCOMPARE(main.geometry(),geometry); QCOMPARE(main.windowState(),state);
            QCOMPARE(main.isActiveWindow(),active);
            const auto port=discoveryPort(); QVERIFY(port);
            QVERIFY(collaboration->session_.listen(QHostAddress::LocalHost,8,"lo",port));
            collaboration->receiverEnabled_=true; collaboration->shell_->setReceiving(true);
            QVERIFY(collaboration->running());
            QTest::mouseClick(collaboration->findChild<QPushButton *>("smartcastMode"),Qt::LeftButton);
            QTRY_COMPARE(main.pages_->currentWidget(),main.shell_); QVERIFY(main.isVisible()); QVERIFY(!collaboration->isVisible());
            QVERIFY(!collaboration->running()); QVERIFY(!collaboration->receiverEnabled_);
            QCOMPARE(main.receiveCalls,round);
            QCOMPARE(main.winId(),nativeId); QCOMPARE(main.geometry(),geometry); QCOMPARE(main.windowState(),state);
            QCOMPARE(main.isActiveWindow(),active);
        }
    }
    void closeHostStopsEmbeddedReceiver() {
        ReceivingMainWindow main;
        auto collaboration=main.ensureCollaborationPage(); main.pages_->setCurrentWidget(collaboration);
        main.show(); QVERIFY(QTest::qWaitForWindowExposed(&main));
        const auto port=discoveryPort(); QVERIFY(port);
        QVERIFY(collaboration->session_.listen(QHostAddress::LocalHost,8,"lo",port));
        collaboration->receiverEnabled_=true;
        main.close(); QTRY_VERIFY(!main.isVisible());
        QVERIFY(!collaboration->running()); QVERIFY(!collaboration->receiverEnabled_);
        QCOMPARE(main.receiveCalls,0);
    }
    void collaborationReopensReceiverAfterSessionEnds() {
        ReceivingCollaborationWindow window; window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto port=discoveryPort(); QVERIFY(port);
        QVERIFY(window.session_.listen(QHostAddress::LocalHost,8,"lo",port)); window.receiverEnabled_=true;
        window.session_.stop(); // Same stopped path as a remote disconnect.
        QTRY_COMPARE(window.receiveCalls,1);
        port=discoveryPort(); QVERIFY(port);
        QVERIFY(window.session_.listen(QHostAddress::LocalHost,8,"lo",port)); window.receiverEnabled_=true;
        window.toggleReceiving(); QTest::qWait(50);
        QCOMPARE(window.receiveCalls,1); // Explicit pause must stay paused.
    }
    void desktopModeReceivers() {
        if(!qEnvironmentVariableIsSet("NUBIA_TEST_LIVE_MODES")) QSKIP("Opt-in desktop Wi-Fi / Bluetooth receiver check");
        MainWindow main; main.show(); QVERIFY(QTest::qWaitForWindowExposed(&main)); main.receivePhone();
        QTRY_VERIFY(main.receiverEnabled_ && main.lebo_.running());
        const auto nativeId=main.winId(); const auto geometry=main.geometry();
        const auto state=main.windowState();
        for(int round=0;round<3;++round) {
            QTest::mouseClick(main.shell_->findChild<QPushButton *>("collaborationMode"),Qt::LeftButton);
            auto collaboration=main.collaborationPage_; QVERIFY(collaboration);
            QTRY_VERIFY(collaboration->isVisible() && collaboration->receiverEnabled_ && collaboration->running());
            QVERIFY(main.isVisible()); QVERIFY(!collaboration->isWindow());
            QCOMPARE(main.pages_->currentWidget(),collaboration);
            QCOMPARE(main.winId(),nativeId); QCOMPARE(main.geometry(),geometry); QCOMPARE(main.windowState(),state);
            QVERIFY(!main.lebo_.running()); QVERIFY(!main.lan_.running());
            QTest::mouseClick(collaboration->findChild<QPushButton *>("smartcastMode"),Qt::LeftButton);
            QTRY_VERIFY(main.isVisible() && main.receiverEnabled_ && main.lebo_.running());
            QVERIFY(!collaboration->running()); QVERIFY(!collaboration->receiverEnabled_);
            QCOMPARE(main.pages_->currentWidget(),main.shell_);
            QCOMPARE(main.winId(),nativeId); QCOMPARE(main.geometry(),geometry); QCOMPARE(main.windowState(),state);
        }
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format; format.setVersion(2,1); format.setProfile(QSurfaceFormat::NoProfile);
    format.setDepthBufferSize(0); format.setStencilBufferSize(0); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv); RenderTest test; return QTest::qExec(&test,argc,argv);
}
#include "render_test.moc"
