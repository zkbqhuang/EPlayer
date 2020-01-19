
#include "AudioResampler.h"

AudioResampler::AudioResampler(PlayerState *playerState, AudioDecoder *audioDecoder,
                               MediaSync *mediaSync) {
    this->playerState = playerState;
    this->audioDecoder = audioDecoder;
    this->mediaSync = mediaSync;
    //音频重采样结构体
    audioState = (AudioState *) av_mallocz(sizeof(AudioState));
    memset(audioState, 0, sizeof(AudioState));
    soundTouchWrapper = new SoundTouchWrapper();
    frame = av_frame_alloc();

}

AudioResampler::~AudioResampler() {
    playerState = NULL;
    audioDecoder = NULL;
    mediaSync = NULL;
    if (soundTouchWrapper) {
        delete soundTouchWrapper;
        soundTouchWrapper = NULL;
    }
    if (audioState) {
        swr_free(&audioState->swr_ctx);
        av_freep(&audioState->resampleBuffer);
        memset(audioState, 0, sizeof(AudioState));
        av_free(audioState);
        audioState = NULL;
    }
    if (frame) {
        av_frame_unref(frame);
        av_frame_free(&frame);
        frame = NULL;
    }
}

int AudioResampler::setResampleParams(AudioDeviceSpec *spec, int64_t wanted_channel_layout) {

    audioState->audioParamsSrc = audioState->audioParamsTarget;
    audioState->audio_hw_buf_size = spec->size;
    audioState->bufferSize = 0;
    audioState->bufferIndex = 0;
    audioState->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    audioState->audio_diff_avg_count = 0;
    audioState->audio_diff_threshold =
            (double) (audioState->audio_hw_buf_size) / audioState->audioParamsTarget.bytes_per_sec;
    audioState->audioParamsTarget.fmt = AV_SAMPLE_FMT_S16;
    audioState->audioParamsTarget.freq = spec->freq;
    audioState->audioParamsTarget.channel_layout = wanted_channel_layout;
    audioState->audioParamsTarget.channels = spec->channels;
    //一帧的缓存大小，第三个参数为一个通道的采样点大小，所以这里计算得到的其实是一个采样点的大小，单位是byte
    audioState->audioParamsTarget.frame_size = av_samples_get_buffer_size(NULL,
                                                                          audioState->audioParamsTarget.channels,
                                                                          1,
                                                                          audioState->audioParamsTarget.fmt,
                                                                          1);
    //期望采样达到一秒多少bytes
    audioState->audioParamsTarget.bytes_per_sec = av_samples_get_buffer_size(NULL,
                                                                             audioState->audioParamsTarget.channels,
                                                                             audioState->audioParamsTarget.freq,
                                                                             audioState->audioParamsTarget.fmt,
                                                                             1);

//    LOGE("原始参数：%d--%d--%d",audioState->audioParamsTarget.freq,audioState->audioParamsTarget.channels,audioState->audioParamsTarget.frame_size)
    if (audioState->audioParamsTarget.bytes_per_sec <= 0 ||
        audioState->audioParamsTarget.frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return 0;
}

/**
 * PCM队列回调方法，用于取得PCM数据
 * @param stream
 * @param len
 */
void AudioResampler::pcmQueueCallback(uint8_t *stream, int len) {
    int bufferSize, length;

    // 没有音频解码器时，直接返回
    if (!audioDecoder) {
        memset(stream, 0, len);
        return;
    }

    //单位:AV_TIME_BASE,即ffmpeg内部使用的时间单位，返回的可能是从系统启动那一刻开始计时的时间
    audioState->audio_callback_time = av_gettime_relative();
    while (len > 0) {
        //一般 audioState->bufferSize 为一次audioFrameResample采集音频数据大小，audioState->bufferIndex 实际上就是这些数据写了多少
        if (audioState->bufferIndex >= audioState->bufferSize) {
            //采集的音频数据保存到audioState->outputBuffer缓存区中
            bufferSize = audioFrameResample();
            if (bufferSize < 0) {
                audioState->outputBuffer = NULL;
                audioState->bufferSize = (unsigned int) (AUDIO_MIN_BUFFER_SIZE /
                                                         audioState->audioParamsTarget.frame_size *
                                                         audioState->audioParamsTarget.frame_size);
            } else {
                audioState->bufferSize = bufferSize;
            }
            audioState->bufferIndex = 0;
        }

        length = audioState->bufferSize - audioState->bufferIndex;
        //如果读取长度大于需要数据的长度
        if (length > len) {
            length = len;
        }
        // 复制经过转码输出的PCM数据到缓冲区中
        if (audioState->outputBuffer != NULL && !playerState->mute) {
            //从存储区 str2 复制 n 个字符到存储区 str1。
            memcpy(stream, audioState->outputBuffer + audioState->bufferIndex, length);
        } else {
            //暂停执行这里
            memset(stream, 0, length);
        }
        len -= length;
        stream += length;
        //写了多少index就增加多少
        audioState->bufferIndex += length;
    }
    //循环结束，表示已经写够了len长度的数据

    //保存已经写入的大小
    audioState->writeBufferSize = audioState->bufferSize - audioState->bufferIndex;

    if (!isnan(audioState->audioClock) && mediaSync) {
        //audioState->audioClock代表当前帧播放完时的时刻，
        mediaSync->updateAudioClock(audioState->audioClock - (double) (2 * audioState->audio_hw_buf_size +
                                                                       audioState->writeBufferSize) /
                                                             audioState->audioParamsTarget.bytes_per_sec,
                                    audioState->audio_callback_time / 1000000.0);
    }
}

int AudioResampler::audioSynchronize(int nbSamples) {
    int wanted_nb_samples = nbSamples;

    // 如果时钟不是同步到音频流，则需要进行对音频频进行同步处理
    if (playerState->syncType != AV_SYNC_AUDIO) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;
        diff = mediaSync ? mediaSync->getAudioDiffClock() : 0;
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            audioState->audio_diff_cum = diff + audioState->audio_diff_avg_coef * audioState->audio_diff_cum;
            if (audioState->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                audioState->audio_diff_avg_count++;
            } else {
                avg_diff = audioState->audio_diff_cum * (1.0 - audioState->audio_diff_avg_coef);
                if (fabs(avg_diff) >= audioState->audio_diff_threshold) {
                    wanted_nb_samples = nbSamples + (int) (diff * audioState->audioParamsSrc.freq);
                    min_nb_samples = ((nbSamples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nbSamples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
            }
        } else {
            audioState->audio_diff_avg_count = 0;
            audioState->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

int AudioResampler::audioFrameResample() {
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    int wanted_nb_samples;
    int translate_time = 1;
    int ret = -1;

    // 处于暂停状态
    if (!audioDecoder || playerState->abortRequest || playerState->pauseRequest) {
        return -1;
    }

    for (;;) {

        // 如果数据包解码失败，直接返回
        if ((ret = audioDecoder->getAudioFrame(frame)) < 0) { //获取解码得到的音频帧
            return -1;
        }
        if (ret == 0) {
            continue;
        }
        //获取frame的大小
        data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(frame),
                                               frame->nb_samples,
                                               (AVSampleFormat) frame->format, 1);
        //音频布局
        dec_channel_layout = (frame->channel_layout &&
                              av_frame_get_channels(frame) == av_get_channel_layout_nb_channels(frame->channel_layout))
                             ? frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(frame));

        wanted_nb_samples = audioSynchronize(frame->nb_samples);

        // 帧格式跟源格式不对？？？？当返回frame的格式跟音频原始参数不一样的时候，则修正
        if (frame->format != audioState->audioParamsSrc.fmt
            || dec_channel_layout != audioState->audioParamsSrc.channel_layout
            || frame->sample_rate != audioState->audioParamsSrc.freq
            || (wanted_nb_samples != frame->nb_samples && !audioState->swr_ctx)) {

            swr_free(&audioState->swr_ctx);
            audioState->swr_ctx = swr_alloc_set_opts(NULL,
                                                     audioState->audioParamsTarget.channel_layout,
                                                     audioState->audioParamsTarget.fmt,
                                                     audioState->audioParamsTarget.freq,
                                                     dec_channel_layout,
                                                     (AVSampleFormat) frame->format,
                                                     frame->sample_rate, 0, NULL);

            if (!audioState->swr_ctx || swr_init(audioState->swr_ctx) < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                       frame->sample_rate,
                       av_get_sample_fmt_name((AVSampleFormat) frame->format),
                       av_frame_get_channels(frame),
                       audioState->audioParamsTarget.freq,
                       av_get_sample_fmt_name(audioState->audioParamsTarget.fmt),
                       audioState->audioParamsTarget.channels);
                swr_free(&audioState->swr_ctx);
                return -1;
            }
            audioState->audioParamsSrc.channel_layout = dec_channel_layout;
            audioState->audioParamsSrc.channels = av_frame_get_channels(frame);
            audioState->audioParamsSrc.freq = frame->sample_rate;
            audioState->audioParamsSrc.fmt = (AVSampleFormat) frame->format;
        }

        // 音频重采样处理
        if (audioState->swr_ctx) {
            const uint8_t **in = (const uint8_t **) frame->extended_data;
            uint8_t **out = &audioState->resampleBuffer;
            int out_count = (int64_t) wanted_nb_samples * audioState->audioParamsTarget.freq /
                            frame->sample_rate + 256;
            int out_size = av_samples_get_buffer_size(NULL, audioState->audioParamsTarget.channels,
                                                      out_count, audioState->audioParamsTarget.fmt,
                                                      0);
            int len2;
            if (out_size < 0) {
                av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
                return -1;
            }
            if (wanted_nb_samples != frame->nb_samples) {
                //激活重采样补偿（软补偿）
                if (swr_set_compensation(audioState->swr_ctx,
                                         (wanted_nb_samples - frame->nb_samples) * audioState->audioParamsTarget.freq /
                                         frame->sample_rate,
                                         wanted_nb_samples * audioState->audioParamsTarget.freq / frame->sample_rate) <
                    0) {
                    av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                    return -1;
                }
            }
            av_fast_malloc(&audioState->resampleBuffer, &audioState->resampleSize, out_size);
            if (!audioState->resampleBuffer) {
                return AVERROR(ENOMEM);
            }

            /* 针对每一帧音频的处理。把一帧帧的音频作相应的重采样
            参数1：音频重采样的上下文
            参数2：输出的指针。传递的输出的数组
            参数3：输出的样本数量，不是字节数。单通道的样本数量。
            参数4：输入的数组，AVFrame解码出来的DATA
            参数5：输入的单通道的样本数量。
             */
            len2 = swr_convert(audioState->swr_ctx, out, out_count, in, frame->nb_samples);
            if (len2 < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
                return -1;
            }
            if (len2 == out_count) {
                av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
                if (swr_init(audioState->swr_ctx) < 0) {
                    swr_free(&audioState->swr_ctx);
                }
            }
            audioState->outputBuffer = audioState->resampleBuffer;
            //重采样得到的数据大小，单位byte
            resampled_data_size = len2 * audioState->audioParamsTarget.channels *
                                  av_get_bytes_per_sample(audioState->audioParamsTarget.fmt);

            // 变速变调处理
            if ((playerState->playbackRate != 1.0f || playerState->playbackPitch != 1.0f) &&
                !playerState->abortRequest) {
                int bytes_per_sample = av_get_bytes_per_sample(audioState->audioParamsTarget.fmt);
                av_fast_malloc(&audioState->soundTouchBuffer, &audioState->soundTouchBufferSize,
                               out_size * translate_time);
                for (int i = 0; i < (resampled_data_size / 2); i++) {
                    audioState->soundTouchBuffer[i] = (audioState->resampleBuffer[i * 2] |
                                                       (audioState->resampleBuffer[i * 2 + 1] << 8));
                }
                if (!soundTouchWrapper) {
                    soundTouchWrapper = new SoundTouchWrapper();
                }
                int ret_len = soundTouchWrapper->translate(audioState->soundTouchBuffer,
                                                           (float) (playerState->playbackRate),
                                                           (float) (playerState->playbackPitch !=
                                                                    1.0f
                                                                    ? playerState->playbackPitch :
                                                                    1.0f /
                                                                    playerState->playbackRate),
                                                           resampled_data_size / 2,
                                                           bytes_per_sample,
                                                           audioState->audioParamsTarget.channels,
                                                           frame->sample_rate);
                if (ret_len > 0) {
                    audioState->outputBuffer = (uint8_t *) audioState->soundTouchBuffer;
                    resampled_data_size = ret_len;
                } else {
                    translate_time++;
                    av_frame_unref(frame);
                    continue;
                }
            }
        } else {
            audioState->outputBuffer = frame->data[0];
            resampled_data_size = data_size;
        }

        // 处理完直接退出循环
        break;
    }

    // 利用pts更新音频时钟
    if (frame->pts != AV_NOPTS_VALUE) {
        audioState->audioClock = frame->pts * av_q2d((AVRational) {1, frame->sample_rate})
                                 + (double) frame->nb_samples / frame->sample_rate;
    } else {
        audioState->audioClock = NAN;
    }

    // 使用完成释放引用，防止内存泄漏
    av_frame_unref(frame);

    return resampled_data_size;
}

