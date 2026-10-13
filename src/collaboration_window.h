#pragma once
#include "collaboration_session.h"
#include "qyuvopenglwidget.h"
#include <QMainWindow>
class QLabel;
class QLineEdit;
class QPushButton;
class QCloseEvent;
class LinkShell;

class CollaborationWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit CollaborationWindow(QWidget *parent=nullptr);
    ~CollaborationWindow() override;
    void connectPhone(QString phone,QString local={},QString package="com.android.settings");
    virtual void receivePhone();
    void stopForClose();
    bool running() const { return session_.running(); }
    quint64 decodedFrames() const { return session_.frameCount(); }
    QSize displayedSize() const { return displayed_ ? displayed_->size : QSize(); }
    bool mouseReady() const { return session_.mouseReady(); }
    bool editable() const { return session_.editable(); }
    bool requestApplication(QString package) { return session_.requestApplication(std::move(package)); }
    QString castDisplayType() const { return session_.castDisplayType(); }
    quint64 decodedAudioBytes() const { return session_.audioBytes(); }
    int decodedAudioPeak() const { return session_.audioPeak(); }
signals:
    void smartcastRequested();
    void shutdownFinished();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object,QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    friend class RenderTest;
    void fitView();
    void switchToSmartcast();
    void finishModeSwitch();
    void releasePointer();
    void toggleReceiving();
    LinkShell *shell_;
    CollaborationSession session_;
    QTimer display_;
    QWidget *stage_;
    QYUVOpenGLWidget *view_;
    QLabel *placeholder_,*status_,*statistics_;
    QLineEdit *package_,*text_;
    QPushButton *back_,*sendText_,*application_;
    QHostAddress localAddress_,phoneAddress_;
    std::shared_ptr<VideoFrame> displayed_;
    bool closing_=false;
    bool receiverEnabled_=false;
    bool switchingToSmartcast_=false;
    bool pointerDown_=false;
    QPointF pointerPosition_;
};
