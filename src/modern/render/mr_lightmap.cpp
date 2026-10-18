// mr_lightmap.cpp -- the world's lightmaps as atlas textures.
//
// Every lightmap, the static ones included, is a light animation: the static
// ones are frame 0 of "LightAnim_BASE", compressed 5:5:5 runs of at most
// 32x32 texels per poly (docs/world.md part C). Both shipped renderers pack
// them into 64x64 pages at world bind (r_CreateLMPages) and recomposite a
// poly's lightmap when an animation that touches it changes
// (r_UpdatePolyLightmap). Here the pages are bigger atlases, each lightmap
// gets a one-texel border copied from its edge (so bilinear filtering never
// reaches a neighbour, which the 64x64 pages allowed), and the vertex
// lightmap coordinates are computed at draw time from the same basis.
//
// LightmapSmooth (default 4; 1 is the shipped renderers' look) stores each
// lightmap that many times larger, resampled with a cubic B-spline: the
// lighting the level was built with, its 32x32-at-most texels no longer
// drawn as bilinear blocks and its 5-bit steps no longer as bands. It takes
// effect when a world is bound.

#include <math.h>

#include "mrscene.h"

#define ATLAS_SIZE 512                  // at LightmapSmooth 1; times the scale
#define LM_MAXSCALE 8

// ---- the engine's data -------------------------------------------------------------

LMAnim* mr_FindLMAnim(MainWorld* pWorld, const char* pName)
{
    LMAnim* pAnims = (LMAnim*)pWorld->m_LMAnims.m_pArray;
    for (uint32_t i = 0; pAnims && i < pWorld->m_LMAnims.m_nElements; i++)
        if (!_stricmp(pAnims[i].m_Name, pName))
            return &pAnims[i];
    return NULL;
}

WorldPoly* mr_GetLMPoly(MainWorld* pWorld, const LMPolyRef& ref)
{
    if (ref.m_iWorldModel >= pWorld->m_Worlds.m_nElements)
        return NULL;
    WorldData* pData = mw_World(pWorld, ref.m_iWorldModel);
    if (!pData || !pData->m_pWorldBsp || ref.m_iPoly >= pData->m_pWorldBsp->m_nPolies)
        return NULL;
    return pData->m_pWorldBsp->m_Polies[ref.m_iPoly];
}

// DecompressLMData (Mac 0xbacf0, d3d.ren 0x1006a790): 16-bit words, a set
// top bit meaning "repeat the low 15 bits the next byte's count of times".
static bool mr_DecompressLM(const uint8_t* p, uint32_t nSize, uint16_t* pOut)
{
    const uint8_t* pEnd = p + nSize;
    int nLeft = LM_MAXTEXELS;
    while (p + 1 < pEnd)
    {
        uint16_t w = (uint16_t)(p[0] | p[1] << 8);
        p += 2;
        int n = 1;
        if (w & 0x8000)
        {
            if (p >= pEnd)
                return false;
            n = *p++;
            w &= 0x7fff;
        }
        if ((nLeft -= n) < 0)
            return false;
        while (n--)
            *pOut++ = w;
    }
    return true;
}

// The two 8-bit shadow-map layers (DecompressShadowMap): alternating runs of
// 0 and 255, starting with 0.
static bool mr_DecompressShadowMap(const uint8_t* p, uint32_t nSize, uint8_t* pOut)
{
    uint8_t* pStart = pOut;
    int idx = 0;
    while (nSize--)
    {
        int n = *p++;
        if (pOut + n > pStart + LM_MAXTEXELS)
            return false;
        memset(pOut, idx ? 0xff : 0x00, n);
        pOut += n;
        idx ^= 1;
    }
    return true;
}

// ---- the lightmap basis (C.3) ------------------------------------------------------------

// GetLMPlaneType's six reference axes: the "up" of each of the six plane
// types (lightmap_planes.cpp). The type is in the surface flags' top 3 bits.
static const LTVector g_LMUp[6] = {
    { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 },
};

// Surface::GetLightmapTextureVectors: P = up x N, Q = N x P, unnormalised,
// exactly as the lightmaps were made.
void mr_LMVectors(const Surface* pSurf, LTVector* pP, LTVector* pQ)
{
    uint32_t nType = pSurf->m_Flags >> 29;
    const LTVector& A = g_LMUp[nType < 6 ? nType : 0];
    const LTVector& N = pSurf->m_pPlane->m_Normal;
    pP->x = A.y * N.z - A.z * N.y;
    pP->y = A.z * N.x - A.x * N.z;
    pP->z = A.x * N.y - A.y * N.x;
    pQ->x = N.y * pP->z - N.z * pP->y;
    pQ->y = N.z * pP->x - N.x * pP->z;
    pQ->z = N.x * pP->y - N.y * pP->x;
}

// ---- composing one poly's lightmap (r_UpdatePolyLightmap) -------------------------------

static inline uint16_t mr_Lerp555(uint16_t a, uint16_t b, int t)
{
    int r = (a >> 10) & 31, gg = (a >> 5) & 31, bl = a & 31;
    int r2 = (b >> 10) & 31, g2 = (b >> 5) & 31, b2 = b & 31;
    r += ((r2 - r) * t) >> 6;
    gg += ((g2 - gg) * t) >> 6;
    bl += ((b2 - bl) * t) >> 6;
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    gg = gg < 0 ? 0 : gg > 31 ? 31 : gg;
    bl = bl < 0 ? 0 : bl > 31 ? 31 : bl;
    return (uint16_t)(r << 10 | gg << 5 | bl);
}

// One animation's layer of the poly, frames blended (r_SetupPolyLMAnim).
static bool mr_SetupPolyLMAnim(LMAnim* a, const PolyAnimRef& r, uint32_t n, uint16_t* pOut)
{
    if (r.m_iPoly >= a->m_nPolies)
        return false;
    const LMFrameEntry& A = a->m_Frames[a->m_iFrames[0]][r.m_iPoly];
    const LMFrameEntry& B = a->m_Frames[a->m_iFrames[1]][r.m_iPoly];
    if (a->m_iFrames[0] == a->m_iFrames[1] || a->m_fPercentBetween < 1.0f / 32)
        return mr_DecompressLM(A.m_pData, A.m_nSize, pOut);
    if (a->m_fPercentBetween > 31.0f / 32)
        return mr_DecompressLM(B.m_pData, B.m_nSize, pOut);
    uint16_t tb[LM_MAXTEXELS];
    if (!mr_DecompressLM(A.m_pData, A.m_nSize, pOut) || !mr_DecompressLM(B.m_pData, B.m_nSize, tb))
        return false;
    int t = (int)(a->m_fPercentBetween * 64);
    for (uint32_t i = 0; i < n; i++)
        pOut[i] = mr_Lerp555(pOut[i], tb[i], t);
    return true;
}

// A shadow-map animation's layer: its two 8-bit masks blended, lit by the
// animation's own light (r_SetupPolyShadowMap).
static bool mr_SetupPolyShadowMap(MainWorld* pWorld, WorldPoly* pPoly, LMAnim* a,
                                   const PolyAnimRef& r, uint16_t* pOut)
{
    if (r.m_iPoly >= a->m_nPolies)
        return false;
    uint32_t w = pPoly->m_LMWidth, h = pPoly->m_LMHeight, n = w * h;
    uint8_t la[LM_MAXTEXELS], lb[LM_MAXTEXELS];
    const LMFrameEntry& A = a->m_Frames[a->m_iFrames[0]][r.m_iPoly];
    const LMFrameEntry& B = a->m_Frames[a->m_iFrames[1]][r.m_iPoly];
    if (!mr_DecompressShadowMap(A.m_pData, A.m_nSize, la))
        return false;
    if (a->m_iFrames[0] != a->m_iFrames[1] && a->m_fPercentBetween >= 1.0f / 32)
    {
        if (!mr_DecompressShadowMap(B.m_pData, B.m_nSize, lb))
            return false;
        int t = (int)(a->m_fPercentBetween * 64);
        for (uint32_t i = 0; i < n; i++)
            la[i] = (uint8_t)(la[i] + (((lb[i] - la[i]) * t) >> 6));
    }

    // Each texel's world position: O + x*stepU + y*stepV (C.4).
    LTVector P, Q;
    mr_LMVectors(pPoly->m_pSurface, &P, &Q);
    float grid = pWorld->m_LMGridSize;
    float pp = P.x * P.x + P.y * P.y + P.z * P.z, qq = Q.x * Q.x + Q.y * Q.y + Q.z * Q.z;
    LTVector su = { P.x * grid / pp, P.y * grid / pp, P.z * grid / pp };
    LTVector sv = { Q.x * grid / qq, Q.y * grid / qq, Q.z * grid / qq };
    float r2 = a->m_fLightRadius * a->m_fLightRadius;
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t i = y * w + x;
            float wx = pPoly->m_LMOrigin.x + x * su.x + y * sv.x - a->m_vLightPos.x;
            float wy = pPoly->m_LMOrigin.y + x * su.y + y * sv.y - a->m_vLightPos.y;
            float wz = pPoly->m_LMOrigin.z + x * su.z + y * sv.z - a->m_vLightPos.z;
            float d2 = wx * wx + wy * wy + wz * wz;
            uint16_t out = 0;
            if (la[i] && d2 < r2 && r2 > 0.0f)
            {
                // The mask, the falloff and the colour as the dynamic-light
                // code takes it, 2c - 255 (r_SetupPolyShadowMap).
                float k = (la[i] / 255.0f) * (1.0f - d2 / r2);
                int c[3] = { (int)((2.0f * a->m_vLightColor.x - 255.0f) * k),
                             (int)((2.0f * a->m_vLightColor.y - 255.0f) * k),
                             (int)((2.0f * a->m_vLightColor.z - 255.0f) * k) };
                for (int j = 0; j < 3; j++)
                    c[j] = c[j] < 0 ? 0 : c[j] > 255 ? 255 : c[j] >> 3;
                out = (uint16_t)(c[0] << 10 | c[1] << 5 | c[2]);
            }
            pOut[i] = out;
        }
    }
    return true;
}

// The poly's whole lightmap: every animation that touches it, added
// (r_AddLightmapPixels; the first layer is not scaled, as in both
// renderers). Returns false when the poly has none.
static bool mr_ComposePolyLM(MainWorld* pWorld, WorldPoly* pPoly, uint16_t* pAccum)
{
    uint32_t n = pPoly->m_LMWidth * pPoly->m_LMHeight;
    if (!n || n > LM_MAXTEXELS)
        return false;
    const PolyAnimRef* pRefs = (const PolyAnimRef*)pPoly->m_pData0c;
    uint32_t nRefs = pPoly->m_nData10;
    LMAnim* pAnims = (LMAnim*)pWorld->m_LMAnims.m_pArray;
    uint32_t nAnims = pWorld->m_LMAnims.m_nElements;
    int nDone = 0;
    uint16_t layer[LM_MAXTEXELS];
    for (uint32_t i = 0; pRefs && i < nRefs; i++)
    {
        if (pRefs[i].m_iAnim >= nAnims)
            return false;
        LMAnim* a = &pAnims[pRefs[i].m_iAnim];
        if (a->m_iFrames[0] >= a->m_nFrames || a->m_iFrames[1] >= a->m_nFrames)
            continue;                   // the light is off
        if (a->m_fBlendPercent < 0.02f)
            continue;
        bool bOK = a->m_bShadowMap ? mr_SetupPolyShadowMap(pWorld, pPoly, a, pRefs[i], layer)
                                   : mr_SetupPolyLMAnim(a, pRefs[i], n, layer);
        if (!bOK)
            return false;
        if (!nDone)
            memcpy(pAccum, layer, n * 2);
        else
        {
            int s = (int)(a->m_fBlendPercent * 64);
            for (uint32_t t = 0; t < n; t++)
            {
                uint16_t d = pAccum[t], l = layer[t];
                int r = ((d >> 10) & 31) + ((((l >> 10) & 31) * s) >> 6);
                int gg = ((d >> 5) & 31) + ((((l >> 5) & 31) * s) >> 6);
                int b = (d & 31) + (((l & 31) * s) >> 6);
                pAccum[t] = (uint16_t)((r > 31 ? 31 : r) << 10 | (gg > 31 ? 31 : gg) << 5 | (b > 31 ? 31 : b));
            }
        }
        nDone++;
    }
    if (!nDone)
        memset(pAccum, 0, n * 2);
    return true;
}

// The cubic B-spline's four weights at fraction t: smooth (no overshoot,
// so no rings round a light's edge), at the cost of a little softening.
static inline void mr_BSpline(float t, float* w)
{
    float t2 = t * t, t3 = t2 * t;
    w[0] = (1.0f - 3.0f * t + 3.0f * t2 - t3) / 6.0f;
    w[1] = (4.0f - 6.0f * t2 + 3.0f * t3) / 6.0f;
    w[2] = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) / 6.0f;
    w[3] = t3 / 6.0f;
}

// A poly's texels resampled s times larger into its atlas place, with the
// one-texel border: each atlas texel's centre at lightmap texel
// (X + 0.5) / s - 0.5, the 4x4 texels round it weighted, the lightmap's
// edge texels standing in past its edge.
static void mr_StoreScaledLM(const MRPolyLM* pLM, uint32_t w, uint32_t h, const uint16_t* pTexels, uint32_t s)
{
    MRLMAtlas* pAtlas = pLM->m_pAtlas;
    static float rgb[LM_MAXTEXELS][3];
    for (uint32_t i = 0; i < w * h; i++)
    {
        uint16_t c = pTexels[i];
        rgb[i][0] = (float)((c >> 10) & 31) * (255.0f / 31.0f);
        rgb[i][1] = (float)((c >> 5) & 31) * (255.0f / 31.0f);
        rgb[i][2] = (float)(c & 31) * (255.0f / 31.0f);
    }
    int W = (int)(w * s), H = (int)(h * s);
    for (int Y = -1; Y <= H; Y++)
    {
        int cy = Y < 0 ? 0 : Y >= H ? H - 1 : Y;
        float fy = ((float)cy + 0.5f) / (float)s - 0.5f;
        int iy = (int)floorf(fy);
        float wy[4];
        mr_BSpline(fy - (float)iy, wy);
        uint8_t* pRow = pAtlas->m_pRGB + ((pLM->m_Y + Y) * pAtlas->m_nSize + pLM->m_X - 1) * 3;
        for (int X = -1; X <= W; X++)
        {
            int cx = X < 0 ? 0 : X >= W ? W - 1 : X;
            float fx = ((float)cx + 0.5f) / (float)s - 0.5f;
            int ix = (int)floorf(fx);
            float wx[4];
            mr_BSpline(fx - (float)ix, wx);
            float acc[3] = { 0.0f, 0.0f, 0.0f };
            for (int j = 0; j < 4; j++)
            {
                int sy = iy - 1 + j;
                sy = sy < 0 ? 0 : sy >= (int)h ? (int)h - 1 : sy;
                for (int i = 0; i < 4; i++)
                {
                    int sx = ix - 1 + i;
                    sx = sx < 0 ? 0 : sx >= (int)w ? (int)w - 1 : sx;
                    float k = wx[i] * wy[j];
                    const float* t = rgb[sy * w + sx];
                    acc[0] += t[0] * k;
                    acc[1] += t[1] * k;
                    acc[2] += t[2] * k;
                }
            }
            for (int k = 0; k < 3; k++)
                pRow[k] = (uint8_t)(acc[k] < 0.0f ? 0 : acc[k] > 255.0f ? 255 : (int)(acc[k] + 0.5f));
            pRow += 3;
        }
    }
    pAtlas->m_bDirty = true;
    if (pLM->m_Y - 1u < pAtlas->m_DirtyY0)
        pAtlas->m_DirtyY0 = pLM->m_Y - 1;
    if (pLM->m_Y + H + 1 > pAtlas->m_DirtyY1)
        pAtlas->m_DirtyY1 = pLM->m_Y + H + 1;
}

// Write a poly's 5:5:5 texels into its atlas, with the one-texel border.
static void mr_StorePolyLM(const MRPolyLM* pLM, uint32_t w, uint32_t h, const uint16_t* pTexels)
{
    MRLMAtlas* pAtlas = pLM->m_pAtlas;
    if (pAtlas->m_nScale > 1)
    {
        mr_StoreScaledLM(pLM, w, h, pTexels, pAtlas->m_nScale);
        return;
    }
    for (int y = -1; y <= (int)h; y++)
    {
        int sy = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
        uint8_t* pRow = pAtlas->m_pRGB + ((pLM->m_Y + y) * pAtlas->m_nSize + pLM->m_X - 1) * 3;
        for (int x = -1; x <= (int)w; x++)
        {
            int sx = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
            uint16_t c = pTexels[sy * w + sx];
            pRow[0] = (uint8_t)(((c >> 10) & 31) * 255 / 31);
            pRow[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31);
            pRow[2] = (uint8_t)((c & 31) * 255 / 31);
            pRow += 3;
        }
    }
    pAtlas->m_bDirty = true;
    if (pLM->m_Y - 1u < pAtlas->m_DirtyY0)
        pAtlas->m_DirtyY0 = pLM->m_Y - 1;
    if (pLM->m_Y + h + 1 > pAtlas->m_DirtyY1)
        pAtlas->m_DirtyY1 = pLM->m_Y + h + 1;
}

// ---- packing --------------------------------------------------------------------------

static MRLMAtlas* mr_NewAtlas(MRContext* pContext, uint32_t nScale)
{
    MRLMAtlas* pAtlas = (MRLMAtlas*)calloc(1, sizeof(MRLMAtlas));
    if (!pAtlas)
        return NULL;
    pAtlas->m_nScale = nScale;
    pAtlas->m_nSize = ATLAS_SIZE * nScale;
    pAtlas->m_pRGB = (uint8_t*)calloc((size_t)pAtlas->m_nSize * pAtlas->m_nSize, 3);
    if (!pAtlas->m_pRGB)
    {
        free(pAtlas);
        return NULL;
    }
    pAtlas->m_pNext = pContext->m_pAtlases;
    pContext->m_pAtlases = pAtlas;
    return pAtlas;
}

// Shelf packing: rows of lightmaps, left to right, a new row when one is
// full, a new atlas when the rows are.
static bool mr_Place(MRContext* pContext, uint32_t w, uint32_t h, uint32_t nScale, MRPolyLM* pLM)
{
    uint32_t bw = w * nScale + 2, bh = h * nScale + 2;
    MRLMAtlas* pAtlas = pContext->m_pAtlases;
    if (pAtlas && pAtlas->m_ShelfX + bw > pAtlas->m_nSize)
    {
        pAtlas->m_ShelfY += pAtlas->m_ShelfH;
        pAtlas->m_ShelfX = 0;
        pAtlas->m_ShelfH = 0;
    }
    if (!pAtlas || pAtlas->m_ShelfY + bh > pAtlas->m_nSize)
    {
        pAtlas = mr_NewAtlas(pContext, nScale);
        if (!pAtlas)
            return false;
    }
    pLM->m_pAtlas = pAtlas;
    pLM->m_X = (uint16_t)(pAtlas->m_ShelfX + 1);
    pLM->m_Y = (uint16_t)(pAtlas->m_ShelfY + 1);
    pAtlas->m_ShelfX += bw;
    if (bh > pAtlas->m_ShelfH)
        pAtlas->m_ShelfH = bh;
    return true;
}

static void mr_UploadAtlases(MRContext* pContext)
{
    for (MRLMAtlas* pAtlas = pContext->m_pAtlases; pAtlas; pAtlas = pAtlas->m_pNext)
    {
        if (!pAtlas->m_Id)
        {
            pAtlas->m_Id = rd_GenTexture();
            rd_BindTexture(pAtlas->m_Id);
            rd_TexFilter(RD_LINEAR, RD_LINEAR);
            rd_TexWrap(false, false);
            rd_TexMaxLevel(0);
            rd_TexImage(0, RD_FMT_RGB8, pAtlas->m_nSize, pAtlas->m_nSize, RD_PIX_RGB, pAtlas->m_pRGB);
        }
        else if (pAtlas->m_bDirty && pAtlas->m_DirtyY0 < pAtlas->m_DirtyY1)
        {
            // Whole rows, so the pixels need no row length of their own.
            uint32_t y0 = pAtlas->m_DirtyY0, y1 = pAtlas->m_DirtyY1;
            if (y1 > pAtlas->m_nSize)
                y1 = pAtlas->m_nSize;
            rd_BindTexture(pAtlas->m_Id);
            rd_TexSubImage(0, 0, y0, pAtlas->m_nSize, y1 - y0, RD_PIX_RGB,
                           pAtlas->m_pRGB + y0 * pAtlas->m_nSize * 3);
        }
        pAtlas->m_bDirty = false;
        pAtlas->m_DirtyY0 = pAtlas->m_nSize;
        pAtlas->m_DirtyY1 = 0;
    }
}

// r_CreateLMPages: place every poly the base animation names (the polys
// that have lightmaps), compose each and upload.
bool mr_BuildLightmaps(MRContext* pContext)
{
    MainWorld* pWorld = pContext->m_pWorld;
    if (!pWorld || !g.bContext)
        return false;

    // The poly field is ours: nothing hangs on it until placed.
    for (uint32_t i = 0; i < pWorld->m_Worlds.m_nElements; i++)
    {
        WorldData* pData = mw_World(pWorld, i);
        WorldBsp* pBsp = pData ? pData->m_pWorldBsp : NULL;
        for (uint32_t p = 0; pBsp && p < pBsp->m_nPolies; p++)
            if (pBsp->m_Polies[p])
                pBsp->m_Polies[p]->m_pLMPage = NULL;
    }
    if (!(pWorld->m_Flags & MAINWORLD_HASLIGHTMAPS))
        return false;
    LMAnim* pBase = mr_FindLMAnim(pWorld, "LightAnim_BASE");
    if (!pBase || pBase->m_nFrames < 1 || !pBase->m_nPolies)
        return false;

    pContext->m_pPolyLMs = (MRPolyLM*)calloc(pBase->m_nPolies, sizeof(MRPolyLM));
    if (!pContext->m_pPolyLMs)
        return false;
    pContext->m_nContextGen = g.nContextGen;

    int nScale = cvi(CV_LightmapSmooth);
    nScale = nScale < 1 ? 1 : nScale > LM_MAXSCALE ? LM_MAXSCALE : nScale;
    uint16_t texels[LM_MAXTEXELS];
    uint32_t nPlaced = 0;
    for (uint32_t i = 0; i < pBase->m_nPolies; i++)
    {
        WorldPoly* pPoly = mr_GetLMPoly(pWorld, pBase->m_Polies[i]);
        if (!pPoly || pPoly->m_pLMPage || !pPoly->m_pSurface ||
            !(pPoly->m_pSurface->m_Flags & SURF_LIGHTMAP))
            continue;
        uint32_t w = pPoly->m_LMWidth, h = pPoly->m_LMHeight;
        if (!w || !h || w * h > LM_MAXTEXELS)
            continue;
        MRPolyLM* pLM = &pContext->m_pPolyLMs[nPlaced];
        if (!mr_Place(pContext, w, h, (uint32_t)nScale, pLM))
            break;
        if (!mr_ComposePolyLM(pWorld, pPoly, texels))
            continue;
        mr_StorePolyLM(pLM, w, h, texels);
        pPoly->m_pLMPage = pLM;
        pPoly->m_Flags &= ~POLYFLAG_LMCHANGED;
        nPlaced++;
    }
    pContext->m_nPolyLMs = nPlaced;
    mr_UploadAtlases(pContext);

    int nAtlases = 0;
    for (MRLMAtlas* p = pContext->m_pAtlases; p; p = p->m_pNext)
        nAtlases++;
    mr_Debug(1, "%u lightmaps in %d atlases of %d (LightmapSmooth %d)", nPlaced, nAtlases, ATLAS_SIZE * nScale, nScale);
    return nPlaced > 0;
}

void mr_FreeLightmaps(MRContext* pContext)
{
    MainWorld* pWorld = pContext->m_pWorld;
    for (uint32_t i = 0; pWorld && i < pWorld->m_Worlds.m_nElements; i++)
    {
        WorldData* pData = mw_World(pWorld, i);
        WorldBsp* pBsp = pData ? pData->m_pWorldBsp : NULL;
        for (uint32_t p = 0; pBsp && p < pBsp->m_nPolies; p++)
            if (pBsp->m_Polies[p])
                pBsp->m_Polies[p]->m_pLMPage = NULL;
    }
    while (pContext->m_pAtlases)
    {
        MRLMAtlas* pAtlas = pContext->m_pAtlases;
        pContext->m_pAtlases = pAtlas->m_pNext;
        if (pAtlas->m_Id && g.bContext && pContext->m_nContextGen == g.nContextGen)
            rd_DeleteTexture(pAtlas->m_Id);
        free(pAtlas->m_pRGB);
        free(pAtlas);
    }
    free(pContext->m_pPolyLMs);
    pContext->m_pPolyLMs = NULL;
    free(pContext->m_pLitPolys);
    pContext->m_pLitPolys = NULL;
    pContext->m_nLitPolys = pContext->m_nLitAlloc = 0;
    pContext->m_nPolyLMs = 0;
}

// The polys lit last frame get their plain lightmaps back; those still lit
// are lit again as they draw.
static void mr_RestoreLitPolys(MRContext* pContext)
{
    uint16_t texels[LM_MAXTEXELS];
    for (uint32_t i = 0; i < pContext->m_nLitPolys; i++)
    {
        WorldPoly* pPoly = pContext->m_pLitPolys[i];
        MRPolyLM* pLM = (MRPolyLM*)pPoly->m_pLMPage;
        if (!pLM)
            continue;
        pLM->m_bLit = false;
        if (mr_ComposePolyLM(pContext->m_pWorld, pPoly, texels))
            mr_StorePolyLM(pLM, pPoly->m_LMWidth, pPoly->m_LMHeight, texels);
    }
    pContext->m_nLitPolys = 0;
}

// r_LightData (docs/world.md C.6): the poly's lightmap with the lights
// added, per texel at its world position, over its atlas texels until
// mr_RestoreLitPolys.
void mr_LightPolyLM(MRContext* pContext, WorldPoly* pPoly, const MRWorldLight* pLights,
                     uint32_t nLights)
{
    MRPolyLM* pLM = (MRPolyLM*)pPoly->m_pLMPage;
    uint16_t texels[LM_MAXTEXELS];
    if (!pLM || !nLights || pLM->m_bLit || !mr_ComposePolyLM(pContext->m_pWorld, pPoly, texels))
        return;
    if (pContext->m_nLitPolys == pContext->m_nLitAlloc)
    {
        uint32_t n = pContext->m_nLitAlloc ? pContext->m_nLitAlloc * 2 : 256;
        WorldPoly** p = (WorldPoly**)realloc(pContext->m_pLitPolys, n * sizeof(WorldPoly*));
        if (!p)
            return;
        pContext->m_pLitPolys = p;
        pContext->m_nLitAlloc = n;
    }
    pContext->m_pLitPolys[pContext->m_nLitPolys++] = pPoly;
    pLM->m_bLit = true;

    LTVector P, Q;
    mr_LMVectors(pPoly->m_pSurface, &P, &Q);
    float grid = pContext->m_pWorld->m_LMGridSize;
    float pp = P.x * P.x + P.y * P.y + P.z * P.z, qq = Q.x * Q.x + Q.y * Q.y + Q.z * Q.z;
    if (pp <= 0.0f || qq <= 0.0f)
        return;
    LTVector su = { P.x * grid / pp, P.y * grid / pp, P.z * grid / pp };
    LTVector sv = { Q.x * grid / qq, Q.y * grid / qq, Q.z * grid / qq };
    // r_InitSaturateTable: ramp[i] = min(1, i/63 * LightSaturate).
    float fSat = cvf(CV_LightSaturate) / 63.0f;
    uint32_t w = pPoly->m_LMWidth, h = pPoly->m_LMHeight;
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < w; x++)
        {
            uint16_t& t = texels[y * w + x];
            int c[3] = { ((t >> 10) & 31) << 3, ((t >> 5) & 31) << 3, (t & 31) << 3 };
            float wx = pPoly->m_LMOrigin.x + x * su.x + y * sv.x;
            float wy = pPoly->m_LMOrigin.y + x * su.y + y * sv.y;
            float wz = pPoly->m_LMOrigin.z + x * su.z + y * sv.z;
            for (uint32_t l = 0; l < nLights; l++)
            {
                const MRWorldLight& L = pLights[l];
                float dx = wx - L.m_Pos.x, dy = wy - L.m_Pos.y, dz = wz - L.m_Pos.z;
                float d2 = dx * dx + dy * dy + dz * dz, r2 = L.m_fRadius * L.m_fRadius;
                if (d2 >= r2)
                    continue;
                if (L.m_bSolid)
                {
                    c[0] = L.m_Color[0];
                    c[1] = L.m_Color[1];
                    c[2] = L.m_Color[2];
                    continue;
                }
                float ramp = (int)((1.0f - d2 / r2) * 63.0f) * fSat;
                if (ramp > 1.0f)
                    ramp = 1.0f;
                for (int j = 0; j < 3; j++)
                    c[j] += (int)(L.m_Add[j] * ramp);
            }
            for (int j = 0; j < 3; j++)
                c[j] = c[j] < 0 ? 0 : c[j] > 255 ? 31 : c[j] >> 3;
            t = (uint16_t)(c[0] << 10 | c[1] << 5 | c[2]);
        }
    }
    mr_StorePolyLM(pLM, w, h, texels);
}

void mr_UploadLightmaps(MRContext* pContext)
{
    mr_UploadAtlases(pContext);
}

// Light animations the engine changed this frame marked their polys
// (MainWorld::SetLMPoliesChanged); recompose those (the PC renderer's way:
// straight into the page).
void mr_UpdateLightmaps(MRContext* pContext)
{
    MainWorld* pWorld = pContext->m_pWorld;
    uint16_t texels[LM_MAXTEXELS];
    mr_RestoreLitPolys(pContext);
    for (uint32_t i = 0; i < pWorld->m_Worlds.m_nElements; i++)
    {
        WorldData* pData = mw_World(pWorld, i);
        WorldBsp* pBsp = pData ? pData->m_pWorldBsp : NULL;
        for (uint32_t p = 0; pBsp && p < pBsp->m_nPolies; p++)
        {
            WorldPoly* pPoly = pBsp->m_Polies[p];
            if (!pPoly || !(pPoly->m_Flags & POLYFLAG_LMCHANGED))
                continue;
            pPoly->m_Flags &= ~POLYFLAG_LMCHANGED;
            MRPolyLM* pLM = (MRPolyLM*)pPoly->m_pLMPage;
            if (pLM && mr_ComposePolyLM(pWorld, pPoly, texels))
                mr_StorePolyLM(pLM, pPoly->m_LMWidth, pPoly->m_LMHeight, texels);
        }
    }
    // Uploaded as the polys draw (mr_UploadLightmaps), dynamic light and all.
}
