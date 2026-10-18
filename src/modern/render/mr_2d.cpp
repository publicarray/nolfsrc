// mr_2d.cpp -- the engine's 2D surfaces on the device: menus, the HUD, fonts,
// fades and the console all arrive here.
//
// As in the Mac renderer (r_CreateSurface, r_BlitToScreen, r_DrawImage,
// r_UpdateTexImage): a surface is CPU memory in the screen pixel format,
// which the engine draws into itself; drawing one to the screen uploads it
// to a texture when it has changed and draws a textured quad. The colour key
// becomes alpha 0 with black colour, so it vanishes under every blend mode
// with the alpha test on. Unlike the Mac renderer, the screen lock
// (FillRect and the engine's software paths onto the screen), BlitFromScreen
// and WarpToScreen work.

#include "mr.h"

struct MRSurface {
    uint32_t m_nWidth, m_nHeight;
    uint32_t m_nPitch;              // bytes
    uint8_t* m_pPixels;
    uint32_t   m_Tex;
    uint32_t m_nTexWidth, m_nTexHeight;   // power-of-two texture holding it
    uint32_t m_nTexGen;             // context the texture belongs to
    bool     m_bDirty;              // pixels changed since the upload
    bool     m_bTexKeyed;           // the upload's keying
    uint32_t m_nTexKey;
    // The MASK blend's texture (see mr_UploadMask): black, the colour's
    // brightness in alpha.
    uint32_t m_TexMask;
    uint32_t m_nMaskGen;
    bool     m_bMaskDirty;
    bool     m_bMaskKeyed;
    uint32_t m_nMaskKey;
};

static uint32_t mr_Pow2(uint32_t n)
{
    uint32_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

// ---- state ------------------------------------------------------------------

// Blend modes as both d3d.ren and the Mac renderer map them.
void mr_ApplyBlend(LTSurfaceBlend eBlend)
{
    static const RDBlend blend[7][2] = {
        { RD_SRC_ALPHA, RD_ONE_MINUS_SRC_ALPHA },   // ALPHA
        { RD_ONE,       RD_ZERO },                  // SOLID
        { RD_ONE,       RD_ONE },                   // ADD
        { RD_DST_COLOR, RD_ZERO },                  // MULTIPLY
        { RD_DST_COLOR, RD_SRC_COLOR },             // MULTIPLY2
        { RD_ZERO,      RD_ONE_MINUS_SRC_COLOR },   // MASK
        { RD_ONE,       RD_ONE_MINUS_SRC_COLOR },   // MASKADD
    };
    uint32_t i = (uint32_t)eBlend <= LTSURFACEBLEND_MASKADD ? (uint32_t)eBlend : 0;
    rd_Enable(RD_BLEND);
    rd_BlendFunc(blend[i][0], blend[i][1]);
}

// Screen pixels, top-left origin, nothing from the 3D state.
void mr_Begin2D(void)
{
    rd_PushAttrib(RD_ATTR_ALL);
    rd_MatrixMode(RD_PROJECTION);
    rd_PushMatrix();
    rd_LoadIdentity();
    rd_Ortho(0, g.nWidth, g.nHeight, 0, -1, 1);
    rd_MatrixMode(RD_MODELVIEW);
    rd_PushMatrix();
    rd_LoadIdentity();
    rd_MatrixMode(RD_TEXTURE);
    rd_PushMatrix();
    rd_LoadIdentity();
    rd_Viewport(0, 0, g.nWidth, g.nHeight);
    rd_Disable(RD_DEPTH_TEST);
    rd_DepthMask(false);
    rd_Disable(RD_FOG);
    rd_Disable(RD_SCISSOR_TEST);
}

void mr_End2D(void)
{
    rd_MatrixMode(RD_TEXTURE);
    rd_PopMatrix();
    rd_MatrixMode(RD_PROJECTION);
    rd_PopMatrix();
    rd_MatrixMode(RD_MODELVIEW);
    rd_PopMatrix();
    rd_PopAttrib();
}

LTBOOL mr_StartOptimized2D(void)
{
    if (!g.bContext || !g.bIn3D)
        return 0;
    g.bInOptimized2D = true;
    return 1;
}

void mr_EndOptimized2D(void)
{
    g.bInOptimized2D = false;
}

// ---- surfaces -------------------------------------------------------------------

void* mr_CreateSurface(uint32_t nWidth, uint32_t nHeight)
{
    if (!nWidth || !nHeight)
        return NULL;
    MRSurface* s = (MRSurface*)calloc(1, sizeof(MRSurface));
    if (!s)
        return NULL;
    s->m_nWidth = nWidth;
    s->m_nHeight = nHeight;
    s->m_nPitch = (nWidth * mr_BytesPerPixel() + 3) & ~3u;
    s->m_pPixels = (uint8_t*)calloc(1, s->m_nPitch * nHeight);
    if (!s->m_pPixels)
    {
        free(s);
        return NULL;
    }
    s->m_bDirty = s->m_bMaskDirty = true;
    return s;
}

static void mr_FreeSurfaceTexture(MRSurface* s)
{
    // A texture from an earlier context is gone with that context.
    if (s->m_Tex && s->m_nTexGen == g.nContextGen && g.bContext)
        rd_DeleteTexture(s->m_Tex);
    s->m_Tex = 0;
    if (s->m_TexMask && s->m_nMaskGen == g.nContextGen && g.bContext)
        rd_DeleteTexture(s->m_TexMask);
    s->m_TexMask = 0;
}

void mr_DeleteSurface(void* pSurface)
{
    MRSurface* s = (MRSurface*)pSurface;
    if (!s)
        return;
    mr_FreeSurfaceTexture(s);
    free(s->m_pPixels);
    free(s);
}

void mr_GetSurfaceInfo(void* pSurface, uint32_t* pWidth, uint32_t* pHeight, uint32_t* pPitch)
{
    MRSurface* s = (MRSurface*)pSurface;
    if (pWidth)  *pWidth  = s ? s->m_nWidth : 0;
    if (pHeight) *pHeight = s ? s->m_nHeight : 0;
    if (pPitch)  *pPitch  = s ? s->m_nPitch : 0;
}

// Any lock may write, so any lock makes the texture stale (the Mac sets its
// dirty byte here too).
void* mr_LockSurface(void* pSurface)
{
    MRSurface* s = (MRSurface*)pSurface;
    if (!s)
        return NULL;
    s->m_bDirty = s->m_bMaskDirty = true;
    return s->m_pPixels;
}

void mr_UnlockSurface(void* pSurface)
{
    (void)pSurface;
}

// Keying is done per blit from the request, so there is nothing to prepare;
// answering TRUE is what lets the engine's ILTClient::OptimizeSurface
// succeed.
LTBOOL mr_OptimizeSurface(void* pSurface, HLTCOLOR hTransColor)
{
    (void)hTransColor;
    return pSurface != NULL;
}

void mr_UnoptimizeSurface(void* pSurface)
{
    (void)pSurface;
}

// Bring the surface's texture up to date for a blit keyed (or not) on nKey.
static bool mr_UploadSurface(MRSurface* s, bool bKeyed, uint32_t nKey)
{
    if (s->m_Tex && s->m_nTexGen != g.nContextGen)
        s->m_Tex = 0;                           // made by an earlier context
    if (s->m_Tex && !s->m_bDirty && s->m_bTexKeyed == bKeyed && (!bKeyed || s->m_nTexKey == nKey))
        return true;

    uint32_t tw = mr_Pow2(s->m_nWidth), th = mr_Pow2(s->m_nHeight);
    if ((int)tw > g_RDCaps.nMaxTextureSize || (int)th > g_RDCaps.nMaxTextureSize)
        return false;
    uint32_t* pRGBA = (uint32_t*)calloc(tw * th, 4);    // padding: transparent black
    if (!pRGBA)
        return false;

    if (g.nBitDepth == 16)
        nKey &= 0xffff;
    else
        nKey &= 0x00ffffff;
    for (uint32_t y = 0; y < s->m_nHeight; y++)
    {
        const uint8_t* pRow = s->m_pPixels + y * s->m_nPitch;
        uint32_t* pOut = pRGBA + y * tw;
        if (g.nBitDepth == 16)
        {
            const uint16_t* p = (const uint16_t*)pRow;
            for (uint32_t x = 0; x < s->m_nWidth; x++)
                pOut[x] = (bKeyed && p[x] == nKey) ? 0 : mr_PixelToARGB(p[x]);
        }
        else
        {
            const uint32_t* p = (const uint32_t*)pRow;
            for (uint32_t x = 0; x < s->m_nWidth; x++)
            {
                uint32_t px = p[x] & 0x00ffffff;
                pOut[x] = (bKeyed && px == nKey) ? 0 : (px | 0xff000000);
            }
        }
    }

    if (!s->m_Tex || s->m_nTexWidth != tw || s->m_nTexHeight != th)
    {
        if (s->m_Tex)
            rd_DeleteTexture(s->m_Tex);
        s->m_Tex = rd_GenTexture();
        rd_BindTexture(s->m_Tex);
        rd_TexWrap(false, false);
        rd_TexMaxLevel(0);
        rd_TexImage(0, RD_FMT_RGBA8, tw, th, RD_PIX_BGRA, pRGBA);
        s->m_nTexWidth = tw;
        s->m_nTexHeight = th;
    }
    else
    {
        rd_BindTexture(s->m_Tex);
        rd_TexSubImage(0, 0, 0, tw, th, RD_PIX_BGRA, pRGBA);
    }
    free(pRGBA);
    s->m_nTexGen = g.nContextGen;
    s->m_bDirty = false;
    s->m_bTexKeyed = bKeyed;
    s->m_nTexKey = nKey;
    return true;
}

// The MASK blend (dest * (1 - src colour)) on a multisampled target: Intel
// Arc's driver drops whole pixels of what it draws with a ZERO or
// INV_SRC_COLOR-style factor there (fonts came out as outlines, in
// Direct3D 9, OpenGL and dgVoodoo alike), while plain alpha blending draws
// right. So the mask goes as black alpha-blended by the source's brightness,
// from a second texture holding that brightness in alpha: the same result
// for the grey fonts it is used for.
static bool mr_UploadMask(MRSurface* s, bool bKeyed, uint32_t nKey)
{
    if (s->m_TexMask && s->m_nMaskGen != g.nContextGen)
        s->m_TexMask = 0;
    if (s->m_TexMask && !s->m_bMaskDirty && s->m_bMaskKeyed == bKeyed && (!bKeyed || s->m_nMaskKey == nKey))
        return true;

    uint32_t tw = s->m_nTexWidth, th = s->m_nTexHeight;   // mr_UploadSurface's
    uint32_t* pRGBA = (uint32_t*)calloc(tw * th, 4);
    if (!pRGBA)
        return false;
    nKey &= g.nBitDepth == 16 ? 0xffff : 0x00ffffff;
    for (uint32_t y = 0; y < s->m_nHeight; y++)
    {
        const uint8_t* pRow = s->m_pPixels + y * s->m_nPitch;
        uint32_t* pOut = pRGBA + y * tw;
        for (uint32_t x = 0; x < s->m_nWidth; x++)
        {
            uint32_t px = g.nBitDepth == 16 ? ((const uint16_t*)pRow)[x] : ((const uint32_t*)pRow)[x] & 0x00ffffff;
            if (bKeyed && px == nKey)
                continue;
            uint32_t argb = g.nBitDepth == 16 ? mr_PixelToARGB(px) : px;
            uint32_t r = (argb >> 16) & 0xff, gg = (argb >> 8) & 0xff, b = argb & 0xff;
            uint32_t a = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
            pOut[x] = a << 24;
        }
    }
    if (!s->m_TexMask)
    {
        s->m_TexMask = rd_GenTexture();
        rd_BindTexture(s->m_TexMask);
        rd_TexWrap(false, false);
        rd_TexMaxLevel(0);
        rd_TexImage(0, RD_FMT_RGBA8, tw, th, RD_PIX_BGRA, pRGBA);
    }
    else
    {
        rd_BindTexture(s->m_TexMask);
        rd_TexSubImage(0, 0, 0, tw, th, RD_PIX_BGRA, pRGBA);
    }
    free(pRGBA);
    s->m_nMaskGen = g.nContextGen;
    s->m_bMaskDirty = false;
    s->m_bMaskKeyed = bKeyed;
    s->m_nMaskKey = nKey;
    return true;
}

// Draw [src] of a surface's texture onto four screen corners (x,y pairs, in
// the order top-left, top-right, bottom-right, bottom-left), under the
// Optimized-2D colour and blend when that bracket is open.
static void mr_DrawSurfaceQuad(MRSurface* s, const float* pSrc, const float* pCorners,
                                float fAlpha, bool bFilter, bool bMask)
{
    mr_Begin2D();
    rd_Enable(RD_TEXTURE_2D);
    rd_BindTexture(bMask ? s->m_TexMask : s->m_Tex);
    RDFilter filter = bFilter ? RD_LINEAR : RD_NEAREST;
    rd_TexFilter(filter, filter);
    rd_TexEnv(RD_ENV_MODULATE);
    rd_Enable(RD_ALPHA_TEST);
    rd_AlphaGreater(0.0f);

    float r = 1.0f, gg = 1.0f, b = 1.0f;
    if (g.bInOptimized2D)
    {
        r = ((g.hOptColor >> 16) & 0xff) / 255.0f;
        gg = ((g.hOptColor >> 8) & 0xff) / 255.0f;
        b = (g.hOptColor & 0xff) / 255.0f;
        mr_ApplyBlend(g.eBlend);
    }
    else
        mr_ApplyBlend(LTSURFACEBLEND_ALPHA);
    if (fAlpha < 0.0f) fAlpha = 0.0f;
    if (fAlpha > 1.0f) fAlpha = 1.0f;
    if (bMask)
    {
        rd_BlendFunc(RD_SRC_ALPHA, RD_ONE_MINUS_SRC_ALPHA);
        rd_Color4f(0, 0, 0, r > gg ? (r > b ? r : b) : (gg > b ? gg : b));
    }
    else
        rd_Color4f(r, gg, b, fAlpha);

    float u0 = pSrc[0] / s->m_nTexWidth, v0 = pSrc[1] / s->m_nTexHeight;
    float u1 = pSrc[2] / s->m_nTexWidth, v1 = pSrc[3] / s->m_nTexHeight;
    rd_Begin(RD_QUADS);
    rd_TexCoord2f(u0, v0); rd_Vertex2f(pCorners[0], pCorners[1]);
    rd_TexCoord2f(u1, v0); rd_Vertex2f(pCorners[2], pCorners[3]);
    rd_TexCoord2f(u1, v1); rd_Vertex2f(pCorners[4], pCorners[5]);
    rd_TexCoord2f(u0, v1); rd_Vertex2f(pCorners[6], pCorners[7]);
    rd_End();
    mr_End2D();
}

// DrawSurfaceToSurface[Transparent] and ScaleSurfaceToSurface[Transparent]
// onto the screen. Rects come clipped; a scaled blit stretches.
void mr_BlitToScreen(RenderBlitRequest* pRequest)
{
    if (!g.bContext || !pRequest || !pRequest->m_pSurfaceData || g.pScreenLock)
        return;
    MRSurface* s = (MRSurface*)pRequest->m_pSurfaceData;
    const LTRect* src = pRequest->m_pSrcRect;
    const LTRect* dst = pRequest->m_pDestRect;
    if (!src || !dst || src->right <= src->left || src->bottom <= src->top)
        return;
    mr_Debug(4, "blit %ux%u (%d,%d,%d,%d)->(%d,%d,%d,%d) key %u:%06x alpha %.2f 3d %d opt %d blend %d col %08x",
              s->m_nWidth, s->m_nHeight, src->left, src->top, src->right, src->bottom,
              dst->left, dst->top, dst->right, dst->bottom, pRequest->m_bTransparent,
              pRequest->m_TransColor, pRequest->m_fAlpha, g.bIn3D, g.bInOptimized2D, g.eBlend, g.hOptColor);
    if (!mr_UploadSurface(s, (pRequest->m_bTransparent & 1) != 0, pRequest->m_TransColor))
        return;

    float fSrc[4] = { (float)src->left, (float)src->top, (float)src->right, (float)src->bottom };
    float x0 = (float)dst->left, y0 = (float)dst->top, x1 = (float)dst->right, y1 = (float)dst->bottom;
    float fCorners[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    bool bScaled = (dst->right - dst->left) != (src->right - src->left) ||
                   (dst->bottom - dst->top) != (src->bottom - src->top);
    bool bMask = g.bInOptimized2D && g.eBlend == LTSURFACEBLEND_MASK && g_RDCaps.nSamples > 1 &&
                 mr_UploadMask(s, (pRequest->m_bTransparent & 1) != 0, pRequest->m_TransColor);
    mr_DrawSurfaceQuad(s, fSrc, fCorners, pRequest->m_fAlpha,
                        bScaled && cvi(CV_FilterOptimized), bMask);
}

// TransformSurfaceToSurface onto the screen: four warp points, the source
// corners onto the destination corners. Points 0..3 run around the quad.
LTBOOL mr_WarpToScreen(RenderBlitRequest* pRequest)
{
    if (!g.bContext || !pRequest || !pRequest->m_pSurfaceData || !pRequest->m_pPoints ||
        pRequest->m_nPoints != 4 || g.pScreenLock)
        return 0;
    MRSurface* s = (MRSurface*)pRequest->m_pSurfaceData;
    if (!mr_UploadSurface(s, (pRequest->m_bTransparent & 1) != 0, pRequest->m_TransColor))
        return 0;
    const LTWarpPt* p = pRequest->m_pPoints;
    mr_Begin2D();
    rd_Enable(RD_TEXTURE_2D);
    rd_BindTexture(s->m_Tex);
    rd_TexFilter(RD_LINEAR, RD_LINEAR);
    rd_TexEnv(RD_ENV_MODULATE);
    rd_Enable(RD_ALPHA_TEST);
    rd_AlphaGreater(0.0f);
    if (g.bInOptimized2D)
    {
        mr_ApplyBlend(g.eBlend);
        rd_Color4f(((g.hOptColor >> 16) & 0xff) / 255.0f, ((g.hOptColor >> 8) & 0xff) / 255.0f,
                  (g.hOptColor & 0xff) / 255.0f, pRequest->m_fAlpha);
    }
    else
    {
        mr_ApplyBlend(LTSURFACEBLEND_ALPHA);
        rd_Color4f(1, 1, 1, pRequest->m_fAlpha);
    }
    rd_Begin(RD_QUADS);
    for (int i = 0; i < 4; i++)
    {
        rd_TexCoord2f((p[i].source_x + 0.5f) / s->m_nTexWidth, (p[i].source_y + 0.5f) / s->m_nTexHeight);
        rd_Vertex2f(p[i].dest_x, p[i].dest_y);
    }
    rd_End();
    mr_End2D();
    return 1;
}

// ---- the screen as pixels ------------------------------------------------------------

// The engine's software paths onto the screen (FillRect, solid-colour and
// masked draws, text drawn straight to the screen) lock it, always whole.
// The back buffer is read into a top-down buffer in the screen format and
// written back on unlock.
LTBOOL mr_LockScreen(int left, int top, int right, int bottom, void** ppPixels, uint32_t* pPitch)
{
    mr_Debug(4, "lockscreen (%d,%d,%d,%d)", left, top, right, bottom);
    if (!g.bContext || g.pScreenLock || !ppPixels || !pPitch)
        return 0;
    uint32_t bpp = mr_BytesPerPixel();
    uint32_t nPitch = g.nWidth * bpp;
    uint8_t* pBuf = (uint8_t*)malloc(nPitch * g.nHeight);
    if (!pBuf)
        return 0;
    rd_ReadPixels(0, 0, g.nWidth, g.nHeight, bpp == 2 ? RD_PIX_565 : RD_PIX_BGRA, pBuf);
    if (bpp == 4)
    {
        uint32_t* p = (uint32_t*)pBuf;
        for (uint32_t i = 0; i < g.nWidth * g.nHeight; i++)
            p[i] &= 0x00ffffff;                 // X8R8G8B8: no alpha in the format
    }
    g.pScreenLock = pBuf;
    *ppPixels = pBuf;
    *pPitch = nPitch;
    return 1;
}

void mr_UnlockScreen(void)
{
    if (!g.pScreenLock)
        return;
    uint8_t* pBuf = g.pScreenLock;
    g.pScreenLock = NULL;
    if (!g.bContext)
    {
        free(pBuf);
        return;
    }
    uint32_t tw = mr_Pow2(g.nWidth), th = mr_Pow2(g.nHeight);
    uint32_t tex = rd_GenTexture();
    rd_BindTexture(tex);
    rd_TexFilter(RD_NEAREST, RD_NEAREST);
    rd_TexMaxLevel(0);
    if (g.nBitDepth == 16)
    {
        rd_TexImage(0, RD_FMT_RGB, tw, th, RD_PIX_565, NULL);
        rd_TexSubImage(0, 0, 0, g.nWidth, g.nHeight, RD_PIX_565, pBuf);
    }
    else
    {
        rd_TexImage(0, RD_FMT_RGB8, tw, th, RD_PIX_BGRA, NULL);
        rd_TexSubImage(0, 0, 0, g.nWidth, g.nHeight, RD_PIX_BGRA, pBuf);
    }
    free(pBuf);

    mr_Begin2D();
    rd_Enable(RD_TEXTURE_2D);
    rd_Disable(RD_BLEND);
    rd_Disable(RD_ALPHA_TEST);
    rd_TexEnv(RD_ENV_REPLACE);
    float u = (float)g.nWidth / tw, v = (float)g.nHeight / th;
    rd_Begin(RD_QUADS);
    rd_TexCoord2f(0, 0); rd_Vertex2f(0, 0);
    rd_TexCoord2f(u, 0); rd_Vertex2f((float)g.nWidth, 0);
    rd_TexCoord2f(u, v); rd_Vertex2f((float)g.nWidth, (float)g.nHeight);
    rd_TexCoord2f(0, v); rd_Vertex2f(0, (float)g.nHeight);
    rd_End();
    mr_End2D();
    rd_DeleteTexture(tex);
}

// The modern engine's FillRect on the screen (MODERN.md M-P0-03): a
// scissored clear instead of a whole-screen lock. nPixel is in the screen
// format.
LTBOOL mr_FillScreenRect(LTRect* pRect, uint32_t nPixel)
{
    if (pRect)
        mr_Debug(4, "fill (%d,%d,%d,%d) %08x", pRect->left, pRect->top, pRect->right, pRect->bottom, nPixel);
    if (!g.bContext || !pRect || g.pScreenLock)
        return 0;
    int l = pRect->left < 0 ? 0 : pRect->left, t = pRect->top < 0 ? 0 : pRect->top;
    int r = pRect->right > (int)g.nWidth ? (int)g.nWidth : pRect->right;
    int b = pRect->bottom > (int)g.nHeight ? (int)g.nHeight : pRect->bottom;
    if (r <= l || b <= t)
        return 1;
    uint32_t argb = mr_PixelToARGB(nPixel);
    rd_PushAttrib(RD_ATTR_COLOR | RD_ATTR_SCISSOR);
    rd_Enable(RD_SCISSOR_TEST);
    rd_Scissor(l, t, r - l, b - t);
    rd_ColorMask(true);
    rd_ClearColor(((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
                  (argb & 0xff) / 255.0f, 1.0f);
    rd_Clear(RD_CLEAR_COLOR);
    rd_PopAttrib();
    return 1;
}

// DrawSurfaceToSurface[Transparent] from the screen into a surface: the rect
// read back and converted, keyed pixels (on the screen's colour) left as
// they were.
void mr_BlitFromScreen(RenderBlitRequest* pRequest)
{
    if (!g.bContext || !pRequest || !pRequest->m_pSurfaceData || !pRequest->m_pSrcRect ||
        !pRequest->m_pDestRect || g.pScreenLock)
        return;
    MRSurface* s = (MRSurface*)pRequest->m_pSurfaceData;
    const LTRect* src = pRequest->m_pSrcRect;
    const LTRect* dst = pRequest->m_pDestRect;
    int w = src->right - src->left, h = src->bottom - src->top;
    if (w <= 0 || h <= 0)
        return;
    uint32_t bpp = mr_BytesPerPixel();
    uint8_t* pRead = (uint8_t*)malloc(w * h * bpp);
    if (!pRead)
        return;
    rd_ReadPixels(src->left, src->top, w, h, bpp == 2 ? RD_PIX_565 : RD_PIX_BGRA, pRead);
    bool bKeyed = (pRequest->m_bTransparent & 1) != 0;
    uint32_t nKey = pRequest->m_TransColor & (bpp == 2 ? 0xffff : 0x00ffffff);
    for (int y = 0; y < h; y++)
    {
        int dy = dst->top + y;
        if (dy < 0 || dy >= (int)s->m_nHeight)
            continue;
        const uint8_t* pIn = pRead + y * w * bpp;
        uint8_t* pOut = s->m_pPixels + dy * s->m_nPitch;
        for (int x = 0; x < w; x++)
        {
            int dx = dst->left + x;
            if (dx < 0 || dx >= (int)s->m_nWidth)
                continue;
            uint32_t px = bpp == 2 ? ((const uint16_t*)pIn)[x] : (((const uint32_t*)pIn)[x] & 0x00ffffff);
            if (bKeyed && px == nKey)
                continue;
            if (bpp == 2)
                ((uint16_t*)pOut)[dx] = (uint16_t)px;
            else
                ((uint32_t*)pOut)[dx] = px;
        }
    }
    free(pRead);
    s->m_bDirty = s->m_bMaskDirty = true;
}
