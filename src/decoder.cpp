#include "decoder.h"
#include "video_crop.h"
#include <QMutexLocker>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <cstring>

namespace {
QString avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}
}
struct H264Decoder::Impl {
    AVCodecContext *codec = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    SwsContext *scaler = nullptr;
    QByteArray config;
    QString initError;
    bool trimPadding=false;
    VideoCropDetector crop;
    Impl() {
        const auto decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (decoder) codec = avcodec_alloc_context3(decoder);
        if (!codec || !frame || !packet) { initError = QStringLiteral("无法分配解码器"); return; }
        codec->thread_count = 2;
        codec->thread_type = FF_THREAD_SLICE;
        codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        int result = avcodec_open2(codec, decoder, nullptr);
        if (result < 0) initError = avError(result);
    }
    ~Impl() {
        sws_freeContext(scaler); av_packet_free(&packet);
        av_frame_free(&frame); avcodec_free_context(&codec);
    }
};
H264Decoder::H264Decoder(bool trimPadding) : impl_(std::make_unique<Impl>()) { impl_->trimPadding=trimPadding; }
H264Decoder::~H264Decoder() = default;
bool H264Decoder::decode(const QByteArray &bytes, qint64 timestamp,
                        const std::function<void(std::shared_ptr<VideoFrame>)> &output, QString &error) {
    auto &s = *impl_;
    if (!s.initError.isEmpty()) { error = s.initError; return false; }
    if (timestamp == -1) { s.config = bytes; return true; }
    QByteArray payload = s.config + bytes;
    av_packet_unref(s.packet);
    int result = av_new_packet(s.packet, int(payload.size()));
    if (result < 0) { error = avError(result); return false; }
    std::memcpy(s.packet->data, payload.constData(), size_t(payload.size()));
    s.packet->pts = timestamp;
    result = avcodec_send_packet(s.codec, s.packet);
    if (result < 0) { error = avError(result); return false; }
    s.config.clear();
    while ((result = avcodec_receive_frame(s.codec, s.frame)) >= 0) {
        const int w = s.frame->width, h = s.frame->height;
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
            error = QStringLiteral("画面尺寸超出支持范围"); return false;
        }
        auto frame = std::make_shared<VideoFrame>();
        frame->size = QSize(w, h);
        if(s.trimPadding) {
            frame->contentRect=s.crop.detect(s.frame);
            // Avoid displaying the padded landscape canvas before the phone
            // boundary has been established at the start of a connection.
            if(!s.crop.ready()) { av_frame_unref(s.frame); continue; }
        }
        frame->bt709 = s.frame->colorspace == AVCOL_SPC_BT709 ||
            (s.frame->colorspace == AVCOL_SPC_UNSPECIFIED && h >= 720);
        frame->fullRange = s.frame->color_range == AVCOL_RANGE_JPEG;
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        frame->y.resize(w * h); frame->u.resize(cw * ch); frame->v.resize(cw * ch);
        auto y = reinterpret_cast<uint8_t *>(frame->y.data());
        auto u = reinterpret_cast<uint8_t *>(frame->u.data());
        auto v = reinterpret_cast<uint8_t *>(frame->v.data());
        if (s.frame->format == AV_PIX_FMT_YUV420P) {
            uint8_t *dest[] = {y, u, v};
            int widths[] = {w, cw, cw}, heights[] = {h, ch, ch};
            for (int p = 0; p < 3; ++p)
                for (int row = 0; row < heights[p]; ++row)
                    std::memcpy(dest[p] + row * widths[p], s.frame->data[p] + row * s.frame->linesize[p], size_t(widths[p]));
        } else {
            s.scaler = sws_getCachedContext(s.scaler, w, h, AVPixelFormat(s.frame->format),
                                           w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!s.scaler) { error = QStringLiteral("无法转换视频格式"); return false; }
            const auto coefficients = sws_getCoefficients(frame->bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            sws_setColorspaceDetails(s.scaler, coefficients, frame->fullRange, coefficients, frame->fullRange, 0, 1 << 16, 1 << 16);
            uint8_t *dest[] = {y, u, v, nullptr}; int strides[] = {w, cw, cw, 0};
            sws_scale(s.scaler, s.frame->data, s.frame->linesize, 0, h, dest, strides);
        }
        output(std::move(frame)); av_frame_unref(s.frame);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) { error = avError(result); return false; }
    return true;
}
VideoDecoder::VideoDecoder(QObject *parent) : QThread(parent) { start(); }
VideoDecoder::~VideoDecoder() {
    { QMutexLocker lock(&mutex_); quitting_ = true; ready_.wakeAll(); }
    wait();
}
void VideoDecoder::submit(QByteArray bytes, qint64 timestamp) {
    enqueue(std::move(bytes),timestamp,false);
}
void VideoDecoder::submitSmartcast(QByteArray bytes,qint64 timestamp) {
    enqueue(std::move(bytes),timestamp,true);
}
void VideoDecoder::enqueue(QByteArray bytes,qint64 timestamp,bool trimPadding) {
    QMutexLocker lock(&mutex_);
    if (failed_ || quitting_) return;
    if (queuedBytes_ + bytes.size() > 32 * 1024 * 1024 || queue_.size() >= 120) {
        failed_ = true; queue_.clear(); queuedBytes_ = 0;
        const auto generation = epoch_; lock.unlock();
        emit error(QStringLiteral("视频解码跟不上数据，请重新连接"), generation); return;
    }
    queuedBytes_ += bytes.size(); queue_.push_back({std::move(bytes), timestamp, epoch_,trimPadding}); ready_.wakeOne();
}
void VideoDecoder::reset() {
    QMutexLocker lock(&mutex_); ++epoch_; queue_.clear(); queuedBytes_ = 0; latest_.reset(); failed_ = false;
}
quint64 VideoDecoder::epoch() { QMutexLocker lock(&mutex_); return epoch_; }
std::shared_ptr<VideoFrame> VideoDecoder::takeFrame() {
    QMutexLocker lock(&mutex_); auto result = std::move(latest_); return result;
}
void VideoDecoder::run() {
    std::unique_ptr<H264Decoder> codec;
    quint64 generation = quint64(-1);
    bool trimPadding=false;
    for (;;) {
        Packet packet;
        {
            QMutexLocker lock(&mutex_);
            while (queue_.empty() && !quitting_) ready_.wait(&mutex_);
            if (quitting_) return;
            packet = std::move(queue_.front()); queue_.pop_front(); queuedBytes_ -= packet.bytes.size();
        }
        if (generation != packet.epoch || trimPadding!=packet.trimPadding) {
            trimPadding=packet.trimPadding; codec = std::make_unique<H264Decoder>(trimPadding); generation = packet.epoch;
        }
        QString message;
        bool ok = codec->decode(packet.bytes, packet.timestamp, [this, generation](std::shared_ptr<VideoFrame> frame) {
            QMutexLocker lock(&mutex_);
            if (epoch_ == generation && !failed_) latest_ = std::move(frame);
        }, message);
        if (!ok) {
            QMutexLocker lock(&mutex_);
            if (epoch_ != generation) continue;
            failed_ = true; queue_.clear(); queuedBytes_ = 0;
            lock.unlock(); emit error(QStringLiteral("视频解码失败：") + message, generation);
        }
    }
}
