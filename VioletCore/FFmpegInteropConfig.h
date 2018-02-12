#pragma once

using namespace Windows::Foundation::Collections;

namespace FFmpegInterop
{
	public ref class FFmpegInteropConfig sealed
	{
	public:
		FFmpegInteropConfig()
		{
			VideoOutputAllowIyuv = true; // false;
			VideoOutputAllowBgra8 = false;

			SkipErrors = 50;
			MaxAudioThreads = 2;
			StreamBufferSize = 16384;

			FFmpegOptions = ref new PropertySet();
		};

		property bool VideoOutputAllowIyuv;
		property bool VideoOutputAllowBgra8;
		property bool VideoOutputAllowNv12;

		property unsigned int SkipErrors;

		property unsigned int MaxVideoThreads;
		property unsigned int MaxAudioThreads;

		property unsigned int StreamBufferSize;

		property PropertySet^ FFmpegOptions;

	internal:
		property bool IsFrameGrabber;
	};
}