#pragma once
#include <QObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QHash>
#include <QTimer>
#include "eld_audio.h"

// HomeActivity browses Lelink v1, independently of the ZLINK PC list.
class LeboReceiver : public QObject {
    Q_OBJECT
public:
    explicit LeboReceiver(QObject *parent=nullptr);
    ~LeboReceiver() override;
    bool listen(QHostAddress address, int prefix, const QString &interfaceName);
    void stop();
    void disconnectPhone() { endStream(); }
    bool active() const { return active_; }
    bool running() const { return !groupPath_.isEmpty(); }
signals:
    void status(QString text);
    void activeChanged(bool active);
    void peerChanged(QHostAddress phone);
    void videoPacket(QByteArray data, qint64 timestamp);
    void audioData(QByteArray pcm);
    void audioStatus(QString text);
    void publicationFailed(QString text);
private slots:
    void publicationStateChanged(int state, QString error);
private:
    friend class LeboReceiverTest;
    void accept(QTcpServer &server, bool mirror);
    void read(QTcpSocket *socket, bool mirror);
    bool frame(QByteArray header, QByteArray data);
    void endStream();
    void readAudio();
    QByteArray streamInfo() const;
    QTcpServer control_, mirror_;
    QUdpSocket audio_;
    QHash<QTcpSocket *,QByteArray> buffers_;
    QHash<QTcpSocket *,bool> streaming_;
    QHostAddress address_, peer_;
    int prefix_=0;
    QString groupPath_;
    QByteArray iv_;
    QTimer idle_;
    bool active_=false;
    EldAudio audioDecoder_;
    quint16 audioPeerPort_=0, audioSequence_=0;
    bool audioSequenceValid_=false, audioReceived_=false;
};
