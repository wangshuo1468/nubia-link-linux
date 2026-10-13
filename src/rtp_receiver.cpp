#include "rtp_receiver.h"
#include <QFile>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QtEndian>
#include <chrono>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}
namespace {
struct AudioDecode {
    AVCodecContext *codec=nullptr;
    AVFrame *frame=av_frame_alloc();
    SwrContext *resampler=nullptr;
    ~AudioDecode() { swr_free(&resampler); av_frame_free(&frame); avcodec_free_context(&codec); }
};
}

RtpReceiver::RtpReceiver(QObject *parent):QThread(parent) {}
RtpReceiver::~RtpReceiver() { stop(); }
bool RtpReceiver::begin(QHostAddress local,QHostAddress phone,quint16 port,QByteArray sps,QByteArray pps,int audioPayload) {
    if(isRunning() || local.protocol()!=QAbstractSocket::IPv4Protocol ||
       phone.protocol()!=QAbstractSocket::IPv4Protocol || port<1024 || port>=65535 ||
       sps.isEmpty() || pps.isEmpty() || sps.size()>4096 || pps.size()>4096 ||
       (quint8(sps[0])&31)!=7 || (quint8(pps[0])&31)!=8 ||
       (audioPayload!=-1 && (audioPayload<96 || audioPayload>127 || audioPayload==114 || port<1026))) return false;
    local_=local; phone_=phone; port_=port; sps_=std::move(sps); pps_=std::move(pps);
    audioPayload_=audioPayload; stopping_=false; frames_=0; audioBytes_=0; audioPeak_=0; ++epoch_;
    { QMutexLocker lock(&mutex_); latest_.reset(); audioBuffer_.clear(); }
    start(); return true;
}
void RtpReceiver::stop() {
    stopping_=true; wait(); ++epoch_;
    QMutexLocker lock(&mutex_); latest_.reset(); audioBuffer_.clear();
}
std::shared_ptr<VideoFrame> RtpReceiver::takeFrame() {
    QMutexLocker lock(&mutex_); return std::move(latest_);
}
QByteArray RtpReceiver::takeAudio() { QMutexLocker lock(&mutex_); return std::move(audioBuffer_); }
void RtpReceiver::run() {
    const auto generation=epoch_.load();
    const auto report=[this,generation](QString message) { emit error(message,generation); };
    using Clock=std::chrono::steady_clock;
    struct Interrupt { std::atomic<bool> *stop; Clock::time_point started; bool opening=true; } interrupt{&stopping_,Clock::now(),true};
    QTemporaryDir directory;
    QFile sdp(directory.filePath("stream.sdp"));
    if(!directory.isValid() || !sdp.open(QIODevice::WriteOnly)) { report(QStringLiteral("无法准备多屏协同接收器")); return; }
    auto description=QString("v=0\no=- 0 0 IN IP4 %1\ns=Nubia collaboration\nc=IN IP4 %1\nt=0 0\n"
        "m=video %2 RTP/AVP 114\na=rtpmap:114 H264/90000\na=fmtp:114 packetization-mode=1;sprop-parameter-sets=%3,%4\n"
        "a=source-filter:incl IN IP4 %1 %5\n").arg(local_.toString()).arg(port_)
        .arg(QString::fromLatin1(sps_.toBase64()),QString::fromLatin1(pps_.toBase64()),phone_.toString());
    if(audioPayload_>=0) description+=QString("m=audio %1 RTP/AVP %2\na=rtpmap:%2 opus/48000/2\na=fmtp:%2 sprop-stereo=1\na=source-filter:incl IN IP4 %3 %4\n")
        .arg(port_-2).arg(audioPayload_).arg(local_.toString(),phone_.toString());
    const auto data=description.toUtf8();
    if(sdp.write(data)!=data.size()) { report(QStringLiteral("无法写入多屏协同接收参数")); return; }
    sdp.close();
    AVFormatContext *format=avformat_alloc_context();
    if(!format) { report(QStringLiteral("无法分配 RTP 接收器")); return; }
    format->interrupt_callback={[](void *opaque) {
        auto *state=static_cast<Interrupt *>(opaque);
        return int(state->stop->load() || (state->opening && Clock::now()-state->started>std::chrono::seconds(15)));
    },&interrupt};
    format->max_delay=100000;
    AVDictionary *options=nullptr;
    av_dict_set(&options,"protocol_whitelist","file,udp,rtp",0);
    av_dict_set(&options,"buffer_size","1048576",0);
    av_dict_set(&options,"reorder_queue_size","128",0);
    int result=avformat_open_input(&format,sdp.fileName().toUtf8().constData(),av_find_input_format("sdp"),&options);
    av_dict_free(&options);
    if(result<0) {
        if(!stopping_) { char message[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result,message,sizeof(message)); report(QStringLiteral("RTP 接收失败：")+QString::fromUtf8(message)); }
        avformat_close_input(&format); return;
    }
    // A static phone screen can legitimately stop producing video packets.
    // The TCP control/session lifecycle detects disconnects; video inactivity
    // alone must not disconnect a healthy session or erase its last frame.
    interrupt.opening=false;
    int video=-1;
    for(unsigned int i=0;i<format->nb_streams;++i)
        if(format->streams[i]->codecpar->codec_id==AV_CODEC_ID_H264) { video=int(i); break; }
    AVPacket *packet=av_packet_alloc();
    if(video<0 || !packet) { av_packet_free(&packet); avformat_close_input(&format); report(QStringLiteral("没有可用的 H.264 视频流")); return; }
    std::unique_ptr<H264Decoder> codec=std::make_unique<H264Decoder>();
    AudioDecode audio; int audioStream=-1;
    for(unsigned int i=0;i<format->nb_streams;++i) {
        if(format->streams[i]->codecpar->codec_id!=AV_CODEC_ID_OPUS) continue;
        const auto decoder=avcodec_find_decoder(AV_CODEC_ID_OPUS);
        audio.codec=decoder ? avcodec_alloc_context3(decoder) : nullptr;
        if(audio.codec && audio.frame && avcodec_parameters_to_context(audio.codec,format->streams[i]->codecpar)>=0 && avcodec_open2(audio.codec,decoder,nullptr)>=0) audioStream=int(i);
        else emit audioWarning(QStringLiteral("Opus 音频解码器不可用"),generation);
        break;
    }
    const auto config=QByteArray::fromHex("00000001")+sps_+QByteArray::fromHex("00000001")+pps_;
    QString diagnostic;
    codec->decode(config,-1,[](auto) {},diagnostic);
    emit listening(generation);
    int failures=0;
    while(!stopping_) {
        result=av_read_frame(format,packet);
        if(result<0) break;
        if(packet->stream_index==video && packet->size>0 && packet->size<=32*1024*1024) {
            const bool ok=codec->decode(QByteArray(reinterpret_cast<const char *>(packet->data),packet->size),
                packet->pts==AV_NOPTS_VALUE ? 0 : packet->pts,[this,&failures,generation](auto frame) {
                    QMutexLocker lock(&mutex_); latest_=std::move(frame); const bool first=++frames_==1; failures=0;
                    lock.unlock(); if(first) emit firstFrame(generation);
                },diagnostic);
            if(!ok) {
                codec=std::make_unique<H264Decoder>(); codec->decode(config,-1,[](auto) {},diagnostic);
                if(++failures==1) emit keyFrameNeeded(generation);
                if(failures>=20) { result=AVERROR_INVALIDDATA; av_packet_unref(packet); break; }
            }
        } else if(packet->stream_index==audioStream && packet->size>0 && packet->size<=65536) {
            if(avcodec_send_packet(audio.codec,packet)>=0) {
                while(avcodec_receive_frame(audio.codec,audio.frame)>=0) {
                    if(!audio.resampler) {
                        AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
                        if(swr_alloc_set_opts2(&audio.resampler,&stereo,AV_SAMPLE_FMT_S16,48000,
                            &audio.frame->ch_layout,AVSampleFormat(audio.frame->format),audio.frame->sample_rate,0,nullptr)<0 || swr_init(audio.resampler)<0) {
                            swr_free(&audio.resampler); audioStream=-1;
                            emit audioWarning(QStringLiteral("无法转换协同声音格式"),generation); av_frame_unref(audio.frame); break;
                        }
                    }
                    const int capacity=swr_get_out_samples(audio.resampler,audio.frame->nb_samples);
                    if(capacity>0 && capacity<=57600) {
                        QByteArray pcm(capacity*4,Qt::Uninitialized); auto out=reinterpret_cast<uint8_t *>(pcm.data());
                        const int samples=swr_convert(audio.resampler,&out,capacity,const_cast<const uint8_t **>(audio.frame->extended_data),audio.frame->nb_samples);
                        if(samples>0) {
                            pcm.resize(samples*4);
                            int peak=audioPeak_.load();
                            for(qsizetype i=0;i<pcm.size();i+=2) peak=qMax(peak,qAbs(int(qFromLittleEndian<qint16>(pcm.constData()+i))));
                            audioPeak_=peak;
                            if(audioBytes_.fetch_add(quint64(pcm.size()))==0) qInfo()<<"Collaboration Opus decoded first PCM"<<pcm.size()<<"bytes";
                            QMutexLocker lock(&mutex_); audioBuffer_+=pcm;
                            constexpr qsizetype capacityBytes=48000;
                            if(audioBuffer_.size()>capacityBytes) audioBuffer_.remove(0,(audioBuffer_.size()-capacityBytes+3)/4*4);
                        }
                    }
                    av_frame_unref(audio.frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet); avformat_close_input(&format);
    if(!stopping_) report(QStringLiteral("多屏协同视频已中断，请重新连接"));
}
