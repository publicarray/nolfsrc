// glscene.h -- the 3D half's shared declarations: the per-world context,
// the lightmap atlases, the multitexture entry points.

#ifndef GLSCENE_H
#define GLSCENE_H

#include "mr.h"
#include "ltworld.h"

// ---- lightmap data, as the engine keeps it (docs/world.md C.1) -----------------

struct LMPolyRef { uint16_t m_iWorldModel, m_iPoly; };
struct LMFrameEntry { uint8_t* m_pData; uint32_t m_nSize; };
struct PolyAnimRef { uint16_t m_iAnim, m_iPoly; };

struct LMAnim {                         // 0x60
    char           m_Name[32];          // 0x00
    uint32_t       m_bShadowMap;        // 0x20
    LMFrameEntry** m_Frames;            // 0x24 [m_nFrames] -> LMFrameEntry[m_nPolies]
    uint32_t       m_nFrames;           // 0x28
    LMPolyRef*     m_Polies;            // 0x2c
    uint32_t       m_nPolies;           // 0x30
    uint32_t       m_iFrames[2];        // 0x34
    float          m_fPercentBetween;   // 0x3c
    float          m_fBlendPercent;     // 0x40
    LTVector       m_vLightPos;         // 0x44
    LTVector       m_vLightColor;       // 0x50
    float          m_fLightRadius;      // 0x5c
};
static_assert(sizeof(LMAnim) == 0x60, "LMAnim");

#define LM_MAXTEXELS 1024               // 32x32: the decompressor's cap
#define POLYFLAG_LMCHANGED 0x8000       // WorldPoly::m_Flags: the engine marks a changed animation

// ---- the renderer's per-world state ---------------------------------------------

struct MRLMAtlas {
    uint32_t      m_Id;
    uint32_t    m_nSize;                // square
    uint32_t    m_nScale;               // texels per lightmap texel (LightmapSmooth)
    uint32_t    m_ShelfX, m_ShelfY, m_ShelfH;
    uint8_t*    m_pRGB;                 // m_nSize^2 * 3, kept for animated updates
    bool        m_bDirty;
    uint32_t    m_DirtyY0, m_DirtyY1;   // the rows changed since the last upload
    MRLMAtlas* m_pNext;
};

// Where a lightmapped poly's texels are; hung on WorldPoly::m_pLMPage, which
// is the renderer's field.
struct MRPolyLM {
    MRLMAtlas* m_pAtlas;
    uint16_t    m_X, m_Y;               // the lightmap's first texel (inside its border)
    bool        m_bLit;                 // holds dynamic light this frame
};

// A dynamic light as a BSP's polys see it: in that BSP's own space.
struct MRWorldLight {
    LTVector m_Pos;
    float    m_fRadius;
    float    m_Add[3];                  // 2c - 255: a colour below 128 darkens
    uint8_t  m_Color[3];
    bool     m_bSolid;                  // FLAG_SOLIDLIGHT or FastLight
    bool     m_bFrontOnly;              // FLAG_DONTLIGHTBACKFACING
};
#define MR_MAXWORLDLIGHTS 40           // r_DrawLight's cap

struct MRContext {
    MainWorld*  m_pWorld;
    uint16_t    m_FrameCode;
    MRLMAtlas* m_pAtlases;
    MRPolyLM*  m_pPolyLMs;             // one block for every lightmapped poly
    uint32_t    m_nPolyLMs;
    uint32_t    m_nContextGen;          // device the atlases were made on
    WorldPoly** m_pLitPolys;            // polys whose atlas texels hold dynamic light
    uint32_t    m_nLitPolys, m_nLitAlloc;
};

bool mr_BuildLightmaps(MRContext* pContext);
void mr_FreeLightmaps(MRContext* pContext);
void mr_UpdateLightmaps(MRContext* pContext);   // animated lightmaps, before drawing
void mr_UploadLightmaps(MRContext* pContext);   // the atlas rows changed since the last call
void mr_LightPolyLM(MRContext* pContext, WorldPoly* pPoly, const MRWorldLight* pLights,
                     uint32_t nLights);
LMAnim* mr_FindLMAnim(MainWorld* pWorld, const char* pName);
void mr_LMVectors(const Surface* pSurf, LTVector* pP, LTVector* pQ);
WorldPoly* mr_GetLMPoly(MainWorld* pWorld, const LMPolyRef& ref);

// ---- the frame ------------------------------------------------------------------------

struct MRCamera {
    LTVector m_Pos;
    float    m_R[3][3];                 // camera -> world: columns right, up, forward
    float    m_fTanX, m_fTanY;          // tan of the half field of view
    float    m_fNear, m_fFar;
    float    m_fScale;                  // view scale (the sky's SkyScale)
    int      m_nViewX, m_nViewY, m_nViewW, m_nViewH;   // viewport, from the screen's top left
    LTPlane  m_Planes[6];               // world-space frustum, normals in
};

struct MRFrame {
    SceneDesc*  m_pScene;
    MRContext* m_pContext;
    MRCamera   m_Cam;
    MRCamera   m_SkyCam;               // while sky objects draw
    uint16_t    m_WorldFrameCode;
    uint32_t    m_ObjectFrameCode;
    uint32_t    m_nSkyPolys;            // visible sky polygons: the sky pass runs if any
    float       m_fLMGrid;
    bool        m_bLightmaps;
    bool        m_bFog;
};
extern MRFrame g_Frame;

// The camera's projection and view; its depth into the depth buffer's
// MR_CLOSE_DEPTH..1, the front kept for what really-close objects have
// nearer than its near plane (mr_BeginReallyClose).
#define MR_CLOSE_DEPTH (1.0f / 16.0f)
void mr_LoadCamera(const MRCamera* pCam);
void mr_MultMatrix(const LTMatrix& M);
void mr_InverseAffine(LTMatrix* pOut, const LTMatrix& M);
void mr_RotationMatrix(const LTRotation& q, float m[3][3]);
bool mr_SphereVisible(const LTVector& c, float r);
bool mr_ShouldDraw(LTObject* pObj);
void mr_ObjectFog(LTObject* pObj, bool bBegin);
void mr_SetBaseTexture(MRTexture* pTex);
void mr_ForgetBindings(void);
MRTexture* mr_SurfaceTexture(Surface* pSurf);
void mr_ProcessWorldModel(LTObject* pObj);
void mr_DrawSolidWorldModel(LTObject* pObj);
void mr_DrawTranslucentWorldModel(LTObject* pObj);
bool mr_SeesSky(const LTVector& vFrom, const LTVector& vRay);
void mr_ReflectCoords(const LTVector& vCam, const LTVector& pos, const LTVector& N, float* pS, float* pT);
MRTexture* mr_EnvMapOf(SharedTexture* pBase);
bool mr_BeginEnvUnit(MRTexture* pTex);
void mr_EndEnvUnit(void);
float mr_WorldSize(void);

#endif
