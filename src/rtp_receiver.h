#pragma once
#include "decoder.h"
#include <QHostAddress>
#include <atomic>

// FFmpeg owns SDP/RTP/RTCP and codec work on this thread. Only the latest
// decoded frame is retained, so a slow UI cannot grow a frame event queue.
class RtpReceiver : public QThread {
    Q_OBJECT
public:
    explicit RtpReceiver(QObject *parent=nullptr);
    ~RtpReceiver() override;
    bool begin(QHostAddress local, QHostAddress phone, quint16 videoPort,
               QByteArray sps, QByteArray pps,int audioPayload=-1);
    void stop();
    std::shared_ptr<VideoFrame> takeFrame();
    QByteArray takeAudio(); // bounded 250ms of stereo 48kHz PCM16, no Qt frame queue
    quint64 audioBytes() const { return audioBytes_.load(); }
    int audioPeak() const { return audioPeak_.load(); }
    quint64 frameCount() const { return frames_.load(); }
    quint64 epoch() const { return epoch_.load(); }
signals:
    void listening(quint64 epoch);
    void firstFrame(quint64 epoch);
    void error(QString message,quint64 epoch);
    void keyFrameNeeded(quint64 epoch);
    void audioWarning(QString message,quint64 epoch);
protected:
    void run() override;
private:
    QHostAddress local_, phone_;
    quint16 port_=0;
    QByteArray sps_, pps_;
    int audioPayload_=-1;
    QByteArray audioBuffer_;
    std::atomic<quint64> audioBytes_{0};
    std::atomic<int> audioPeak_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<quint64> frames_{0};
    std::atomic<quint64> epoch_{0};
    QMutex mutex_;
    std::shared_ptr<VideoFrame> latest_;
};
