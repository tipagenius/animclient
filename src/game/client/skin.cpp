#include "skin.h"

#include <base/str.h>
#include <base/time.h>

#include <engine/shared/config.h>

#include <algorithm>
#include <limits>

void CSkin::CSkinTextures::Reset()
{
	m_Body = IGraphics::CTextureHandle();
	m_BodyOutline = IGraphics::CTextureHandle();
	m_Feet = IGraphics::CTextureHandle();
	m_FeetOutline = IGraphics::CTextureHandle();
	m_Hands = IGraphics::CTextureHandle();
	m_HandsOutline = IGraphics::CTextureHandle();
	for(auto &Eye : m_aEyes)
	{
		Eye = IGraphics::CTextureHandle();
	}
}

void CSkin::CSkinTextures::Unload(IGraphics *pGraphics)
{
	pGraphics->UnloadTexture(&m_Body);
	pGraphics->UnloadTexture(&m_BodyOutline);
	pGraphics->UnloadTexture(&m_Feet);
	pGraphics->UnloadTexture(&m_FeetOutline);
	pGraphics->UnloadTexture(&m_Hands);
	pGraphics->UnloadTexture(&m_HandsOutline);
	for(auto &Eye : m_aEyes)
	{
		pGraphics->UnloadTexture(&Eye);
	}
}

CSkin::CSkinMetricVariableInt::operator int() const
{
	return m_Value;
}

CSkin::CSkinMetricVariableInt &CSkin::CSkinMetricVariableInt::operator=(int NewVal)
{
	m_Value = std::min(m_Value, NewVal);
	return *this;
}

CSkin::CSkinMetricVariableInt::CSkinMetricVariableInt()
{
	Reset();
}

void CSkin::CSkinMetricVariableInt::Reset()
{
	m_Value = std::numeric_limits<int>::max();
}

CSkin::CSkinMetricVariableSize::operator int() const
{
	return m_Value;
}

CSkin::CSkinMetricVariableSize &CSkin::CSkinMetricVariableSize::operator=(int NewVal)
{
	m_Value = std::max(m_Value, NewVal);
	return *this;
}

CSkin::CSkinMetricVariableSize::CSkinMetricVariableSize()
{
	Reset();
}

void CSkin::CSkinMetricVariableSize::Reset()
{
	m_Value = std::numeric_limits<int>::lowest();
}

float CSkin::CSkinMetricVariable::WidthNormalized() const
{
	return (float)m_Width / (float)m_MaxWidth;
}

float CSkin::CSkinMetricVariable::HeightNormalized() const
{
	return (float)m_Height / (float)m_MaxHeight;
}

float CSkin::CSkinMetricVariable::OffsetXNormalized() const
{
	return (float)m_OffsetX / (float)m_MaxWidth;
}

float CSkin::CSkinMetricVariable::OffsetYNormalized() const
{
	return (float)m_OffsetY / (float)m_MaxHeight;
}

void CSkin::CSkinMetricVariable::Reset()
{
	m_Width.Reset();
	m_Height.Reset();
	m_OffsetX.Reset();
	m_OffsetY.Reset();
	m_MaxWidth.Reset();
	m_MaxHeight.Reset();
}

CSkin::CSkinMetrics::CSkinMetrics()
{
	Reset();
}

void CSkin::CSkinMetrics::Reset()
{
	m_Body.Reset();
	m_Feet.Reset();
}

bool CSkin::operator<(const CSkin &Other) const
{
	return str_comp(m_aName, Other.m_aName) < 0;
}

bool CSkin::operator==(const CSkin &Other) const
{
	return !str_comp(m_aName, Other.m_aName);
}

CSkin::CSkin(const char *pName)
{
	str_copy(m_aName, pName);
}

// has to be kept in sync with m_aSkinNameRestrictions
bool CSkin::IsValidName(const char *pName)
{
	if(str_length(pName) >= (int)sizeof(CSkin("").m_aName))
	{
		return false;
	}

	return str_valid_filename(pName);
}

const char CSkin::m_aSkinNameRestrictions[] = "Skin names must be valid filenames shorter than 24 characters.";

// bestclient
void CSkinAnimation::AddFrame(CFrame &&Frame)
{
	// same limits as the media decoder: at most 120 fps, at most 10 seconds per frame
	Frame.m_DurationMs = std::clamp(Frame.m_DurationMs, 9, 10000);
	m_TotalDurationMs += Frame.m_DurationMs;
	m_vFrames.push_back(std::move(Frame));
}

const CSkinAnimation::CFrame *CSkinAnimation::CurrentFrame() const
{
	if(m_vFrames.empty() || !g_Config.m_BcAnimSkins)
	{
		return nullptr;
	}
	if(m_vFrames.size() == 1 || m_TotalDurationMs <= 0)
	{
		return &m_vFrames.front();
	}

	int64_t Offset = (time_get() / (time_freq() / 1000)) % m_TotalDurationMs;
	for(const CFrame &Frame : m_vFrames)
	{
		if(Offset < Frame.m_DurationMs)
		{
			return &Frame;
		}
		Offset -= Frame.m_DurationMs;
	}
	return &m_vFrames.back();
}

void CSkinAnimation::Unload(IGraphics *pGraphics)
{
	for(CFrame &Frame : m_vFrames)
	{
		Frame.m_OriginalSkin.Unload(pGraphics);
		Frame.m_ColorableSkin.Unload(pGraphics);
	}
	m_vFrames.clear();
	m_TotalDurationMs = 0;
}
// bestclient
