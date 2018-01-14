
#include <nana/audio/detail/audio_device.hpp>

#ifdef NANA_ENABLE_AUDIO

#include <nana/system/platform.hpp>

#if defined(NANA_POSIX)
	#include <pthread.h>
	#include <unistd.h>
	#include <sys/time.h>
	#include <errno.h>
#endif

namespace nana{namespace audio
{
	namespace detail
	{
#if defined(NANA_WINDOWS)
		class wave_native
		{
			typedef MMRESULT (__stdcall *out_open_t)(LPHWAVEOUT, UINT_PTR, LPWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
			typedef MMRESULT (__stdcall *out_close_t)(HWAVEOUT);
			typedef MMRESULT (__stdcall *out_op_header_t)(HWAVEOUT, LPWAVEHDR, UINT);
		public:
			out_open_t out_open;
			out_close_t out_close;
			out_op_header_t out_write;
			out_op_header_t out_prepare;
			out_op_header_t out_unprepare;

			wave_native()
			{
				HMODULE winmm = ::GetModuleHandleA("Winmm.DLL");
				if(0 == winmm)
					winmm = ::LoadLibraryA("Winmm.DLL");

				out_open = reinterpret_cast<out_open_t>(::GetProcAddress(winmm, "waveOutOpen"));
				out_close = reinterpret_cast<out_close_t>(::GetProcAddress(winmm, "waveOutClose"));
				out_write = reinterpret_cast<out_op_header_t>(::GetProcAddress(winmm, "waveOutWrite"));
				out_prepare = reinterpret_cast<out_op_header_t>(::GetProcAddress(winmm, "waveOutPrepareHeader"));
				out_unprepare = reinterpret_cast<out_op_header_t>(::GetProcAddress(winmm, "waveOutUnprepareHeader"));
			}
		}wave_native_if;
#endif
		//class audio_device
			audio_device::audio_device()
#if defined(NANA_WINDOWS)
				: handle_(nullptr), buf_prep_(nullptr)
#elif defined(NANA_LINUX)
				: handle_(nullptr), buf_prep_(nullptr)
#elif defined(NANA_POSIX)
				: handle_(-1), buf_prep_(nullptr)
#endif
			{}

			audio_device::~audio_device()
			{
				close();
			}

			bool audio_device::empty() const
			{
			    #if defined(NANA_WINDOWS) || defined(NANA_LINUX)
				return (nullptr == handle_);
				#else
				return -1 == handle_;
				#endif
			}

			bool audio_device::open(std::size_t channels, std::size_t rate, std::size_t bits_per_sample)
			{
#if defined(NANA_WINDOWS)
				close();

				WAVEFORMATEX wfx;
				wfx.wFormatTag = WAVE_FORMAT_PCM;
				wfx.nChannels = static_cast<WORD>(channels);
				wfx.nSamplesPerSec = static_cast<DWORD>(rate);
				wfx.wBitsPerSample = static_cast<WORD>(bits_per_sample);

				wfx.nBlockAlign = (wfx.wBitsPerSample >> 3 ) * wfx.nChannels;
				wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
				wfx.cbSize = 0;

				MMRESULT mmr = wave_native_if.out_open(&handle_, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(&audio_device::_m_dev_callback), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
				return (mmr == MMSYSERR_NOERROR);
#elif defined(NANA_LINUX)
				if(nullptr == handle_)
				{
					if(::snd_pcm_open(&handle_, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0)
						return false;
				}

				if(handle_)
				{
					channels_ = channels;
					rate_ = rate;
					bytes_per_sample_ = (bits_per_sample >> 3);
					bytes_per_frame_ = bytes_per_sample_ * channels;

					snd_pcm_hw_params_t * params;
					if(snd_pcm_hw_params_malloc(&params) < 0)
					{
						close();
						return false;
					}

					if(::snd_pcm_hw_params_any(handle_, params) < 0)
					{
						close();
						::snd_pcm_hw_params_free(params);
						return false;
					}

					if(::snd_pcm_hw_params_set_access(handle_, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
					{
						close();
						::snd_pcm_hw_params_free(params);
						return false;
					}

					snd_pcm_format_t format = SND_PCM_FORMAT_U8;
					switch(bits_per_sample)
					{
					case 8:
						format = SND_PCM_FORMAT_U8;	break;
					case 16:
						format = SND_PCM_FORMAT_S16_LE;	break;
					case 32:
						format = SND_PCM_FORMAT_S32_LE;	break;
					}

					if(::snd_pcm_hw_params_set_format(handle_, params, format) < 0)
					{
						close();
						::snd_pcm_hw_params_free(params);
						return false;
					}

					unsigned tmp = rate;
					if(::snd_pcm_hw_params_set_rate_near(handle_, params, &tmp, 0) < 0)
					{
						close();
						::snd_pcm_hw_params_free(params);
						return false;
					}

					if(::snd_pcm_hw_params_set_channels(handle_, params, channels) < 0)
					{
						close();
						::snd_pcm_hw_params_free(params);
						return false;
					}

					if(::snd_pcm_hw_params(handle_, params) < 0)
					{
						close();
						::snd_pcm_hw_params_free(params);
						return false;
					}

					::snd_pcm_hw_params_free(params);
					::snd_pcm_prepare(handle_);
					return true;
				}
				return false;
#elif defined(NANA_POSIX)
                // TODO: dig through output from /dev/sndstat.
                // Find the default audio device and use that.
                // Example output:
                // > Installed devices:
                // > pcm0: <Intel Haswell (HDMI/DP 8ch)> (play)
                // > pcm1: <Realtek ALC292 (Analog 2.0+HP/2.0)> (play/rec) default
                // > pcm2: <Realtek ALC292 (Internal Analog Mic)> (rec)
                // The line marked "default" above is the one we want.
                // pcm1 is the same as /dev/dsp1 (and /dev/dspW1 is the 16 bit version).
                handle_ = ::open("/dev/dspW1.0", O_WRONLY);
                if (handle_ == -1)
                {
                    return false;
                }
                int zero = 0;
                int caps = 0;
                int fragment = 0x200008L;
                ioctl(handle_, SNDCTL_DSP_COOKEDMODE, &zero);
                ioctl(handle_, SNDCTL_DSP_SETFRAGMENT, &fragment);
                ioctl(handle_, SNDCTL_DSP_GETCAPS, &caps);
                ioctl(handle_, SNDCTL_DSP_SETFMT, &bits_per_sample);
                ioctl(handle_, SNDCTL_DSP_CHANNELS, &channels);
                ioctl(handle_, SNDCTL_DSP_SPEED, &rate);
                channels_ = channels;
                rate_ = rate;
                bytes_per_sample_ = (bits_per_sample >> 3);
                bytes_per_frame_ = bytes_per_sample_ * channels;
				return true;
#endif
			}

			void audio_device::close()
			{
				if( !empty() )
				{
#if defined(NANA_WINDOWS)
					wave_native_if.out_close(handle_);
					handle_ = nullptr;
#elif defined(NANA_LINUX)
					::snd_pcm_close(handle_);
					handle_ = nullptr;
#elif defined(NANA_POSIX)
					::close(handle_);
					handle_ = -1;
#endif
				}
			}

			void audio_device::prepare(buffer_preparation & buf_prep)
			{
				buf_prep_ = & buf_prep;
			}

			void audio_device::write(buffer_preparation::meta * m)
			{
#if defined(NANA_WINDOWS)
				std::lock_guard<decltype(queue_lock_)> lock(queue_lock_);
				done_queue_.emplace_back(m);
				if(m->dwFlags & WHDR_PREPARED)
					wave_native_if.out_unprepare(handle_, m, sizeof(WAVEHDR));

				wave_native_if.out_prepare(handle_, m, sizeof(WAVEHDR));
				wave_native_if.out_write(handle_, m, sizeof(WAVEHDR));
#elif defined(NANA_LINUX)
				std::size_t frames = m->bufsize / bytes_per_frame_;
				std::size_t buffered = 0; //in bytes
				while(frames > 0)
				{
					int err = ::snd_pcm_writei(handle_, m->buf + buffered, frames);
					if(err > 0)
					{
						frames -= err;
						buffered += err * bytes_per_frame_;
					}
					else if(-EPIPE == err)
						::snd_pcm_prepare(handle_);
				}
				buf_prep_->revert(m);
#elif defined(NANA_POSIX)
                // consider moving this to a background thread.
                // currently this blocks calling thread.
                ::write(handle_, m->buf, m->bufsize);
				buf_prep_->revert(m);
#endif
			}

			void audio_device::wait_for_drain() const
			{
#if defined(NANA_WINDOWS)
				while(buf_prep_->data_finished() == false)
					nana::system::sleep(200);
#elif defined(NANA_LINUX)
				while(::snd_pcm_state(handle_) == SND_PCM_STATE_RUNNING)
					nana::system::sleep(200);
#endif
			}

#if defined(NANA_WINDOWS)
			void __stdcall audio_device::_m_dev_callback(HWAVEOUT handle, UINT msg, audio_device * self, DWORD_PTR, DWORD_PTR)
			{
				if(WOM_DONE == msg)
				{
					buffer_preparation::meta * m;
					{
						std::lock_guard<decltype(queue_lock_)> lock(self->queue_lock_);
						m = self->done_queue_.front();
						self->done_queue_.erase(self->done_queue_.begin());
					}
					wave_native_if.out_unprepare(handle, m, sizeof(WAVEHDR));
					self->buf_prep_->revert(m);
				}
			}
#endif
		//end class audio_device
	}//end namespace detail
}//end namespace audio
}//end namespace nana

#endif //NANA_ENABLE_AUDIO
