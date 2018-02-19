//*****************************************************************************
//
//	Copyright 2015 Microsoft Corporation
//
//	Licensed under the Apache License, Version 2.0 (the "License");
//	you may not use this file except in compliance with the License.
//	You may obtain a copy of the License at
//
//	http ://www.apache.org/licenses/LICENSE-2.0
//
//	Unless required by applicable law or agreed to in writing, software
//	distributed under the License is distributed on an "AS IS" BASIS,
//	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//	See the License for the specific language governing permissions and
//	limitations under the License.
//
//*****************************************************************************

#include "pch.h"
#include "FFmpegInteropMSS.h"
#include "UncompressedAudioSampleProvider.h"
#include "UncompressedVideoSampleProvider.h"
#include "CritSec.h"

using namespace concurrency;
using namespace FFmpegInterop;
using namespace Platform;
using namespace Windows::Storage::Streams;
using namespace Windows::Media::MediaProperties;

// Static functions passed to FFmpeg
static int FileStreamRead(void* ptr, uint8_t* buf, int bufSize);
static int64_t FileStreamSeek(void* ptr, int64_t pos, int whence);
static int lock_manager(void **mtx, enum AVLockOp op);

// Flag for ffmpeg global setup
static bool isRegistered = false;

// Initialize an FFmpegInteropObject
FFmpegInteropMSS::FFmpegInteropMSS(FFmpegInteropConfig^ interopConfig)
	: config(interopConfig)
	, audioStreamIndex(AVERROR_STREAM_NOT_FOUND)
	, videoStreamIndex(AVERROR_STREAM_NOT_FOUND)
	, isFirstSeek(true)
{
	if (!isRegistered)
	{
		av_register_all();
		av_lockmgr_register(lock_manager);
		isRegistered = true;
	}
}

FFmpegInteropMSS::~FFmpegInteropMSS()
{
	mutexGuard.lock();
	if (mss)
	{
		mss->Starting -= startingRequestedToken;
		mss->SampleRequested -= sampleRequestedToken;
		mss = nullptr;
	}

	// Clear our data
	audioSampleProvider = nullptr;
	videoSampleProvider = nullptr;

	if (m_pReader != nullptr)
	{
		m_pReader->SetAudioStream(AVERROR_STREAM_NOT_FOUND, nullptr);
		m_pReader->SetVideoStream(AVERROR_STREAM_NOT_FOUND, nullptr);
		m_pReader = nullptr;
	}

	avcodec_close(avVideoCodecCtx);
	avcodec_close(avAudioCodecCtx);
	avformat_close_input(&avFormatCtx);
	av_free(avIOCtx);
	av_dict_free(&avDict);
	
	if (fileStreamData != nullptr)
	{
		fileStreamData->Release();
	}
	mutexGuard.unlock();
}

FFmpegInteropMSS^ FFmpegInteropMSS::CreateFromStream(IRandomAccessStream^ stream, FFmpegInteropConfig^ config, MediaStreamSource^ mss)
{
	auto interopMSS = ref new FFmpegInteropMSS(config);
	auto hr = interopMSS->CreateMediaStreamSource(stream, mss);
	if (!SUCCEEDED(hr))
	{
		throw ref new Exception(hr, "Failed to open media.");
	}
	return interopMSS;
}

FFmpegInteropMSS^ FFmpegInteropMSS::CreateFromUri(String^ uri, FFmpegInteropConfig^ config)
{
	auto interopMSS = ref new FFmpegInteropMSS(config);
	auto hr = interopMSS->CreateMediaStreamSource(uri);
	if (!SUCCEEDED(hr))
	{
		throw ref new Exception(hr, "Failed to open media.");
	}
	return interopMSS;
}

MediaStreamSource^ FFmpegInteropMSS::GetMediaStreamSource()
{
	return mss;
}

HRESULT FFmpegInteropMSS::CreateMediaStreamSource(String^ uri)
{
	HRESULT hr = S_OK;
	const char* charStr = nullptr;
	if (!uri)
	{
		hr = E_INVALIDARG;
	}

	if (SUCCEEDED(hr))
	{
		avFormatCtx = avformat_alloc_context();
		if (avFormatCtx == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr))
	{
		// Populate AVDictionary avDict based on PropertySet ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
		hr = ParseOptions(config->FFmpegOptions);
	}

	if (SUCCEEDED(hr))
	{
		std::wstring uriW(uri->Begin());
		std::string uriA(uriW.begin(), uriW.end());
		charStr = uriA.c_str();

		// Open media in the given URI using the specified options
		if (avformat_open_input(&avFormatCtx, charStr, NULL, &avDict) < 0)
		{
			hr = E_FAIL; // Error opening file
		}

		// avDict is not NULL only when there is an issue with the given ffmpegOptions such as invalid key, value type etc. Iterate through it to see which one is causing the issue.
		if (avDict != nullptr)
		{
			DebugMessage(L"Invalid FFmpeg option(s)");
			av_dict_free(&avDict);
			avDict = nullptr;
		}
	}

	if (SUCCEEDED(hr))
	{
		this->mss = nullptr;
		hr = InitFFmpegContext();
	}

	return hr;
}

HRESULT FFmpegInteropMSS::CreateMediaStreamSource(IRandomAccessStream^ stream, MediaStreamSource^ mss)
{
	HRESULT hr = S_OK;
	if (!stream)
	{
		hr = E_INVALIDARG;
	}

	if (SUCCEEDED(hr))
	{
		// Convert asynchronous IRandomAccessStream to synchronous IStream. This API requires shcore.h and shcore.lib
		hr = CreateStreamOverRandomAccessStream(reinterpret_cast<IUnknown*>(stream), IID_PPV_ARGS(&fileStreamData));
	}

	if (SUCCEEDED(hr))
	{
		// Setup FFmpeg custom IO to access file as stream. This is necessary when accessing any file outside of app installation directory and appdata folder.
		// Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
		fileStreamBuffer = (unsigned char*)av_malloc(config->StreamBufferSize);
		if (fileStreamBuffer == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr))
	{
		avIOCtx = avio_alloc_context(fileStreamBuffer, config->StreamBufferSize, 0, fileStreamData, FileStreamRead, 0, FileStreamSeek);
		if (avIOCtx == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr))
	{
		avFormatCtx = avformat_alloc_context();
		if (avFormatCtx == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr))
	{
		// Populate AVDictionary avDict based on PropertySet ffmpegOptions. List of options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
		hr = ParseOptions(config->FFmpegOptions);
	}

	if (SUCCEEDED(hr))
	{
		avFormatCtx->pb = avIOCtx;
		avFormatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

		// Open media file using custom IO setup above instead of using file name. Opening a file using file name will invoke fopen C API call that only have
		// access within the app installation directory and appdata folder. Custom IO allows access to file selected using FilePicker dialog.
		if (avformat_open_input(&avFormatCtx, "", NULL, &avDict) < 0)
		{
			hr = E_FAIL; // Error opening file
		}

		// avDict is not NULL only when there is an issue with the given ffmpegOptions such as invalid key, value type etc. Iterate through it to see which one is causing the issue.
		if (avDict != nullptr)
		{
			DebugMessage(L"Invalid FFmpeg option(s)");
			av_dict_free(&avDict);
			avDict = nullptr;
		}
	}

	if (SUCCEEDED(hr))
	{
		this->mss = mss;
		hr = InitFFmpegContext();
	}

	return hr;
}

static int is_hwaccel_pix_fmt(enum AVPixelFormat pix_fmt)
{
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
	return desc->flags & AV_PIX_FMT_FLAG_HWACCEL;
}

static AVPixelFormat get_format(struct AVCodecContext *s, const enum AVPixelFormat *fmt)
{
	AVPixelFormat result = (AVPixelFormat)-1;
	AVPixelFormat format;
	int index = 0;
	do
	{
		format = fmt[index++];
		if (format != -1 && result == -1 && !is_hwaccel_pix_fmt(format))
		{
			// take first non hw accelerated format
			result = format;
		}
		else if (format == AV_PIX_FMT_NV12 && result != AV_PIX_FMT_YUVA420P)
		{
			// switch to NV12 if available, unless this is an alpha channel file
			result = format;
		}
	} while (format != -1);
	return result;
}

HRESULT FFmpegInteropMSS::InitFFmpegContext()
{
	HRESULT hr = S_OK;

	if (SUCCEEDED(hr))
	{
		if (avformat_find_stream_info(avFormatCtx, NULL) < 0)
		{
			hr = E_FAIL; // Error finding info
		}
	}

	if (SUCCEEDED(hr))
	{
		m_pReader = ref new FFmpegReader(avFormatCtx);
		if (m_pReader == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr) && !config->IsFrameGrabber)
	{
		// Find the audio stream and its decoder
		AVCodec* avAudioCodec = nullptr;
		audioStreamIndex = av_find_best_stream(avFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &avAudioCodec, 0);
		if (audioStreamIndex != AVERROR_STREAM_NOT_FOUND && avAudioCodec)
		{
			// allocate a new decoding context
			avAudioCodecCtx = avcodec_alloc_context3(avAudioCodec);
			if (!avAudioCodecCtx)
			{
				hr = E_OUTOFMEMORY;
				DebugMessage(L"Could not allocate a decoding context\n");
				avformat_close_input(&avFormatCtx);
			}

			if (SUCCEEDED(hr))
			{
				// initialize the stream parameters with demuxer information
				if (avcodec_parameters_to_context(avAudioCodecCtx, avFormatCtx->streams[audioStreamIndex]->codecpar) < 0)
				{
					hr = E_FAIL;
					avformat_close_input(&avFormatCtx);
					avcodec_free_context(&avAudioCodecCtx);
				}

				if (SUCCEEDED(hr))
				{
					if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_S16P)
					{
						avAudioCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_S16;
					}
					else if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_S32P)
					{
						avAudioCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_S32;
					}
					else if (avAudioCodecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP)
					{
						avAudioCodecCtx->request_sample_fmt = AV_SAMPLE_FMT_FLT;
					}

					// enable multi threading
					unsigned threads = std::thread::hardware_concurrency();
					if (threads > 0)
					{
						avAudioCodecCtx->thread_count = config->MaxAudioThreads == 0 ? threads : min(threads, config->MaxAudioThreads);
						avAudioCodecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
					}

					if (avcodec_open2(avAudioCodecCtx, avAudioCodec, NULL) < 0)
					{
						avAudioCodecCtx = nullptr;
						hr = E_FAIL;
					}
					else
					{
						// Detect audio format and create audio stream descriptor accordingly
						hr = CreateAudioStreamDescriptor();
						if (SUCCEEDED(hr))
						{
							if (SUCCEEDED(hr))
							{
								m_pReader->SetAudioStream(audioStreamIndex, audioSampleProvider);
							}
						}

						if (SUCCEEDED(hr))
						{
							// Convert audio codec name for property
							hr = ConvertCodecName(avAudioCodec->name, &audioCodecName);
						}
					}
				}
			}
		}
	}

	if (SUCCEEDED(hr))
	{
		// Find the video stream and its decoder
		AVCodec* avVideoCodec = nullptr;
		videoStreamIndex = av_find_best_stream(avFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &avVideoCodec, 0);
		if (videoStreamIndex != AVERROR_STREAM_NOT_FOUND && avVideoCodec)
		{
			// FFmpeg identifies album/cover art from a music file as a video stream
			// Avoid creating unnecessarily video stream from this album/cover art
			if (avFormatCtx->streams[videoStreamIndex]->disposition == AV_DISPOSITION_ATTACHED_PIC)
			{
				videoStreamIndex = AVERROR_STREAM_NOT_FOUND;
				avVideoCodec = nullptr;
			}
			else
			{
				// allocate a new decoding context
				avVideoCodecCtx = avcodec_alloc_context3(avVideoCodec);
				if (!avVideoCodecCtx)
				{
					DebugMessage(L"Could not allocate a decoding context\n");
					avformat_close_input(&avFormatCtx);
					hr = E_OUTOFMEMORY;
				}

				if (SUCCEEDED(hr))
				{
					avVideoCodecCtx->get_format = &get_format;

					// initialize the stream parameters with demuxer information
					if (avcodec_parameters_to_context(avVideoCodecCtx, avFormatCtx->streams[videoStreamIndex]->codecpar) < 0)
					{
						avformat_close_input(&avFormatCtx);
						avcodec_free_context(&avVideoCodecCtx);
						hr = E_FAIL;
					}
				}

				if (SUCCEEDED(hr))
				{
					// enable multi threading
					unsigned threads = std::thread::hardware_concurrency();
					if (threads > 0)
					{
						avVideoCodecCtx->thread_count = config->MaxVideoThreads == 0 ? threads : min(threads, config->MaxVideoThreads);
						avVideoCodecCtx->thread_type = config->IsFrameGrabber ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
					}

					if (avcodec_open2(avVideoCodecCtx, avVideoCodec, NULL) < 0)
					{
						avVideoCodecCtx = nullptr;
						hr = E_FAIL; // Cannot open the video codec
					}
					else
					{
						// Detect video format and create video stream descriptor accordingly
						hr = CreateVideoStreamDescriptor();
						if (SUCCEEDED(hr))
						{
							if (SUCCEEDED(hr))
							{
								m_pReader->SetVideoStream(videoStreamIndex, videoSampleProvider);
							}
						}

						if (SUCCEEDED(hr))
						{
							// Convert video codec name for property
							hr = ConvertCodecName(avVideoCodec->name, &videoCodecName);
						}
					}
				}
			}
		}
	}

	if (SUCCEEDED(hr))
	{
		// Convert media duration from AV_TIME_BASE to TimeSpan unit
		mediaDuration = { LONGLONG(avFormatCtx->duration * 10000000 / double(AV_TIME_BASE)) };

		if (audioStreamDescriptor)
		{
			if (videoStreamDescriptor)
			{
				if (mss)
				{
					mss->AddStreamDescriptor(videoStreamDescriptor);
					mss->AddStreamDescriptor(audioStreamDescriptor);
				}
				else
				{
					mss = ref new MediaStreamSource(videoStreamDescriptor, audioStreamDescriptor);
				}
			}
			else
			{
				if (mss)
				{
					mss->AddStreamDescriptor(audioStreamDescriptor);
				}
				else
				{
					mss = ref new MediaStreamSource(audioStreamDescriptor);
				}
			}
		}
		else if (videoStreamDescriptor)
		{
			if (mss)
			{
				mss->AddStreamDescriptor(videoStreamDescriptor);
			}
			else
			{
				mss = ref new MediaStreamSource(videoStreamDescriptor);
			}
		}
		if (mss)
		{
			mss->BufferTime = { 0 };
			if (mediaDuration.Duration > 0)
			{
				mss->Duration = mediaDuration;
				mss->CanSeek = true;
			}

			startingRequestedToken = mss->Starting += ref new TypedEventHandler<MediaStreamSource ^, MediaStreamSourceStartingEventArgs ^>(this, &FFmpegInteropMSS::OnStarting);
			sampleRequestedToken = mss->SampleRequested += ref new TypedEventHandler<MediaStreamSource ^, MediaStreamSourceSampleRequestedEventArgs ^>(this, &FFmpegInteropMSS::OnSampleRequested);
		}
		else
		{
			hr = E_OUTOFMEMORY;
		}
	}

	return hr;
}

HRESULT FFmpegInteropMSS::ConvertCodecName(const char* codecName, String^ *outputCodecName)
{
	HRESULT hr = S_OK;

	// Convert codec name from const char* to Platform::String
	auto codecNameChars = codecName;
	size_t newsize = strlen(codecNameChars) + 1;
	wchar_t * wcstring = new(std::nothrow) wchar_t[newsize];
	if (wcstring == nullptr)
	{
		hr = E_OUTOFMEMORY;
	}

	if (SUCCEEDED(hr))
	{
		size_t convertedChars = 0;
		mbstowcs_s(&convertedChars, wcstring, newsize, codecNameChars, _TRUNCATE);
		*outputCodecName = ref new Platform::String(wcstring);
		delete[] wcstring;
	}

	return hr;
}

HRESULT FFmpegInteropMSS::CreateAudioStreamDescriptor()
{
	audioSampleProvider = ref new UncompressedAudioSampleProvider(m_pReader, avFormatCtx, avAudioCodecCtx, config, audioStreamIndex);
	auto hr = audioSampleProvider->Initialize();

	if (SUCCEEDED(hr))
	{
		audioStreamDescriptor = dynamic_cast<AudioStreamDescriptor^>(audioSampleProvider->StreamDescriptor);

		if (audioStreamDescriptor == nullptr)
		{
			hr = E_FAIL;
		}
	}

	if (FAILED(hr))
	{
		audioSampleProvider = nullptr;
	}

	return hr;
}

HRESULT FFmpegInteropMSS::CreateVideoStreamDescriptor()
{
	videoSampleProvider = ref new UncompressedVideoSampleProvider(m_pReader, avFormatCtx, avVideoCodecCtx, config, videoStreamIndex);
	auto hr = videoSampleProvider->Initialize();

	if (SUCCEEDED(hr))
	{
		videoStreamDescriptor = dynamic_cast<VideoStreamDescriptor^>(videoSampleProvider->StreamDescriptor);

		if (videoStreamDescriptor == nullptr)
		{
			hr = E_FAIL;
		}
	}

	if (FAILED(hr))
	{
		videoSampleProvider = nullptr;
	}

	return hr;
}

HRESULT FFmpegInteropMSS::ParseOptions(PropertySet^ ffmpegOptions)
{
	HRESULT hr = S_OK;

	// Convert FFmpeg options given in PropertySet to AVDictionary. List of 
	// options can be found in https://www.ffmpeg.org/ffmpeg-protocols.html
	if (ffmpegOptions != nullptr)
	{
		for (auto option = ffmpegOptions->First(); option->HasCurrent; option->MoveNext())
		{
			String^ key = option->Current->Key;
			std::wstring keyW(key->Begin());
			std::string keyA(keyW.begin(), keyW.end());
			const char* keyChar = keyA.c_str();

			// Convert value from Object^ to const char*. avformat_open_input will internally convert value from const char* to the correct type
			String^ value = option->Current->Value->ToString();
			std::wstring valueW(value->Begin());
			std::string valueA(valueW.begin(), valueW.end());
			const char* valueChar = valueA.c_str();

			// Add key and value pair entry
			if (av_dict_set(&avDict, keyChar, valueChar, 0) < 0)
			{
				hr = E_INVALIDARG;
				break;
			}
		}
	}

	return hr;
}

void FFmpegInteropMSS::OnStarting(MediaStreamSource ^sender, MediaStreamSourceStartingEventArgs ^args)
{
	MediaStreamSourceStartingRequest^ request = args->Request;

	// Perform seek operation when MediaStreamSource received seek event from MediaElement
	if (request->StartPosition && request->StartPosition->Value.Duration <= mediaDuration.Duration && (!isFirstSeek || request->StartPosition->Value.Duration > 0))
	{
		auto hr = Seek(request->StartPosition->Value);
		if (SUCCEEDED(hr))
		{
			request->SetActualStartPosition(request->StartPosition->Value);
		}
	}

	isFirstSeek = false;
}

void FFmpegInteropMSS::OnSampleRequested(Windows::Media::Core::MediaStreamSource ^sender, MediaStreamSourceSampleRequestedEventArgs ^args)
{
	mutexGuard.lock();
	if (mss != nullptr)
	{
		if (args->Request->StreamDescriptor == audioStreamDescriptor && audioSampleProvider != nullptr)
		{
			args->Request->Sample = audioSampleProvider->GetNextSample();
		}
		else if (args->Request->StreamDescriptor == videoStreamDescriptor && videoSampleProvider != nullptr)
		{
			args->Request->Sample = videoSampleProvider->GetNextSample();
		}
		else
		{
			args->Request->Sample = nullptr;
		}
	}
	mutexGuard.unlock();
}

HRESULT FFmpegInteropMSS::Seek(TimeSpan position)
{
	auto hr = S_OK;

	// Select the first valid stream either from video or audio
	int streamIndex = videoStreamIndex >= 0 ? videoStreamIndex : audioStreamIndex >= 0 ? audioStreamIndex : -1;

	if (streamIndex >= 0)
	{
		// Compensate for file start_time, then convert to stream time_base
		auto correctedPosition = position.Duration + (avFormatCtx->start_time * 10); 
		int64_t seekTarget = static_cast<int64_t>(correctedPosition / (av_q2d(avFormatCtx->streams[streamIndex]->time_base) * 10000000));

		if (av_seek_frame(avFormatCtx, streamIndex, seekTarget, AVSEEK_FLAG_BACKWARD) < 0)
		{
			hr = E_FAIL;
			DebugMessage(L" - ### Error while seeking\n");
		}
		else
		{
			// Flush the AudioSampleProvider
			if (audioSampleProvider != nullptr)
			{
				audioSampleProvider->Flush();
				avcodec_flush_buffers(avAudioCodecCtx);
			}

			// Flush the VideoSampleProvider
			if (videoSampleProvider != nullptr)
			{
				videoSampleProvider->Flush();
				avcodec_flush_buffers(avVideoCodecCtx);
			}
		}
	}
	else
	{
		hr = E_FAIL;
	}

	return hr;
}

// Static function to read file stream and pass data to FFmpeg. Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
static int FileStreamRead(void* ptr, uint8_t* buf, int bufSize)
{
	IStream* pStream = reinterpret_cast<IStream*>(ptr);
	ULONG bytesRead = 0;
	HRESULT hr = pStream->Read(buf, bufSize, &bytesRead);

	if (FAILED(hr))
	{
		return -1;
	}

	// If we succeed but don't have any bytes, assume end of file
	if (bytesRead == 0)
	{
		return AVERROR_EOF;  // Let FFmpeg know that we have reached eof
	}

	return bytesRead;
}

// Static function to seek in file stream. Credit to Philipp Sch http://www.codeproject.com/Tips/489450/Creating-Custom-FFmpeg-IO-Context
static int64_t FileStreamSeek(void* ptr, int64_t pos, int whence)
{
	IStream* pStream = reinterpret_cast<IStream*>(ptr);
	if (whence == AVSEEK_SIZE)
	{
		// get stream size
		STATSTG status;
		if (FAILED(pStream->Stat(&status, STATFLAG_NONAME)))
		{
			return -1;
		}
		return status.cbSize.QuadPart;
	}
	else
	{
		LARGE_INTEGER in;
		in.QuadPart = pos;
		ULARGE_INTEGER out = { 0 };

		if (FAILED(pStream->Seek(in, whence, &out)))
		{
			return -1;
		}

		return out.QuadPart; // Return the new position:
	}
}

static int lock_manager(void **mtx, enum AVLockOp op)
{
	switch (op)
	{
	case AV_LOCK_CREATE:
	{
		*mtx = new CritSec();
		return 0;
	}
	case AV_LOCK_OBTAIN:
	{
		auto mutex = static_cast<CritSec*>(*mtx);
		mutex->Lock();
		return 0;
	}
	case AV_LOCK_RELEASE:
	{
		auto mutex = static_cast<CritSec*>(*mtx);
		mutex->Unlock();
		return 0;
	}
	case AV_LOCK_DESTROY:
	{
		auto mutex = static_cast<CritSec*>(*mtx);
		delete mutex;
		return 0;
	}
	}
	return 1;
}
