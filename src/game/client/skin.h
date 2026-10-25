#ifndef GAME_CLIENT_SKIN_H
#define GAME_CLIENT_SKIN_H

#include <base/color.h>

#include <engine/graphics.h>
#include <engine/shared/protocol.h>

#include <memory>
#include <vector>

class CSkinAnimation;

// do this better and nicer
class CSkin
{
	char m_aName[MAX_SKIN_LENGTH];

public:
	class CSkinTextures
	{
	public:
		IGraphics::CTextureHandle m_Body;
		IGraphics::CTextureHandle m_BodyOutline;

		IGraphics::CTextureHandle m_Feet;
		IGraphics::CTextureHandle m_FeetOutline;

		IGraphics::CTextureHandle m_Hands;
		IGraphics::CTextureHandle m_HandsOutline;

		IGraphics::CTextureHandle m_aEyes[6];

		void Reset();
		void Unload(IGraphics *pGraphics);
	};

	CSkinTextures m_OriginalSkin;
	CSkinTextures m_ColorableSkin;
	ColorRGBA m_BloodColor;

	/**
	 * Frames of an animated skin (GIF/APNG/WebP), nullptr for static skins. // bestclient
	 * The first frame is also stored in @link m_OriginalSkin @endlink and @link m_ColorableSkin @endlink.
	 */
	std::shared_ptr<CSkinAnimation> m_pAnimation;

	class CSkinMetricVariableInt
	{
	public:
		int m_Value;

		operator int() const;
		CSkinMetricVariableInt &operator=(int NewVal);
		CSkinMetricVariableInt();
		void Reset();
	};

	class CSkinMetricVariableSize
	{
	public:
		int m_Value;

		operator int() const;
		CSkinMetricVariableSize &operator=(int NewVal);
		CSkinMetricVariableSize();
		void Reset();
	};

	class CSkinMetricVariable
	{
	public:
		CSkinMetricVariableSize m_Width;
		CSkinMetricVariableSize m_Height;
		CSkinMetricVariableInt m_OffsetX;
		CSkinMetricVariableInt m_OffsetY;

		// these can be used to normalize the metrics
		CSkinMetricVariableSize m_MaxWidth;
		CSkinMetricVariableSize m_MaxHeight;

		float WidthNormalized() const;
		float HeightNormalized() const;
		float OffsetXNormalized() const;
		float OffsetYNormalized() const;
		void Reset();
	};

	class CSkinMetrics
	{
	public:
		CSkinMetricVariable m_Body;
		CSkinMetricVariable m_Feet;

		CSkinMetrics();
		void Reset();
	};
	CSkinMetrics m_Metrics;

	bool operator<(const CSkin &Other) const;
	bool operator==(const CSkin &Other) const;

	CSkin(const char *pName);
	CSkin(CSkin &&) = default;
	CSkin &operator=(CSkin &&) = default;

	const char *GetName() const { return m_aName; }

	static bool IsValidName(const char *pName);
	static const char m_aSkinNameRestrictions[];
};

// bestclient
class CSkinAnimation
{
public:
	class CFrame
	{
	public:
		CSkin::CSkinTextures m_OriginalSkin;
		CSkin::CSkinTextures m_ColorableSkin;
		int m_DurationMs = 0;
	};

	std::vector<CFrame> m_vFrames;

	/**
	 * Adds a frame, durations are clamped to a sane range.
	 */
	void AddFrame(CFrame &&Frame);
	/**
	 * The frame that should be displayed right now, nullptr if the animation is unloaded or animated skins are disabled.
	 * All tees with the same skin are synchronized because the global clock is used.
	 */
	const CFrame *CurrentFrame() const;
	void Unload(IGraphics *pGraphics);

private:
	int64_t m_TotalDurationMs = 0;
};
// bestclient

#endif
