#pragma once
#include "collaboration_link.h"
#include "collaboration_wire.h"
#include "rtp_receiver.h"
#include "collaboration_input.h"
#include "audio.h"
#include <QUdpSocket>
#include <QPointF>

// Independent ScreenCastSource/Sink connection. Sharing discovery/framing
// libraries with RMCast never starts a smart-cast media session.
class CollaborationSession : public QObject {
    Q_OBJECT
public:
    explicit CollaborationSession(QObject *parent=nullptr);
    ~CollaborationSession() override;
    bool start(QHostAddress local,QHostAddress phone,QString package="com.android.settings");
    bool listen(QHostAddress local,int prefix,QString interfaceName,quint16 discoveryPort=25683);
    bool requestApplication(QString package);
    void stop();
    bool running() const { return state_!=Idle; }
    bool playing() const { return state_==Playing; }
    bool applicationReady() const { return playing() && associated_; }
    QString castDisplayType() const { return displayType_; }
    std::shared_ptr<VideoFrame> takeFrame() { return receiver_.takeFrame(); }
    quint64 frameCount() const { return receiver_.frameCount(); }
    void key(int androidCode);
    bool mouseReady() const { return playing() && mouse_.ready(); }
    bool editable() const { return playing() && text_.editable(); }
    void pointer(int action,QPointF position,QSize display,int buttons);
    void scroll(QPointF position,QSize display,QPointF delta);
    bool commitText(QString text);
    quint64 audioBytes() const { return receiver_.audioBytes(); }
    int audioPeak() const { return receiver_.audioPeak(); }
    void setMuted(bool muted) { audio_.setMuted(muted); }
signals:
    void status(QString text);
    void activeChanged(bool active);
    void stopped();
    void inputChanged();
    void incomingPeerChanged(QHostAddress local,QHostAddress phone);
private:
    friend class CollaborationLifecycleTest;
    friend class CollaborationApplicationProbe;
    enum State { Idle,Discovering,Associating,Offering,Transport,Playing,Stopping } state_=Idle;
    void publish();
    void receive(const QJsonObject &rpc);
    void readControl();
    void requestWindow();
    void acceptOffer(const QJsonObject &rpc);
    void fail(QString message);
    void finish();
    void command(QString action,QJsonObject extra={});
    CollaborationLink link_;
    CollaborationInput mouse_{link_.manager(),CollaborationInput::Mouse};
    CollaborationInput text_{link_.manager(),CollaborationInput::Text};
    QTcpSocket control_;
    Collaboration::MessageReader reader_;
    RtpReceiver receiver_;
    PhoneAudio audio_;
    QList<QUdpSocket *> mediaSockets_;
    QTimer deadline_, delayed_, heartbeat_,audioPoll_;
    QHostAddress local_,phone_;
    QString package_;
    QString displayType_;
    quint16 port_=0;
    int messageId_=0;
    bool associated_=false,offered_=false,transportReceived_=false;
    bool incoming_=false;
    bool replacing_=false;
    QList<QJsonObject> earlyRequests_;
    qsizetype earlyBytes_=0;
    QString peerName_;
    quint64 generation_=0;
};
