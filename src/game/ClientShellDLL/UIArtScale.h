// UIArtScale.h: support for upscaled interface art (HD UI packs).
//
// The menus scale interface photos with the page (P3_MenuScale). An HD pack
// ships them at an integer multiple of the retail size (48px icons, 180px
// mission photos); UIArtFactor finds that multiple so the page scale can be
// divided by it and the art keeps its retail layout size.

#ifndef __UIARTSCALE_H__
#define __UIARTSCALE_H__

// Integer upscale factor of hSurf relative to a retail width, or 1.
inline int UIArtFactor(HSURFACE hSurf, uint32 nRetailWidth)
{
	if (!hSurf || !nRetailWidth) return 1;
	uint32 w = 0, h = 0;
	g_pLTClient->GetSurfaceDims(hSurf, &w, &h);
	if (w > nRetailWidth && (w % nRetailWidth) == 0)
		return (int)(w / nRetailWidth);
	return 1;
}

// The factor of a selection-folder photo: a 48px icon, else a 180px photo.
inline int UIArtPhotoFactor(HSURFACE hSurf)
{
	int f = UIArtFactor(hSurf, 48);
	return (f > 1) ? f : UIArtFactor(hSurf, 180);
}

// UIArtScale: how big the choosers draw their 48px icons, as a multiple of
// the retail size (capped at the screen's height ratio; 1 is retail).
inline float UIArtUserScale(float fYRatio)
{
	static VarTrack s_vtUIArtScale;
	if (!s_vtUIArtScale.IsInitted())
		s_vtUIArtScale.Init(g_pLTClient, "UIArtScale", LTNULL, 1.5f);
	float f = s_vtUIArtScale.GetFloat(1.5f);
	if (f > fYRatio) f = fYRatio;
	return (f < 1.0f) ? 1.0f : f;
}

// Draw hSrc at (x, y) sized as if it were nRetailWidth wide, times fScale.
inline void UIArtDraw(HSURFACE hDest, HSURFACE hSrc, int x, int y, uint32 nRetailWidth,
					  float fScale, LTBOOL bTransparent, HLTCOLOR hTrans)
{
	int f = UIArtFactor(hSrc, nRetailWidth);
	if (f <= 1 && fScale == 1.0f)
	{
		if (bTransparent)
			g_pLTClient->DrawSurfaceToSurfaceTransparent(hDest, hSrc, LTNULL, x, y, hTrans);
		else
			g_pLTClient->DrawSurfaceToSurface(hDest, hSrc, LTNULL, x, y);
		return;
	}
	uint32 w = 0, h = 0;
	g_pLTClient->GetSurfaceDims(hSrc, &w, &h);
	LTRect rect(x, y, x + (int)((float)(w / f) * fScale), y + (int)((float)(h / f) * fScale));
	if (bTransparent)
		g_pLTClient->ScaleSurfaceToSurfaceTransparent(hDest, hSrc, &rect, LTNULL, hTrans);
	else
		g_pLTClient->ScaleSurfaceToSurface(hDest, hSrc, &rect, LTNULL);
}

#endif
