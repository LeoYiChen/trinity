/*
 * Copyright (C) 2019 Trinity. All rights reserved.
 * Copyright (C) 2019 Wang LianJie <wlanjie888@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Created by wlanjie on 2019-06-15.
//

#include <cmath>
#include "error_code.h"
#include "video_export.h"
#include "soft_encoder_adapter.h"
#include "media_encode_adapter.h"
#include "android_xlog.h"
#include "tools.h"
#include "gl.h"

#define MAX_IMAGE_WIDTH 1080
#define MAX_IMAGE_HEIGHT 1920

namespace trinity {

VideoExport::VideoExport(JNIEnv* env, jobject object)
    : vm_(nullptr)
    , object_(nullptr)
    , video_duration_(0)
    , export_video_thread_()
    , export_audio_thread_()
    , clip_deque_()
    , music_decoder_deque_()
    , resample_deque_()
    , accompany_packet_buffer_size_(2048)
    , accompany_sample_rate_(44100)
    , vocal_sample_rate_(0)
    , channel_count_(0)
    , audio_current_time_(0)
    , export_ing_(false)
    , egl_core_(nullptr)
    , egl_surface_(EGL_NO_SURFACE)
    , encode_texture_id_(0)
    , image_texture_(0)
    , image_render_time_(0)
    , load_image_texture_(false)
    , export_index_(0)
    , video_width_(0)
    , video_height_(0)
    , frame_rate_(0)
    , frame_width_(0)
    , frame_height_(0)
    , yuv_render_(nullptr)
    , image_process_(nullptr)
    , media_codec_encode_(true)
    , encoder_(nullptr)
    , audio_encoder_adapter_(nullptr)
    , packet_thread_(nullptr)
    , packet_pool_(nullptr)
    , current_time_(0)
    , previous_time_(0)
    , swr_context_(nullptr)
    , audio_buffer_(nullptr)
    , audio_buf1(nullptr)
    , audio_samples_(nullptr)
    , video_export_handler_(nullptr)
    , video_export_message_queue_(nullptr)
    , export_message_thread_()
    , media_mutex_()
    , media_cond_()
    , export_config_json_(nullptr)
    , vertex_coordinate_(nullptr)
    , texture_coordinate_(nullptr)
    , texture_matrix_(nullptr)
    , av_play_context_(nullptr)
    , image_audio_buffer_(nullptr)
    , image_audio_buffer_time_(0)
    , image_frame_buffer_(nullptr) {
    env->GetJavaVM(&vm_);
    object_ = env->NewGlobalRef(object);
    audio_samples_ = new short[8192];
    // 因为encoder_render时不能改变顶点和纹理坐标
    // 而glReadPixels读取的图像又是上下颠倒的
    // 所以这里显示的把纹理坐标做180度旋转
    // 从而保证从glReadPixels读取的数据不是上下颠倒的,而是正确的
    crop_vertex_coordinate_ = new GLfloat[8];
    crop_texture_coordinate_ = new GLfloat[8];
    vertex_coordinate_ = new GLfloat[8];
    texture_coordinate_ = new GLfloat[8];
    vertex_coordinate_[0] = -1.0f;
    vertex_coordinate_[1] = -1.0f;
    vertex_coordinate_[2] = 1.0f;
    vertex_coordinate_[3] = -1.0f;

    vertex_coordinate_[4] = -1.0f;
    vertex_coordinate_[5] = 1.0f;
    vertex_coordinate_[6] = 1.0f;
    vertex_coordinate_[7] = 1.0f;

    texture_coordinate_[0] = 0.0f;
    texture_coordinate_[1] = 0.0f;
    texture_coordinate_[2] = 1.0f;
    texture_coordinate_[3] = 0.0f;

    texture_coordinate_[4] = 0.0f;
    texture_coordinate_[5] = 1.0f;
    texture_coordinate_[6] = 1.0f;
    texture_coordinate_[7] = 1.0f;

    texture_matrix_ = new GLfloat[16];
    memset(texture_matrix_, 0, 16 * sizeof(GLfloat));
    texture_matrix_[0] = texture_matrix_[5] = texture_matrix_[10] = texture_matrix_[15] = 1.0F;

    av_play_context_ = av_play_create(env, object_, 0, 44100);
    av_play_context_->priv_data = this;
    av_play_context_->on_complete = OnCompleteEvent;
    av_play_set_buffer_time(av_play_context_, 5);

    packet_pool_ = PacketPool::GetInstance();
    video_export_message_queue_ = new MessageQueue("Video Export Message Queue");
    video_export_handler_ = new VideoExportHandler(this, video_export_message_queue_);
    pthread_create(&export_message_thread_, nullptr, ExportMessageThread, this);
}

VideoExport::~VideoExport() {
    if (nullptr != av_play_context_) {
        av_play_release(av_play_context_);
        av_play_context_ = nullptr;
    }
    video_export_handler_->PostMessage(new Message(MESSAGE_QUEUE_LOOP_QUIT_FLAG));
    pthread_join(export_message_thread_, nullptr);
    delete[] audio_samples_;
    audio_samples_ = nullptr;
    if (nullptr != object_ && nullptr != vm_) {
        JNIEnv *env = nullptr;
        if ((vm_)->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(object_);
            object_ = nullptr;
        }
    }
    delete[] vertex_coordinate_;
    delete[] texture_coordinate_;
    delete[] texture_matrix_;
}

void* VideoExport::ExportMessageThread(void *context) {
    auto* video_export = reinterpret_cast<VideoExport*>(context);
    video_export->ProcessMessage();
    pthread_exit(nullptr);
}

void VideoExport::ProcessMessage() {
    bool rendering = true;
    while (rendering) {
        Message *msg = NULL;
        if (video_export_message_queue_->DequeueMessage(&msg, true) > 0) {
            if (msg == NULL) {
                return;
            }
            if (MESSAGE_QUEUE_LOOP_QUIT_FLAG == msg->Execute()) {
                rendering = false;
            }
            delete msg;
        }
    }
}

void VideoExport::OnCompleteEvent(AVPlayContext* context) {
    auto* video_export = reinterpret_cast<VideoExport*>(context->priv_data);
    video_export->OnComplete();
}

int VideoExport::OnComplete() {
    LOGI("enter %s", __func__);
    pthread_mutex_lock(&media_mutex_);
    FreeResource();
    if (clip_deque_.size() == 1) {
        export_ing_ = false;
    } else {
        export_index_++;
        if (export_index_ >= clip_deque_.size()) {
            export_ing_ = false;
        } else {
            previous_time_ = current_time_;
            MediaClip *clip = clip_deque_.at(export_index_);
            StartDecode(clip);
        }
    }
    pthread_cond_signal(&media_cond_);
    pthread_mutex_unlock(&media_mutex_);
    LOGI("leave %s", __func__);
    return 0;
}

void VideoExport::FreeResource() {
    audio_current_time_ = 0;
    image_render_time_ = 0;
    if (nullptr != av_play_context_) {
        av_play_stop(av_play_context_);
    }
}

void VideoExport::OnFilter() {
    if (nullptr != export_config_json_) {
        cJSON* filters = cJSON_GetObjectItem(export_config_json_, "filters");
        if (nullptr != filters) {
            int filter_size = cJSON_GetArraySize(filters);
            for (int i = 0; i < filter_size; i++) {
                cJSON* filter_child = cJSON_GetArrayItem(filters, i);
                cJSON* config_json = cJSON_GetObjectItem(filter_child, "config");
                cJSON* action_id_json = cJSON_GetObjectItem(filter_child, "actionId");
                cJSON* start_time_json = cJSON_GetObjectItem(filter_child, "startTime");
                cJSON* end_time_json = cJSON_GetObjectItem(filter_child, "endTime");
                int start_time = 0;
                if (nullptr != start_time_json) {
                    start_time = start_time_json->valueint;
                }
                int end_time = INT32_MAX;
                if (nullptr != end_time_json) {
                    end_time = end_time_json->valueint;
                }
                int action_id = 0;
                if (nullptr != action_id_json) {
                    action_id = action_id_json->valueint;
                }
                if (nullptr != config_json) {
                    char* config = config_json->valuestring;
                    image_process_->OnFilter(config, action_id, start_time, end_time);
                }
            }
        }
    }
}

void VideoExport::OnEffect() {
    if (nullptr != export_config_json_) {
        cJSON* effects = cJSON_GetObjectItem(export_config_json_, "effects");
        if (nullptr != effects) {
            int effect_size = cJSON_GetArraySize(effects);
            if (effect_size > 0) {
                for (int i = 0; i < effect_size; ++i) {
                    cJSON* effects_child = cJSON_GetArrayItem(effects, i);
                    cJSON* config_json = cJSON_GetObjectItem(effects_child, "config");
                    cJSON* action_id_json = cJSON_GetObjectItem(effects_child, "actionId");
                    cJSON* start_time_json = cJSON_GetObjectItem(effects_child, "startTime");
                    cJSON* end_time_json = cJSON_GetObjectItem(effects_child, "endTime");
                    int start_time = 0;
                    if (nullptr != start_time_json) {
                        start_time = start_time_json->valueint;
                    }
                    int end_time = INT32_MAX;
                    if (nullptr != end_time_json) {
                        end_time = end_time_json->valueint;
                    }
                    int action_id = 0;
                    if (nullptr != action_id_json) {
                        action_id = action_id_json->valueint;
                    }
                    if (nullptr != config_json) {
                        char* config = config_json->valuestring;
                        image_process_->OnAction(config, action_id);
                        image_process_->OnUpdateActionTime(start_time, end_time, action_id);
                    }
                }
            }
        }
    }
}

void VideoExport::OnMusics() {
    if (nullptr != export_config_json_) {
        cJSON* musics = cJSON_GetObjectItem(export_config_json_, "musics");
        if (nullptr != musics) {
            int music_size = cJSON_GetArraySize(musics);
            for (int i = 0; i < music_size; ++i) {
                cJSON* music_child = cJSON_GetArrayItem(musics, i);
                cJSON* config_json = cJSON_GetObjectItem(music_child, "config");
                if (nullptr != config_json) {
                    cJSON* config = cJSON_Parse(config_json->valuestring);
                    cJSON* path_json = cJSON_GetObjectItem(config, "path");
                    cJSON* start_time_json = cJSON_GetObjectItem(config, "statTime");
                    cJSON* end_time_json = cJSON_GetObjectItem(config, "endTime");

                    if (nullptr != path_json) {
                        char* path = path_json->valuestring;
                        MusicDecoder* decoder = new MusicDecoder();
                        int ret = decoder->Init(path, accompany_packet_buffer_size_);
                        int actualAccompanyPacketBufferSize = accompany_packet_buffer_size_;
                        if (ret >= 0) {
                            accompany_sample_rate_ = decoder->GetSampleRate();
                            if (vocal_sample_rate_ != accompany_sample_rate_) {

                            }
                            auto* resample = new trinity::Resample();
                            float ratio = accompany_sample_rate_ * 1.0f / vocal_sample_rate_;
                            actualAccompanyPacketBufferSize = ratio * accompany_packet_buffer_size_;
                            ret = resample->Init(accompany_sample_rate_, vocal_sample_rate_, actualAccompanyPacketBufferSize / 2, 2);
                            if (ret < 0) {
                                LOGE("resample InitMessageQueue error");
                            }
                            resample_deque_.push_back(resample);
                            decoder->SetPacketBufferSize(actualAccompanyPacketBufferSize);
                            // TODO time
                            music_decoder_deque_.push_back(decoder);
                        }
                    }
                }
            }
        }
    }
}

void VideoExport::StartDecode(MediaClip *clip) {
    LOGE("file: %s export_index: %d", clip->file_name, export_index_);
    LOGI("enter %s path: %s start_time: %d", __func__, clip->file_name, clip->start_time);
    if (clip->type == VIDEO) {
        av_play_play(clip->file_name, clip->start_time, av_play_context_);
    } else if (clip->type == IMAGE) {
        load_image_texture_ = true;
    }
    LOGI("leave %s", __func__);
}

void VideoExport::LoadImageTexture(MediaClip *clip) {
//    stbi_set_flip_vertically_on_load(1);

    int width = 0;
    int height = 0;
    int channels = 0;
    auto image_data = stbi_load(clip->file_name, &width, &height, &channels, STBI_rgb_alpha);
    if (width == 0 || height == 0 || nullptr == image_data) {
        return;
    }
    if (image_texture_ == 0) {
        glGenTextures(1, &image_texture_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, image_texture_);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBindTexture(GL_TEXTURE_2D, image_texture_);
    if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
        // 当图片大于1080p时, 缩放到1080p
        auto resize_width_ratio = MAX_IMAGE_WIDTH * 1.0F / width;
        auto resize_height_ratio = MAX_IMAGE_HEIGHT * 1.0F / height;
        auto resize_width = static_cast<int>(MAX(resize_width_ratio, resize_height_ratio) * width);
        auto resize_height = static_cast<int>(MAX(resize_width_ratio, resize_height_ratio) * height);
        auto resize_image_data = reinterpret_cast<unsigned char*>(
                malloc(static_cast<size_t>(resize_width * resize_height * channels)));
        stbir_resize_uint8(image_data, width, height, 0, resize_image_data, resize_width, resize_height, 0, channels);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, resize_width, resize_height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, resize_image_data);
        free(resize_image_data);
        frame_width_ = resize_width;
        frame_height_ = resize_height;
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
        frame_width_ = width;
        frame_height_ = height;
    }
    SetFrame(frame_width_, frame_height_, video_width_, video_height_, FIT);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(image_data);

    if (image_frame_buffer_ == nullptr) {
        image_frame_buffer_ = new FrameBuffer(video_width_, video_height_, DEFAULT_VERTEX_SHADER, DEFAULT_FRAGMENT_SHADER);
    }
}

int VideoExport::Export(const char *export_config, const char *path,
        int width, int height, int frame_rate, int video_bit_rate,
        int sample_rate, int channel_count, int audio_bit_rate,
        bool media_codec_decode, bool media_codec_encode) {
    LOGI("enter %s path: %s width: %d height: %d frame_rate: %d video_bit_rate: %d sample_rate: %d channel_count: %d audio_bit_rate: %d media_codec_decode: %d media_codec_encode: %d",
            __func__, path, width, height, frame_rate, video_bit_rate, sample_rate, channel_count, audio_bit_rate, media_codec_decode, media_codec_encode);
    FILE* file = fopen(export_config, "r");
    if (nullptr == file) {
        return -1;
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);
    char* buffer = reinterpret_cast<char*>(malloc(sizeof(char) * file_size + 1));
    if (nullptr == buffer) {
        fclose(file);
        return -1;
    }
    buffer[file_size] = '\0';
    size_t read_size = fread(buffer, 1, file_size, file);
    if (read_size != file_size) {
        fclose(file);
        return -1;
    }
    fclose(file);

    LOGE("buffer: %s", buffer);

    export_config_json_ = cJSON_Parse(buffer);
    if (nullptr == export_config_json_) {
        LOGE("nullptr == json");
        return EXPORT_CONFIG;
    }
    cJSON* clips = cJSON_GetObjectItem(export_config_json_, "clips");
    if (nullptr == clips) {
        LOGE("nullptr == clips");
        return CLIP_EMPTY;
    }
    int clip_size = cJSON_GetArraySize(clips);
    if (clip_size == 0) {
        LOGE("clip_size == 0");
        return CLIP_EMPTY;
    }
    cJSON* item = clips->child;

    export_ing_ = true;
    channel_count_ = channel_count;
    vocal_sample_rate_ = sample_rate;
    packet_thread_ = new VideoConsumerThread();
    int ret = packet_thread_->Init(path, width, height, frame_rate, video_bit_rate * 1000, sample_rate, channel_count, audio_bit_rate * 1000, "libfdk_aac");
    if (ret < 0) {
        return ret;
    }
    PacketPool::GetInstance()->InitRecordingVideoPacketQueue();
    PacketPool::GetInstance()->InitAudioPacketQueue(44100);
    AudioPacketPool::GetInstance()->InitAudioPacketQueue();
    packet_thread_->StartAsync();

    video_width_ = static_cast<int>((floor(width / 16.0F)) * 16);
    video_height_ = static_cast<int>((floor(height / 16.0F)) * 16);
    frame_rate_ = frame_rate;

    for (int i = 0; i < clip_size; i++) {
        cJSON* path_item = cJSON_GetObjectItem(item, "path");
        cJSON* start_time_item = cJSON_GetObjectItem(item, "startTime");
        cJSON* end_time_item = cJSON_GetObjectItem(item, "endTime");
        cJSON* type_item = cJSON_GetObjectItem(item, "type");
        item = item->next;

        auto* export_clip = new MediaClip();
        export_clip->start_time = start_time_item->valueint;
        export_clip->end_time  = end_time_item->valueint;
        export_clip->file_name = path_item->valuestring;
        export_clip->type = type_item->valueint;
        clip_deque_.push_back(export_clip);

        video_duration_ += export_clip->end_time - export_clip->start_time;
    }


    free(buffer);
    if (media_codec_encode) {
        encoder_ = new MediaEncodeAdapter(vm_, object_);
    } else {
        encoder_ = new SoftEncoderAdapter(vertex_coordinate_, texture_coordinate_);
    }
    media_codec_encode_ = media_codec_encode;
    // 软解
    av_play_context_->force_sw_decode = !media_codec_decode;
    encoder_->Init(width, height, video_bit_rate * 1000, frame_rate);
    audio_encoder_adapter_ = new AudioEncoderAdapter();
    audio_encoder_adapter_->Init(packet_pool_, 44100, 1, 128 * 1000, "libfdk_aac");
    pthread_mutex_init(&media_mutex_, nullptr);
    pthread_cond_init(&media_cond_, nullptr);
    pthread_create(&export_video_thread_, nullptr, ExportVideoThread, this);
    pthread_create(&export_audio_thread_, nullptr, ExportAudioThread, this);
    LOGI("leave %s", __func__);
    return 0;
}

void* VideoExport::ExportVideoThread(void* context) {
    auto* video_export = reinterpret_cast<VideoExport*>(context);
    video_export->ProcessVideoExport();
    pthread_exit(nullptr);
}

void VideoExport::SetFrame(int source_width, int source_height,
        int target_width, int target_height, trinity::RenderFrame frame_type) {
    float target_ratio = target_width * 1.0F / target_height;
    float scale_width = 1.0F;
    float scale_height = 1.0F;
    if (frame_type == FIT) {
        float source_ratio = source_width * 1.0F / source_height;
        if (source_ratio > target_ratio) {
            scale_width = 1.0F;
            scale_height = target_ratio / source_ratio;
        } else {
            scale_width = source_ratio / target_ratio;
            scale_height = 1.0F;
        }
    } else if (frame_type == CROP) {
        float source_ratio = source_width * 1.0F / source_height;
        if (source_ratio > target_ratio) {
            scale_width = source_ratio / target_ratio;
            scale_height = 1.0F;
        } else {
            scale_width = 1.0F;
            scale_height = target_ratio / source_ratio;
        }
    }
    crop_vertex_coordinate_[0] = -scale_width;
    crop_vertex_coordinate_[1] = -scale_height;
    crop_vertex_coordinate_[2] = scale_width;
    crop_vertex_coordinate_[3] = -scale_height;
    crop_vertex_coordinate_[4] = -scale_width;
    crop_vertex_coordinate_[5] = scale_height;
    crop_vertex_coordinate_[6] = scale_width;
    crop_vertex_coordinate_[7] = scale_height;
    LOGI("SetFrame source_width: %d source_height: %d target_width: %d target_height: %d scale_width; %f scale_height: %f",
         source_width, source_height, target_width, target_height, scale_width, scale_height); // NOLINT
}

void VideoExport::ProcessVideoExport() {
    LOGI("enter %s", __func__);
    egl_core_ = new EGLCore();
    egl_core_->InitWithSharedContext();
    egl_surface_ = egl_core_->CreateOffscreenSurface(64, 64);
    if (nullptr == egl_surface_ || EGL_NO_SURFACE == egl_surface_) {
        return;
    }
    egl_core_->MakeCurrent(egl_surface_);

    GLuint oes_texture_ = 0;
    glGenTextures(1, &oes_texture_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, oes_texture_);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    av_play_context_->media_codec_texture_id = oes_texture_;

    MediaClip* c = clip_deque_.at(0);
    StartDecode(c);

    // 如果硬解码会重新创建
    FrameBuffer* media_codec_render_ = nullptr;
    image_process_ = new ImageProcess();

    // 添加config解析出来的滤镜
    OnFilter();

    // 添加config解析出来的特效
    OnEffect();

    encoder_->CreateEncoder(egl_core_);
    while (true) {
        if (!export_ing_) {
            break;
        }
        auto clip = clip_deque_.at(export_index_);

        if (load_image_texture_) {
            load_image_texture_ = false;
            LoadImageTexture(clip);
        }

        if (clip->type == IMAGE) {
            image_frame_buffer_->ActiveProgram();
            static GLfloat texture_coordinate[] = {
                    0.F, 1.F,
                    1.F, 1.F,
                    0.F, 0.F,
                    1.F, 0.F
            };
            // 硬编码时图片上下镜像
            encode_texture_id_ = image_frame_buffer_->OnDrawFrame(image_texture_,
                    crop_vertex_coordinate_, media_codec_encode_ ? texture_coordinate : texture_coordinate_);
            image_render_time_ += 1000 / frame_rate_;
            current_time_ = static_cast<uint64_t>(image_render_time_);
            if (image_render_time_ >= clip->end_time) {
                current_time_ += previous_time_;
                OnComplete();
                continue;
            }
        } else if (clip->type == VIDEO) {
            if (av_play_context_->error_code == BUFFER_FLAG_END_OF_STREAM && av_play_context_->video_frame_queue->count == 0) {
                pthread_mutex_lock(&media_mutex_);
                LOGE("export video message stop");
                av_play_context_->send_message(av_play_context_, message_stop);
                pthread_cond_wait(&media_cond_, &media_mutex_);
                pthread_mutex_unlock(&media_mutex_);
                continue;
            }

            if (av_play_context_->video_frame == nullptr) {
                av_play_context_->video_frame = frame_queue_get(av_play_context_->video_frame_queue);
            }

            if (!av_play_context_->video_frame) {
                continue;
            }

            AVFrame* frame = av_play_context_->video_frame;
            int width = MIN(frame->linesize[0], frame->width);
            int height = frame->height;
            // 如果当前记录的视频的宽和高不相等时,重建各个FrameBuffer
            if (frame_width_ != width || frame_height_ != height) {
                frame_width_ = width;
                frame_height_ = height;
                SetFrame(frame_width_, frame_height_,
                        video_width_, video_height_, FIT);
                if (av_play_context_->is_sw_decode) {
                    if (nullptr != yuv_render_) {
                        delete yuv_render_;
                    }
                    yuv_render_ = new YuvRender(0);
                } else {
                    if (nullptr != media_codec_render_) {
                        delete media_codec_render_;
                    }
                    media_codec_render_ = new FrameBuffer(frame_width_, frame_height_,
                            media_codec_encode_ ? DEFAULT_VERTEX_MATRIX_SHADER : DEFAULT_VERTEX_SHADER,
                            DEFAULT_OES_FRAGMENT_SHADER);
                    media_codec_render_->SetTextureType(TEXTURE_OES);
                }
            }
            // 如果是软编码, 使用YuvRender把AVFrame渲染成纹理
            // 硬编码时, 把OES纹理渲染成普通 的TEXTURE_2D纹理
            if (av_play_context_->is_sw_decode) {
                // 硬编和软解时, yuv_render时需要旋做上下镜像处理
                if (media_codec_encode_) {
                    GLfloat texture_coordinate[] = {
                            0.0F, 1.0F,
                            1.0F, 1.0F,
                            0.0F, 0.0F,
                            1.0F, 0.0F
                    };
                    encode_texture_id_ = yuv_render_->DrawFrame(frame, crop_vertex_coordinate_, texture_coordinate);
                } else {
                    encode_texture_id_ = yuv_render_->DrawFrame(frame, crop_vertex_coordinate_, texture_coordinate_);
                }
                current_time_ = av_rescale_q(av_play_context_->video_frame->pts,
                                                                av_play_context_->format_context->streams[av_play_context_->video_index]->time_base,
                                                                AV_TIME_BASE_Q) / 1000;
            } else {
                mediacodec_update_image(av_play_context_);
                int ret = mediacodec_get_texture_matrix(av_play_context_, texture_matrix_);
                media_codec_render_->ActiveProgram();
                encode_texture_id_ = media_codec_render_->OnDrawFrame(oes_texture_,
                        crop_vertex_coordinate_, texture_coordinate_,
                        media_codec_encode_ ? texture_matrix_ : nullptr);
                current_time_ = av_play_context_->video_frame->pts / 1000;
                mediacodec_release_buffer(av_play_context_, av_play_context_->video_frame);
            }
            frame_pool_unref_frame(av_play_context_->video_frame_pool, av_play_context_->video_frame);
            av_play_context_->video_frame = nullptr;
        }

        if (current_time_ == 0)  {
            continue;
        }
        if (previous_time_ != 0) {
            current_time_ = current_time_ + previous_time_;
        }
        // 执行特效操作
        if (image_process_ != nullptr) {
            encode_texture_id_ = image_process_->Process(encode_texture_id_, current_time_, frame_width_, frame_height_, 0, 0);
        }
        if (!egl_core_->SwapBuffers(egl_surface_)) {
            LOGE("eglSwapBuffers error: %d", eglGetError());
        }

        // 编码视频
        encoder_->Encode(current_time_, encode_texture_id_);
        // 回调合成进度给上层
        OnExportProgress(current_time_);
    }
    LOGE("export thread ===========> exit");
    encoder_->DestroyEncoder();
    delete encoder_;
    packet_thread_->Stop();
//    PacketPool::GetInstance()->AbortRecordingVideoPacketQueue();
//    PacketPool::GetInstance()->DestroyRecordingVideoPacketQueue();
//    AudioPacketPool::GetInstance()->DestroyAudioPacketQueue();
    delete packet_thread_;

    if (nullptr != image_process_) {
        delete image_process_;
        image_process_ = nullptr;
    }

    if (image_texture_ != 0) {
        glDeleteTextures(1, &image_texture_);
        image_texture_ = 0;
    }

    if (nullptr != image_frame_buffer_) {
        delete image_frame_buffer_;
        image_frame_buffer_ = nullptr;
    }

    if (nullptr != export_config_json_) {
        cJSON_Delete(export_config_json_);
        export_config_json_ = nullptr;
    }

    for (auto clip : clip_deque_) {
        delete clip;
    }
    clip_deque_.clear();

    egl_core_->ReleaseSurface(egl_surface_);
    egl_core_->Release();
    egl_surface_ = EGL_NO_SURFACE;
    delete egl_core_;
    FreeResource();
    // 通知java层合成完成
    OnExportComplete();
    LOGI("leave %s", __func__);
}

void VideoExport::OnExportProgress(uint64_t current_time) {
    if (video_duration_ == 0) {
        LOGE("video_duration is 0");
        return;
    }
    if (nullptr == vm_) {
        return;
    }
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return;
    }
    if (nullptr == env) {
        return;
    }
    jclass clazz = env->GetObjectClass(object_);
    if (nullptr != clazz) {
        jmethodID  export_progress_id = env->GetMethodID(clazz, "onExportProgress", "(F)V");
        if (nullptr != export_progress_id) {
            env->CallVoidMethod(object_, export_progress_id, current_time * 1.0f / video_duration_);
        }
    }
    if (vm_->DetachCurrentThread() != JNI_OK) {
       LOGE("DetachCurrentThread failed");
    }
}

void VideoExport::OnExportComplete() {
    if (nullptr == vm_) {
        return;
    }
    JNIEnv* env = nullptr;
    if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return;
    }
    if (nullptr == env) {
        return;
    }
    jclass clazz = env->GetObjectClass(object_);
    if (nullptr != clazz) {
        jmethodID  complete_id = env->GetMethodID(clazz, "onExportComplete", "()V");
        if (nullptr != complete_id) {
            env->CallVoidMethod(object_, complete_id);
        }
    }
    if (vm_->DetachCurrentThread() != JNI_OK) {
        LOGE("DetachCurrentThread failed");
    }
}

void* VideoExport::ExportAudioThread(void *context) {
    VideoExport* video_export = reinterpret_cast<VideoExport*>(context);
    video_export->ProcessAudioExport();
    pthread_exit(0);
}

void VideoExport::ProcessAudioExport() {
    OnMusics();
    while (true) {
        if (!export_ing_) {
            break;
        }

        auto clip = clip_deque_.at(export_index_);
        AudioPacket* music_packet = nullptr;
        for (int i = 0; i < music_decoder_deque_.size(); ++i) {
            auto* decoder = music_decoder_deque_.at(i);
            music_packet = decoder->DecodePacket();
            auto resample = resample_deque_.at(i);

            short* stereoSamples = music_packet->buffer;
            int stereoSampleSize = music_packet->size;
            if (stereoSampleSize > 0) {
                int monoSampleSize = stereoSampleSize / 2;
                auto** samples = new short*[2];
                samples[0] = new short[monoSampleSize];
                samples[1] = new short[monoSampleSize];
                for (int index = 0; index < monoSampleSize; index++) {
                    samples[0][index] = stereoSamples[2 * index];
                    samples[1][index] = stereoSamples[2 * index + 1];
                }
                float transfer_ratio = accompany_sample_rate_ / static_cast<float>(vocal_sample_rate_);
                int accompanySampleSize = static_cast<int>(monoSampleSize * 1.0f / transfer_ratio);
                uint8_t out_data[accompanySampleSize * 2 * 2];
                int out_nb_bytes = 0;
                resample->Process(samples, out_data, monoSampleSize, &out_nb_bytes);
                delete[] samples[0];
                delete[] samples[1];
                delete[] stereoSamples;
                if (out_nb_bytes > 0) {
                    accompanySampleSize = out_nb_bytes / 2;
                    auto* accompanySamples = new short[accompanySampleSize];
                    convertShortArrayFromByteArray(out_data, out_nb_bytes, accompanySamples, 1.0);
                    music_packet->buffer = accompanySamples;
                    music_packet->size = accompanySampleSize;
                }
            }
        }

        int audio_size = 0;
        if (clip->type == IMAGE) {
            if (audio_current_time_ < clip->end_time) {
                audio_size = FillMuteAudio();
            }
        } else if (clip->type == VIDEO) {
            audio_size = Resample();
        }
        if (audio_size > 0) {
            // TODO buffer池
            auto *packet = new AudioPacket();
            // TODO delete
            auto *samples = new short[audio_size / sizeof(short)];
            memcpy(samples, audio_buffer_, audio_size);
            if (music_packet != nullptr && nullptr != music_packet->buffer) {
                auto* mix = new short[audio_size];
                mixtureAccompanyAudio(samples, music_packet->buffer, audio_size / sizeof(short), mix);
                packet->buffer = mix;
            } else {
                packet->buffer = samples;
            }
            packet->size = audio_size / sizeof(short);
            packet_pool_->PushAudioPacketToQueue(packet);
        }
    }
    LOGE("audio export done");
    if (nullptr != swr_context_) {
        swr_free(&swr_context_);
    }
    for (auto decoder : music_decoder_deque_) {
        decoder->Destroy();
        delete decoder;
    }
    music_decoder_deque_.clear();
    for (auto resample : resample_deque_) {
        resample->Destroy();
        delete resample;
    }
    resample_deque_.clear();
    if (nullptr != image_audio_buffer_) {
        delete[] image_audio_buffer_;
        image_audio_buffer_ = nullptr;
    }
    if (nullptr != audio_encoder_adapter_) {
        audio_encoder_adapter_->Destroy();
        delete audio_encoder_adapter_;
        audio_encoder_adapter_ = nullptr;
    }
}

/**
 * 图片时补空音频
 * @return 音频数据大小
 */
int VideoExport::FillMuteAudio() {
    if (image_audio_buffer_ == nullptr) {
        image_audio_buffer_ = new uint8_t[2048];
        memset(image_audio_buffer_, 0, 2048);
    }
    audio_buffer_ = image_audio_buffer_;
    audio_current_time_ += (2048 * 1000 / vocal_sample_rate_ * channel_count_);
    return 2048;
}

int VideoExport::Resample() {
    AVPlayContext* context = av_play_context_;
    do {
        AVFrame* frame = frame_queue_get(context->audio_frame_queue);
        if (frame != nullptr) {
            context->audio_frame = frame;
            break;
        } else {
            if (av_play_context_->audio_index == -1) {
                return -1;
            }
        }
    } while (true);
    AVFrame* frame = context->audio_frame;
    int data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(frame),
                                               frame->nb_samples, (AVSampleFormat) frame->format, 1);

    int channel_layout = AV_CH_LAYOUT_MONO;
    AVSampleFormat format = AV_SAMPLE_FMT_S16;
    int sample_rate = 44100;
    int channels = 1;

    if (nullptr == swr_context_) {
        uint64_t dec_channel_layout = (frame->channel_layout && av_frame_get_channels(frame) == av_get_channel_layout_nb_channels(frame->channel_layout)) ?
                                      frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(frame));
        swr_context_ = swr_alloc_set_opts(NULL, channel_layout, format, sample_rate,
                                          dec_channel_layout, (AVSampleFormat) frame->format, frame->sample_rate, 0, NULL);
        if (!swr_context_ || swr_init(swr_context_) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   frame->sample_rate, av_get_sample_fmt_name((AVSampleFormat) frame->format), av_frame_get_channels(frame),
                   sample_rate, av_get_sample_fmt_name(format), channels);
            swr_free(&swr_context_);
            return -1;
        }
    }
    unsigned int audio_buf1_size = 0;
    int wanted_nb_sample = frame->nb_samples;
    int resample_data_size;
    if (swr_context_) {
        const uint8_t **in = (const uint8_t **) frame->extended_data;
        uint8_t **out = &audio_buf1;
        int out_count = wanted_nb_sample * sample_rate / frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, channels, out_count, format, 0);
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_sample != frame->nb_samples) {
            if (swr_set_compensation(swr_context_, (wanted_nb_sample - frame->nb_samples) * sample_rate / frame->sample_rate,
                                     wanted_nb_sample * sample_rate / frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&audio_buf1, &audio_buf1_size, out_size);
        if (!audio_buf1) {
            return AVERROR(ENOMEM);
        }
        int len2 = swr_convert(swr_context_, out, out_count, in, frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too samll.\n");
            if (swr_init(swr_context_) < 0) {
                swr_free(&swr_context_);
            }
        }
        audio_buffer_ = audio_buf1;
        resample_data_size = len2 * channels * av_get_bytes_per_sample(format);
    } else {
        audio_buffer_ = frame->data[0];
        resample_data_size = data_size;
    }
    return resample_data_size;
}

}  // namespace trinity
