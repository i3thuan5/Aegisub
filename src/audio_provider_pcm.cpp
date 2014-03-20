// Copyright (c) 2007-2008, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file audio_provider_pcm.cpp
/// @brief PCM WAV and WAV64 audio provider
/// @ingroup audio_input
///

#include "config.h"

#include "audio_provider_pcm.h"

#include "audio_controller.h"
#include "utils.h"

#include <libaegisub/file_mapping.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/util.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <cassert>
#include <cstdint>

using namespace boost::interprocess;

PCMAudioProvider::PCMAudioProvider(agi::fs::path const& filename)
: file(agi::util::make_unique<agi::file_mapping>(filename, read_only))
{
	float_samples = false;

	try {
		file_size = agi::fs::Size(filename);
	}
	catch (agi::Exception const& e) {
		throw agi::AudioPlayerOpenError("Could not get file size", e.Copy());
	}
}

PCMAudioProvider::~PCMAudioProvider() { }

char *PCMAudioProvider::EnsureRangeAccessible(int64_t start, int64_t length) const {
	if (start + length > file_size)
		throw AudioDecodeError("Attempted to map beyond end of file");

	// Check if we can just use the current mapping
	if (region && start >= mapping_start && start + length <= mapping_start + region->get_size())
		return static_cast<char *>(region->get_address()) + start - mapping_start;

	if (sizeof(size_t) == 4) {
		mapping_start = start & ~0xFFFFFULL; // Align to 1 MB bondary
		length += static_cast<size_t>(start - mapping_start);
		// Map 16 MB or length rounded up to the next MB
		length = std::min<size_t>(std::max<size_t>(0x1000000U, (length + 0xFFFFF) & ~0xFFFFF), file_size - mapping_start);
	}
	else {
		// Just map the whole file
		mapping_start = 0;
		length = file_size;
	}

	try {
		region = agi::util::make_unique<mapped_region>(*file, read_only, mapping_start, length);
	}
	catch (interprocess_exception const&) {
		throw AudioDecodeError("Failed mapping a view of the file");
	}

	return static_cast<char *>(region->get_address()) + start - mapping_start;
}

void PCMAudioProvider::FillBuffer(void *buf, int64_t start, int64_t count) const {
	// Read blocks from the file
	size_t index = 0;
	while (count > 0 && index < index_points.size()) {
		// Check if this index contains the samples we're looking for
		const IndexPoint &ip = index_points[index];
		if (ip.start_sample <= start && ip.start_sample+ip.num_samples > start) {

			// How many samples we can maximum take from this block
			int64_t samples_can_do = ip.num_samples - start + ip.start_sample;
			if (samples_can_do > count) samples_can_do = count;

			// Read as many samples we can
			char *src = EnsureRangeAccessible(
				ip.start_byte + (start - ip.start_sample) * bytes_per_sample * channels,
				samples_can_do * bytes_per_sample * channels);
			memcpy(buf, src, samples_can_do * bytes_per_sample * channels);

			// Update data
			buf = (char*)buf + samples_can_do * bytes_per_sample * channels;
			start += samples_can_do;
			count -= samples_can_do;
		}
		index++;
	}
}

/// @class RiffWavPCMAudioProvider
/// @brief RIFF WAV PCM provider
///
/// Overview of RIFF WAV: <http://www.sonicspot.com/guide/wavefiles.html>
class  RiffWavPCMAudioProvider : public PCMAudioProvider {
	struct ChunkHeader {
		/// Always "RIFF"
		char type[4];
		/// File size minus sizeof(ChunkHeader) (i.e. 8)
		uint32_t size;
	};

	struct RIFFChunk {
		ChunkHeader ch;
		/// Always "WAVE"
		char format[4];
	};

	struct fmtChunk {
		/// compression format used
		/// We support only PCM (0x1)
		uint16_t compression;

		/// Number of channels
		uint16_t channels;

		/// Samples per second
		uint32_t samplerate;

		/// Bytes per second
		/// can't always be trusted
		uint32_t avg_bytes_sec;

		/// Bytes per sample
		uint16_t block_align;

		/// Bits per sample that are actually used; rest should be ignored
		uint16_t significant_bits_sample;
		// Here was supposed to be some more fields but we don't need them
		// and just skipping by the size of the struct wouldn't be safe
		// either way, as the fields can depend on the compression.
	};

	static bool CheckFourcc(const char (&str1)[4], const char (&str2)[5])
	{
		return
			(str1[0] == str2[0]) &&
			(str1[1] == str2[1]) &&
			(str1[2] == str2[2]) &&
			(str1[3] == str2[3]);
	}

public:

	RiffWavPCMAudioProvider(agi::fs::path const& filename)
	: PCMAudioProvider(filename)
	{
		this->filename = filename;

		// Read header
		void *filestart = EnsureRangeAccessible(0, sizeof(RIFFChunk));
		RIFFChunk &header = *(RIFFChunk*)filestart;

		// Check magic values
		if (!CheckFourcc(header.ch.type, "RIFF"))
			throw agi::AudioDataNotFoundError("File is not a RIFF file", nullptr);
		if (!CheckFourcc(header.format, "WAVE"))
			throw agi::AudioDataNotFoundError("File is not a RIFF WAV file", nullptr);

		// Count how much more data we can have in the entire file
		// The first 4 bytes are already eaten by the header.format field
		uint32_t data_left = header.ch.size - 4;
		// How far into the file we have processed.
		// Must be incremented by the riff chunk size fields.
		uint32_t filepos = sizeof(header);

		bool got_fmt_header = false;

		// Inherited from AudioProvider
		num_samples = 0;

		// Continue reading chunks until out of data
		while (data_left) {
			ChunkHeader &ch = *(ChunkHeader*)EnsureRangeAccessible(filepos, sizeof(ChunkHeader));

			// Update counters
			data_left -= sizeof(ch);
			filepos += sizeof(ch);

			if (CheckFourcc(ch.type, "fmt ")) {
				if (got_fmt_header) throw agi::AudioProviderOpenError("Invalid file, multiple 'fmt ' chunks", nullptr);
				got_fmt_header = true;

				fmtChunk &fmt = *(fmtChunk*)EnsureRangeAccessible(filepos, sizeof(fmtChunk));

				if (fmt.compression != 1)
					throw agi::AudioProviderOpenError("Can't use file, not PCM encoding", nullptr);

				// Set stuff inherited from the AudioProvider class
				sample_rate = fmt.samplerate;
				channels = fmt.channels;
				bytes_per_sample = (fmt.significant_bits_sample + 7) / 8; // round up to nearest whole byte
			}

			else if (CheckFourcc(ch.type, "data")) {
				// This won't pick up 'data' chunks inside 'wavl' chunks
				// since the 'wavl' chunks wrap those.

				if (!got_fmt_header) throw agi::AudioProviderOpenError("Found 'data' chunk before 'fmt ' chunk, file is invalid.", nullptr);

				int64_t samples = ch.size / bytes_per_sample;
				int64_t frames = samples / channels;

				IndexPoint ip;
				ip.start_sample = num_samples;
				ip.num_samples = frames;
				ip.start_byte = filepos;
				index_points.push_back(ip);

				num_samples += frames;
			}

			// Support wavl (wave list) chunks too?

			// Update counters
			// Make sure they're word aligned
			data_left -= (ch.size + 1) & ~1;
			filepos += (ch.size + 1) & ~1;
		}
	}
};

static const uint8_t w64GuidRIFF[16] = {
	// {66666972-912E-11CF-A5D6-28DB04C10000}
	0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00
};

static const uint8_t w64GuidWAVE[16] = {
	// {65766177-ACF3-11D3-8CD1-00C04F8EDB8A}
	0x77, 0x61, 0x76, 0x65, 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};

static const uint8_t w64Guidfmt[16] = {
	// {20746D66-ACF3-11D3-8CD1-00C04F8EDB8A}
	0x66, 0x6D, 0x74, 0x20, 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};

static const uint8_t w64Guiddata[16] = {
	// {61746164-ACF3-11D3-8CD1-00C04F8EDB8A}
	0x64, 0x61, 0x74, 0x61, 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};

/// @class Wave64AudioProvider
/// @brief Sony Wave64 audio provider
///
/// http://www.vcs.de/fileadmin/user_upload/MBS/PDF/Whitepaper/Informations_about_Sony_Wave64.pdf
class Wave64AudioProvider final : public PCMAudioProvider {
	// Here's some copy-paste from the FFmpegSource2 code

	/// http://msdn.microsoft.com/en-us/library/dd757720(VS.85).aspx
	struct WaveFormatEx {
		uint16_t wFormatTag;
		uint16_t nChannels;
		uint32_t nSamplesPerSec;
		uint32_t nAvgBytesPerSec;
		uint16_t nBlockAlign;
		uint16_t wBitsPerSample;
		uint16_t cbSize;
	};

	struct RiffChunk {
		uint8_t riff_guid[16];
		uint64_t file_size;
		uint8_t format_guid[16];
	};

	struct FormatChunk {
		uint8_t chunk_guid[16];
		uint64_t chunk_size;
		WaveFormatEx format;
		uint8_t padding[6];
	};

	struct DataChunk {
		uint8_t chunk_guid[16];
		uint64_t chunk_size;
	};

	bool CheckGuid(const uint8_t *guid1, const uint8_t *guid2) {
		return memcmp(guid1, guid2, 16) == 0;
	}

public:

	Wave64AudioProvider(agi::fs::path const& filename)
	: PCMAudioProvider(filename)
	{
		this->filename = filename;

		int64_t smallest_possible_file = sizeof(RiffChunk) + sizeof(FormatChunk) + sizeof(DataChunk);

		if (file_size < smallest_possible_file)
			throw agi::AudioDataNotFoundError("File is too small to be a Wave64 file", nullptr);

		// Read header
		// This should throw an exception if the mapping fails
		void *filestart = EnsureRangeAccessible(0, sizeof(RiffChunk));
		assert(filestart);
		RiffChunk &header = *(RiffChunk*)filestart;

		// Check magic values
		if (!CheckGuid(header.riff_guid, w64GuidRIFF))
			throw agi::AudioDataNotFoundError("File is not a Wave64 RIFF file", nullptr);
		if (!CheckGuid(header.format_guid, w64GuidWAVE))
			throw agi::AudioDataNotFoundError("File is not a Wave64 WAVE file", nullptr);

		// Count how much more data we can have in the entire file
		uint64_t data_left = header.file_size - sizeof(RiffChunk);
		// How far into the file we have processed.
		// Must be incremented by the riff chunk size fields.
		uint64_t filepos = sizeof(header);

		bool got_fmt_header = false;

		// Inherited from AudioProvider
		num_samples = 0;

		// Continue reading chunks until out of data
		while (data_left) {
			uint8_t *chunk_guid = (uint8_t*)EnsureRangeAccessible(filepos, 16);
			uint64_t chunk_size = *(uint64_t*)EnsureRangeAccessible(filepos+16, sizeof(uint64_t));

			if (CheckGuid(chunk_guid, w64Guidfmt)) {
				if (got_fmt_header)
					throw agi::AudioProviderOpenError("Bad file, found more than one 'fmt' chunk", nullptr);

				FormatChunk &fmt = *(FormatChunk*)EnsureRangeAccessible(filepos, sizeof(FormatChunk));
				got_fmt_header = true;

				if (fmt.format.wFormatTag == 3)
					throw agi::AudioProviderOpenError("File is IEEE 32 bit float format which isn't supported. Bug the developers if this matters.", nullptr);
				if (fmt.format.wFormatTag != 1)
					throw agi::AudioProviderOpenError("Can't use file, not PCM encoding", nullptr);

				// Set stuff inherited from the AudioProvider class
				sample_rate = fmt.format.nSamplesPerSec;
				channels = fmt.format.nChannels;
				bytes_per_sample = (fmt.format.wBitsPerSample + 7) / 8; // round up to nearest whole byte
			}
			else if (CheckGuid(chunk_guid, w64Guiddata)) {
				if (!got_fmt_header)
					throw agi::AudioProviderOpenError("Found 'data' chunk before 'fmt ' chunk, file is invalid.", nullptr);

				int64_t samples = chunk_size / bytes_per_sample;
				int64_t frames = samples / channels;

				IndexPoint ip;
				ip.start_sample = num_samples;
				ip.num_samples = frames;
				ip.start_byte = filepos;
				index_points.push_back(ip);

				num_samples += frames;
			}

			// Update counters
			// Make sure they're 64 bit aligned
			data_left -= (chunk_size + 7) & ~7;
			filepos += (chunk_size + 7) & ~7;
		}
	}
};

std::unique_ptr<AudioProvider> CreatePCMAudioProvider(agi::fs::path const& filename) {
	bool wrong_file_type = true;
	std::string msg;

	try {
		return agi::util::make_unique<RiffWavPCMAudioProvider>(filename);
	}
	catch (agi::AudioDataNotFoundError const& err) {
		msg = "RIFF PCM WAV audio provider: " + err.GetMessage();
	}
	catch (agi::AudioProviderOpenError const& err) {
		wrong_file_type = false;
		msg = "RIFF PCM WAV audio provider: " + err.GetMessage();
	}

	try {
		return agi::util::make_unique<Wave64AudioProvider>(filename);
	}
	catch (agi::AudioDataNotFoundError const& err) {
		msg += "\nWave64 audio provider: " + err.GetMessage();
	}
	catch (agi::AudioProviderOpenError const& err) {
		wrong_file_type = false;
		msg += "\nWave64 audio provider: " + err.GetMessage();
	}

	if (wrong_file_type)
		throw agi::AudioDataNotFoundError(msg, nullptr);
	else
		throw agi::AudioProviderOpenError(msg, nullptr);
}