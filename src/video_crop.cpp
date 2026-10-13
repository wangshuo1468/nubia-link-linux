#include "video_crop.h"
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h>
#include <libavutil/dict.h>
}

struct VideoCropDetector::Impl {
    AVFilterGraph *graph=nullptr;
    AVFilterContext *source=nullptr, *sink=nullptr;
    AVFrame *filtered=av_frame_alloc();
    QSize size;
    int format=-1, range=-1, colorspace=-1, stable=0;
    QRect current, pending, portrait, landscape;
    bool calibrated=false;
    ~Impl() { av_frame_free(&filtered); avfilter_graph_free(&graph); }
    bool configure(AVFrame *frame) {
        avfilter_graph_free(&graph); source=nullptr; sink=nullptr;
        const QSize nextSize(frame->width,frame->height);
        // Pixel format/range changes must not forget an already learned
        // boundary. A new encoded canvas starts a fresh calibration.
        if(size!=nextSize) {
            current=QRect(QPoint(),nextSize); portrait={}; landscape={}; calibrated=false;
        }
        size=nextSize; format=frame->format; range=frame->color_range; colorspace=frame->colorspace;
        pending={}; stable=0;
        graph=avfilter_graph_alloc(); if(!graph || !filtered) return false;
        graph->nb_threads=1;
        const auto args=QString("video_size=%1x%2:pix_fmt=%3:time_base=1/1000000:pixel_aspect=1/1:range=%4:colorspace=%5")
            .arg(size.width()).arg(size.height()).arg(format).arg(range).arg(colorspace).toLatin1();
        AVFilterContext *crop=nullptr;
        const char *options=range==AVCOL_RANGE_JPEG ? "limit=4:round=2:skip=0:reset_count=1" : "limit=20:round=2:skip=0:reset_count=1";
        if(avfilter_graph_create_filter(&source,avfilter_get_by_name("buffer"),"input",args.constData(),nullptr,graph)<0 ||
           avfilter_graph_create_filter(&crop,avfilter_get_by_name("cropdetect"),"padding",options,nullptr,graph)<0 ||
           avfilter_graph_create_filter(&sink,avfilter_get_by_name("buffersink"),"output",nullptr,nullptr,graph)<0) return false;
        return avfilter_link(source,0,crop,0)>=0 && avfilter_link(crop,0,sink,0)>=0 && avfilter_graph_config(graph,nullptr)>=0;
    }
};
VideoCropDetector::VideoCropDetector():impl_(std::make_unique<Impl>()) {}
VideoCropDetector::~VideoCropDetector()=default;
bool VideoCropDetector::ready() const { return impl_->calibrated; }
QRect VideoCropDetector::detect(AVFrame *frame) {
    auto &s=*impl_;
    const QRect full(0,0,frame->width,frame->height);
    if(s.size!=full.size() || s.format!=frame->format || s.range!=frame->color_range || s.colorspace!=frame->colorspace) {
        if(!s.configure(frame)) { s.source=nullptr; s.sink=nullptr; return s.current; }
    }
    if(!s.source || !s.sink || av_buffersrc_add_frame_flags(s.source,frame,AV_BUFFERSRC_FLAG_KEEP_REF)<0) return s.current;
    if(av_buffersink_get_frame(s.sink,s.filtered)<0) return s.current;
    auto number=[&](const char *key) {
        auto entry=av_dict_get(s.filtered->metadata,key,nullptr,0);
        bool ok=false; const int value=entry ? QString::fromLatin1(entry->value).toInt(&ok) : -1;
        return ok ? value : -1;
    };
    const int x1=number("lavfi.cropdetect.x1"), x2=number("lavfi.cropdetect.x2");
    const int y1=number("lavfi.cropdetect.y1"), y2=number("lavfi.cropdetect.y2");
    av_frame_unref(s.filtered);
    // Blank frames cannot describe screen boundaries. Keep the last crop.
    if(x1<0 || y1<0 || x2<x1 || y2<y1 || x2>=frame->width || y2>=frame->height) {
        s.stable=0; s.pending={}; return s.current;
    }
    QRect candidate=full;
    auto padding=[](int first,int last,int length) {
        const int far=length-last-1;
        const int border=qMin(first,far);
        return border>=length*0.03 && last-first+1>=length*0.15 ? border : 0;
    };
    // Only remove the common border on both sides. Dark phone status/header
    // areas can make one side wider; preserve that extra space and all content.
    const int horizontal=padding(x1,x2,frame->width), vertical=padding(y1,y2,frame->height);
    candidate.adjust(horizontal,vertical,-horizontal,-vertical);
    const auto isPortrait=[](QRect rect) { return rect.height()>rect.width(); };
    const bool portrait=isPortrait(candidate);
    if(s.calibrated && portrait==isPortrait(s.current)) {
        // A dark page or letterboxed movie does not redefine the phone's
        // screen. Keep both the crop and its display aspect for this direction.
        s.stable=0; s.pending={}; return s.current;
    }
    // A rotation should span the long axis of the encoded canvas; a small
    // bright item surrounded by a dark page is insufficient evidence.
    if((portrait ? y2-y1+1<full.height()*0.9 : x2-x1+1<full.width()*0.9)) {
        s.stable=0; s.pending={}; return s.current;
    }
    if(s.pending.isEmpty() || portrait!=isPortrait(s.pending)) {
        s.pending=candidate; s.stable=1;
    } else {
        // Accumulate the outermost content across changing pages/animation,
        // rather than shrinking to the darkest individual frame.
        s.pending=s.pending.united(candidate); ++s.stable;
    }
    if(s.stable>=15) {
        auto &saved=portrait ? s.portrait : s.landscape;
        if(saved.isEmpty()) saved=s.pending;
        s.current=saved; s.calibrated=true; s.stable=0; s.pending={};
    }
    return s.current;
}
