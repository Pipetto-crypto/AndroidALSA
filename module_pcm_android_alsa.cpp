#include <cstdlib>
#include <memory>
#include <vector>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include <android/log.h>
#include <oboe/Oboe.h>

#define LOG_TAG "AndroidALSA"
#define printf(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)             

static oboe::AudioFormat toOboeFormat(snd_pcm_format_t alsaFormat) {
	oboe::AudioFormat format;

	switch (alsaFormat) {
		case SND_PCM_FORMAT_U8:
		case SND_PCM_FORMAT_S16_LE:
			format = oboe::AudioFormat::I16;
		    break;
		case SND_PCM_FORMAT_S24_3LE:
			format = oboe::AudioFormat::I24;
			break;
		case SND_PCM_FORMAT_S32_LE:
			format = oboe::AudioFormat::I32;
			break;
		case SND_PCM_FORMAT_FLOAT_LE:
		    format = oboe::AudioFormat::Float;
		    break;
		default:
		    format = oboe::AudioFormat::Unspecified;
		    break;
	}

	return format;
}

static int oboe_to_alsa_error(oboe::Result result) {
	switch (result) {
		case oboe::Result::ErrorDisconnected:
			return -EPIPE;
		case oboe::Result::ErrorWouldBlock:
			return -EAGAIN;
		case oboe::Result::ErrorNoMemory:
			return -ENOMEM;
		default:
			return -1;
	}
}

class AndroidALSA {
	private:
		std::shared_ptr<oboe::AudioStream> mStream;
		std::mutex mtx;
		snd_pcm_ioplug_t io = {
			.version = SND_PCM_IOPLUG_VERSION,
			.name = "ALSA <-> Android PCM Plugin",
			.callback = &android_alsa_callback,
			.mmap_rw = 0,
			.private_data = this
		};

		uint8_t *convertU8ToI16(snd_pcm_ioplug_t* io, uint8_t *data, snd_pcm_uframes_t size) {
			auto numSamples = io->channels * size;
			uint8_t *output = reinterpret_cast<uint8_t *>(malloc(numSamples * sizeof(int16_t)));

			for (size_t i = 0; i < numSamples; i++) {
				auto sample = static_cast<int16_t>((data[i] - 128) * 256);
				output[i * 2] = sample & 0xFF;
				output[i * 2 + 1] = (sample >> 8) & 0xFF;
			}

			return output;
		}

		int android_alsa_set_hw_constraint(snd_pcm_ioplug_t* io) 
		{
		    static const std::vector<unsigned int> access_list = { SND_PCM_ACCESS_RW_INTERLEAVED };
		    static const std::vector<unsigned int> format_list = { SND_PCM_FORMAT_U8, SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE };
		    int err;

		    err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, access_list.size(), access_list.data());                                      
		    if (err < 0) return err;
		
		    err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, format_list.size(), format_list.data());
		    if (err < 0) 
		    	return err;
		
		    err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 2);
		    if (err < 0) 
		    	return err;
		
		    err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, 8000, 48000); 
		    if (err < 0) 
		    	return err;
		
		    err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 64, 64 * 1024);
		    if (err < 0) 
		    	return err;
		
		    err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64);
		    if (err < 0) 
		    	return err;
		    	
		    return 0;
		}
		
		static int android_alsa_prepare(snd_pcm_ioplug_t* io) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
		
			std::scoped_lock lock(self->mtx);
			
			oboe::AudioStreamBuilder mStreamBuilder;
			mStreamBuilder.setSampleRate(io->rate);
			mStreamBuilder.setChannelCount(io->channels);
			mStreamBuilder.setFormat(toOboeFormat(io->format));
			mStreamBuilder.setAudioApi(oboe::AudioApi::AAudio);
			mStreamBuilder.setSharingMode(oboe::SharingMode::Shared);
			mStreamBuilder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
			mStreamBuilder.setChannelConversionAllowed(true);
			mStreamBuilder.setFormatConversionAllowed(true);
			
			oboe::Result result = mStreamBuilder.openStream(self->mStream);
			if (result != oboe::Result::OK) {
				printf("Failed to prepare stream, result %d", result);
				return -1;
			}

			self->mStream->setBufferSizeInFrames(io->buffer_size);

			if (self->mStream->getBufferSizeInFrames() != io->buffer_size) {
				printf("Buffer size mismatch, recovering stream");
				self->mStream.reset();
				return -1;
			}
			
			return 0;
		}

		static snd_pcm_sframes_t android_alsa_transfer(snd_pcm_ioplug_t* io, const snd_pcm_channel_area_t* areas, snd_pcm_uframes_t offset, snd_pcm_uframes_t size) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return -EBADFD;

			if (size == 0)
				return 0;

			std::scoped_lock lock(self->mtx);

			auto state = self->mStream->getState();
			if (state == oboe::StreamState::Disconnected ||
				state == oboe::StreamState::Closing ||
				state == oboe::StreamState::Uninitialized ||
				state == oboe::StreamState::Unknown)
			{
				printf("Detected abnormal stream state, %d. Recreating stream", state);    
				return -EPIPE;
			}
				
			if (state != oboe::StreamState::Started) 
			{
				auto result = self->mStream->start();
				if (result != oboe::Result::OK) {
					printf("Failed to start stream, state %d result %d", state, result);
					return -1;
				}
			}

			/*
			printf("size=%lu buffer_size=%lu offset=%lu",
			       (unsigned long)size, (unsigned long)io->buffer_size, (unsigned long)offset);
			*/
			uint8_t *data = static_cast<uint8_t *>(areas[0].addr) + (areas->first + areas->step * offset) / 8;
			if (!data)
				return 0;

			if (io->format == SND_PCM_FORMAT_U8)
				data = self->convertU8ToI16(io, data, size);
			
			auto framesWritten = self->mStream->write(data, size, self->io.nonblock ? 0 : INT64_MAX);
			if (framesWritten.error() != oboe::Result::OK) {
				printf("Failed to write audio frames, result %d", framesWritten.error());
				return oboe_to_alsa_error(framesWritten.error());
			}
			/*
			printf("write result=%d state=%d value=%d", framesWritten.error(), (int)self->mStream->getState(), framesWritten.value());
			*/
			return framesWritten.value() < size ? framesWritten.value() : size;
		}

		static int android_alsa_drain(snd_pcm_ioplug_t* io) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return -EBADFD;

			std::scoped_lock lock(self->mtx);

			if (self->mStream->getState() == oboe::StreamState::Flushed)
				return 0;
			
			auto result = self->mStream->flush();
			if (result != oboe::Result::OK) {
				printf("Failed to flush stream, result %d", result);
				return -1;
			}

			return 0;
		}

		static int android_alsa_start(snd_pcm_ioplug_t* io) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return -EBADFD;

			std::scoped_lock lock(self->mtx);

			if (self->mStream->getState() == oboe::StreamState::Started)
				return 0;
			
			auto result = self->mStream->start();
			if (result != oboe::Result::OK) {
				printf("Failed to start stream, result %d", result);
			    return -1;
			}

			return 0;
		}

		static int android_alsa_pause(snd_pcm_ioplug_t* io, int enabled) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return -EBADFD;

			std::scoped_lock lock(self->mtx);

			if (self->mStream->getState() == oboe::StreamState::Paused)
				return 0;
			
			auto result = self->mStream->pause();
			if (result != oboe::Result::OK) {
				printf("Failed to pause stream, result %d", result);
			    return -1;
			}

			return 0;
		}

		static int android_alsa_stop(snd_pcm_ioplug_t* io) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return -EBADFD;

			std::scoped_lock lock(self->mtx);

			if (self->mStream->getState() == oboe::StreamState::Stopped)
				return 0;
			
			auto result = self->mStream->stop();
			if (result != oboe::Result::OK) {
				printf("Failed to stop stream, result %d", result);
			    return -1;
			}

			return 0;
		}

		static int android_alsa_close(snd_pcm_ioplug_t* io) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return 0;

			std::scoped_lock l(self->mtx);
			self->mStream.reset();
			
			return 0;
		}

		static snd_pcm_sframes_t android_alsa_pointer(snd_pcm_ioplug_t* io) 
		{
			auto self = static_cast<AndroidALSA *>(io->private_data);
			
			if (!self->mStream)
				return -EBADFD;

			std::scoped_lock lock(self->mtx);
		
			//printf("read=%lld written=%lld state=%d", (long long)self->mStream->getFramesRead(), (long long)self->mStream->getFramesWritten(), (int)self->mStream->getState());
			
			return (snd_pcm_sframes_t)(self->mStream->getFramesRead() % self->io.buffer_size);
		}

		constexpr static snd_pcm_ioplug_callback_t android_alsa_callback = {
		    .close = android_alsa_close,
		    .start = android_alsa_start,
		    .stop = android_alsa_stop,
		    .pause = android_alsa_pause,
		    .resume = android_alsa_start,
		    .prepare = android_alsa_prepare,
		    .transfer = android_alsa_transfer,
		    .drain = android_alsa_drain,
		    .pointer = android_alsa_pointer,
		};

	public:
		AndroidALSA() = default;
		
		~AndroidALSA() {
			std::scoped_lock lock(mtx);
			mStream.reset();
		}
	
		int create (snd_pcm_t** pcmp, const char* name, snd_pcm_stream_t stream, int mode) 
		{
			int res;
				
			printf("Creating Android ALSA plugin");
				
			if (stream != SND_PCM_STREAM_PLAYBACK)
				return -ENOTSUP;
				
			res = snd_pcm_ioplug_create(&io, name, stream, mode);
			if (res < 0) {
				printf("Failed to create ioplugin, result %d ", res);
				return res;
			}
				
			res = android_alsa_set_hw_constraint(&io);
			if (res < 0) {
				printf("Failed to set hardware constraint, result %d", res);
				snd_pcm_ioplug_delete(&io);
				return res;
			}
				
			*pcmp = io.pcm;
				
			printf("Android ALSA plugin created successfully");
				
			return 0;
		}
};

extern "C" {
SND_PCM_PLUGIN_DEFINE_FUNC(android_alsa) {
    snd_config_iterator_t i, next;
    
    snd_config_for_each(i, next, conf) {
        snd_config_t* n = snd_config_iterator_entry(i);
        const char* id;
        
        if (snd_config_get_id(n, &id) < 0) continue;
        if (strcmp(id, "type") == 0 || strcmp(id, "hint") == 0) continue;
        
        return -EINVAL;
    }

    AndroidALSA *androidAlsa = new AndroidALSA();
    if (!androidAlsa) {
    	printf("Failed to allocate AndroidALSA object");
     	return -ENOMEM;
    }
    
    int err = androidAlsa->create(pcmp, name, stream, mode);
    return err;
}

SND_PCM_PLUGIN_SYMBOL(android_alsa);
}
