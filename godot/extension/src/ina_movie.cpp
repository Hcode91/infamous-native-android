#include "ina_movie.h"

#include <godot_cpp/core/class_db.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

namespace godot {

struct InaMovie::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    // The movies keep their sound as mono tracks: ids 1-6 are the 5.1 mix (L, R, C, LFE, Ls, Rs) and
    // ids 10+language hold the voices (11 English, 12 French, 13 Spanish, 14 German, 15 Italian,
    // 16 Dutch, 17 Portuguese, 18 Russian, 26 Polish: the console's language numbers plus 10).
    struct Track {
        int stream = -1;
        AVCodecContext* dec = nullptr;
        float gainL = 0, gainR = 0;
        std::deque<float> pcm;
    };
    std::vector<Track> tracks;
    int vstream = -1, audioTracks = 0, rate = 0;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* aframe = av_frame_alloc();    // audio scratch
    AVFrame* pending = av_frame_alloc();   // decoded video frame not shown yet
    bool havePending = false, eof = false, finished = false;
    double fps = 30.0, duration = 0.0, timeBase = 0.0;
    PackedByteArray rgba;
    Ref<Image> image;
    std::deque<Vector2> audio;

    ~Impl() {
        avcodec_free_context(&vdec);
        for (Track& t : tracks) avcodec_free_context(&t.dec);
        avformat_close_input(&fmt);
        sws_freeContext(sws);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&aframe);
        av_frame_free(&pending);
    }

    void takeAudio(Track& t, AVFrame* f) {
        const int ch = f->ch_layout.nb_channels;
        for (int i = 0; i < f->nb_samples; ++i) {
            float v = 0;
            for (int c = 0; c < ch; ++c)
                v += f->format == AV_SAMPLE_FMT_FLTP ? reinterpret_cast<const float*>(f->data[c])[i]
                                                     : reinterpret_cast<const float*>(f->data[0])[i * ch + c];
            t.pcm.push_back(v / ch);
        }
        mix();
    }

    // Mixes what every track has ready into stereo.
    void mix() {
        if (tracks.empty()) return;
        size_t n = SIZE_MAX;
        for (const Track& t : tracks) n = std::min(n, t.pcm.size());
        for (size_t i = 0; i < n; ++i) {
            float l = 0, r = 0;
            for (Track& t : tracks) {
                const float v = t.pcm.front();
                t.pcm.pop_front();
                l += v * t.gainL;
                r += v * t.gainR;
            }
            audio.emplace_back(std::clamp(l, -1.0f, 1.0f), std::clamp(r, -1.0f, 1.0f));
        }
    }

    void flushAudio() {
        for (Track& t : tracks) {
            avcodec_send_packet(t.dec, nullptr);
            while (avcodec_receive_frame(t.dec, aframe) == 0) takeAudio(t, aframe);
        }
        // pad tracks that ended early so the rest still plays
        size_t m = 0;
        for (const Track& t : tracks) m = std::max(m, t.pcm.size());
        for (Track& t : tracks) t.pcm.resize(m, 0.0f);
        mix();
    }

    // Next decoded video frame into `pending`; false at the end.
    bool nextVideo() {
        for (;;) {
            if (avcodec_receive_frame(vdec, pending) == 0) return true;
            if (eof) return false;
            if (av_read_frame(fmt, pkt) < 0) {
                eof = true;
                avcodec_send_packet(vdec, nullptr);
                flushAudio();
                continue;
            }
            if (pkt->stream_index == vstream) {
                avcodec_send_packet(vdec, pkt);
            } else {
                for (Track& t : tracks)
                    if (t.stream == pkt->stream_index && avcodec_send_packet(t.dec, pkt) == 0)
                        while (avcodec_receive_frame(t.dec, aframe) == 0) takeAudio(t, aframe);
            }
            av_packet_unref(pkt);
        }
    }

    void show(AVFrame* f) {
        const int w = f->width, h = f->height;
        sws = sws_getCachedContext(sws, w, h, AVPixelFormat(f->format), w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
        if (rgba.size() != w * h * 4) rgba.resize(w * h * 4);
        uint8_t* dst[4] = {rgba.ptrw(), nullptr, nullptr, nullptr};
        int stride[4] = {w * 4, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, h, dst, stride);
        if (image.is_null() || image->get_width() != w || image->get_height() != h)
            image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, rgba);
        else
            image->set_data(w, h, false, Image::FORMAT_RGBA8, rgba);
    }
};

InaMovie::InaMovie() = default;
InaMovie::~InaMovie() = default;

String InaMovie::open(const String& path, int voice) {
    d_ = std::make_unique<Impl>();
    const CharString p = path.utf8();
    if (avformat_open_input(&d_->fmt, p.get_data(), nullptr, nullptr) < 0) return "can't open " + path;
    if (avformat_find_stream_info(d_->fmt, nullptr) < 0) return "can't read " + path;
    auto openDec = [&](int idx) -> AVCodecContext* {
        AVStream* s = d_->fmt->streams[idx];
        const AVCodec* c = avcodec_find_decoder(s->codecpar->codec_id);
        if (!c) return nullptr;
        AVCodecContext* ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ctx, s->codecpar);
        if (avcodec_open2(ctx, c, nullptr) < 0) avcodec_free_context(&ctx);
        return ctx;
    };
    // which voice track: the one asked for, else English, else the first
    std::vector<int> voices;
    for (unsigned i = 0; i < d_->fmt->nb_streams; ++i)
        if (d_->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && d_->fmt->streams[i]->id >= 10)
            voices.push_back(d_->fmt->streams[i]->id);
    int voiceId = -1;
    for (int v : voices) if (v == voice) voiceId = v;
    if (voiceId < 0) for (int v : voices) if (v == 11) voiceId = v;
    if (voiceId < 0 && !voices.empty()) voiceId = voices.front();
    if (voice < 0) voiceId = -2;   // no voices at all
    const float k = 0.7071f;
    for (unsigned i = 0; i < d_->fmt->nb_streams; ++i) {
        AVStream* s = d_->fmt->streams[i];
        s->discard = AVDISCARD_ALL;
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && d_->vstream < 0) {
            d_->vstream = int(i);
            s->discard = AVDISCARD_DEFAULT;
            continue;
        }
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        ++d_->audioTracks;
        Impl::Track t;
        t.stream = int(i);
        const int id = s->id;
        if (s->codecpar->ch_layout.nb_channels >= 2 || (id < 1 && voices.empty())) { t.gainL = t.gainR = 1; }
        else if (id == 1) t.gainL = 1;
        else if (id == 2) t.gainR = 1;
        else if (id == 3) t.gainL = t.gainR = k;
        else if (id == 4) t.gainL = t.gainR = 0.5f;
        else if (id == 5) t.gainL = k;
        else if (id == 6) t.gainR = k;
        else if (id == voiceId) t.gainL = t.gainR = k;
        else continue;
        t.dec = openDec(int(i));
        if (!t.dec) continue;
        d_->rate = t.dec->sample_rate;
        s->discard = AVDISCARD_DEFAULT;
        d_->tracks.push_back(std::move(t));
    }
    if (d_->vstream < 0) return "no video in " + path;
    d_->vdec = openDec(d_->vstream);
    if (!d_->vdec) return "no Bink video decoder";
    AVStream* vs = d_->fmt->streams[d_->vstream];
    d_->fps = vs->avg_frame_rate.num ? av_q2d(vs->avg_frame_rate) : 30.0;
    d_->timeBase = av_q2d(vs->time_base);
    d_->duration = vs->nb_frames > 0 ? vs->nb_frames / d_->fps : d_->fmt->duration / double(AV_TIME_BASE);
    if (!d_->nextVideo()) return "no frames in " + path;
    d_->show(d_->pending);
    d_->havePending = d_->nextVideo();
    return "";
}

int InaMovie::get_width() const { return d_ && d_->vdec ? d_->vdec->width : 0; }
int InaMovie::get_height() const { return d_ && d_->vdec ? d_->vdec->height : 0; }
double InaMovie::get_fps() const { return d_ ? d_->fps : 0.0; }
double InaMovie::get_duration() const { return d_ ? d_->duration : 0.0; }
int InaMovie::get_audio_rate() const { return d_ && !d_->tracks.empty() ? d_->rate : 0; }
int InaMovie::get_audio_tracks() const { return d_ ? d_->audioTracks : 0; }
bool InaMovie::is_finished() const { return !d_ || d_->finished; }
Ref<Image> InaMovie::get_image() const { return d_ ? d_->image : Ref<Image>(); }

bool InaMovie::update(double t) {
    if (!d_ || !d_->vdec) return false;
    bool changed = false;
    AVFrame* show = nullptr;
    while (d_->havePending) {
        const int64_t pts = d_->pending->best_effort_timestamp;
        const double ft = pts == AV_NOPTS_VALUE ? 0.0 : pts * d_->timeBase;
        if (ft > t) break;
        av_frame_unref(d_->frame);
        av_frame_move_ref(d_->frame, d_->pending);
        show = d_->frame;
        d_->havePending = d_->nextVideo();
    }
    if (show) {
        d_->show(show);
        changed = true;
    }
    if (!d_->havePending && t >= d_->duration) d_->finished = true;
    return changed;
}

PackedVector2Array InaMovie::take_audio(int max_frames) {
    PackedVector2Array out;
    if (!d_) return out;
    const int n = std::min<int>(max_frames, int(d_->audio.size()));
    out.resize(n);
    for (int i = 0; i < n; ++i) {
        out.set(i, d_->audio.front());
        d_->audio.pop_front();
    }
    return out;
}

void InaMovie::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open", "path", "voice"), &InaMovie::open, DEFVAL(11));
    ClassDB::bind_method(D_METHOD("get_width"), &InaMovie::get_width);
    ClassDB::bind_method(D_METHOD("get_height"), &InaMovie::get_height);
    ClassDB::bind_method(D_METHOD("get_fps"), &InaMovie::get_fps);
    ClassDB::bind_method(D_METHOD("get_duration"), &InaMovie::get_duration);
    ClassDB::bind_method(D_METHOD("get_audio_rate"), &InaMovie::get_audio_rate);
    ClassDB::bind_method(D_METHOD("get_audio_tracks"), &InaMovie::get_audio_tracks);
    ClassDB::bind_method(D_METHOD("update", "t"), &InaMovie::update);
    ClassDB::bind_method(D_METHOD("is_finished"), &InaMovie::is_finished);
    ClassDB::bind_method(D_METHOD("get_image"), &InaMovie::get_image);
    ClassDB::bind_method(D_METHOD("take_audio", "max_frames"), &InaMovie::take_audio);
}

}  // namespace godot
