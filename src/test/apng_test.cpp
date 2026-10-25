#include "test.h"

#include <base/bytes.h>
#include <base/mem.h>

#include <engine/gfx/apng.h>
#include <engine/gfx/image_loader.h>

#include <gtest/gtest.h>

#include <vector>

namespace
{
	class CApngBuilder
	{
	public:
		std::vector<uint8_t> m_vData;
		unsigned m_Sequence = 0;

		void Chunk(const char *pType, const std::vector<uint8_t> &vData)
		{
			uint8_t aLength[4];
			uint_to_bytes_be(aLength, vData.size());
			m_vData.insert(m_vData.end(), aLength, aLength + 4);
			m_vData.insert(m_vData.end(), pType, pType + 4);
			m_vData.insert(m_vData.end(), vData.begin(), vData.end());
			// the decoder does not check input CRCs, it writes new chunks for libpng
			m_vData.insert(m_vData.end(), 4, 0);
		}

		static void Put32(std::vector<uint8_t> &vData, unsigned Value)
		{
			uint8_t aBytes[4];
			uint_to_bytes_be(aBytes, Value);
			vData.insert(vData.end(), aBytes, aBytes + 4);
		}

		void FrameControl(unsigned Width, unsigned Height, unsigned OffsetX, unsigned OffsetY, uint16_t DelayNum, uint16_t DelayDen, uint8_t DisposeOp, uint8_t BlendOp)
		{
			std::vector<uint8_t> vData;
			Put32(vData, m_Sequence++);
			Put32(vData, Width);
			Put32(vData, Height);
			Put32(vData, OffsetX);
			Put32(vData, OffsetY);
			vData.push_back(DelayNum >> 8);
			vData.push_back(DelayNum & 0xff);
			vData.push_back(DelayDen >> 8);
			vData.push_back(DelayDen & 0xff);
			vData.push_back(DisposeOp);
			vData.push_back(BlendOp);
			Chunk("fcTL", vData);
		}
	};

	CImageInfo SolidImage(size_t Width, size_t Height, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
	{
		CImageInfo Image;
		Image.m_Width = Width;
		Image.m_Height = Height;
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		Image.Allocate();
		for(size_t i = 0; i < Width * Height; i++)
		{
			Image.m_pData[i * 4] = R;
			Image.m_pData[i * 4 + 1] = G;
			Image.m_pData[i * 4 + 2] = B;
			Image.m_pData[i * 4 + 3] = A;
		}
		return Image;
	}

	// Encodes the image as PNG and returns the IHDR data and the concatenated IDAT data
	void EncodePng(const CImageInfo &Image, std::vector<uint8_t> &vHeader, std::vector<uint8_t> &vImageData)
	{
		CByteBufferWriter Writer;
		ASSERT_TRUE(CImageLoader::SavePng(Writer, Image));
		const uint8_t *pData = Writer.Data();
		size_t Offset = 8;
		while(Offset + 12 <= Writer.Size())
		{
			const size_t Length = bytes_be_to_uint(pData + Offset);
			const uint8_t *pType = pData + Offset + 4;
			const uint8_t *pChunkData = pData + Offset + 8;
			if(mem_comp(pType, "IHDR", 4) == 0)
				vHeader.assign(pChunkData, pChunkData + Length);
			else if(mem_comp(pType, "IDAT", 4) == 0)
				vImageData.insert(vImageData.end(), pChunkData, pChunkData + Length);
			Offset += 12 + Length;
		}
	}

	void ExpectPixel(const CImageInfo &Image, size_t x, size_t y, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
	{
		const uint8_t *pPixel = Image.m_pData + (y * Image.m_Width + x) * 4;
		EXPECT_EQ(pPixel[0], R) << "x=" << x << " y=" << y;
		EXPECT_EQ(pPixel[1], G) << "x=" << x << " y=" << y;
		EXPECT_EQ(pPixel[2], B) << "x=" << x << " y=" << y;
		EXPECT_EQ(pPixel[3], A) << "x=" << x << " y=" << y;
	}

	/*
	 * 4x4 animation with three frames:
	 * 0: whole canvas red, in IDAT (also the default image), 100 ms
	 * 1: 2x2 half transparent blue at (1, 1) blended over, disposed to previous, 50 ms
	 * 2: 1x1 green at (0, 0), disposed to background, delay 0 (shown for 100 ms)
	 */
	std::vector<uint8_t> BuildTestApng()
	{
		CApngBuilder Builder;
		const uint8_t aSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		Builder.m_vData.assign(aSignature, aSignature + 8);

		std::vector<uint8_t> vHeader, vRed, vBlue, vGreen;
		EncodePng(SolidImage(4, 4, 255, 0, 0, 255), vHeader, vRed);
		std::vector<uint8_t> vUnusedHeader;
		EncodePng(SolidImage(2, 2, 0, 0, 255, 128), vUnusedHeader, vBlue);
		EncodePng(SolidImage(1, 1, 0, 255, 0, 255), vUnusedHeader, vGreen);
		Builder.Chunk("IHDR", vHeader);

		std::vector<uint8_t> vAnimationControl;
		CApngBuilder::Put32(vAnimationControl, 3);
		CApngBuilder::Put32(vAnimationControl, 0);
		Builder.Chunk("acTL", vAnimationControl);

		Builder.FrameControl(4, 4, 0, 0, 1, 10, 0, 0);
		Builder.Chunk("IDAT", vRed);

		Builder.FrameControl(2, 2, 1, 1, 50, 1000, 2, 1);
		std::vector<uint8_t> vFrameData;
		CApngBuilder::Put32(vFrameData, Builder.m_Sequence++);
		vFrameData.insert(vFrameData.end(), vBlue.begin(), vBlue.end());
		Builder.Chunk("fdAT", vFrameData);

		Builder.FrameControl(1, 1, 0, 0, 0, 0, 1, 0);
		vFrameData.clear();
		CApngBuilder::Put32(vFrameData, Builder.m_Sequence++);
		vFrameData.insert(vFrameData.end(), vGreen.begin(), vGreen.end());
		Builder.Chunk("fdAT", vFrameData);

		Builder.Chunk("IEND", {});
		return Builder.m_vData;
	}
}

TEST(Apng, IsAnimated)
{
	const std::vector<uint8_t> vApng = BuildTestApng();
	EXPECT_TRUE(Apng::IsAnimated(vApng.data(), vApng.size()));

	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SavePng(Writer, SolidImage(2, 2, 1, 2, 3, 4)));
	EXPECT_FALSE(Apng::IsAnimated(Writer.Data(), Writer.Size()));

	const uint8_t aGarbage[] = {1, 2, 3};
	EXPECT_FALSE(Apng::IsAnimated(aGarbage, sizeof(aGarbage)));
}

TEST(Apng, ComposeFrames)
{
	const std::vector<uint8_t> vApng = BuildTestApng();
	std::vector<CApngFrame> vFrames;
	ASSERT_TRUE(Apng::Decode(vApng.data(), vApng.size(), "test", 100, 64 * 1024 * 1024, vFrames));
	ASSERT_EQ(vFrames.size(), 3u);

	for(const CApngFrame &Frame : vFrames)
	{
		EXPECT_EQ(Frame.m_Image.m_Width, 4u);
		EXPECT_EQ(Frame.m_Image.m_Height, 4u);
		EXPECT_EQ(Frame.m_Image.m_Format, CImageInfo::FORMAT_RGBA);
	}
	EXPECT_EQ(vFrames[0].m_DurationMs, 100);
	EXPECT_EQ(vFrames[1].m_DurationMs, 50);
	EXPECT_EQ(vFrames[2].m_DurationMs, 100);

	ExpectPixel(vFrames[0].m_Image, 0, 0, 255, 0, 0, 255);
	ExpectPixel(vFrames[0].m_Image, 3, 3, 255, 0, 0, 255);

	// half transparent blue over red
	ExpectPixel(vFrames[1].m_Image, 0, 0, 255, 0, 0, 255);
	ExpectPixel(vFrames[1].m_Image, 1, 1, 127, 0, 128, 255);
	ExpectPixel(vFrames[1].m_Image, 2, 2, 127, 0, 128, 255);
	ExpectPixel(vFrames[1].m_Image, 3, 3, 255, 0, 0, 255);

	// frame 1 was disposed to the previous state, so the blue is gone again
	ExpectPixel(vFrames[2].m_Image, 0, 0, 0, 255, 0, 255);
	ExpectPixel(vFrames[2].m_Image, 1, 1, 255, 0, 0, 255);
}

TEST(Apng, LimitFrames)
{
	const std::vector<uint8_t> vApng = BuildTestApng();
	std::vector<CApngFrame> vFrames;
	ASSERT_TRUE(Apng::Decode(vApng.data(), vApng.size(), "test", 2, 64 * 1024 * 1024, vFrames));
	ASSERT_EQ(vFrames.size(), 2u);
	// the skipped frame's duration is added to the previous frame, so the total duration stays the same
	EXPECT_EQ(vFrames[0].m_DurationMs, 150);
	EXPECT_EQ(vFrames[1].m_DurationMs, 100);
	ExpectPixel(vFrames[1].m_Image, 0, 0, 0, 255, 0, 255);
}

TEST(Apng, InvalidFrameOutsideImage)
{
	std::vector<uint8_t> vApng = BuildTestApng();
	// move the second frame (2x2) to x offset 3, so it does not fit into the 4x4 image anymore
	for(size_t i = 8; i + 8 < vApng.size(); i++)
	{
		if(mem_comp(&vApng[i], "fcTL", 4) == 0 && bytes_be_to_uint(&vApng[i + 4]) == 1)
		{
			uint_to_bytes_be(&vApng[i + 4 + 12], 3);
			break;
		}
	}
	std::vector<CApngFrame> vFrames;
	EXPECT_FALSE(Apng::Decode(vApng.data(), vApng.size(), "test", 100, 64 * 1024 * 1024, vFrames));
	EXPECT_TRUE(vFrames.empty());
}
