/* Copyright © 2026 BestProject Team */
#include "apng.h"

#include "image_loader.h"
#include "image_manipulation.h"

#include <base/bytes.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <algorithm>

namespace
{
	constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	constexpr size_t MAX_DIMENSION = 4096;

	enum
	{
		DISPOSE_OP_NONE = 0,
		DISPOSE_OP_BACKGROUND = 1,
		DISPOSE_OP_PREVIOUS = 2,
	};

	enum
	{
		BLEND_OP_SOURCE = 0,
		BLEND_OP_OVER = 1,
	};

	class CChunk
	{
	public:
		char m_aType[5] = {0};
		const uint8_t *m_pData = nullptr;
		size_t m_Size = 0;
	};

	class CFrameControl
	{
	public:
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_OffsetX = 0;
		uint32_t m_OffsetY = 0;
		int m_DurationMs = 0;
		uint8_t m_DisposeOp = DISPOSE_OP_NONE;
		uint8_t m_BlendOp = BLEND_OP_SOURCE;
		// chunks with the compressed image data of the frame and how many bytes to skip in each (fdAT sequence number)
		std::vector<const CChunk *> m_vpData;
		std::vector<size_t> m_vDataSkip;
	};

	class CCrcTable
	{
	public:
		uint32_t m_aTable[256];

		CCrcTable()
		{
			for(uint32_t n = 0; n < 256; n++)
			{
				uint32_t c = n;
				for(int k = 0; k < 8; k++)
					c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
				m_aTable[n] = c;
			}
		}
	};

	uint32_t Crc32(const uint8_t *pData, size_t Size, uint32_t Crc = 0)
	{
		static const CCrcTable s_CrcTable;
		Crc = ~Crc;
		for(size_t i = 0; i < Size; i++)
			Crc = s_CrcTable.m_aTable[(Crc ^ pData[i]) & 0xff] ^ (Crc >> 8);
		return ~Crc;
	}

	void WriteChunk(CByteBufferWriter &Writer, const char *pType, const uint8_t *pData, size_t Size)
	{
		uint8_t aLength[4];
		uint_to_bytes_be(aLength, (unsigned)Size);
		Writer.Write(aLength, sizeof(aLength));
		Writer.Write(pType, 4);
		if(Size > 0)
			Writer.Write(pData, Size);
		uint32_t Crc = Crc32(reinterpret_cast<const uint8_t *>(pType), 4);
		Crc = Crc32(pData, Size, Crc);
		uint8_t aCrc[4];
		uint_to_bytes_be(aCrc, Crc);
		Writer.Write(aCrc, sizeof(aCrc));
	}

	bool ReadChunks(const uint8_t *pData, size_t DataSize, std::vector<CChunk> &vChunks)
	{
		if(pData == nullptr || DataSize < sizeof(PNG_SIGNATURE) || mem_comp(pData, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) != 0)
			return false;

		size_t Offset = sizeof(PNG_SIGNATURE);
		while(Offset + 12 <= DataSize)
		{
			const size_t Length = bytes_be_to_uint(pData + Offset);
			if(Length > DataSize - Offset - 12)
				return false;
			CChunk Chunk;
			mem_copy(Chunk.m_aType, pData + Offset + 4, 4);
			Chunk.m_pData = pData + Offset + 8;
			Chunk.m_Size = Length;
			vChunks.push_back(Chunk);
			Offset += 12 + Length;
			if(str_comp(Chunk.m_aType, "IEND") == 0)
				break;
		}
		return !vChunks.empty() && str_comp(vChunks.front().m_aType, "IHDR") == 0 && vChunks.front().m_Size == 13;
	}

	int FrameDurationMs(uint16_t DelayNum, uint16_t DelayDen)
	{
		if(DelayDen == 0)
			DelayDen = 100;
		// Same as browsers: a delay of zero would be as fast as possible, show it for 100 ms instead
		if(DelayNum == 0)
			return 100;
		return std::max(10, (int)((uint32_t)DelayNum * 1000 / DelayDen));
	}

	// Decodes a single frame by wrapping its image data into a regular PNG
	bool DecodeFrame(const CChunk &Header, const std::vector<const CChunk *> &vpSharedChunks, const CFrameControl &Frame, const char *pContextName, CImageInfo &Image)
	{
		CByteBufferWriter Writer;
		Writer.Write(PNG_SIGNATURE, sizeof(PNG_SIGNATURE));

		uint8_t aHeader[13];
		mem_copy(aHeader, Header.m_pData, sizeof(aHeader));
		uint_to_bytes_be(aHeader, Frame.m_Width);
		uint_to_bytes_be(aHeader + 4, Frame.m_Height);
		WriteChunk(Writer, "IHDR", aHeader, sizeof(aHeader));

		for(const CChunk *pChunk : vpSharedChunks)
			WriteChunk(Writer, pChunk->m_aType, pChunk->m_pData, pChunk->m_Size);

		std::vector<uint8_t> vImageData;
		for(size_t i = 0; i < Frame.m_vpData.size(); i++)
		{
			const CChunk *pChunk = Frame.m_vpData[i];
			const size_t Skip = Frame.m_vDataSkip[i];
			vImageData.insert(vImageData.end(), pChunk->m_pData + Skip, pChunk->m_pData + pChunk->m_Size);
		}
		WriteChunk(Writer, "IDAT", vImageData.data(), vImageData.size());
		WriteChunk(Writer, "IEND", nullptr, 0);

		CByteBufferReader Reader(Writer.Data(), Writer.Size());
		int PngliteIncompatible;
		if(!CImageLoader::LoadPng(Reader, pContextName, Image, PngliteIncompatible))
			return false;
		ConvertToRgba(Image);
		return Image.m_Width == Frame.m_Width && Image.m_Height == Frame.m_Height;
	}

	void BlendFrame(CImageInfo &Canvas, const CImageInfo &FrameImage, const CFrameControl &Frame)
	{
		for(uint32_t y = 0; y < Frame.m_Height; y++)
		{
			const uint8_t *pSrc = FrameImage.m_pData + (size_t)y * Frame.m_Width * 4;
			uint8_t *pDst = Canvas.m_pData + ((size_t)(Frame.m_OffsetY + y) * Canvas.m_Width + Frame.m_OffsetX) * 4;
			if(Frame.m_BlendOp == BLEND_OP_SOURCE)
			{
				mem_copy(pDst, pSrc, (size_t)Frame.m_Width * 4);
				continue;
			}
			for(uint32_t x = 0; x < Frame.m_Width; x++, pSrc += 4, pDst += 4)
			{
				const uint32_t SrcAlpha = pSrc[3];
				if(SrcAlpha == 255)
				{
					mem_copy(pDst, pSrc, 4);
					continue;
				}
				if(SrcAlpha == 0)
					continue;
				// straight alpha "over" operator
				const uint32_t DstAlpha = pDst[3] * (255 - SrcAlpha) / 255;
				const uint32_t OutAlpha = SrcAlpha + DstAlpha;
				for(int c = 0; c < 3; c++)
					pDst[c] = (uint8_t)((pSrc[c] * SrcAlpha + pDst[c] * DstAlpha) / OutAlpha);
				pDst[3] = (uint8_t)OutAlpha;
			}
		}
	}

	void ClearRegion(CImageInfo &Canvas, const CFrameControl &Frame)
	{
		for(uint32_t y = 0; y < Frame.m_Height; y++)
			mem_zero(Canvas.m_pData + ((size_t)(Frame.m_OffsetY + y) * Canvas.m_Width + Frame.m_OffsetX) * 4, (size_t)Frame.m_Width * 4);
	}
}

namespace Apng
{
	bool IsAnimated(const uint8_t *pData, size_t DataSize)
	{
		std::vector<CChunk> vChunks;
		if(!ReadChunks(pData, DataSize, vChunks))
			return false;
		for(const CChunk &Chunk : vChunks)
		{
			// The acTL chunk must appear before the first IDAT chunk
			if(str_comp(Chunk.m_aType, "acTL") == 0)
				return true;
			if(str_comp(Chunk.m_aType, "IDAT") == 0)
				return false;
		}
		return false;
	}

	bool Decode(const uint8_t *pData, size_t DataSize, const char *pContextName, size_t MaxFrames, size_t MaxTotalBytes, std::vector<CApngFrame> &vFrames)
	{
		vFrames.clear();

		std::vector<CChunk> vChunks;
		if(!ReadChunks(pData, DataSize, vChunks))
		{
			log_error("apng", "%s: not a valid PNG file", pContextName);
			return false;
		}

		const CChunk &Header = vChunks.front();
		const uint32_t Width = bytes_be_to_uint(Header.m_pData);
		const uint32_t Height = bytes_be_to_uint(Header.m_pData + 4);
		if(Width == 0 || Height == 0 || Width > MAX_DIMENSION || Height > MAX_DIMENSION)
		{
			log_error("apng", "%s: unsupported size %ux%u", pContextName, Width, Height);
			return false;
		}

		std::vector<const CChunk *> vpSharedChunks;
		std::vector<CFrameControl> vFrameControls;
		bool SeenImageData = false;
		for(const CChunk &Chunk : vChunks)
		{
			if(str_comp(Chunk.m_aType, "PLTE") == 0 || str_comp(Chunk.m_aType, "tRNS") == 0)
			{
				if(!SeenImageData)
					vpSharedChunks.push_back(&Chunk);
			}
			else if(str_comp(Chunk.m_aType, "fcTL") == 0)
			{
				if(Chunk.m_Size != 26)
					return false;
				CFrameControl Frame;
				Frame.m_Width = bytes_be_to_uint(Chunk.m_pData + 4);
				Frame.m_Height = bytes_be_to_uint(Chunk.m_pData + 8);
				Frame.m_OffsetX = bytes_be_to_uint(Chunk.m_pData + 12);
				Frame.m_OffsetY = bytes_be_to_uint(Chunk.m_pData + 16);
				Frame.m_DurationMs = FrameDurationMs((Chunk.m_pData[20] << 8) | Chunk.m_pData[21], (Chunk.m_pData[22] << 8) | Chunk.m_pData[23]);
				Frame.m_DisposeOp = Chunk.m_pData[24];
				Frame.m_BlendOp = Chunk.m_pData[25];
				if(Frame.m_Width == 0 || Frame.m_Height == 0 ||
					Frame.m_OffsetX > Width || Frame.m_Width > Width - Frame.m_OffsetX ||
					Frame.m_OffsetY > Height || Frame.m_Height > Height - Frame.m_OffsetY)
				{
					log_error("apng", "%s: frame %d is outside of the image", pContextName, (int)vFrameControls.size());
					return false;
				}
				vFrameControls.push_back(Frame);
			}
			else if(str_comp(Chunk.m_aType, "IDAT") == 0)
			{
				SeenImageData = true;
				// IDAT without a preceding fcTL is the default image, which is not part of the animation
				if(!vFrameControls.empty())
				{
					vFrameControls.back().m_vpData.push_back(&Chunk);
					vFrameControls.back().m_vDataSkip.push_back(0);
				}
			}
			else if(str_comp(Chunk.m_aType, "fdAT") == 0)
			{
				SeenImageData = true;
				if(vFrameControls.empty() || Chunk.m_Size < 4)
					return false;
				vFrameControls.back().m_vpData.push_back(&Chunk);
				vFrameControls.back().m_vDataSkip.push_back(4);
			}
		}

		// drop frames without data (broken files)
		vFrameControls.erase(std::remove_if(vFrameControls.begin(), vFrameControls.end(), [](const CFrameControl &Frame) { return Frame.m_vpData.empty(); }), vFrameControls.end());
		if(vFrameControls.empty())
		{
			log_error("apng", "%s: no animation frames found", pContextName);
			return false;
		}

		const size_t CanvasBytes = (size_t)Width * Height * 4;
		MaxFrames = std::max<size_t>(1, std::min(MaxFrames, MaxTotalBytes / CanvasBytes));
		const size_t NumFrames = vFrameControls.size();

		CImageInfo Canvas;
		Canvas.m_Width = Width;
		Canvas.m_Height = Height;
		Canvas.m_Format = CImageInfo::FORMAT_RGBA;
		Canvas.AllocateFillZero();
		CImageInfo PreviousCanvas;

		size_t LastKeptSlot = (size_t)-1;
		for(size_t i = 0; i < NumFrames; i++)
		{
			CFrameControl &Frame = vFrameControls[i];
			// the first frame cannot restore a previous frame, so it is cleared instead
			if(i == 0 && Frame.m_DisposeOp == DISPOSE_OP_PREVIOUS)
				Frame.m_DisposeOp = DISPOSE_OP_BACKGROUND;
			if(Frame.m_DisposeOp == DISPOSE_OP_PREVIOUS)
				PreviousCanvas = Canvas.DeepCopy();

			CImageInfo FrameImage;
			if(!DecodeFrame(Header, vpSharedChunks, Frame, pContextName, FrameImage))
			{
				log_error("apng", "%s: failed to decode frame %d", pContextName, (int)i);
				vFrames.clear();
				return false;
			}
			BlendFrame(Canvas, FrameImage, Frame);

			// keep frames evenly distributed if there are too many
			const size_t Slot = i * MaxFrames / NumFrames;
			if(Slot != LastKeptSlot)
			{
				LastKeptSlot = Slot;
				CApngFrame OutFrame;
				OutFrame.m_Image = Canvas.DeepCopy();
				OutFrame.m_DurationMs = Frame.m_DurationMs;
				vFrames.push_back(std::move(OutFrame));
			}
			else
			{
				vFrames.back().m_DurationMs += Frame.m_DurationMs;
			}

			if(Frame.m_DisposeOp == DISPOSE_OP_BACKGROUND)
				ClearRegion(Canvas, Frame);
			else if(Frame.m_DisposeOp == DISPOSE_OP_PREVIOUS)
				Canvas = std::move(PreviousCanvas);
		}
		return !vFrames.empty();
	}
}
