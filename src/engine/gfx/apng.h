/* Copyright © 2026 BestProject Team */
#ifndef ENGINE_GFX_APNG_H
#define ENGINE_GFX_APNG_H

#include <engine/image.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class CApngFrame
{
public:
	/**
	 * The fully composed frame in RGBA format with the size of the whole animation.
	 */
	CImageInfo m_Image;
	int m_DurationMs = 0;
};

/**
 * Decoder for animated PNGs (APNG) that only depends on libpng, so it also works
 * when FFmpeg is not available or was built without the PNG decoders.
 */
namespace Apng
{
	/**
	 * Whether the data is a PNG containing an animation control chunk.
	 */
	bool IsAnimated(const uint8_t *pData, size_t DataSize);

	/**
	 * Decodes all frames of an APNG, composed according to their blend and dispose operations.
	 *
	 * If the animation has more than MaxFrames frames or the frames would need more than MaxTotalBytes,
	 * frames are skipped evenly and the durations of skipped frames are added to the previous kept frame.
	 *
	 * @return Whether at least one frame was decoded.
	 */
	bool Decode(const uint8_t *pData, size_t DataSize, const char *pContextName, size_t MaxFrames, size_t MaxTotalBytes, std::vector<CApngFrame> &vFrames);
}

#endif // ENGINE_GFX_APNG_H
