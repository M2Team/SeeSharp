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
#include "MediaSampleProvider.h"
#include "FFmpegInteropMSS.h"
#include "FFmpegReader.h"

using namespace FFmpegInterop;

MediaSampleProvider::MediaSampleProvider(
	FFmpegReader^ reader,
	AVFormatContext* avFormatCtx,
	AVCodecContext* avCodecCtx,
	FFmpegInteropConfig^ config,
	int streamIndex)
	: m_pReader(reader)
	, m_pAvFormatCtx(avFormatCtx)
	, m_pAvCodecCtx(avCodecCtx)
	, m_pAvStream(m_pAvFormatCtx->streams[m_streamIndex])
	, m_config(config)
	, m_streamIndex(streamIndex)
{
	DebugMessage(L"MediaSampleProvider\n");

	if (m_pAvFormatCtx->start_time != 0)
	{
		auto streamStartTime = (long long)(av_q2d(m_pAvFormatCtx->streams[m_streamIndex]->time_base) * m_pAvFormatCtx->streams[m_streamIndex]->start_time * 1000000);

		if (m_pAvFormatCtx->start_time == streamStartTime)
		{
			// calculate more precise start time
			m_startOffset = (long long)(av_q2d(m_pAvFormatCtx->streams[m_streamIndex]->time_base) * m_pAvFormatCtx->streams[m_streamIndex]->start_time * 10000000);
		}
		else
		{
			m_startOffset = m_pAvFormatCtx->start_time * 10;
		}
	}
}

HRESULT FFmpegInterop::MediaSampleProvider::Initialize()
{	
	this->m_streamDescriptor = this->CreateStreamDescriptor();
	if (nullptr == this->m_streamDescriptor)
		return E_FAIL;

	if (m_streamDescriptor)
	{
		// unfortunately, setting Name or Language on MediaStreamDescriptor does not have any effect, they are not shown in track selection list
		auto title = av_dict_get(m_pAvStream->metadata, "title", NULL, 0);
		if (title)
		{
			Name = ConvertString(title->value);
		}

		auto language = av_dict_get(m_pAvStream->metadata, "language", NULL, 0);
		if (language)
		{
			Language = ConvertString(language->value);
		}

		auto codec = m_pAvCodecCtx->codec_descriptor->name;
		if (codec)
		{
			CodecName = ConvertString(codec);
		}
	}

	return this->AllocateResources();
}

HRESULT FFmpegInterop::MediaSampleProvider::AllocateResources()
{
	DebugMessage(L"AllocateResources\n");
	return S_OK;
}

MediaSampleProvider::~MediaSampleProvider()
{
	DebugMessage(L"~MediaSampleProvider\n");

	avcodec_close(m_pAvCodecCtx);
	avcodec_free_context(&m_pAvCodecCtx);
}

MediaStreamSample^ MediaSampleProvider::GetNextSample()
{
	DebugMessage(L"GetNextSample\n");

	HRESULT hr = S_OK;

	MediaStreamSample^ sample;
	if (m_isEnabled)
	{
		IBuffer^ buffer = nullptr;
		LONGLONG pts = 0;
		LONGLONG dur = 0;

		hr = CreateNextSampleBuffer(&buffer, pts, dur);

		if (hr == S_OK)
		{
			pts = LONGLONG(av_q2d(m_pAvFormatCtx->streams[m_streamIndex]->time_base) * 10000000 * pts) - m_startOffset;
			dur = LONGLONG(av_q2d(m_pAvFormatCtx->streams[m_streamIndex]->time_base) * 10000000 * dur);

			sample = MediaStreamSample::CreateFromBuffer(buffer, { pts });
			sample->Duration = { dur };
			sample->Discontinuous = m_isDiscontinuous;

			hr = SetSampleProperties(sample);

			m_isDiscontinuous = false;
		}
		else if (hr == S_FALSE)
		{
			DebugMessage(L"End of stream reached.\n");
			DisableStream();
		}
		else
		{
			DebugMessage(L"Error reading next packet.\n");
			DisableStream();
		}
	}

	return sample;
}

HRESULT MediaSampleProvider::GetNextPacket(AVPacket** avPacket, LONGLONG & packetPts, LONGLONG & packetDuration)
{
	HRESULT hr = S_OK;

	// Continue reading until there is an appropriate packet in the stream
	while (m_packetQueue.empty())
	{
		if (m_pReader->ReadPacket() < 0)
		{
			DebugMessage(L"GetNextSample reaching EOF\n");
			break;
		}
	}

	if (!m_packetQueue.empty())
	{
		// read next packet and set pts values
		auto packet = PopPacket();
		*avPacket = packet;

		packetDuration = packet->duration;
		if (packet->pts != AV_NOPTS_VALUE)
		{
			packetPts = packet->pts;
			// Set the PTS for the next sample if it doesn't one.
			m_nextPacketPts = packetPts + packetDuration;
		}
		else
		{
			packetPts = m_nextPacketPts;
			// Set the PTS for the next sample if it doesn't one.
			m_nextPacketPts += packetDuration;
		}
	}
	else
	{
		hr = S_FALSE;
	}

	return hr;
}


void MediaSampleProvider::QueuePacket(AVPacket *packet)
{
	DebugMessage(L" - QueuePacket\n");

	if (m_isEnabled)
	{
		m_packetQueue.push(packet);
	}
	else
	{
		av_packet_free(&packet);
	}
}

AVPacket* MediaSampleProvider::PopPacket()
{
	DebugMessage(L" - PopPacket\n");
	AVPacket* result = NULL;

	if (!m_packetQueue.empty())
	{
		result = m_packetQueue.front();
		m_packetQueue.pop();
	}

	return result;
}

void MediaSampleProvider::Flush()
{
	DebugMessage(L"Flush\n");
	while (!m_packetQueue.empty())
	{
		AVPacket *avPacket = PopPacket();
		av_packet_free(&avPacket);
	}
	avcodec_flush_buffers(m_pAvCodecCtx);
	m_isDiscontinuous = true;
}

void MediaSampleProvider::EnableStream()
{
	DebugMessage(L"EnableStream\n");
	m_isEnabled = true;
}

void MediaSampleProvider::DisableStream()
{
	DebugMessage(L"DisableStream\n");
	Flush();
	m_isEnabled = false;
}

void MediaSampleProvider::SetCommonVideoEncodingProperties(VideoEncodingProperties^ videoEncodingProperties)
{
	AVDictionaryEntry *rotate_tag = av_dict_get(m_pAvStream->metadata, "rotate", nullptr, 0);
	if (nullptr != rotate_tag)
	{
		videoEncodingProperties->Properties->Insert(
			Guid(MF_MT_VIDEO_ROTATION),
			ref new Box<uint32>(atoi(rotate_tag->value)));
	}

	// Detect the correct framerate
	if (m_pAvCodecCtx->framerate.num != 0 || m_pAvCodecCtx->framerate.den != 1)
	{
		videoEncodingProperties->FrameRate->Numerator = m_pAvCodecCtx->framerate.num;
		videoEncodingProperties->FrameRate->Denominator = m_pAvCodecCtx->framerate.den;
	}
	else if (m_pAvStream->avg_frame_rate.num != 0 || m_pAvStream->avg_frame_rate.den != 0)
	{
		videoEncodingProperties->FrameRate->Numerator = m_pAvStream->avg_frame_rate.num;
		videoEncodingProperties->FrameRate->Denominator = m_pAvStream->avg_frame_rate.den;
	}

	videoEncodingProperties->Bitrate = static_cast<unsigned int>(m_pAvCodecCtx->bit_rate);
}

Platform::String^ ConvertString(const char* charString)
{
	return M2MakeCXString(M2MakeUTF16String(std::string(charString)));
}

void free_buffer(void *lpVoid)
{
	auto buffer = (AVBufferRef *)lpVoid;
	av_buffer_unref(&buffer);
}