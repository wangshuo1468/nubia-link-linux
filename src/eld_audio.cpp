#include "eld_audio.h"
#include <QRegularExpression>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/mem.h>
}

struct EldAudio::State {
    AVCodecContext *codec=nullptr;
    AVFrame *frame=av_frame_alloc();
    AVPacket *packet=av_packet_alloc();
    SwrContext *resampler=nullptr;
    ~State() {
        swr_free(&resampler); av_frame_free(&frame);
        av_packet_free(&packet); avcodec_free_context(&codec);
    }
};
EldAudio::EldAudio()=default;
EldAudio::~EldAudio()=default;
void EldAudio::reset() { state_.reset(); }
bool EldAudio::ready() const { return bool(state_); }
bool EldAudio::configure(const QByteArray &sdp) {
    reset();
    const auto text=QString::fromLatin1(sdp);
    const QRegularExpression mapping(QStringLiteral("(?:^|[\\r\\n])a=rtpmap:96\\s+mpeg4-generic/44100/2(?:[\\r\\n]|$)"),QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression format(QStringLiteral("(?:^|[\\r\\n])a=fmtp:96\\s+mode=AAC-eld;\\s*constantDuration=480(?:[\\r\\n]|$)"),QRegularExpression::CaseInsensitiveOption);
    if(!mapping.match(text).hasMatch() || !format.match(text).hasMatch()) return false;
    auto state=std::make_unique<State>();
    const auto decoder=avcodec_find_decoder(AV_CODEC_ID_AAC);
    if(!decoder || !state->frame || !state->packet) return false;
    state->codec=avcodec_alloc_context3(decoder);
    if(!state->codec) return false;
    auto codec=state->codec;
    codec->sample_rate=44100; av_channel_layout_default(&codec->ch_layout,2);
    // ISO AudioSpecificConfig: AOT 39 (ELD), 44.1 kHz, stereo,
    // 480 samples, no SBR. SDP provides these parameters, not an ADTS header.
    codec->extradata=static_cast<uint8_t *>(av_mallocz(4+AV_INPUT_BUFFER_PADDING_SIZE));
    if(!codec->extradata) return false;
    const uint8_t config[]={0xf8,0xe8,0x50,0x00};
    std::copy(std::begin(config),std::end(config),codec->extradata);
    codec->extradata_size=4;
    if(avcodec_open2(codec,decoder,nullptr)<0) return false;
    state_=std::move(state); return true;
}
QByteArray EldAudio::decode(const QByteArray &data) {
    if(!state_ || data.isEmpty() || data.size()>4096) return {};
    auto &s=*state_;
    av_packet_unref(s.packet);
    if(av_new_packet(s.packet,int(data.size()))<0) return {};
    std::copy(data.begin(),data.end(),s.packet->data);
    if(avcodec_send_packet(s.codec,s.packet)<0) return {};
    QByteArray pcm;
    while(avcodec_receive_frame(s.codec,s.frame)==0) {
        if(s.frame->sample_rate!=44100 || s.frame->ch_layout.nb_channels!=2 || s.frame->nb_samples!=480) {
            av_frame_unref(s.frame); continue;
        }
        if(!s.resampler) {
            AVChannelLayout output=AV_CHANNEL_LAYOUT_STEREO;
            const auto allocated=swr_alloc_set_opts2(&s.resampler,&output,AV_SAMPLE_FMT_S16,48000,
                &s.frame->ch_layout,AVSampleFormat(s.frame->format),s.frame->sample_rate,0,nullptr);
            if(allocated<0 || swr_init(s.resampler)<0) { swr_free(&s.resampler); av_frame_unref(s.frame); return {}; }
        }
        const auto count=swr_get_out_samples(s.resampler,s.frame->nb_samples);
        if(count<0 || count>2048) { av_frame_unref(s.frame); return {}; }
        QByteArray bytes(count*4,0); auto out=reinterpret_cast<uint8_t *>(bytes.data());
        const auto converted=swr_convert(s.resampler,&out,count,const_cast<const uint8_t **>(s.frame->extended_data),s.frame->nb_samples);
        av_frame_unref(s.frame);
        if(converted<0) return {};
        bytes.resize(converted*4);
        pcm+=bytes;
    }
    return pcm;
}
