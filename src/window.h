#pragma once
#include "audio.h"
#include "decoder.h"
#include "session.h"
#include "lan_projection.h"
#include "lebo_receiver.h"
#include "qyuvopenglwidget.h"
#include <QMainWindow>
#include <QSet>
class QLabel;
class QPushButton;
class QCheckBox;
class LinkShell;
class QStackedWidget;
class CollaborationWindow;

class PhoneView : public QYUVOpenGLWidget {
    Q_OBJECT
public:
    explicit PhoneView(QWidget *parent = nullptr);
    void showFrame(const std::shared_ptr<VideoFrame> &frame);
    void releaseInput();
signals:
    void control(QByteArray bytes);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusOutEvent(QFocusEvent *event) override;
private:
    QPoint phonePoint(QPointF point) const;
    void sendText(const QString &text);
    QSize videoSize_;
    bool touching_ = false;
    QPoint lastPoint_;
    QSet<int> heldKeys_;
    std::shared_ptr<VideoFrame> displayed_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void connectDevice(const QString &serial);
    void connectLan(const QString &phone, const QString &local = {});
    virtual void receivePhone();
    void openFiles(const QString &phone, const QString &local = {});
    void openCollaboration(const QString &phone,const QString &local={},bool autoConnect=false,QString package="com.android.settings");
private:
    friend class SmartcastDeviceTest;
    friend class RenderTest;
    void fitView();
    void setActive(bool active);
    void deviceMessage(quint8 type, const QString &value);
    void toggleReceiving();
    CollaborationWindow *ensureCollaborationPage();
    void showSmartcast();
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    QStackedWidget *pages_;
    CollaborationWindow *collaborationPage_=nullptr;
    bool closing_=false;
    LinkShell *shell_;
    ProjectionSession session_;
    LanProjection lan_{session_};
    LeboReceiver lebo_;
    VideoDecoder decoder_;
    PhoneAudio audio_;
    QTimer displayTimer_, clipboardTimeout_;
    QWidget *stage_;
    PhoneView *view_;
    QLabel *placeholder_, *state_, *details_, *audioState_;
    QPushButton *getClipboard_, *pushClipboard_;
    QString phoneAddress_;
    QCheckBox *expanded_;
    QPushButton *mute_;
    QList<QPushButton *> navigation_;
    QSize frameSize_;
    bool connecting_ = false, clipboardRequested_ = false;
    bool receiverEnabled_=false;
};
