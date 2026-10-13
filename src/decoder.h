#pragma once
#include <QByteArray>
#include <QMutex>
#include <QSize>
#include <QRect>
#include <QThread>
#include <QWaitCondition>
#include <deque>
#include <memory>
#include <functional>

struct VideoFrame {
    QSize size;
    QRect contentRect;
    QRect visibleRect() const {
        const QRect full(QPoint(),size);
        return contentRect.isValid() && full.contains(contentRect) ? contentRect : full;
    }
    QSize displaySize() const { return visibleRect().size(); }
    QByteArray y, u, v;
    bool bt709 = true;
    bool fullRange = false;
};

// The codec is owned and called by one thread. Frames own their plane data.
class H264Decoder {
public:
    explicit H264Decoder(bool trimPadding=false);
    ~H264Decoder();
    bool decode(const QByteArray &bytes, qint64 timestamp,
                const std::function<void(std::shared_ptr<VideoFrame>)> &output, QString &error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class VideoDecoder : public QThread {
    Q_OBJECT
public:
    explicit VideoDecoder(QObject *parent = nullptr);
    ~VideoDecoder() override;
    void submit(QByteArray bytes, qint64 timestamp);
    void submitSmartcast(QByteArray bytes, qint64 timestamp);
    void reset();
    quint64 epoch();
    std::shared_ptr<VideoFrame> takeFrame();
signals:
    void error(QString text, quint64 epoch);
protected:
    void run() override;
private:
    void enqueue(QByteArray bytes,qint64 timestamp,bool trimPadding);
    struct Packet { QByteArray bytes; qint64 timestamp; quint64 epoch; bool trimPadding; };
    QMutex mutex_;
    QWaitCondition ready_;
    std::deque<Packet> queue_;
    qsizetype queuedBytes_ = 0;
    quint64 epoch_ = 0;
    bool quitting_ = false, failed_ = false;
    std::shared_ptr<VideoFrame> latest_;
};
