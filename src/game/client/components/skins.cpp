/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "skins.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/gfx/apng.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <generated/client_data.h>

#include <game/client/components/bestclient/media_decoder.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

#include <optional>

using namespace std::chrono_literals;

CSkins::CAbstractSkinLoadJob::CAbstractSkinLoadJob(CSkins *pSkins, const char *pName) :
	m_pSkins(pSkins)
{
	str_copy(m_aName, pName);
	Abortable(true);
}

CSkins::CAbstractSkinLoadJob::~CAbstractSkinLoadJob()
{
	m_Data.Free();
}

// bestclient
void CSkins::CSkinLoadData::FreeAnimFrames()
{
	m_vAnimFrames.clear();
	m_LocalAnimation = false;
}

void CSkins::CSkinLoadData::Free()
{
	m_Info.Free();
	m_InfoGrayscale.Free();
	FreeAnimFrames();
}
// bestclient

CSkins::CSkinLoadJob::CSkinLoadJob(CSkins *pSkins, const char *pName, int StorageType, bool Cached) :
	CAbstractSkinLoadJob(pSkins, pName),
	m_StorageType(StorageType),
	m_Cached(Cached)
{
}

CSkins::CSkinContainer::CSkinContainer(CSkins *pSkins, const char *pName, EType Type, int StorageType) :
	m_pSkins(pSkins),
	m_Type(Type),
	m_StorageType(StorageType)
{
	str_copy(m_aName, pName);
	m_Vanilla = IsVanillaSkin(m_aName);
	m_Special = IsSpecialSkin(m_aName);
	m_AlwaysLoaded = m_Vanilla; // Vanilla skins are loaded immediately and not unloaded
	m_CacheAvailable = Type == EType::CACHED;
	m_CacheOnly = Type == EType::CACHED;
}

CSkins::CSkinContainer::~CSkinContainer()
{
	dbg_assert(m_pLoadJob == nullptr, "Skin container load job was not cleared");
	if(m_pAnimDownloadJob) // bestclient
	{
		m_pAnimDownloadJob->Abort();
	}
}

bool CSkins::CSkinContainer::operator<(const CSkinContainer &Other) const
{
	return str_comp(m_aName, Other.m_aName) < 0;
}

static constexpr std::chrono::nanoseconds MIN_REQUESTED_TIME_FOR_PENDING = 250ms;
static constexpr std::chrono::nanoseconds MAX_REQUESTED_TIME_FOR_PENDING = 500ms;
static constexpr std::chrono::nanoseconds MIN_UNLOAD_TIME_PENDING = 1s;
static constexpr std::chrono::nanoseconds MIN_UNLOAD_TIME_LOADED = 2s;
static_assert(MIN_REQUESTED_TIME_FOR_PENDING < MAX_REQUESTED_TIME_FOR_PENDING);
static_assert(MIN_REQUESTED_TIME_FOR_PENDING < MIN_UNLOAD_TIME_PENDING, "Unloading pending skins must take longer than adding more pending skins");

void CSkins::CSkinContainer::RequestLoad()
{
	if(m_AlwaysLoaded)
	{
		return;
	}

	// Delay loading skins a bit after the load has been requested to avoid loading a lot of skins
	// when quickly scrolling through lists or if a player with a new skin quickly joins and leaves.
	if(m_State == EState::UNLOADED)
	{
		const std::chrono::nanoseconds Now = time_get_nanoseconds();
		if(!m_FirstLoadRequest.has_value() ||
			!m_LastLoadRequest.has_value() ||
			Now - m_LastLoadRequest.value() > MAX_REQUESTED_TIME_FOR_PENDING)
		{
			m_FirstLoadRequest = Now;
			m_LastLoadRequest = m_FirstLoadRequest;
		}
		else if(Now - m_FirstLoadRequest.value() > MIN_REQUESTED_TIME_FOR_PENDING)
		{
			m_State = EState::PENDING;
		}
	}
	else if(m_State == EState::PENDING ||
		m_State == EState::LOADING ||
		m_State == EState::LOADED)
	{
		m_LastLoadRequest = time_get_nanoseconds();
	}

	if(m_State == EState::PENDING ||
		m_State == EState::LOADED)
	{
		if(m_UsageEntryIterator.has_value())
		{
			m_pSkins->m_SkinsUsageList.erase(m_UsageEntryIterator.value());
		}
		m_pSkins->m_SkinsUsageList.emplace_front(Name());
		m_UsageEntryIterator = m_pSkins->m_SkinsUsageList.begin();
	}
}

// bestclient
bool CSkins::CSkinContainer::IsSuppressedForGame() const
{
	if(g_Config.m_ClVanillaSkinsOnly && !m_Vanilla)
	{
		return true;
	}
	return !g_Config.m_ClDownloadSkins && (m_Type == EType::DOWNLOAD || m_Type == EType::CACHED);
}
// bestclient

CSkins::CSkinContainer::EState CSkins::CSkinContainer::DetermineInitialState() const
{
	if(m_AlwaysLoaded)
	{
		// Load immediately if it should always be loaded
		return EState::PENDING;
	}
	else if(IsSuppressedForGame()) // bestclient
	{
		return EState::NOT_FOUND;
	}
	else
	{
		return EState::UNLOADED;
	}
}

void CSkins::CSkinContainer::SetState(EState State)
{
	m_State = State;

	if(m_State == EState::PENDING ||
		m_State == EState::LOADING ||
		m_State == EState::LOADED)
	{
		RequestLoad();
	}
	else
	{
		m_FirstLoadRequest = std::nullopt;
		m_LastLoadRequest = std::nullopt;
	}

	if(m_State != EState::PENDING &&
		m_State != EState::LOADED &&
		m_UsageEntryIterator.has_value())
	{
		m_pSkins->m_SkinsUsageList.erase(m_UsageEntryIterator.value());
		m_UsageEntryIterator = std::nullopt;
	}

	m_pSkins->m_SkinList.ForceRefresh();
	m_pSkins->m_LocalSkinListNeedsUpdate = true;
}

bool CSkins::CSkinListEntry::operator<(const CSkins::CSkinListEntry &Other) const
{
	if(m_Favorite && !Other.m_Favorite)
	{
		return true;
	}
	if(!m_Favorite && Other.m_Favorite)
	{
		return false;
	}
	return str_comp(m_pSkinContainer->Name(), Other.m_pSkinContainer->Name()) < 0;
}

void CSkins::CSkinListEntry::RequestLoad()
{
	m_pSkinContainer->RequestLoad();
}

CSkins::CSkins() :
	m_PlaceholderSkin("dummy")
{
	m_PlaceholderSkin.m_OriginalSkin.Reset();
	m_PlaceholderSkin.m_ColorableSkin.Reset();
	m_PlaceholderSkin.m_BloodColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	m_PlaceholderSkin.m_Metrics.m_Body.m_Width = 64;
	m_PlaceholderSkin.m_Metrics.m_Body.m_Height = 64;
	m_PlaceholderSkin.m_Metrics.m_Body.m_OffsetX = 16;
	m_PlaceholderSkin.m_Metrics.m_Body.m_OffsetY = 16;
	m_PlaceholderSkin.m_Metrics.m_Body.m_MaxWidth = 96;
	m_PlaceholderSkin.m_Metrics.m_Body.m_MaxHeight = 96;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_Width = 32;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_Height = 16;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_OffsetX = 16;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_OffsetY = 8;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_MaxWidth = 64;
	m_PlaceholderSkin.m_Metrics.m_Feet.m_MaxHeight = 32;
}

bool CSkins::IsSpecialSkin(const char *pName)
{
	return str_utf8_comp_nocase_num(pName, "x_", 2) == 0;
}

bool CSkins::IsVanillaSkin(const char *pName)
{
	return std::any_of(std::begin(VANILLA_SKINS), std::end(VANILLA_SKINS), [pName](const char *pVanillaSkin) {
		return str_comp(pName, pVanillaSkin) == 0;
	});
}

class CSkinScanUser
{
public:
	CSkins *m_pThis;
	CSkins::TSkinLoadedCallback m_SkinLoadedCallback;
	bool m_CachedSkins;
};

int CSkins::SkinScan(const char *pName, int IsDir, int StorageType, void *pUser)
{
	auto *pUserReal = static_cast<CSkinScanUser *>(pUser);
	CSkins *pSelf = pUserReal->m_pThis;

	if(IsDir)
	{
		return 0;
	}

	const char *pSuffix = str_endswith(pName, ".png");
	if(pSuffix == nullptr && !pUserReal->m_CachedSkins) // bestclient: skins can also be only animated
	{
		for(const char *pExtension : {".gif", ".apng", ".webp"})
		{
			pSuffix = str_endswith_nocase(pName, pExtension);
			if(pSuffix != nullptr)
			{
				break;
			}
		}
	}
	if(pSuffix == nullptr)
	{
		return 0;
	}

	char aSkinName[IO_MAX_PATH_LENGTH];
	str_truncate(aSkinName, sizeof(aSkinName), pName, pSuffix - pName);
	if(!CSkin::IsValidName(aSkinName))
	{
		log_error("skins", "Skin name is not valid: %s", aSkinName);
		log_error("skins", "%s", CSkin::m_aSkinNameRestrictions);
		return 0;
	}

	if(pSelf->m_Skins.find(aSkinName) != pSelf->m_Skins.end())
	{
		return 0;
	}
	CSkinContainer SkinContainer(pSelf, aSkinName, pUserReal->m_CachedSkins ? CSkins::CSkinContainer::EType::CACHED : CSkins::CSkinContainer::EType::LOCAL, StorageType);
	auto &&pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
	pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
	pSelf->m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)});
	pSelf->m_LocalSkinListNeedsUpdate = true;
	pUserReal->m_SkinLoadedCallback();
	return 0;
}

static void CheckMetrics(CSkin::CSkinMetricVariable &Metrics, const uint8_t *pImg, int ImgWidth, int ImgX, int ImgY, int CheckWidth, int CheckHeight)
{
	int MaxY = -1;
	int MinY = CheckHeight + 1;
	int MaxX = -1;
	int MinX = CheckWidth + 1;

	for(int y = 0; y < CheckHeight; y++)
	{
		for(int x = 0; x < CheckWidth; x++)
		{
			int OffsetAlpha = (y + ImgY) * ImgWidth + (x + ImgX) * 4 + 3;
			uint8_t AlphaValue = pImg[OffsetAlpha];
			if(AlphaValue > 0)
			{
				if(MaxY < y)
					MaxY = y;
				if(MinY > y)
					MinY = y;
				if(MaxX < x)
					MaxX = x;
				if(MinX > x)
					MinX = x;
			}
		}
	}

	Metrics.m_Width = std::clamp((MaxX - MinX) + 1, 1, CheckWidth);
	Metrics.m_Height = std::clamp((MaxY - MinY) + 1, 1, CheckHeight);
	Metrics.m_OffsetX = std::clamp(MinX, 0, CheckWidth - 1);
	Metrics.m_OffsetY = std::clamp(MinY, 0, CheckHeight - 1);
	Metrics.m_MaxWidth = CheckWidth;
	Metrics.m_MaxHeight = CheckHeight;
}

bool CSkins::PrepareSkinImage(const char *pName, CImageInfo &Info, size_t MaxWidth) const
{
	const int DivX = g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx; // bestclient
	const int DivY = g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy; // bestclient
	if(!Graphics()->CheckImageDivisibility(pName, Info, DivX, DivY, true))
	{
		log_error("skins", "Skin failed image divisibility: %s", pName);
		Info.Free();
		return false;
	}
	if(!Graphics()->IsImageFormatRgba(pName, Info))
	{
		log_error("skins", "Skin format is not RGBA: %s", pName);
		Info.Free();
		return false;
	}
	// Downscale oversized skins before uploading dozens of sprite textures to the GPU. // bestclient
	if(Info.m_Width > MaxWidth && DivX > 0 && DivY > 0)
	{
		size_t NewWidth = (MaxWidth / (size_t)DivX) * (size_t)DivX;
		size_t NewHeight = (NewWidth * Info.m_Height) / Info.m_Width;
		NewHeight = (NewHeight / (size_t)DivY) * (size_t)DivY;
		if(NewWidth >= (size_t)DivX && NewHeight >= (size_t)DivY &&
			(NewWidth < Info.m_Width || NewHeight < Info.m_Height))
		{
			log_info("skins", "Downscaling skin '%s' from %" PRIzu "x%" PRIzu " to %" PRIzu "x%" PRIzu " (max width %" PRIzu ")",
				pName, Info.m_Width, Info.m_Height, NewWidth, NewHeight, MaxWidth);
			ResizeImage(Info, NewWidth, NewHeight);
		}
	} // bestclient

	const size_t BodyWidth = g_pData->m_aSprites[SPRITE_TEE_BODY].m_W * (Info.m_Width / (size_t)DivX);
	const size_t BodyHeight = g_pData->m_aSprites[SPRITE_TEE_BODY].m_H * (Info.m_Height / (size_t)DivY);
	if(BodyWidth > Info.m_Width || BodyHeight > Info.m_Height)
	{
		log_error("skins", "Skin size unsupported (w=%" PRIzu ", h=%" PRIzu "): %s", Info.m_Width, Info.m_Height, pName);
		Info.Free();
		return false;
	}
	return true;
}

void CSkins::CreateColorableImage(const CImageInfo &Info, CImageInfo &InfoGrayscale)
{
	const size_t BodyWidth = g_pData->m_aSprites[SPRITE_TEE_BODY].m_W * (Info.m_Width / (size_t)g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx);
	const size_t BodyHeight = g_pData->m_aSprites[SPRITE_TEE_BODY].m_H * (Info.m_Height / (size_t)g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy);
	const size_t PixelStep = Info.PixelSize();
	const size_t Pitch = Info.m_Width * PixelStep;

	InfoGrayscale = Info.DeepCopy();
	ConvertToGrayscale(InfoGrayscale);

	int aFreq[256] = {0};
	uint8_t OrgWeight = 1;
	uint8_t NewWeight = 192;

	// find most common non-zero frequency
	for(size_t y = 0; y < BodyHeight; y++)
	{
		for(size_t x = 0; x < BodyWidth; x++)
		{
			const size_t Offset = y * Pitch + x * PixelStep;
			if(InfoGrayscale.m_pData[Offset + 3] > 128)
			{
				aFreq[InfoGrayscale.m_pData[Offset]]++;
			}
		}
	}

	for(int i = 1; i < 256; i++)
	{
		if(aFreq[OrgWeight] < aFreq[i])
		{
			OrgWeight = i;
		}
	}

	// reorder
	for(size_t y = 0; y < BodyHeight; y++)
	{
		for(size_t x = 0; x < BodyWidth; x++)
		{
			const size_t Offset = y * Pitch + x * PixelStep;
			uint8_t v = InfoGrayscale.m_pData[Offset];
			if(v <= OrgWeight)
			{
				v = (uint8_t)((v / (float)OrgWeight) * NewWeight);
			}
			else
			{
				v = (uint8_t)(((v - OrgWeight) / (float)(255 - OrgWeight)) * (255 - NewWeight) + NewWeight);
			}
			InfoGrayscale.m_pData[Offset] = v;
			InfoGrayscale.m_pData[Offset + 1] = v;
			InfoGrayscale.m_pData[Offset + 2] = v;
		}
	}
}

bool CSkins::LoadSkinData(const char *pName, CSkinLoadData &Data) const
{
	if(!PrepareSkinImage(pName, Data.m_Info, (size_t)g_Config.m_ClSkinMaxWidth))
	{
		Data.FreeAnimFrames();
		return false;
	}

	int FeetGridPixelsWidth = Data.m_Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_FOOT].m_pSet->m_Gridx;
	int FeetGridPixelsHeight = Data.m_Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_FOOT].m_pSet->m_Gridy;
	int FeetWidth = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_W * FeetGridPixelsWidth;
	int FeetHeight = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_H * FeetGridPixelsHeight;
	int FeetOffsetX = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_X * FeetGridPixelsWidth;
	int FeetOffsetY = g_pData->m_aSprites[SPRITE_TEE_FOOT].m_Y * FeetGridPixelsHeight;

	int FeetOutlineGridPixelsWidth = Data.m_Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_pSet->m_Gridx;
	int FeetOutlineGridPixelsHeight = Data.m_Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_pSet->m_Gridy;
	int FeetOutlineWidth = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_W * FeetOutlineGridPixelsWidth;
	int FeetOutlineHeight = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_H * FeetOutlineGridPixelsHeight;
	int FeetOutlineOffsetX = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_X * FeetOutlineGridPixelsWidth;
	int FeetOutlineOffsetY = g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE].m_Y * FeetOutlineGridPixelsHeight;

	int BodyOutlineGridPixelsWidth = Data.m_Info.m_Width / g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_pSet->m_Gridx;
	int BodyOutlineGridPixelsHeight = Data.m_Info.m_Height / g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_pSet->m_Gridy;
	int BodyOutlineWidth = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_W * BodyOutlineGridPixelsWidth;
	int BodyOutlineHeight = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_H * BodyOutlineGridPixelsHeight;
	int BodyOutlineOffsetX = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_X * BodyOutlineGridPixelsWidth;
	int BodyOutlineOffsetY = g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE].m_Y * BodyOutlineGridPixelsHeight;

	const size_t BodyWidth = g_pData->m_aSprites[SPRITE_TEE_BODY].m_W * (Data.m_Info.m_Width / (size_t)g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx);
	const size_t BodyHeight = g_pData->m_aSprites[SPRITE_TEE_BODY].m_H * (Data.m_Info.m_Height / (size_t)g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy);
	const size_t PixelStep = Data.m_Info.PixelSize();
	const size_t Pitch = Data.m_Info.m_Width * PixelStep;

	// dig out blood color
	{
		int64_t aColors[3] = {0};
		for(size_t y = 0; y < BodyHeight; y++)
		{
			for(size_t x = 0; x < BodyWidth; x++)
			{
				const size_t Offset = y * Pitch + x * PixelStep;
				if(Data.m_Info.m_pData[Offset + 3] > 128)
				{
					for(size_t c = 0; c < 3; c++)
					{
						aColors[c] += Data.m_Info.m_pData[Offset + c];
					}
				}
			}
		}
		const vec3 NormalizedColor = normalize(vec3(aColors[0], aColors[1], aColors[2]));
		Data.m_BloodColor = ColorRGBA(NormalizedColor.x, NormalizedColor.y, NormalizedColor.z);
	}

	Data.m_Metrics.Reset();
	CheckMetrics(Data.m_Metrics.m_Body, Data.m_Info.m_pData, Pitch, 0, 0, BodyWidth, BodyHeight);
	CheckMetrics(Data.m_Metrics.m_Body, Data.m_Info.m_pData, Pitch, BodyOutlineOffsetX, BodyOutlineOffsetY, BodyOutlineWidth, BodyOutlineHeight);
	CheckMetrics(Data.m_Metrics.m_Feet, Data.m_Info.m_pData, Pitch, FeetOffsetX, FeetOffsetY, FeetWidth, FeetHeight);
	CheckMetrics(Data.m_Metrics.m_Feet, Data.m_Info.m_pData, Pitch, FeetOutlineOffsetX, FeetOutlineOffsetY, FeetOutlineWidth, FeetOutlineHeight);

	CreateColorableImage(Data.m_Info, Data.m_InfoGrayscale);

	PrepareAnimFrames(pName, Data); // bestclient
	return true;
}

// bestclient
void CSkins::PrepareAnimFrames(const char *pName, CSkinLoadData &Data) const
{
	if(Data.m_vAnimFrames.empty())
	{
		return;
	}

	const int DivX = g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridx;
	const int DivY = g_pData->m_aSprites[SPRITE_TEE_BODY].m_pSet->m_Gridy;
	const size_t MaxWidth = (size_t)std::min(g_Config.m_ClSkinMaxWidth, g_Config.m_BcAnimSkinsMaxWidth);
	char aContextName[64];
	str_format(aContextName, sizeof(aContextName), "%s (animated)", pName);

	std::vector<CSkinLoadData::CAnimFrameData> vFrames;
	vFrames.reserve(Data.m_vAnimFrames.size());
	for(CSkinLoadData::CAnimFrameData &Frame : Data.m_vAnimFrames)
	{
		// Resize to the maximum width and snap to the skin grid ourselves, so we do not log a warning for every frame
		const float Scale = std::min(1.0f, MaxWidth / (float)Frame.m_Info.m_Width);
		const int TargetWidth = std::max(DivX, round_to_int(Frame.m_Info.m_Width * Scale / DivX) * DivX);
		const int TargetHeight = std::max(DivY, round_to_int(Frame.m_Info.m_Height * Scale / DivY) * DivY);
		if((size_t)TargetWidth != Frame.m_Info.m_Width || (size_t)TargetHeight != Frame.m_Info.m_Height)
		{
			// Spread the color of visible pixels into transparent ones, so resizing does not create dark edges
			DilateImage(Frame.m_Info);
			ResizeImage(Frame.m_Info, TargetWidth, TargetHeight);
		}
		if(!PrepareSkinImage(aContextName, Frame.m_Info, MaxWidth))
		{
			continue;
		}
		CreateColorableImage(Frame.m_Info, Frame.m_InfoGrayscale);
		vFrames.push_back(std::move(Frame));
	}

	if(vFrames.size() < 2)
	{
		log_error("skins", "Animated skin '%s' has less than 2 valid frames", pName);
		Data.FreeAnimFrames();
		return;
	}
	Data.m_vAnimFrames = std::move(vFrames);
}

bool CSkins::DecodeAnimatedSkin(const char *pName, const unsigned char *pData, size_t DataSize, CSkinLoadData &Data) const
{
	constexpr size_t MAX_DECODED_BYTES = 256 * 1024 * 1024;
	SMediaDecodedFrames DecodedFrames;
	if(Apng::IsAnimated(pData, DataSize))
	{
		// APNGs are decoded with libpng, FFmpeg builds often come without the PNG decoders
		std::vector<CApngFrame> vApngFrames;
		if(!Apng::Decode(pData, DataSize, pName, g_Config.m_BcAnimSkinsMaxFrames, MAX_DECODED_BYTES, vApngFrames))
		{
			log_error("skins", "Failed to decode animated PNG skin '%s'", pName);
			return false;
		}
		for(CApngFrame &ApngFrame : vApngFrames)
		{
			SMediaRawFrame Frame;
			Frame.m_Image = std::move(ApngFrame.m_Image);
			Frame.m_DurationMs = ApngFrame.m_DurationMs;
			DecodedFrames.m_vFrames.push_back(std::move(Frame));
		}
	}
	else
	{
		SMediaDecodeLimits Limits;
		// Decode in the original size, scaling the palette images with FFmpeg blends transparent pixels into the edges.
		// Frames are downscaled later like regular skins.
		Limits.m_MaxDimension = 1024;
		Limits.m_MaxFrames = g_Config.m_BcAnimSkinsMaxFrames;
		Limits.m_MaxTotalBytes = MAX_DECODED_BYTES;
		Limits.m_MaxAnimationDurationMs = g_Config.m_BcAnimSkinsMaxDuration;
		Limits.m_ExactTiming = true;
		if(!MediaDecoder::DecodeAnimatedImageCpu(Graphics(), pData, DataSize, pName, DecodedFrames, Limits))
		{
			log_error("skins", "Failed to decode animated skin '%s'", pName);
			return false;
		}
	}

	Data.FreeAnimFrames();
	if(!Data.m_Info.m_pData)
	{
		// Skin is only available as animation, use first frame as static skin
		Data.m_Info = DecodedFrames.m_vFrames.front().m_Image.DeepCopy();
	}
	if(DecodedFrames.m_vFrames.size() < 2 || !g_Config.m_BcAnimSkins)
	{
		return true;
	}
	for(SMediaRawFrame &DecodedFrame : DecodedFrames.m_vFrames)
	{
		CSkinLoadData::CAnimFrameData Frame;
		Frame.m_Info = std::move(DecodedFrame.m_Image);
		Frame.m_DurationMs = DecodedFrame.m_DurationMs;
		Data.m_vAnimFrames.push_back(std::move(Frame));
	}
	return true;
}

void CSkins::LoadLocalAnimatedSkin(const char *pName, bool Cached, const unsigned char *pPngData, size_t PngSize, CSkinLoadData &Data) const
{
	Data.FreeAnimFrames();
	if(!g_Config.m_BcAnimSkins && Data.m_Info.m_pData)
	{
		return;
	}

	// APNG stored with the regular .png extension
	if(Apng::IsAnimated(pPngData, PngSize) && DecodeAnimatedSkin(pName, pPngData, PngSize, Data) && !Data.m_vAnimFrames.empty())
	{
		Data.m_LocalAnimation = !Cached;
		return;
	}

	const auto &&TryLoadFile = [&](const char *pPath, int StorageType) {
		void *pFileData;
		unsigned FileSize;
		if(!Storage()->ReadFile(pPath, StorageType, &pFileData, &FileSize))
		{
			return false;
		}
		const bool Success = DecodeAnimatedSkin(pName, static_cast<const unsigned char *>(pFileData), FileSize, Data);
		free(pFileData);
		return Success;
	};

	char aPath[IO_MAX_PATH_LENGTH];
	if(!Cached)
	{
		for(const char *pExtension : {"gif", "apng", "webp"})
		{
			str_format(aPath, sizeof(aPath), "skins/%s.%s", pName, pExtension);
			if(TryLoadFile(aPath, IStorage::TYPE_ALL))
			{
				Data.m_LocalAnimation = true;
				return;
			}
		}
	}

	if(g_Config.m_BcAnimSkinsDownloadUrl[0] != '\0')
	{
		str_format(aPath, sizeof(aPath), "downloadedskins/%s.anim", pName);
		TryLoadFile(aPath, IStorage::TYPE_SAVE);
	}
}
// bestclient

// bestclient
void CSkins::LoadSkinTextures(const CImageInfo &Info, const CImageInfo &InfoGrayscale, CSkin::CSkinTextures &OriginalSkin, CSkin::CSkinTextures &ColorableSkin)
{
	// people load the jankiest skins, so we ignore empty sprites
	OriginalSkin.m_Body = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY]);
	OriginalSkin.m_BodyOutline = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE]);
	OriginalSkin.m_Feet = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT]);
	OriginalSkin.m_FeetOutline = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE]);
	OriginalSkin.m_Hands = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND]);
	OriginalSkin.m_HandsOutline = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND_OUTLINE]);
	for(size_t i = 0; i < std::size(OriginalSkin.m_aEyes); ++i)
	{
		OriginalSkin.m_aEyes[i] = Graphics()->LoadSpriteTexture(Info, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_EYE_NORMAL + i]);
	}

	ColorableSkin.m_Body = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY]);
	ColorableSkin.m_BodyOutline = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_BODY_OUTLINE]);
	ColorableSkin.m_Feet = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT]);
	ColorableSkin.m_FeetOutline = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_FOOT_OUTLINE]);
	ColorableSkin.m_Hands = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND]);
	ColorableSkin.m_HandsOutline = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_HAND_OUTLINE]);
	for(size_t i = 0; i < std::size(ColorableSkin.m_aEyes); ++i)
	{
		ColorableSkin.m_aEyes[i] = Graphics()->LoadSpriteTexture(InfoGrayscale, std::nullopt, &g_pData->m_aSprites[SPRITE_TEE_EYE_NORMAL + i]);
	}
}

std::shared_ptr<CSkinAnimation> CSkins::LoadSkinAnimationFinish(const char *pName, const CSkinLoadData &Data)
{
	if(Data.m_vAnimFrames.size() < 2)
	{
		return nullptr;
	}
	auto pAnimation = std::make_shared<CSkinAnimation>();
	pAnimation->m_vFrames.reserve(Data.m_vAnimFrames.size());
	for(const CSkinLoadData::CAnimFrameData &FrameData : Data.m_vAnimFrames)
	{
		CSkinAnimation::CFrame Frame;
		LoadSkinTextures(FrameData.m_Info, FrameData.m_InfoGrayscale, Frame.m_OriginalSkin, Frame.m_ColorableSkin);
		Frame.m_DurationMs = FrameData.m_DurationMs;
		pAnimation->AddFrame(std::move(Frame));
	}
	{
		char aDurations[512] = "";
		for(const CSkinAnimation::CFrame &Frame : pAnimation->m_vFrames)
		{
			char aDuration[16];
			str_format(aDuration, sizeof(aDuration), "%s%d", aDurations[0] == '\0' ? "" : " ", Frame.m_DurationMs);
			str_append(aDurations, aDuration);
		}
		log_debug("skins", "Loaded animation of skin '%s' with %d frames of %dx%d, durations (ms): %s", pName, (int)pAnimation->m_vFrames.size(),
			(int)Data.m_vAnimFrames.front().m_Info.m_Width, (int)Data.m_vAnimFrames.front().m_Info.m_Height, aDurations);
	}
	return pAnimation;
}

void CSkins::UnloadSkin(IGraphics *pGraphics, CSkin &Skin)
{
	Skin.m_OriginalSkin.Unload(pGraphics);
	Skin.m_ColorableSkin.Unload(pGraphics);
	if(Skin.m_pAnimation)
	{
		// render infos may still reference the animation, they fall back to the static textures
		Skin.m_pAnimation->Unload(pGraphics);
		Skin.m_pAnimation = nullptr;
	}
}
// bestclient

void CSkins::LoadSkinFinish(CSkinContainer *pSkinContainer, const CSkinLoadData &Data)
{
	CSkin Skin{pSkinContainer->Name()};

	LoadSkinTextures(Data.m_Info, Data.m_InfoGrayscale, Skin.m_OriginalSkin, Skin.m_ColorableSkin); // bestclient
	Skin.m_pAnimation = LoadSkinAnimationFinish(pSkinContainer->Name(), Data); // bestclient
	pSkinContainer->m_LocalAnimation = Data.m_LocalAnimation; // bestclient

	Skin.m_Metrics = Data.m_Metrics;
	Skin.m_BloodColor = Data.m_BloodColor;

	if(g_Config.m_Debug)
	{
		log_trace("skins", "Loaded skin '%s'", Skin.GetName());
	}

	auto SkinIt = m_Skins.find(pSkinContainer->Name());
	dbg_assert(SkinIt != m_Skins.end(), "LoadSkinFinish on skin '%s' which is not in m_Skins", pSkinContainer->Name());
	SkinIt->second->m_pSkin = std::make_unique<CSkin>(std::move(Skin));
	pSkinContainer->SetState(CSkinContainer::EState::LOADED);
}

void CSkins::LoadSkinDirect(const char *pName)
{
	if(m_Skins.contains(pName))
	{
		return;
	}
	CSkinContainer SkinContainer(this, pName, CSkinContainer::EType::LOCAL, IStorage::TYPE_ALL);
	auto &&pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
	pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
	const auto &[SkinIt, _] = m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)});

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "skins/%s.png", pName);
	CSkinLoadData DefaultSkinData;
	SkinIt->second->SetState(CSkinContainer::EState::LOADING);
	if(!Graphics()->LoadPng(DefaultSkinData.m_Info, aPath, SkinIt->second->StorageType()))
	{
		log_error("skins", "Failed to load PNG of skin '%s' from '%s'", pName, aPath);
		SkinIt->second->SetState(CSkinContainer::EState::ERROR);
	}
	else if(LoadSkinData(pName, DefaultSkinData))
	{
		LoadSkinFinish(SkinIt->second.get(), DefaultSkinData);
	}
	else
	{
		SkinIt->second->SetState(CSkinContainer::EState::ERROR);
	}
	DefaultSkinData.m_Info.Free();
	DefaultSkinData.m_InfoGrayscale.Free();
}

void CSkins::OnConsoleInit()
{
	ConfigManager()->RegisterCallback(CSkins::ConfigSaveCallback, this);
	Console()->Register("add_favorite_skin", "s[skin_name]", CFGFLAG_CLIENT, ConAddFavoriteSkin, this, "Add a skin as a favorite");
	Console()->Register("remove_favorite_skin", "s[skin_name]", CFGFLAG_CLIENT, ConRemFavoriteSkin, this, "Remove a skin from the favorites");

	Console()->Chain("player_skin", ConchainRefreshSkinList, this);
	Console()->Chain("dummy_skin", ConchainRefreshSkinList, this);
}

void CSkins::OnInit()
{
	RefreshEventSkins();

	// load skins
	Refresh([this]() {
		GameClient()->m_Menus.RenderLoading(Localize("Loading DDNet Client"), Localize("Loading skin files"), 0);
	});
}

void CSkins::OnShutdown()
{
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_pLoadJob)
		{
			pSkinContainer->m_pLoadJob->Abort();
			pSkinContainer->m_pLoadJob = nullptr;
		}
	}
	m_Skins.clear();
	m_HasPendingFinishedSkins = false;
}

void CSkins::OnUpdate()
{
	// Only update skins periodically to reduce FPS impact, unless we have finished skins pending GPU upload
	const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
	constexpr std::chrono::nanoseconds PeriodicInterval = std::chrono::milliseconds(50);
	if(!m_HasPendingFinishedSkins && m_ContainerUpdateTime.has_value() && StartTime - m_ContainerUpdateTime.value() < PeriodicInterval)
	{
		return;
	}
	m_ContainerUpdateTime = StartTime;

	// Update loaded state of managed skins which are not retrieved with the FindOrNullptr function
	GameClient()->CollectManagedTeeRenderInfos([&](const char *pSkinName) {
		// This will update the loaded state of the container
		dbg_assert(FindContainerOrNullptr(pSkinName) != nullptr, "No skin container found for managed tee render info: %s", pSkinName);
	});
	// Keep player and dummy skin loaded
	FindContainerOrNullptr(g_Config.m_ClPlayerSkin);
	FindContainerOrNullptr(g_Config.m_ClDummySkin);

	CSkinLoadingStats Stats = LoadingStats();
	UpdateUnloadSkins(Stats);
	UpdateStartLoading(Stats);
	UpdateFinishLoading(Stats, StartTime, std::chrono::microseconds(1500));
	UpdateAnimSkinDownloads(); // bestclient
}

// bestclient
void CSkins::AbortAnimSkinDownload(CSkinContainer &SkinContainer)
{
	if(!SkinContainer.m_pAnimDownloadJob)
	{
		return;
	}
	SkinContainer.m_pAnimDownloadJob->Abort();
	SkinContainer.m_pAnimDownloadJob = nullptr;
	// request it again when the skin is loaded the next time
	SkinContainer.m_pSkins->m_AnimSkinsRequested.erase(SkinContainer.Name());
}

void CSkins::UpdateAnimSkinDownloads()
{
	constexpr size_t MAX_RUNNING_DOWNLOADS = 4;
	const bool DownloadEnabled = g_Config.m_BcAnimSkins && g_Config.m_BcAnimSkinsDownloadUrl[0] != '\0';
	size_t NumRunning = 0;
	bool FinishedThisUpdate = false;

	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_pAnimDownloadJob)
		{
			if(!pSkinContainer->m_pAnimDownloadJob->Done())
			{
				NumRunning++;
				continue;
			}
			// upload at most one animation per update to avoid stutters
			if(FinishedThisUpdate)
			{
				continue;
			}
			FinishedThisUpdate = true;
			std::shared_ptr<CAnimSkinDownloadJob> pJob = std::exchange(pSkinContainer->m_pAnimDownloadJob, nullptr);
			if(pJob->State() == IJob::STATE_DONE &&
				pJob->m_Data.m_vAnimFrames.size() >= 2 &&
				pSkinContainer->m_State == CSkinContainer::EState::LOADED &&
				pSkinContainer->m_pSkin)
			{
				if(pSkinContainer->m_pSkin->m_pAnimation)
				{
					pSkinContainer->m_pSkin->m_pAnimation->Unload(Graphics());
				}
				pSkinContainer->m_pSkin->m_pAnimation = LoadSkinAnimationFinish(pSkinContainer->Name(), pJob->m_Data);
				m_Revision++;
				GameClient()->OnSkinUpdate(pSkinContainer->Name());
			}
			continue;
		}

		if(!DownloadEnabled ||
			NumRunning >= MAX_RUNNING_DOWNLOADS ||
			pSkinContainer->m_State != CSkinContainer::EState::LOADED ||
			!pSkinContainer->m_pSkin ||
			pSkinContainer->m_LocalAnimation ||
			m_AnimSkinsRequested.contains(pSkinContainer->Name()))
		{
			continue;
		}
		m_AnimSkinsRequested.emplace(pSkinContainer->Name());
		pSkinContainer->m_pAnimDownloadJob = std::make_shared<CAnimSkinDownloadJob>(this, pSkinContainer->Name());
		Engine()->AddJob(pSkinContainer->m_pAnimDownloadJob);
		NumRunning++;
	}
}

CSkins::CAnimSkinDownloadJob::CAnimSkinDownloadJob(CSkins *pSkins, const char *pName) :
	m_pSkins(pSkins)
{
	str_copy(m_aName, pName);
	Abortable(true);
}

CSkins::CAnimSkinDownloadJob::~CAnimSkinDownloadJob()
{
	m_Data.Free();
}

bool CSkins::CAnimSkinDownloadJob::Abort()
{
	if(!IJob::Abort())
	{
		return false;
	}

	const CLockScope LockScope(m_Lock);
	if(m_pGetRequest)
	{
		m_pGetRequest->Abort();
		m_pGetRequest = nullptr;
	}
	return true;
}

void CSkins::CAnimSkinDownloadJob::Run()
{
	char aEscapedName[256];
	EscapeUrl(aEscapedName, m_aName);

	char aUrl[IO_MAX_PATH_LENGTH];
	str_format(aUrl, sizeof(aUrl), "%s%s", g_Config.m_BcAnimSkinsDownloadUrl, aEscapedName);

	char aPathReal[IO_MAX_PATH_LENGTH];
	str_format(aPathReal, sizeof(aPathReal), "downloadedskins/%s.anim", m_aName);

	std::shared_ptr<IHttpRequest> pGet = HttpGetBoth(aUrl, m_pSkins->Storage(), aPathReal, IStorage::TYPE_SAVE);
	pGet->Timeout(CTimeout{10000, 0, 8192, 10});
	pGet->MaxResponseSize(8 * 1024 * 1024); // 8 MiB
	pGet->ValidateBeforeOverwrite(true);
	pGet->LogProgress(HTTPLOG::NONE);
	pGet->FailOnErrorStatus(false);
	{
		const CLockScope LockScope(m_Lock);
		m_pGetRequest = pGet;
	}
	m_pSkins->Http()->Run(pGet);
	pGet->Wait();
	{
		const CLockScope LockScope(m_Lock);
		m_pGetRequest = nullptr;
	}
	if(pGet->State() != EHttpState::DONE || State() == IJob::STATE_ABORTED)
	{
		return;
	}
	if(pGet->StatusCode() == 404)
	{
		// the animation was removed, so we do not want to show the cached one anymore next time
		m_pSkins->Storage()->RemoveFile(aPathReal, IStorage::TYPE_SAVE);
		return;
	}
	if(pGet->StatusCode() >= 400)
	{
		return;
	}
	if(pGet->StatusCode() == 304) // 304 Not Modified, the cached animation was already loaded together with the skin
	{
		pGet->OnValidation(true);
		return;
	}

	unsigned char *pResult;
	size_t ResultSize;
	pGet->Result(&pResult, &ResultSize);
	m_Data.m_Info.Free();
	const bool Success = m_pSkins->DecodeAnimatedSkin(m_aName, pResult, ResultSize, m_Data) && !m_Data.m_vAnimFrames.empty();
	pGet->OnValidation(Success);
	if(!Success || State() == IJob::STATE_ABORTED)
	{
		m_Data.FreeAnimFrames();
		return;
	}
	m_pSkins->PrepareAnimFrames(m_aName, m_Data);
	m_Data.m_Info.Free();
}
// bestclient

void CSkins::UpdateUnloadSkins(CSkinLoadingStats &Stats)
{
	if(Stats.m_NumPending + Stats.m_NumLoaded + Stats.m_NumLoading <= (size_t)g_Config.m_ClSkinsLoadedMax)
	{
		return;
	}

	const std::chrono::nanoseconds UnloadStart = time_get_nanoseconds();
	size_t NumToUnload = std::min(Stats.m_NumPending + Stats.m_NumLoaded + Stats.m_NumLoading - (size_t)g_Config.m_ClSkinsLoadedMax, (size_t)16);
	const size_t MaxSkipped = m_SkinsUsageList.size() / 8;
	size_t NumSkipped = 0;
	for(auto It = m_SkinsUsageList.rbegin(); It != m_SkinsUsageList.rend() && NumToUnload != 0 && NumSkipped < MaxSkipped; ++It)
	{
		auto SkinIt = m_Skins.find(*It);
		dbg_assert(SkinIt != m_Skins.end(), "m_SkinsUsageList contains skin not in m_Skins");
		auto &pSkinContainer = SkinIt->second;
		dbg_assert(!pSkinContainer->m_AlwaysLoaded, "m_SkinsUsageList contains skins with m_AlwaysLoaded");
		if(pSkinContainer->m_State != CSkinContainer::EState::PENDING &&
			pSkinContainer->m_State != CSkinContainer::EState::LOADED)
		{
			dbg_assert(pSkinContainer->m_State == CSkinContainer::EState::LOADING, "m_SkinsUsageList contains skin which is not PENDING, LOADING or LOADED");
			NumSkipped++;
			continue;
		}
		const std::chrono::nanoseconds TimeUnused = UnloadStart - pSkinContainer->m_LastLoadRequest.value();
		if(TimeUnused < (pSkinContainer->m_State == CSkinContainer::EState::LOADED ? MIN_UNLOAD_TIME_LOADED : MIN_UNLOAD_TIME_PENDING))
		{
			NumSkipped++;
			continue;
		}
		if(pSkinContainer->m_State == CSkinContainer::EState::LOADED)
		{
			UnloadSkin(Graphics(), *pSkinContainer->m_pSkin); // bestclient
			AbortAnimSkinDownload(*pSkinContainer); // bestclient
			pSkinContainer->m_pSkin = nullptr;
			m_Revision++;
			Stats.m_NumLoaded--;
		}
		else
		{
			Stats.m_NumPending--;
		}
		Stats.m_NumUnloaded++;
		pSkinContainer->SetState(CSkinContainer::EState::UNLOADED);
		NumToUnload--;
	}
}

void CSkins::UpdateStartLoading(CSkinLoadingStats &Stats)
{
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(Stats.m_NumPending == 0 || Stats.m_NumLoading + Stats.m_NumLoaded >= (size_t)g_Config.m_ClSkinsLoadedMax)
		{
			break;
		}
		if(pSkinContainer->m_State != CSkinContainer::EState::PENDING)
		{
			continue;
		}
		switch(pSkinContainer->Type())
		{
		case CSkinContainer::EType::LOCAL:
			pSkinContainer->m_pLoadJob = std::make_shared<CSkinLoadJob>(this, pSkinContainer->Name(), pSkinContainer->StorageType(), false);
			break;
		case CSkinContainer::EType::CACHED:
			pSkinContainer->m_pLoadJob = std::make_shared<CSkinLoadJob>(this, pSkinContainer->Name(), pSkinContainer->StorageType(), true);
			break;
		case CSkinContainer::EType::DOWNLOAD:
			if(pSkinContainer->m_CacheAvailable)
			{
				pSkinContainer->m_pLoadJob = std::make_shared<CSkinLoadJob>(this, pSkinContainer->Name(), pSkinContainer->StorageType(), true);
			}
			else
			{
				pSkinContainer->m_pLoadJob = std::make_shared<CSkinDownloadJob>(this, pSkinContainer->Name());
			}
			break;
		default:
			dbg_assert_failed("pSkinContainer->Type() invalid");
		}
		Engine()->AddJob(pSkinContainer->m_pLoadJob);
		pSkinContainer->SetState(CSkinContainer::EState::LOADING);
		Stats.m_NumPending--;
		Stats.m_NumLoading++;
	}
}

void CSkins::UpdateFinishLoading(CSkinLoadingStats &Stats, std::chrono::nanoseconds StartTime, std::chrono::nanoseconds MaxTime)
{
	m_HasPendingFinishedSkins = false;
	size_t LoadedThisFrame = 0;

	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(Stats.m_NumLoading == 0)
		{
			break;
		}
		if(pSkinContainer->m_State != CSkinContainer::EState::LOADING)
		{
			continue;
		}
		dbg_assert(pSkinContainer->m_pLoadJob != nullptr, "Skin container in loading state must have a load job");
		if(!pSkinContainer->m_pLoadJob->Done())
		{
			continue;
		}

		if(LoadedThisFrame >= 1 || time_get_nanoseconds() - StartTime >= MaxTime)
		{
			m_HasPendingFinishedSkins = true;
			break;
		}

		Stats.m_NumLoading--;
		if(pSkinContainer->m_pLoadJob->State() == IJob::STATE_DONE && pSkinContainer->m_pLoadJob->m_Data.m_Info.m_pData)
		{
			if(pSkinContainer->Type() == CSkinContainer::EType::DOWNLOAD)
			{
				pSkinContainer->m_CacheAvailable = true;
				m_LocalSkinListNeedsUpdate = true;
			}
			LoadSkinFinish(pSkinContainer.get(), pSkinContainer->m_pLoadJob->m_Data);
			m_Revision++;
			GameClient()->OnSkinUpdate(pSkinContainer->Name());
			pSkinContainer->m_pLoadJob = nullptr;
			Stats.m_NumLoaded++;
			LoadedThisFrame++;
		}
		else
		{
			if(pSkinContainer->m_pLoadJob->State() == IJob::STATE_DONE && pSkinContainer->m_pLoadJob->m_NotFound)
			{
				pSkinContainer->SetState(CSkinContainer::EState::NOT_FOUND);
				Stats.m_NumNotFound++;
			}
			else
			{
				pSkinContainer->SetState(CSkinContainer::EState::ERROR);
				Stats.m_NumError++;
			}
			pSkinContainer->m_pLoadJob = nullptr;
		}
	}
}

void CSkins::RefreshEventSkins()
{
	m_aEventSkinPrefix[0] = '\0';

	if(g_Config.m_Events)
	{
		if(time_season() == ETimeSeason::XMAS)
		{
			str_copy(m_aEventSkinPrefix, "santa");
		}
	}
	m_Revision++;
}

void CSkins::Refresh(TSkinLoadedCallback &&SkinLoadedCallback)
{
	m_Revision++;
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_pLoadJob)
		{
			pSkinContainer->m_pLoadJob->Abort();
			pSkinContainer->m_pLoadJob = nullptr;
		}
		if(pSkinContainer->m_pSkin)
		{
			UnloadSkin(Graphics(), *pSkinContainer->m_pSkin); // bestclient
		}
	}
	m_Skins.clear();
	m_AnimSkinsRequested.clear(); // bestclient
	m_SkinsUsageList.clear();
	m_vLocalSkins.clear();
	m_LocalSkinListNeedsUpdate = true;

	LoadSkinDirect("default");
	SkinLoadedCallback();

	CSkinScanUser SkinScanUser;
	SkinScanUser.m_pThis = this;
	SkinScanUser.m_SkinLoadedCallback = SkinLoadedCallback;
	SkinScanUser.m_CachedSkins = false;
	Storage()->ListDirectory(IStorage::TYPE_ALL, "skins", SkinScan, &SkinScanUser);
	SkinScanUser.m_CachedSkins = true;
	Storage()->ListDirectory(IStorage::TYPE_SAVE, "downloadedskins", SkinScan, &SkinScanUser);
}

CSkins::CSkinLoadingStats CSkins::LoadingStats() const
{
	CSkinLoadingStats Stats;
	for(const auto &[_, pSkinContainer] : m_Skins)
	{
		switch(pSkinContainer->m_State)
		{
		case CSkinContainer::EState::UNLOADED:
			Stats.m_NumUnloaded++;
			break;
		case CSkinContainer::EState::PENDING:
			Stats.m_NumPending++;
			break;
		case CSkinContainer::EState::LOADING:
			Stats.m_NumLoading++;
			break;
		case CSkinContainer::EState::LOADED:
			Stats.m_NumLoaded++;
			break;
		case CSkinContainer::EState::ERROR:
			Stats.m_NumError++;
			break;
		case CSkinContainer::EState::NOT_FOUND:
			Stats.m_NumNotFound++;
			break;
		}
	}
	return Stats;
}

CSkins::CSkinList &CSkins::SkinList()
{
	if(!m_SkinList.m_NeedsUpdate)
	{
		return m_SkinList;
	}

	m_SkinList.m_vSkins.clear();
	m_SkinList.m_UnfilteredCount = 0;

	// Ensure all favorite skins are present as skin containers so they are included in the next loop.
	for(const auto &FavoriteSkin : m_Favorites)
	{
		FindContainerOrNullptr(FavoriteSkin.c_str());
	}

	m_SkinList.m_vSkins.reserve(m_Skins.size());
	for(const auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->IsSpecial())
		{
			continue;
		}

		const bool SelectedMain = str_comp(pSkinContainer->Name(), g_Config.m_ClPlayerSkin) == 0;
		const bool SelectedDummy = str_comp(pSkinContainer->Name(), g_Config.m_ClDummySkin) == 0;
		const bool Favorite = IsFavorite(pSkinContainer->Name());
		if(pSkinContainer->m_CacheOnly && !SelectedMain && !SelectedDummy && !Favorite)
		{
			continue;
		}

		// Don't include skins in the list that couldn't be found in the database except the current player
		// and dummy skins to avoid showing a lot of not-found entries while the user is typing a skin name.
		if((pSkinContainer->m_State == CSkinContainer::EState::NOT_FOUND ||
			   (g_Config.m_ClVanillaSkinsOnly && !pSkinContainer->m_Vanilla)) &&
			!pSkinContainer->IsSpecial() &&
			!SelectedMain &&
			!SelectedDummy &&
			!Favorite)
		{
			continue;
		}
		m_SkinList.m_UnfilteredCount++;

		std::optional<std::pair<int, int>> NameMatch;
		if(g_Config.m_ClSkinFilterString[0] != '\0')
		{
			const char *pNameMatchEnd;
			const char *pNameMatchStart = str_utf8_find_nocase(pSkinContainer->Name(), g_Config.m_ClSkinFilterString, &pNameMatchEnd);
			if(pNameMatchStart == nullptr)
			{
				continue;
			}
			NameMatch = std::make_pair<int, int>(pNameMatchStart - pSkinContainer->Name(), pNameMatchEnd - pNameMatchStart);
		}
		m_SkinList.m_vSkins.emplace_back(pSkinContainer.get(), Favorite, SelectedMain, SelectedDummy, NameMatch);
	}

	std::sort(m_SkinList.m_vSkins.begin(), m_SkinList.m_vSkins.end());
	m_SkinList.m_NeedsUpdate = false;
	return m_SkinList;
}

const std::vector<CSkins::CSkinContainer *> &CSkins::LocalSkinList()
{
	if(!m_LocalSkinListNeedsUpdate)
	{
		return m_vLocalSkins;
	}

	m_vLocalSkins.clear();
	m_vLocalSkins.reserve(m_Skins.size());
	for(auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->Type() == CSkinContainer::EType::LOCAL ||
			pSkinContainer->Type() == CSkinContainer::EType::CACHED ||
			pSkinContainer->m_CacheAvailable)
		{
			m_vLocalSkins.push_back(pSkinContainer.get());
		}
	}
	std::sort(m_vLocalSkins.begin(), m_vLocalSkins.end(), [](const CSkinContainer *pLeft, const CSkinContainer *pRight) {
		return *pLeft < *pRight;
	});
	m_LocalSkinListNeedsUpdate = false;
	return m_vLocalSkins;
}

const CSkin *CSkins::FindLocalSkin(const char *pName)
{
	const auto SkinIt = m_Skins.find(pName);
	if(SkinIt == m_Skins.end())
		return nullptr;
	CSkinContainer *pContainer = SkinIt->second.get();
	if(pContainer->Type() == CSkinContainer::EType::DOWNLOAD && !pContainer->m_CacheAvailable)
		return nullptr;
	// bestclient
	if(pContainer->State() == CSkinContainer::EState::NOT_FOUND &&
		(pContainer->Type() == CSkinContainer::EType::LOCAL || pContainer->Type() == CSkinContainer::EType::CACHED))
	{
		pContainer->SetState(CSkinContainer::EState::UNLOADED);
	}
	// bestclient
	pContainer->RequestLoad();
	return pContainer->State() == CSkinContainer::EState::LOADED ? pContainer->Skin().get() : nullptr;
}

const CSkin *CSkins::Find(const char *pName)
{
	const auto *pSkin = FindOrNullptr(pName);
	if(pSkin == nullptr)
	{
		pSkin = FindOrNullptr("default");
	}
	if(pSkin == nullptr)
	{
		pSkin = &m_PlaceholderSkin;
	}
	return pSkin;
}

const CSkins::CSkinContainer *CSkins::FindContainerOrNullptr(const char *pName)
{
	const char *pSkinPrefix = SkinPrefix();
	if(pSkinPrefix[0] != '\0')
	{
		char aNameWithPrefix[2 * MAX_SKIN_LENGTH + 2]; // Larger than skin name length to allow IsValidName to check if it's too long
		str_format(aNameWithPrefix, sizeof(aNameWithPrefix), "%s_%s", pSkinPrefix, pName);
		// If we find something, use it, otherwise fall back to normal skins.
		const CSkinContainer *pSkinContainer = FindContainerImpl(aNameWithPrefix);
		if(pSkinContainer != nullptr && pSkinContainer->State() == CSkinContainer::EState::LOADED)
		{
			return pSkinContainer;
		}
	}
	return FindContainerImpl(pName);
}

const CSkins::CSkinContainer *CSkins::FindContainerImpl(const char *pName)
{
	if(!CSkin::IsValidName(pName))
	{
		return nullptr;
	}

	auto ExistingSkin = m_Skins.find(pName);
	if(ExistingSkin == m_Skins.end())
	{
		CSkinContainer SkinContainer(this, pName, CSkinContainer::EType::DOWNLOAD, IStorage::TYPE_SAVE);
		auto &&pSkinContainer = std::make_unique<CSkinContainer>(std::move(SkinContainer));
		pSkinContainer->SetState(pSkinContainer->DetermineInitialState());
		ExistingSkin = m_Skins.insert({pSkinContainer->Name(), std::move(pSkinContainer)}).first;
	}
	// bestclient
	if(ExistingSkin->second->IsSuppressedForGame())
	{
		return ExistingSkin->second.get();
	}
	// bestclient
	if(ExistingSkin->second->m_CacheOnly)
	{
		ExistingSkin->second->m_CacheOnly = false;
		m_SkinList.ForceRefresh();
	}
	ExistingSkin->second->RequestLoad();
	return ExistingSkin->second.get();
}

const CSkin *CSkins::FindOrNullptr(const char *pName)
{
	const CSkinContainer *pSkinContainer = FindContainerOrNullptr(pName);
	// bestclient
	if(pSkinContainer == nullptr ||
		pSkinContainer->IsSuppressedForGame() ||
		pSkinContainer->m_State != CSkinContainer::EState::LOADED)
	{
		return nullptr;
	}
	// bestclient
	return pSkinContainer->m_pSkin.get();
}

void CSkins::AddFavorite(const char *pName)
{
	if(!CSkin::IsValidName(pName))
	{
		log_error("skins", "Favorite skin name '%s' is not valid", pName);
		log_error("skins", "%s", CSkin::m_aSkinNameRestrictions);
		return;
	}

	const auto &[_, Inserted] = m_Favorites.emplace(pName);
	if(Inserted)
	{
		m_SkinList.ForceRefresh();
	}
}

void CSkins::RemoveFavorite(const char *pName)
{
	const auto FavoriteIt = m_Favorites.find(pName);
	if(FavoriteIt != m_Favorites.end())
	{
		m_Favorites.erase(FavoriteIt);
		m_SkinList.ForceRefresh();
	}
}

bool CSkins::IsFavorite(const char *pName) const
{
	return m_Favorites.contains(pName);
}

void CSkins::RandomizeSkin(int Dummy)
{
	static const float s_aSchemes[] = {1.0f / 2.0f, 1.0f / 3.0f, 1.0f / -3.0f, 1.0f / 12.0f, 1.0f / -12.0f}; // complementary, triadic, analogous
	const bool UseCustomColor = Dummy ? g_Config.m_ClDummyUseCustomColor : g_Config.m_ClPlayerUseCustomColor;
	if(UseCustomColor)
	{
		float GoalSat = random_float(0.3f, 1.0f);
		float MaxBodyLht = 1.0f - GoalSat * GoalSat; // max allowed lightness before we start losing saturation

		ColorHSLA Body;
		Body.h = random_float();
		Body.l = random_float(0.0f, MaxBodyLht);
		Body.s = std::clamp(GoalSat * GoalSat / (1.0f - Body.l), 0.0f, 1.0f);

		ColorHSLA Feet;
		Feet.h = std::fmod(Body.h + s_aSchemes[rand() % std::size(s_aSchemes)], 1.0f);
		Feet.l = random_float();
		Feet.s = std::clamp(GoalSat * GoalSat / (1.0f - Feet.l), 0.0f, 1.0f);

		unsigned *pColorBody = Dummy ? &g_Config.m_ClDummyColorBody : &g_Config.m_ClPlayerColorBody;
		unsigned *pColorFeet = Dummy ? &g_Config.m_ClDummyColorFeet : &g_Config.m_ClPlayerColorFeet;

		*pColorBody = Body.Pack(false);
		*pColorFeet = Feet.Pack(false);
	}

	std::vector<const CSkinContainer *> vpConsideredSkins;
	for(const auto &[_, pSkinContainer] : m_Skins)
	{
		if(pSkinContainer->m_CacheOnly ||
			(g_Config.m_ClVanillaSkinsOnly && !pSkinContainer->m_Vanilla) ||
			pSkinContainer->m_State == CSkinContainer::EState::ERROR ||
			pSkinContainer->m_State == CSkinContainer::EState::NOT_FOUND ||
			pSkinContainer->IsSpecial())
		{
			continue;
		}
		vpConsideredSkins.push_back(pSkinContainer.get());
	}
	const char *pRandomSkin;
	if(vpConsideredSkins.empty())
	{
		pRandomSkin = "default";
	}
	else
	{
		pRandomSkin = vpConsideredSkins[rand() % vpConsideredSkins.size()]->Name();
	}

	char *pSkinName = Dummy ? g_Config.m_ClDummySkin : g_Config.m_ClPlayerSkin;
	const size_t SkinNameSize = Dummy ? sizeof(g_Config.m_ClDummySkin) : sizeof(g_Config.m_ClPlayerSkin);
	str_copy(pSkinName, pRandomSkin, SkinNameSize);
	m_SkinList.ForceRefresh();
}

const char *CSkins::SkinPrefix() const
{
	if(g_Config.m_ClVanillaSkinsOnly)
	{
		return "";
	}
	if(m_aEventSkinPrefix[0] != '\0')
	{
		return m_aEventSkinPrefix;
	}
	return g_Config.m_ClSkinPrefix;
}

void CSkins::CSkinLoadJob::Run()
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "%s/%s.png", m_Cached ? "downloadedskins" : "skins", m_aName);
	// bestclient: read the file ourselves, so APNGs can be detected without reading it twice
	void *pPngData = nullptr;
	unsigned PngSize = 0;
	const bool PngFound = m_pSkins->Storage()->ReadFile(aPath, m_StorageType, &pPngData, &PngSize);
	if(PngFound && !m_pSkins->Graphics()->LoadPng(m_Data.m_Info, static_cast<uint8_t *>(pPngData), PngSize, aPath))
	{
		log_error("skins", "Failed to load PNG of skin '%s' from '%s'", m_aName, aPath);
	}
	if(State() == IJob::STATE_ABORTED)
	{
		free(pPngData);
		return;
	}
	m_pSkins->LoadLocalAnimatedSkin(m_aName, m_Cached, static_cast<const unsigned char *>(pPngData), PngSize, m_Data);
	free(pPngData);
	if(State() == IJob::STATE_ABORTED)
	{
		return;
	}
	if(m_Data.m_Info.m_pData)
	{
		m_pSkins->LoadSkinData(m_aName, m_Data);
	}
	else if(!PngFound)
	{
		log_error("skins", "Failed to load skin '%s', neither '%s' nor an animated version could be loaded", m_aName, aPath);
	}
}

CSkins::CSkinDownloadJob::CSkinDownloadJob(CSkins *pSkins, const char *pName) :
	CAbstractSkinLoadJob(pSkins, pName)
{
}

bool CSkins::CSkinDownloadJob::Abort()
{
	if(!CAbstractSkinLoadJob::Abort())
	{
		return false;
	}

	const CLockScope LockScope(m_Lock);
	if(m_pGetRequest)
	{
		m_pGetRequest->Abort();
		m_pGetRequest = nullptr;
	}
	return true;
}

void CSkins::CSkinDownloadJob::Run()
{
	const char *pBaseUrl = g_Config.m_ClDownloadCommunitySkins != 0 ? g_Config.m_ClSkinCommunityDownloadUrl : g_Config.m_ClSkinDownloadUrl;

	char aEscapedName[256];
	EscapeUrl(aEscapedName, m_aName);

	char aUrl[IO_MAX_PATH_LENGTH];
	str_format(aUrl, sizeof(aUrl), "%s%s.png", pBaseUrl, aEscapedName);

	char aPathReal[IO_MAX_PATH_LENGTH];
	str_format(aPathReal, sizeof(aPathReal), "downloadedskins/%s.png", m_aName);

	const CTimeout Timeout{10000, 0, 8192, 10};
	const size_t MaxResponseSize = 10 * 1024 * 1024; // 10 MiB

	std::shared_ptr<IHttpRequest> pGet = HttpGetBoth(aUrl, m_pSkins->Storage(), aPathReal, IStorage::TYPE_SAVE);
	pGet->Timeout(Timeout);
	pGet->MaxResponseSize(MaxResponseSize);
	pGet->ValidateBeforeOverwrite(true);
	pGet->LogProgress(HTTPLOG::NONE);
	pGet->FailOnErrorStatus(false);
	{
		const CLockScope LockScope(m_Lock);
		m_pGetRequest = pGet;
	}
	m_pSkins->Http()->Run(pGet);

	// Load existing file while waiting for the HTTP request
	{
		void *pPngData;
		unsigned PngSize;
		if(m_pSkins->Storage()->ReadFile(aPathReal, IStorage::TYPE_SAVE, &pPngData, &PngSize))
		{
			if(m_pSkins->Graphics()->LoadPng(m_Data.m_Info, static_cast<uint8_t *>(pPngData), PngSize, aPathReal))
			{
				if(State() == IJob::STATE_ABORTED)
				{
					free(pPngData);
					return;
				}
				m_pSkins->LoadLocalAnimatedSkin(m_aName, true, static_cast<const unsigned char *>(pPngData), PngSize, m_Data); // bestclient
				m_pSkins->LoadSkinData(m_aName, m_Data);
			}
			free(pPngData);
		}
	}

	pGet->Wait();
	{
		const CLockScope LockScope(m_Lock);
		m_pGetRequest = nullptr;
	}
	if(pGet->State() != EHttpState::DONE || State() == IJob::STATE_ABORTED || pGet->StatusCode() >= 400)
	{
		m_NotFound = pGet->State() == EHttpState::DONE && pGet->StatusCode() == 404; // 404 Not Found
		return;
	}
	if(pGet->StatusCode() == 304) // 304 Not Modified
	{
		bool Success = m_Data.m_Info.m_pData != nullptr;
		pGet->OnValidation(Success);
		if(Success)
		{
			return; // Local skin is up-to-date and was loaded successfully
		}

		log_error("skins", "Failed to load PNG of existing downloaded skin '%s' from '%s', downloading it again", m_aName, aPathReal);
		pGet = HttpGetBoth(aUrl, m_pSkins->Storage(), aPathReal, IStorage::TYPE_SAVE);
		pGet->Timeout(Timeout);
		pGet->MaxResponseSize(MaxResponseSize);
		pGet->ValidateBeforeOverwrite(true);
		pGet->SkipByFileTime(false);
		pGet->LogProgress(HTTPLOG::NONE);
		pGet->FailOnErrorStatus(false);
		{
			const CLockScope LockScope(m_Lock);
			m_pGetRequest = pGet;
		}
		m_pSkins->Http()->Run(pGet);
		pGet->Wait();
		{
			const CLockScope LockScope(m_Lock);
			m_pGetRequest = nullptr;
		}
		if(pGet->State() != EHttpState::DONE || State() == IJob::STATE_ABORTED || pGet->StatusCode() >= 400)
		{
			m_NotFound = pGet->State() == EHttpState::DONE && pGet->StatusCode() == 404; // 404 Not Found
			return;
		}
	}

	unsigned char *pResult;
	size_t ResultSize;
	pGet->Result(&pResult, &ResultSize);

	m_Data.Free(); // bestclient
	const bool Success = m_pSkins->Graphics()->LoadPng(m_Data.m_Info, pResult, ResultSize, aUrl);
	if(Success)
	{
		if(State() == IJob::STATE_ABORTED)
		{
			return;
		}
		m_pSkins->LoadLocalAnimatedSkin(m_aName, true, pResult, ResultSize, m_Data); // bestclient
		m_pSkins->LoadSkinData(m_aName, m_Data);
	}
	else
	{
		log_error("skins", "Failed to load PNG of skin '%s' downloaded from '%s' (size %" PRIzu ")", m_aName, aUrl, ResultSize);
	}
	pGet->OnValidation(Success);
}

void CSkins::ConAddFavoriteSkin(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CSkins *>(pUserData);
	pSelf->AddFavorite(pResult->GetString(0));
}

void CSkins::ConRemFavoriteSkin(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CSkins *>(pUserData);
	pSelf->RemoveFavorite(pResult->GetString(0));
}

void CSkins::ConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData)
{
	auto *pSelf = static_cast<CSkins *>(pUserData);
	pSelf->OnConfigSave(pConfigManager);
}

void CSkins::OnConfigSave(IConfigManager *pConfigManager)
{
	for(const auto &Favorite : m_Favorites)
	{
		char aBuffer[32 + MAX_SKIN_LENGTH];
		str_format(aBuffer, sizeof(aBuffer), "add_favorite_skin \"%s\"", Favorite.c_str());
		pConfigManager->WriteLine(aBuffer);
	}
}

void CSkins::ConchainRefreshSkinList(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CSkins *pThis = static_cast<CSkins *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		pThis->m_SkinList.ForceRefresh();
	}
}
