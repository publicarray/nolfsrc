// mr_scene.cpp -- the 3D half: the world context, RenderScene, the world.
//
// RenderScene follows the Mac renderer's r_RenderScene / r_FullDrawScene
// (docs/world.md part A, docs/objects.md part I.2):
//
//   * the camera (r_InitFrustrum2's projection, done by the device here: the
//     Mac projects every vertex itself);
//   * the visible set: in the normal draw mode the engine's own visibility
//     query walks the PVS and calls back for every visible leaf (whose
//     polygons are queued) and object; the object-list draw mode
//     (ILTClient::RenderObjects) queues just the list;
//   * the sky, from its squashed camera, wherever sky polygons are visible;
//   * the world's polygons: base texture times lightmap or vertex colours;
//   * the objects, in r_FlushObjectQueues' order: solid world models, solid
//     models, chromakey, then the translucent ones far to near;
//   * the global light scale and the camera's light add as screen passes.
//
// The object types other than world models are drawn by mr_objects.cpp.

#include <math.h>

#include "mrscene.h"
#include "mrobjects.h"

// r_CreateContext: the engine binds a loaded world to the renderer and keeps
// what this returns at MainWorld+0x1c8, handing it back in every SceneDesc
// (NULL fails the whole renderer init). The lightmaps are built here.
void* mr_CreateContext(RenderContextInit* pInit)
{
    MRContext* pContext = (MRContext*)calloc(1, sizeof(MRContext));
    if (!pContext)
        return NULL;
    pContext->m_FrameCode = 0xffff;
    pContext->m_pWorld = pInit ? (MainWorld*)pInit->m_pWorld : NULL;
    if (pContext->m_pWorld)
        mr_BuildLightmaps(pContext);
    return pContext;
}

void mr_DeleteContext(void* pContext)
{
    if (!pContext)
        return;
    mr_FreeLightmaps((MRContext*)pContext);
    free(pContext);
}

// ---- the frame ----------------------------------------------------------------------

MRFrame g_Frame;

static void mr_QuatToMatrix(const LTRotation& q, float m[3][3])
{
    float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    float s = n > 0.0f ? 2.0f / n : 0.0f;
    float xx = q.x * q.x * s, yy = q.y * q.y * s, zz = q.z * q.z * s;
    float xy = q.x * q.y * s, xz = q.x * q.z * s, yz = q.y * q.z * s;
    float wx = q.w * q.x * s, wy = q.w * q.y * s, wz = q.w * q.z * s;
    m[0][0] = 1 - (yy + zz); m[0][1] = xy - wz;       m[0][2] = xz + wy;
    m[1][0] = xy + wz;       m[1][1] = 1 - (xx + zz); m[1][2] = yz - wx;
    m[2][0] = xz - wy;       m[2][1] = yz + wx;       m[2][2] = 1 - (xx + yy);
}

void mr_RotationMatrix(const LTRotation& q, float m[3][3])
{
    mr_QuatToMatrix(q, m);
}

static void mr_SetupCamera(SceneDesc* pScene, MRCamera* pCam)
{
    const LTRect& r = pScene->m_Rect;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    pCam->m_nViewX = r.left;
    pCam->m_nViewY = r.top;
    pCam->m_nViewW = w;
    pCam->m_nViewH = h;

    pCam->m_Pos = pScene->m_Pos;
    mr_QuatToMatrix(pScene->m_Rotation, pCam->m_R);

    // Hor+ (MODERN.md M-P0-01, as the modern d3d.ren does): the game's
    // horizontal field of view is for a 4:3 view; a wider rect sees more.
    pCam->m_fTanX = tanf(pScene->m_xFov * 0.5f) * ((float)w / (float)h) / (4.0f / 3.0f);
    pCam->m_fTanY = tanf(pScene->m_yFov * 0.5f);

    pCam->m_fNear = 7.0f;
    float fFar = pScene->m_FarZ;
    pCam->m_fFar = fFar < 3.0f ? 3.0f : fFar > 500000.0f ? 500000.0f : fFar;
    pCam->m_fScale = 1.0f;
}

// The six world-space frustum planes, normals pointing in: a point p is
// inside a plane when N.p - D >= 0.
static void mr_SetupFrustum(MRCamera* pCam)
{
    float tx = pCam->m_fTanX, ty = pCam->m_fTanY;
    float cam[6][4] = {
        { 0, 0, 1, pCam->m_fNear },          // near: z >= near
        { 0, 0, -1, -pCam->m_fFar },         // far: -z >= -far
        { 1, 0, tx, 0 }, { -1, 0, tx, 0 },   // left, right: x/z >= -tan, <= tan
        { 0, 1, ty, 0 }, { 0, -1, ty, 0 },   // bottom, top
    };
    const float (*R)[3] = pCam->m_R;
    for (int i = 0; i < 6; i++)
    {
        float l = sqrtf(cam[i][0] * cam[i][0] + cam[i][1] * cam[i][1] + cam[i][2] * cam[i][2]);
        float nx = cam[i][0] / l, ny = cam[i][1] / l, nz = cam[i][2] / l, d = cam[i][3] / l;
        LTPlane& p = pCam->m_Planes[i];
        p.m_Normal.x = R[0][0] * nx + R[0][1] * ny + R[0][2] * nz;
        p.m_Normal.y = R[1][0] * nx + R[1][1] * ny + R[1][2] * nz;
        p.m_Normal.z = R[2][0] * nx + R[2][1] * ny + R[2][2] * nz;
        p.m_Dist = p.m_Normal.x * pCam->m_Pos.x + p.m_Normal.y * pCam->m_Pos.y +
                   p.m_Normal.z * pCam->m_Pos.z + d;
    }
}

bool mr_SphereVisible(const LTVector& c, float r)
{
    for (int i = 0; i < 6; i++)
    {
        const LTPlane& p = g_Frame.m_Cam.m_Planes[i];
        if (p.m_Normal.x * c.x + p.m_Normal.y * c.y + p.m_Normal.z * c.z - p.m_Dist < -r)
            return false;
    }
    return true;
}

// Projection and view (column-major, rdev.h), and the viewport. The view
// carries the camera's scale (the sky's SkyScale) as the Mac's
// m_MatWorld = Scale * R^T * T(-pos) does.
void mr_LoadCamera(const MRCamera* pCam)
{
    rd_Viewport(pCam->m_nViewX, pCam->m_nViewY, pCam->m_nViewW, pCam->m_nViewH);

    float n = pCam->m_fNear, f = pCam->m_fFar;
    float P[16] = {
        1.0f / pCam->m_fTanX, 0, 0, 0,
        0, 1.0f / pCam->m_fTanY, 0, 0,
        0, 0, (f + n) / (f - n), 1,
        0, 0, -2.0f * f * n / (f - n), 0,
    };
    rd_MatrixMode(RD_PROJECTION);
    rd_LoadMatrix(P);
    rd_DepthRange(MR_CLOSE_DEPTH, 1.0f);

    const float (*R)[3] = pCam->m_R;
    const LTVector& p = pCam->m_Pos;
    float s = pCam->m_fScale;
    float V[16];
    for (int row = 0; row < 3; row++)
    {
        V[0 * 4 + row] = R[0][row] * s;
        V[1 * 4 + row] = R[1][row] * s;
        V[2 * 4 + row] = R[2][row] * s;
        V[3 * 4 + row] = -(R[0][row] * p.x + R[1][row] * p.y + R[2][row] * p.z) * s;
    }
    V[3] = V[7] = V[11] = 0.0f;
    V[15] = 1.0f;
    rd_MatrixMode(RD_MODELVIEW);
    rd_LoadMatrix(V);
}

void mr_MultMatrix(const LTMatrix& M)
{
    float m[16];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            m[col * 4 + row] = M.m[row][col];
    rd_MultMatrix(m);
}

// ---- fog ---------------------------------------------------------------------------

static void mr_SetFog(bool bSky)
{
    bool bOn = bSky ? cvi(CV_FogEnable) && cvi(CV_SkyFogEnable) : cvi(CV_FogEnable) != 0;
    if (!bOn)
    {
        rd_Disable(RD_FOG);
        g_Frame.m_bFog = false;
        return;
    }
    rd_FogColor(cvf(CV_FogR) / 255.0f, cvf(CV_FogG) / 255.0f, cvf(CV_FogB) / 255.0f);
    if (bSky)
    {
        // d3d.ren hands SkyFogNearZ/FarZ to the device as the fog range for
        // vertices it has already projected, whose depth runs 0..1 (and the
        // Mac's sky fog coordinate is that fraction too): the sky is fogged
        // all over by (far - 1)/(far - near), not by its distance. A range
        // far wider than the sky gives that constant.
        float fNear = cvf(CV_SkyFogNearZ), fFar = cvf(CV_SkyFogFarZ);
        float f = fFar > fNear ? (fFar - 1.0f) / (fFar - fNear) : 0.0f;
        f = f < 0.0f ? 0.0f : f;
        if (f >= 1.0f)
        {
            rd_Disable(RD_FOG);
            g_Frame.m_bFog = false;
            return;
        }
        const float fEnd = 1.0e7f;
        rd_FogRange(f > 0.0f ? fEnd - fEnd / f : -1.0e30f, fEnd);
    }
    else
        rd_FogRange(cvf(CV_FogNearZ), cvf(CV_FogFarZ));
    rd_Enable(RD_FOG);
    g_Frame.m_bFog = true;
}

// An object that asks for no fog (FLAG_FOGDISABLE) turns it off while it
// draws.
void mr_ObjectFog(LTObject* pObj, bool bBegin)
{
    if (!g_Frame.m_bFog || !(pObj->m_Flags & FLAG_FOGDISABLE))
        return;
    if (bBegin)
        rd_Disable(RD_FOG);
    else
        rd_Enable(RD_FOG);
}

// ---- polygons ----------------------------------------------------------------------

struct MRDrawPoly {
    MRTexture* m_pTex;
    WorldPoly*  m_pPoly;
};

static MRDrawPoly* g_pDrawList;
static uint32_t     g_nDrawList, g_nDrawAlloc;

static void mr_QueuePoly(MRTexture* pTex, WorldPoly* pPoly)
{
    if (g_nDrawList == g_nDrawAlloc)
    {
        uint32_t n = g_nDrawAlloc ? g_nDrawAlloc * 2 : 4096;
        MRDrawPoly* p = (MRDrawPoly*)realloc(g_pDrawList, n * sizeof(MRDrawPoly));
        if (!p)
            return;
        g_pDrawList = p;
        g_nDrawAlloc = n;
    }
    g_pDrawList[g_nDrawList].m_pTex = pTex;
    g_pDrawList[g_nDrawList].m_pPoly = pPoly;
    g_nDrawList++;
}

static int mr_CompareDrawPoly(const void* a, const void* b)
{
    const MRDrawPoly* x = (const MRDrawPoly*)a;
    const MRDrawPoly* y = (const MRDrawPoly*)b;
    if (x->m_pTex != y->m_pTex)
        return (uintptr_t)x->m_pTex < (uintptr_t)y->m_pTex ? -1 : 1;
    MRPolyLM* lx = (MRPolyLM*)x->m_pPoly->m_pLMPage;
    MRPolyLM* ly = (MRPolyLM*)y->m_pPoly->m_pLMPage;
    void* ax = lx ? lx->m_pAtlas : NULL;
    void* ay = ly ? ly->m_pAtlas : NULL;
    return (uintptr_t)ax < (uintptr_t)ay ? -1 : (uintptr_t)ax > (uintptr_t)ay ? 1 : 0;
}

MRTexture* mr_SurfaceTexture(Surface* pSurf)
{
    if (!pSurf->m_pTexture)
        return NULL;
    mr_TextureId(pSurf->m_pTexture);
    return (MRTexture*)pSurf->m_pTexture->m_pRenderData;
}

// Queue a poly unless it faces away from vCam (in the poly's own space).
static void mr_QueueFacingPoly(WorldPoly* pPoly, const LTVector& vCam)
{
    Surface* pSurf = pPoly->m_pSurface;
    const LTPlane* pPlane = pSurf->m_pPlane;
    if (pPlane && pPlane->m_Normal.x * vCam.x + pPlane->m_Normal.y * vCam.y +
                  pPlane->m_Normal.z * vCam.z - pPlane->m_Dist <= 0.01f)
        return;
    mr_QueuePoly(mr_SurfaceTexture(pSurf), pPoly);
}

// ---- texture units ---------------------------------------------------------------------
//
// A lightmapped poly is the retail PC's two passes in one (docs/world.md
// E.4): base texture, plus its detail texture added signed (DetailTextures,
// DetailTextureAdd), times the lightmap, doubled when Saturate is set
// (blend DST_COLOR, SRC_COLOR). Unit 0 is the base, then the detail, then
// the lightmap; without three units or texture combiners the detail goes.

static int g_nDetailUnit = -1, g_nLMUnit = -1;
static MRLMAtlas* g_pBoundAtlas;
static MRTexture* g_pBoundDetail;
static MRTexture* g_pBoundTex;

static void mr_ActiveUnit(int n)
{
    rd_ActiveTexture(n);
}

// combine RGB = op(TEXTURE, PREVIOUS) * scale, alpha = eAlpha
static void mr_CombineUnit(RDCombineOp op, float fScale, RDCombineArg eAlpha = RD_ARG_PREVIOUS)
{
    RDCombine c = { op, RD_ARG_TEXTURE, RD_ARG_PREVIOUS, fScale, RD_OP_REPLACE, eAlpha, eAlpha };
    rd_TexCombine(c);
}

static void mr_BeginUnits(bool bLightmaps)
{
    g_nDetailUnit = g_nLMUnit = -1;
    if (!bLightmaps)
        return;
    g_nLMUnit = 1;
    if (g_RDCaps.bCombine && g_RDCaps.nTextureUnits >= 3 && (cvi(CV_DetailTextures) || cvi(CV_EnvMapWorld)))
    {
        g_nDetailUnit = 1;
        g_nLMUnit = 2;
        mr_ActiveUnit(g_nDetailUnit);
        mr_CombineUnit(cvi(CV_DetailTextureAdd) ? RD_OP_ADD_SIGNED : RD_OP_MODULATE, 1.0f);
        rd_Disable(RD_TEXTURE_2D);
    }
    mr_ActiveUnit(g_nLMUnit);
    if (g_RDCaps.bCombine)
        mr_CombineUnit(RD_OP_MODULATE, cvi(CV_Saturate) ? 2.0f : 1.0f);
    else
        rd_TexEnv(RD_ENV_MODULATE);
    rd_Disable(RD_TEXTURE_2D);
    mr_ActiveUnit(0);
}

static void mr_EndUnits(void)
{
    for (int n = 1; n <= 2; n++)
    {
        if (n != g_nDetailUnit && n != g_nLMUnit)
            continue;
        mr_ActiveUnit(n);
        rd_Disable(RD_TEXTURE_2D);
        rd_TexEnv(RD_ENV_MODULATE);
    }
    if (g_nLMUnit >= 0)
        mr_ActiveUnit(0);
    g_nDetailUnit = g_nLMUnit = -1;
}

static void mr_SetUnitTexture(int nUnit, uint32_t id)
{
    mr_ActiveUnit(nUnit);
    if (id)
    {
        rd_Enable(RD_TEXTURE_2D);
        rd_BindTexture(id);
    }
    else
        rd_Disable(RD_TEXTURE_2D);
    mr_ActiveUnit(0);
}

static void mr_SetLightmapUnit(MRLMAtlas* pAtlas)
{
    if (pAtlas == g_pBoundAtlas || g_nLMUnit < 0)
        return;
    mr_SetUnitTexture(g_nLMUnit, pAtlas ? pAtlas->m_Id : 0);
    g_pBoundAtlas = pAtlas;
}

// The sky-pan texture (the clouds' shadows on SURF_PANNINGSKY polys):
// RenderStruct's PanInfo[GLOBALPAN_SKYSHADOW], planar on world XZ, its
// offsets animated by the engine (docs/world.md 3.4).
struct PanInfo { SharedTexture* m_pTexture; float m_xOffset, m_zOffset, m_xScale, m_zScale; };
static MRTexture* g_pPanTex;
static float g_fPanU, g_fPanV, g_fPanOffX, g_fPanOffZ;
#define MR_PANBOUND ((MRLMAtlas*)&g_pPanTex)  // g_pBoundAtlas when the pan texture is on the unit

static void mr_SetupPan(bool bWorld)
{
    g_pPanTex = NULL;
    const PanInfo* pPan = (const PanInfo*)g.pStruct->m_GlobalPanInfo;
    if (!bWorld || !pPan->m_pTexture || pPan->m_xScale == 0.0f || pPan->m_zScale == 0.0f)
        return;
    mr_TextureId(pPan->m_pTexture);
    g_pPanTex = (MRTexture*)pPan->m_pTexture->m_pRenderData;
    if (!g_pPanTex)
        return;
    g_fPanU = g_pPanTex->m_fUMul / pPan->m_xScale;
    g_fPanV = g_pPanTex->m_fVMul / pPan->m_zScale;
    g_fPanOffX = pPan->m_xOffset;
    g_fPanOffZ = pPan->m_zOffset;
}

static void mr_SetDetailUnit(MRTexture* pDetail)
{
    if (pDetail == g_pBoundDetail || g_nDetailUnit < 0)
        return;
    mr_SetUnitTexture(g_nDetailUnit, pDetail ? pDetail->m_Id : 0);
    g_pBoundDetail = pDetail;
}

void mr_SetBaseTexture(MRTexture* pTex)
{
    if (pTex == g_pBoundTex)
        return;
    if (pTex)
    {
        rd_Enable(RD_TEXTURE_2D);
        rd_BindTexture(pTex->m_Id);
    }
    else
        rd_Disable(RD_TEXTURE_2D);
    g_pBoundTex = pTex;
}

void mr_ForgetBindings(void)
{
    g_pBoundTex = (MRTexture*)-1;
    g_pBoundAtlas = (MRLMAtlas*)-1;
    g_pBoundDetail = (MRTexture*)-1;
}

// The detail texture a base texture names (DTX "DetailTex", link type 0).
// The texture's linked texture of this kind (0 detail, 1 environment map).
static MRTexture* mr_LinkedOf(Surface* pSurf, uint32_t nType)
{
    SharedTexture* pBase = pSurf->m_pTexture;
    if (!pBase || !pBase->m_pLinkedTexture || pBase->m_nLinkType != nType)
        return NULL;
    mr_TextureId(pBase->m_pLinkedTexture);
    return (MRTexture*)pBase->m_pLinkedTexture->m_pRenderData;
}

// The camera in the space of the polys being drawn (world models'), for
// environment-map coordinates.
static LTVector g_vPolyCam;

// d3d.ren's 0x1001e950 (the Mac's r_CalcEnvMapCoords): the eye-to-vertex
// direction reflected about the normal, two of its components into [0, 1].
void mr_ReflectCoords(const LTVector& vCam, const LTVector& pos, const LTVector& N, float* pS, float* pT)
{
    float dx = pos.x - vCam.x, dy = pos.y - vCam.y, dz = pos.z - vCam.z;
    float k = 2.0f * (dx * N.x + dy * N.y + dz * N.z);
    float rx = dx - N.x * k, ry = dy - N.y * k, rz = dz - N.z * k;
    float len = sqrtf(rx * rx + ry * ry + rz * rz);
    if (len > 0.0f)
    {
        rx /= len; ry /= len; rz /= len;
    }
    float s = (N.y * N.y > 0.54439f || N.x * N.x < N.z * N.z) ? rx : rz;
    *pS = -0.5f * s + 0.5f;
    *pT = -0.5f * ry + 0.5f;
}

// The world's coordinates are scaled by DetailTextureScale, as the detail
// texture's are.
static void mr_EnvMapCoords(const LTVector& pos, const LTVector& N, float fScale, float* pS, float* pT)
{
    mr_ReflectCoords(g_vPolyCam, pos, N, pS, pT);
    *pS *= fScale;
    *pT *= fScale;
}

MRTexture* mr_EnvMapOf(SharedTexture* pBase)
{
    if (!pBase || !pBase->m_pLinkedTexture || pBase->m_nLinkType != 1)
        return NULL;
    mr_TextureId(pBase->m_pLinkedTexture);
    return (MRTexture*)pBase->m_pLinkedTexture->m_pRenderData;
}

// An object's surface texture on unit 1 over its environment map on unit
// 0, as both renderers stage them (r_SetEnvMapTextureStates, d3d.ren's
// 0x10011450): the map times the vertex colour, the texture added signed
// under DetailTextureAdd (else modulating), the alpha the texture's alone.
bool mr_BeginEnvUnit(MRTexture* pTex)
{
    if (!pTex || !g_RDCaps.bCombine || g_RDCaps.nTextureUnits < 2)
        return false;
    mr_ActiveUnit(1);
    mr_CombineUnit(cvi(CV_DetailTextureAdd) ? RD_OP_ADD_SIGNED : RD_OP_MODULATE, 1.0f, RD_ARG_TEXTURE);
    rd_Enable(RD_TEXTURE_2D);
    rd_BindTexture(pTex->m_Id);
    mr_ActiveUnit(0);
    return true;
}

void mr_EndEnvUnit(void)
{
    mr_ActiveUnit(1);
    rd_Disable(RD_TEXTURE_2D);
    rd_TexEnv(RD_ENV_MODULATE);
    mr_ActiveUnit(0);
    mr_ForgetBindings();
}

static inline uint8_t mr_Saturate(uint8_t c, float fTint, bool bDouble)
{
    float v = c * fTint * (bDouble ? 2.0f : 1.0f);
    return (uint8_t)(v > 255.0f ? 255.0f : v);
}

// One poly as a fan. Lightmapped: white vertices, the units above. Vertex-
// lit: base times the vertex colours (doubled under Saturate, the colour
// map r_FillColorMap builds). Either way tinted by the drawing object's
// colour (r_SetupWMColorScale) with its alpha.
// ---- dynamic lights on the world (r_DrawLight, r_ProcessBspLight) -------------------

static MRWorldLight g_WLights[MR_MAXWORLDLIGHTS];
static uint32_t      g_nWLights;

// The frame's lights that light the world, in the space of the BSP about to
// draw (pBack: world -> model; NULL for the main world).
static void mr_SetupWorldLights(const LTMatrix* pBack)
{
    g_nWLights = 0;
    if (!cvi(CV_DynamicLight) || !g_Frame.m_pContext)
        return;
    bool bFast = cvi(CV_FastLight) != 0;
    MRObjList* pList = mr_ObjList(VS_LIGHTS);
    for (uint32_t i = 0; i < pList->m_nObjects && g_nWLights < MR_MAXWORLDLIGHTS; i++)
    {
        LTObject* L = pList->m_pObjects[i];
        float r;
        memcpy(&r, (const uint8_t*)L + 0x1a8, 4);
        if ((L->m_Flags & FLAG_ONLYLIGHTOBJECTS) || r <= 0.0f)
            continue;
        MRWorldLight& W = g_WLights[g_nWLights++];
        const LTVector& v = L->m_Pos;
        if (pBack)
        {
            const LTMatrix& M = *pBack;
            W.m_Pos.x = M.m[0][0] * v.x + M.m[0][1] * v.y + M.m[0][2] * v.z + M.m[0][3];
            W.m_Pos.y = M.m[1][0] * v.x + M.m[1][1] * v.y + M.m[1][2] * v.z + M.m[1][3];
            W.m_Pos.z = M.m[2][0] * v.x + M.m[2][1] * v.y + M.m[2][2] * v.z + M.m[2][3];
        }
        else
            W.m_Pos = v;
        W.m_fRadius = r;
        W.m_Color[0] = L->m_ColorR;
        W.m_Color[1] = L->m_ColorG;
        W.m_Color[2] = L->m_ColorB;
        for (int j = 0; j < 3; j++)
            W.m_Add[j] = 2.0f * W.m_Color[j] - 255.0f;
        W.m_bSolid = bFast || (L->m_Flags & FLAG_SOLIDLIGHT);
        W.m_bFrontOnly = (L->m_Flags & FLAG_DONTLIGHTBACKFACING) != 0;
    }
}

// r_DoesLightTouchPoly: the light's sphere reaches the poly's plane and its
// bounding sphere.
static bool mr_LightTouchesPoly(const MRWorldLight& L, const WorldPoly* pPoly)
{
    const LTPlane* pPlane = pPoly->m_pSurface->m_pPlane;
    if (pPlane)
    {
        float d = pPlane->m_Normal.x * L.m_Pos.x + pPlane->m_Normal.y * L.m_Pos.y +
                  pPlane->m_Normal.z * L.m_Pos.z - pPlane->m_Dist;
        if (d >= L.m_fRadius || d <= -L.m_fRadius || (L.m_bFrontOnly && d < 0.0f))
            return false;
    }
    float dx = pPoly->m_Center.x - L.m_Pos.x, dy = pPoly->m_Center.y - L.m_Pos.y,
          dz = pPoly->m_Center.z - L.m_Pos.z, rr = L.m_fRadius + pPoly->m_Radius;
    return dx * dx + dy * dy + dz * dz < rr * rr;
}

// The lights that reach the poly, into pOut; how many.
static uint32_t mr_PolyLights(const WorldPoly* pPoly, MRWorldLight* pOut)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nWLights; i++)
        if (mr_LightTouchesPoly(g_WLights[i], pPoly))
            pOut[n++] = g_WLights[i];
    return n;
}

// A gouraud poly's vertex: its colour plus the lights, as r_LightData's
// texel (r_DrawGouraudDynamicLight).
static void mr_LightVertex(const LTVector& pos, const MRWorldLight* pLights, uint32_t nLights,
                            uint8_t* pRGB)
{
    float fSat = cvf(CV_LightSaturate) / 63.0f;
    int c[3] = { pRGB[0], pRGB[1], pRGB[2] };
    for (uint32_t l = 0; l < nLights; l++)
    {
        const MRWorldLight& L = pLights[l];
        float dx = pos.x - L.m_Pos.x, dy = pos.y - L.m_Pos.y, dz = pos.z - L.m_Pos.z;
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
        pRGB[j] = (uint8_t)(c[j] < 0 ? 0 : c[j] > 255 ? 255 : c[j]);
}

static void mr_DrawPoly(WorldPoly* pPoly, MRTexture* pTex, bool bLightmaps,
                         const float* pTint, uint8_t nAlpha)
{
    float fU = pTex ? pTex->m_fUMul : 0.0f, fV = pTex ? pTex->m_fVMul : 0.0f;
    bool bFullbright = pTex && (pTex->m_DtxFlags & DTX_FULLBRITE);
    MRPolyLM* pLM = bLightmaps && !bFullbright ? (MRPolyLM*)pPoly->m_pLMPage : NULL;
    // A panning-sky receiver takes the pan texture where a lightmap would go
    // (r_DrawSPMTPanningSkyPoly): surface texture times the pan texture.
    MRTexture* pPan = bLightmaps && !pLM && !bFullbright && g_pPanTex && g_nLMUnit >= 0 &&
                       (pPoly->m_pSurface->m_Flags & SURF_PANNINGSKY) ? g_pPanTex : NULL;
    if (pPan)
    {
        if (g_pBoundAtlas != MR_PANBOUND)
        {
            mr_SetUnitTexture(g_nLMUnit, pPan->m_Id);
            g_pBoundAtlas = MR_PANBOUND;
        }
    }
    else if (bLightmaps)
        mr_SetLightmapUnit(pLM ? pLM->m_pAtlas : NULL);
    // The detail unit takes the texture's detail texture or, in its place,
    // its environment map (d3d.ren's two second-pass paths: the base added
    // signed to either).
    MRTexture* pDetail = NULL;
    MRTexture* pEnv = NULL;
    if (pLM && g_nDetailUnit >= 0)
    {
        if (cvi(CV_DetailTextures))
            pDetail = mr_LinkedOf(pPoly->m_pSurface, 0);
        if (!pDetail && cvi(CV_EnvMapWorld) && pPoly->m_pSurface->m_pPlane)
            pEnv = mr_LinkedOf(pPoly->m_pSurface, 1);
    }
    if (g_nDetailUnit >= 0)
        mr_SetDetailUnit(pDetail ? pDetail : pEnv);

    LTVector P, Q;
    float fInvSize = 0.0f, fOffU = 0.0f, fOffV = 0.0f, fGrid = 1.0f;
    if (pLM)
    {
        mr_LMVectors(pPoly->m_pSurface, &P, &Q);
        // A lightmap texel t's centre is at m_X + (t + 0.5) * scale in
        // the atlas (LightmapSmooth: each texel scale atlas texels wide).
        float fScale = (float)pLM->m_pAtlas->m_nScale;
        fInvSize = fScale / pLM->m_pAtlas->m_nSize;
        fOffU = (pLM->m_X + 0.5f * fScale) / fScale;
        fOffV = (pLM->m_Y + 0.5f * fScale) / fScale;
        fGrid = g_Frame.m_fLMGrid;
    }
    // r_DrawOverlayDetail: the detail texture's coordinates are the base
    // texels rotated and scaled.
    float fDA = 0.0f, fDB = 0.0f, fDC = 0.0f, fDD = 0.0f;
    if (pDetail)
    {
        float fScale = cvf(CV_DetailTextureScale) * pTex->m_fDetailScale;
        fDA = pTex->m_fDetailCos * pDetail->m_fUMul * fScale;
        fDB = -pTex->m_fDetailSin * pDetail->m_fVMul * fScale;
        fDC = pTex->m_fDetailSin * pDetail->m_fUMul * fScale;
        fDD = pTex->m_fDetailCos * pDetail->m_fVMul * fScale;
    }
    // The lightmap unit doubles under Saturate, so the pan texture's
    // receivers take their vertex colours undoubled.
    bool bDouble = cvi(CV_Saturate) != 0 && !pPan;
    uint8_t r = 255, gg = 255, b = 255;
    if (bFullbright || pLM)
    {
        r = (uint8_t)(255 * pTint[0]);
        gg = (uint8_t)(255 * pTint[1]);
        b = (uint8_t)(255 * pTint[2]);
    }

    MRWorldLight lights[MR_MAXWORLDLIGHTS];
    uint32_t nLights = !bFullbright && !pLM && g_nWLights ? mr_PolyLights(pPoly, lights) : 0;

    const WorldPolyVertex* pVerts = pPoly->m_Verts;
    uint32_t nVerts = pPoly->m_nVerts;
    if (cvi(CV_FixTJunc) && pPoly->m_pFixVerts)
    {
        pVerts = pPoly->m_pFixVerts;
        nVerts = pPoly->m_nFixVerts;
    }
    rd_Begin(RD_TRIANGLE_FAN);
    for (uint32_t v = 0; v < nVerts; v++)
    {
        const WorldPolyVertex& vert = pVerts[v];
        const LTVector& pos = *vert.m_pPos;
        if (bFullbright || pLM)
            rd_Color4ub(r, gg, b, nAlpha);
        else
        {
            uint8_t rgb[3] = { vert.m_R, vert.m_G, vert.m_B };
            if (nLights)
                mr_LightVertex(pos, lights, nLights, rgb);
            rd_Color4ub(mr_Saturate(rgb[0], pTint[0], bDouble), mr_Saturate(rgb[1], pTint[1], bDouble),
                       mr_Saturate(rgb[2], pTint[2], bDouble), nAlpha);
        }
        rd_TexCoord2f(vert.m_U * fU, vert.m_V * fV);
        if (pDetail)
            rd_MultiTexCoord2f(g_nDetailUnit,
                                      vert.m_U * fDA + vert.m_V * fDB, vert.m_U * fDC + vert.m_V * fDD);
        else if (pEnv)
        {
            float s, t;
            mr_EnvMapCoords(pos, pPoly->m_pSurface->m_pPlane->m_Normal, cvf(CV_DetailTextureScale), &s, &t);
            rd_MultiTexCoord2f(g_nDetailUnit, s, t);
        }
        if (pLM)
        {
            float dx = pos.x - pPoly->m_LMOrigin.x, dy = pos.y - pPoly->m_LMOrigin.y,
                  dz = pos.z - pPoly->m_LMOrigin.z;
            float lu = (dx * P.x + dy * P.y + dz * P.z) / fGrid + fOffU;
            float lv = (dx * Q.x + dy * Q.y + dz * Q.z) / fGrid + fOffV;
            rd_MultiTexCoord2f(g_nLMUnit, lu * fInvSize, lv * fInvSize);
        }
        else if (pPan)
            rd_MultiTexCoord2f(g_nLMUnit, (pos.x + g_fPanOffX) * g_fPanU,
                                      (pos.z + g_fPanOffZ) * g_fPanV);
        rd_Vertex3f(pos.x, pos.y, pos.z);
    }
    rd_End();
}

static const float g_White[3] = { 1, 1, 1 };

// Draw the queue, sorted by texture and lightmap atlas. pBack brings the
// frame's dynamic lights into the queued polys' space (world models).
static void mr_FlushPolys(const float* pTint, uint8_t nAlpha, const LTMatrix* pBack)
{
    qsort(g_pDrawList, g_nDrawList, sizeof(MRDrawPoly), mr_CompareDrawPoly);
    bool bLightmaps = g_Frame.m_bLightmaps;
    mr_SetupWorldLights(pBack);
    mr_SetupPan(!pBack);
    const LTVector& c = g_Frame.m_Cam.m_Pos;
    if (pBack)
    {
        const LTMatrix& M = *pBack;
        g_vPolyCam.x = M.m[0][0] * c.x + M.m[0][1] * c.y + M.m[0][2] * c.z + M.m[0][3];
        g_vPolyCam.y = M.m[1][0] * c.x + M.m[1][1] * c.y + M.m[1][2] * c.z + M.m[1][3];
        g_vPolyCam.z = M.m[2][0] * c.x + M.m[2][1] * c.y + M.m[2][2] * c.z + M.m[2][3];
    }
    else
        g_vPolyCam = c;
    if (bLightmaps)
    {
        MRWorldLight lights[MR_MAXWORLDLIGHTS];
        for (uint32_t i = 0; g_nWLights && i < g_nDrawList; i++)
        {
            WorldPoly* pPoly = g_pDrawList[i].m_pPoly;
            MRTexture* pTex = g_pDrawList[i].m_pTex;
            if (!pPoly->m_pLMPage || (pTex && (pTex->m_DtxFlags & DTX_FULLBRITE)))
                continue;
            uint32_t n = mr_PolyLights(pPoly, lights);
            if (n)
                mr_LightPolyLM(g_Frame.m_pContext, pPoly, lights, n);
        }
        mr_UploadLightmaps(g_Frame.m_pContext);
    }
    mr_ForgetBindings();
    mr_BeginUnits(bLightmaps);
    for (uint32_t i = 0; i < g_nDrawList; i++)
    {
        mr_SetBaseTexture(g_pDrawList[i].m_pTex);
        mr_DrawPoly(g_pDrawList[i].m_pPoly, g_pDrawList[i].m_pTex, bLightmaps, pTint, nAlpha);
    }
    mr_EndUnits();
    g_nDrawList = 0;
    g_nWLights = 0;
}

// ---- visibility -------------------------------------------------------------------

// r_ProcessObject's test (docs/objects.md 2.4); no portal views here.
bool mr_ShouldDraw(LTObject* pObj)
{
    int nType = pObj->m_ObjectType;
    if (nType <= OT_NORMAL || nType >= NUM_OBJECTTYPES || nType == OT_CAMERA || nType == OT_POLYGRID)
        return false;
    if (pObj->m_Flags2 & FLAG2_SKYOBJECT)
        return false;
    return (pObj->m_Flags & FLAG_VISIBLE) != 0;
}

static void mr_ProcessAttachments(LTObject* pObj, uint32_t nDepth)
{
    if (nDepth > 32)
        return;
    for (Attachment* a = pObj->m_pAttachments; a; a = a->m_pNext)
    {
        // The engine places the child at its attachment point.
        LTObject* pChild = g.pStruct->ProcessAttachment(pObj, a);
        if (!pChild || pChild->m_FrameCode == g_Frame.m_ObjectFrameCode)
            continue;
        mr_ProcessAttachments(pChild, nDepth + 1);
        if (mr_ShouldDraw(pChild))
        {
            mr_QueueObject(pChild);
            pChild->m_FrameCode = g_Frame.m_ObjectFrameCode;
        }
    }
}

static void mr_ReallyProcessObject(LTObject* pObj)
{
    mr_QueueObject(pObj);
    if (pObj->m_pAttachments)
        mr_ProcessAttachments(pObj, 0);
}

static void mr_QueueWorldPoly(WorldPoly* pPoly)
{
    if (pPoly->m_FrameCode == g_Frame.m_WorldFrameCode)
        return;
    pPoly->m_FrameCode = g_Frame.m_WorldFrameCode;
    Surface* pSurf = pPoly->m_pSurface;
    if (!pSurf)
        return;
    uint32_t nFlags = pSurf->m_Flags & SURF_FLAGMASK;
    if (nFlags & SURF_INVISIBLE)
        return;
    if (nFlags & SURF_SKY)
    {
        g_Frame.m_nSkyPolys++;      // the sky shows here (never drawn itself)
        return;
    }
    if (cvi(CV_DrawWorld))
        mr_QueueFacingPoly(pPoly, g_Frame.m_Cam.m_Pos);
}

static void cb_Object(LTObject* pObj, void* pUser)
{
    (void)pUser;
    if (mr_ShouldDraw(pObj))
        mr_ReallyProcessObject(pObj);
}

// r_AddRenderObjects: a world tree node's objects, those that may be drawn.
static void cb_AddObjects(LTLink* pHead, LTObject*** ppArray, int* pnCount)
{
    for (LTLink* pCur = pHead->m_pPrev; pCur != pHead; pCur = pCur->m_pPrev)
    {
        LTObject* pObj = (LTObject*)pCur->m_pData;
        if (pObj->m_188)
            break;
        if (mr_ShouldDraw(pObj))
            (*ppArray)[(*pnCount)++] = pObj;
    }
}

// r_ProcessLeaf: the leaf's poly grids and its polygons.
static void cb_Leaf(Leaf* pLeaf, void* pUser)
{
    (void)pUser;
    pLeaf->m_FrameCode = g_Frame.m_WorldFrameCode;
    for (LTLink* pCur = pLeaf->m_Objects.m_pNext; pCur && pCur != &pLeaf->m_Objects; pCur = pCur->m_pNext)
    {
        LTObject* pObj = *(LTObject**)((uint8_t*)pCur->m_pData + 0x18);
        if (pObj && pObj->m_ObjectType == OT_POLYGRID && (pObj->m_Flags & FLAG_VISIBLE) &&
            pObj->m_FrameCode != g_Frame.m_ObjectFrameCode)
        {
            pObj->m_FrameCode = g_Frame.m_ObjectFrameCode;
            mr_ReallyProcessObject(pObj);
        }
    }
    for (uint32_t i = 0; i < pLeaf->m_nPolies; i++)
        if (pLeaf->m_Polies[i])
            mr_QueueWorldPoly(pLeaf->m_Polies[i]);
}

static LTBOOL cb_PortalTest(void* pUserPortal)
{
    (void)pUserPortal;
    return 1;
}

// r_NodeFilter: a world tree node wholly outside the frustum is skipped.
static LTBOOL cb_NodeFilter(WorldTreeNode* pNode)
{
    return mr_SphereVisible(wtn_Center(pNode), wtn_Radius(pNode));
}

// r_IncrementFrameCode: the world's frame code; at its wrap every leaf and
// poly stamp is cleared.
static void mr_IncrementFrameCode(MRContext* pContext)
{
    MainWorld* pWorld = pContext->m_pWorld;
    if (pContext->m_FrameCode == 0xffff)
    {
        pContext->m_FrameCode = 1;
        for (uint32_t i = 0; i < pWorld->m_Worlds.m_nElements; i++)
        {
            WorldData* pData = mw_World(pWorld, i);
            WorldBsp* pBsp = pData ? pData->m_pWorldBsp : NULL;
            if (!pBsp)
                continue;
            for (uint32_t l = 0; l < pBsp->m_nLeafs; l++)
                pBsp->m_Leafs[l].m_FrameCode = 0;
            for (uint32_t p = 0; p < pBsp->m_nPolies; p++)
                if (pBsp->m_Polies[p])
                    pBsp->m_Polies[p]->m_FrameCode = 0;
        }
    }
    else
        pContext->m_FrameCode++;
}

static WorldBsp* mr_FindBsp(MainWorld* pWorld, uint32_t nFlag)
{
    for (uint32_t i = 0; i < pWorld->m_Worlds.m_nElements; i++)
    {
        WorldData* pData = mw_World(pWorld, i);
        if (pData && pData->m_pWorldBsp && (pData->m_pWorldBsp->m_WorldInfoFlags & nFlag))
            return pData->m_pWorldBsp;
    }
    return NULL;
}

// No PVS (or DrawAll): every poly of the main world, and every object filed
// in the vis BSP's nodes (r_DrawBSP).
static void mr_VisitAll(MainWorld* pWorld)
{
    WorldBsp* pMain = mr_FindBsp(pWorld, WORLDINFO_MAINWORLD);
    if (pMain)
        for (uint32_t i = 0; i < pMain->m_nPolies; i++)
            if (pMain->m_Polies[i])
                mr_QueueWorldPoly(pMain->m_Polies[i]);
    WorldBsp* pVis = mr_FindBsp(pWorld, WORLDINFO_VIS);
    if (!pVis)
        pVis = pMain;
    Node* pNodes = pVis ? (Node*)pVis->m_Nodes : NULL;
    for (uint32_t i = 0; pNodes && i < pVis->m_nNodes; i++)
    {
        LTLink* pHead = (LTLink*)&pNodes[i].m_Objects;
        for (LTLink* pCur = pHead->m_pPrev; pCur && pCur != pHead; pCur = pCur->m_pPrev)
        {
            LTObject* pObj = (LTObject*)pCur->m_pData;
            if (!pObj || pObj->m_188)
                break;
            if (pObj->m_FrameCode != g_Frame.m_ObjectFrameCode && mr_ShouldDraw(pObj))
            {
                pObj->m_FrameCode = g_Frame.m_ObjectFrameCode;
                mr_ReallyProcessObject(pObj);
            }
        }
    }
}

// ---- the world's segment test --------------------------------------------------------
//
// The engine's i_IntersectSegment over the main world's BSP, as d3d.ren
// carries its own copy for the sun test (0x1006a1b0): the segment is split
// at each node's plane, the near part searched first; where it crosses
// to a node's front, the node's poly is hit if it is solid and the
// crossing is inside its edges.

#define MR_ISECT_EPS 0.01f

static WorldPoly* mr_IntersectNode(Node* pNode, LTVector vFrom, LTVector vTo, int nDepth)
{
    while (pNode && !(pNode->m_Flags & 3) && nDepth < 512)
    {
        WorldPoly* pPoly = pNode->m_pPoly;
        if (!pPoly || !pPoly->m_pSurface || !pPoly->m_pSurface->m_pPlane)
            return NULL;
        const LTPlane& P = *pPoly->m_pSurface->m_pPlane;
        float d1 = P.m_Normal.x * vFrom.x + P.m_Normal.y * vFrom.y + P.m_Normal.z * vFrom.z - P.m_Dist;
        float d2 = P.m_Normal.x * vTo.x + P.m_Normal.y * vTo.y + P.m_Normal.z * vTo.z - P.m_Dist;
        if (d1 > MR_ISECT_EPS && d2 > MR_ISECT_EPS)
        {
            pNode = pNode->m_Sides[1];
            continue;
        }
        int iSide;
        if (d1 < -MR_ISECT_EPS)
        {
            if (d2 < -MR_ISECT_EPS)
            {
                pNode = pNode->m_Sides[0];
                continue;
            }
            iSide = 0;
        }
        else if (d1 > MR_ISECT_EPS)
            iSide = 1;
        else
            iSide = d2 < 0.0f ? 1 : 0;

        float fDenom = d2 - d1;
        if (fDenom != 0.0f && (d1 < -MR_ISECT_EPS || d1 > MR_ISECT_EPS))
        {
            float t = -d1 / fDenom;
            LTVector vMid = { vFrom.x + (vTo.x - vFrom.x) * t, vFrom.y + (vTo.y - vFrom.y) * t,
                              vFrom.z + (vTo.z - vFrom.z) * t };
            WorldPoly* pHit = mr_IntersectNode(pNode->m_Sides[iSide], vFrom, vMid, nDepth + 1);
            if (pHit)
                return pHit;
            if (iSide == 1)
            {
                bool bHit = (pPoly->m_pSurface->m_Flags & SURF_SOLID) != 0;
                float cx = pPoly->m_Center.x - vMid.x, cy = pPoly->m_Center.y - vMid.y,
                      cz = pPoly->m_Center.z - vMid.z;
                if (bHit && pPoly->m_Radius * pPoly->m_Radius < cx * cx + cy * cy + cz * cz)
                    bHit = false;
                const WorldPolyVertex* pVerts = pPoly->m_Verts;
                const LTVector* pPrev = pPoly->m_nVerts ? pVerts[pPoly->m_nVerts - 1].m_pPos : NULL;
                for (uint32_t e = 0; bHit && e < pPoly->m_nVerts; e++)
                {
                    const LTVector* pV = pVerts[e].m_pPos;
                    LTVector ed = { pPrev->x - pV->x, pPrev->y - pV->y, pPrev->z - pV->z };
                    // N.Cross(edge), which in LithTech is edge x N.
                    LTVector n = { ed.y * P.m_Normal.z - ed.z * P.m_Normal.y,
                                   ed.z * P.m_Normal.x - ed.x * P.m_Normal.z,
                                   ed.x * P.m_Normal.y - ed.y * P.m_Normal.x };
                    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
                    if (len > 0.0f)
                    {
                        n.x /= len; n.y /= len; n.z /= len;
                    }
                    float fEdge = n.x * pV->x + n.y * pV->y + n.z * pV->z;
                    float fMid = n.x * vMid.x + n.y * vMid.y + n.z * vMid.z;
                    if (fMid - fEdge < -MR_ISECT_EPS)
                        bHit = false;
                    pPrev = pV;
                }
                if (bHit)
                    return pPoly;
            }
            else if (t > 0.99f)
                return NULL;
            vFrom = vMid;
        }
        pNode = pNode->m_Sides[iSide == 0];
    }
    return NULL;
}

// d3d.ren's 0x1001be00: does the segment from vFrom along vRay first hit
// a sky poly?
bool mr_SeesSky(const LTVector& vFrom, const LTVector& vRay)
{
    MainWorld* pWorld = g_Frame.m_pContext ? g_Frame.m_pContext->m_pWorld : NULL;
    WorldBsp* pMain = pWorld ? mr_FindBsp(pWorld, WORLDINFO_MAINWORLD) : NULL;
    if (!pMain || !pMain->m_RootNode)
        return false;
    LTVector vTo = { vFrom.x + vRay.x, vFrom.y + vRay.y, vFrom.z + vRay.z };
    WorldPoly* pHit = mr_IntersectNode((Node*)pMain->m_RootNode, vFrom, vTo, 0);
    return pHit && (pHit->m_pSurface->m_Flags & SURF_SKY);
}

// The world's size as the sun test reads it (MainWorld+0xb8, in its world
// tree); the extents' diagonal if that is not set.
float mr_WorldSize(void)
{
    MainWorld* pWorld = g_Frame.m_pContext ? g_Frame.m_pContext->m_pWorld : NULL;
    if (!pWorld)
        return 0.0f;
    float f;
    memcpy(&f, (const uint8_t*)pWorld + 0xb8, 4);
    if (f > 1.0f && f < 1.0e7f)
        return f;
    float dx = pWorld->m_ExtentsMax.x - pWorld->m_ExtentsMin.x, dy = pWorld->m_ExtentsMax.y - pWorld->m_ExtentsMin.y,
          dz = pWorld->m_ExtentsMax.z - pWorld->m_ExtentsMin.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ---- world models -----------------------------------------------------------------

static void mr_ObjectTint(LTObject* pObj, float* pTint)
{
    pTint[0] = pObj->m_ColorR / 255.0f;
    pTint[1] = pObj->m_ColorG / 255.0f;
    pTint[2] = pObj->m_ColorB / 255.0f;
}

static uint32_t mr_FirstSurfaceFlags(WorldModelInstance* pWM)
{
    WorldBsp* pBsp = pWM->m_pWorldBsp;
    if (!pBsp || !pBsp->m_nPolies || !pBsp->m_Polies[0] || !pBsp->m_Polies[0]->m_pSurface)
        return 0;
    return pBsp->m_Polies[0]->m_pSurface->m_Flags & SURF_FLAGMASK;
}

// r_ProcessWorldModel: which set it goes in.
void mr_ProcessWorldModel(LTObject* pObj)
{
    WorldModelInstance* pWM = (WorldModelInstance*)pObj;
    if (!pWM->m_pWorldBsp || !pWM->m_pWorldBsp->m_nPolies)
        return;
    if (mr_FirstSurfaceFlags(pWM) & SURF_TRANSLUCENT)
        mr_AddTranslucent(pObj, mr_DrawTranslucentWorldModel);
    else if (pObj->m_Flags2 & FLAG2_CHROMAKEY)
        mr_ObjList(VS_WORLDMODELS_CHROMAKEY)->Add(pObj);
    else
        mr_ObjList(VS_WORLDMODELS)->Add(pObj);
}

static LTVector mr_ToLocal(const LTMatrix& M, const LTVector& v)
{
    LTVector r;
    r.x = M.m[0][0] * v.x + M.m[0][1] * v.y + M.m[0][2] * v.z + M.m[0][3];
    r.y = M.m[1][0] * v.x + M.m[1][1] * v.y + M.m[1][2] * v.z + M.m[1][3];
    r.z = M.m[2][0] * v.x + M.m[2][1] * v.y + M.m[2][2] * v.z + M.m[2][3];
    return r;
}

// r_DrawSolidWorldModel: every poly of the model's BSP that faces the
// camera, under the model's matrix, lit like the world.
void mr_DrawSolidWorldModel(LTObject* pObj)
{
    WorldModelInstance* pWM = (WorldModelInstance*)pObj;
    WorldBsp* pBsp = pWM->m_pWorldBsp;
    if (!pBsp || !cvi(CV_DrawWorldModels))
        return;
    float fRadius = *(float*)((uint8_t*)pBsp + 0x14c);
    if (fRadius > 0.0f && !mr_SphereVisible(pObj->m_Pos, fRadius))
        return;
    LTVector vCam = mr_ToLocal(pWM->m_BackTransform, g_Frame.m_Cam.m_Pos);
    for (uint32_t i = 0; i < pBsp->m_nPolies; i++)
    {
        WorldPoly* pPoly = pBsp->m_Polies[i];
        if (!pPoly || !pPoly->m_pSurface || pPoly->m_FrameCode == g_Frame.m_WorldFrameCode)
            continue;
        pPoly->m_FrameCode = g_Frame.m_WorldFrameCode;
        if (pPoly->m_pSurface->m_Flags & (SURF_INVISIBLE | SURF_SKY))
            continue;
        mr_QueueFacingPoly(pPoly, vCam);
    }
    float tint[3];
    mr_ObjectTint(pObj, tint);
    mr_ObjectFog(pObj, true);
    rd_PushMatrix();
    mr_MultMatrix(pWM->m_Transform);
    mr_FlushPolys(tint, pObj->m_ColorA, &pWM->m_BackTransform);
    rd_PopMatrix();
    mr_ObjectFog(pObj, false);
}

// Back to front through the model's own BSP (r_RecurseAndDrawWM), with the
// camera in model space (the shipped code uses the world-space camera,
// right only for a model that has not moved).
static void mr_DrawBspBackToFront(Node* pNode, const LTVector& vCam, const float* pTint,
                                   uint8_t nAlpha, int nDepth)
{
    if (!pNode || (pNode->m_Flags & 3) || nDepth > 1024 || !pNode->m_pPoly)
        return;
    WorldPoly* pPoly = pNode->m_pPoly;
    const LTPlane* pPlane = pPoly->m_pSurface->m_pPlane;
    int nSide = (pPlane->m_Normal.x * vCam.x + pPlane->m_Normal.y * vCam.y +
                 pPlane->m_Normal.z * vCam.z - pPlane->m_Dist) >= 0.0f;
    mr_DrawBspBackToFront(pNode->m_Sides[!nSide], vCam, pTint, nAlpha, nDepth + 1);
    if (!(pPoly->m_pSurface->m_Flags & SURF_INVISIBLE))
    {
        MRTexture* pTex = mr_SurfaceTexture(pPoly->m_pSurface);
        mr_SetBaseTexture(pTex);
        mr_DrawPoly(pPoly, pTex, false, pTint, nAlpha);
    }
    mr_DrawBspBackToFront(pNode->m_Sides[nSide], vCam, pTint, nAlpha, nDepth + 1);
}

void mr_DrawTranslucentWorldModel(LTObject* pObj)
{
    WorldModelInstance* pWM = (WorldModelInstance*)pObj;
    WorldBsp* pBsp = pWM->m_pWorldBsp;
    if (!pBsp || !cvi(CV_DrawWorldModels))
        return;
    bool bAdditive = (mr_FirstSurfaceFlags(pWM) & SURF_ADDITIVE) != 0;
    rd_PushAttrib(RD_ATTR_COLOR);
    if (bAdditive)
        rd_BlendFunc(RD_ONE, RD_ONE);
    float tint[3];
    mr_ObjectTint(pObj, tint);
    mr_ObjectFog(pObj, true);
    rd_PushMatrix();
    mr_MultMatrix(pWM->m_Transform);
    LTVector vCam = mr_ToLocal(pWM->m_BackTransform, g_Frame.m_Cam.m_Pos);
    mr_ForgetBindings();
    mr_DrawBspBackToFront((Node*)pBsp->m_RootNode, vCam, tint, pObj->m_ColorA, 0);
    rd_PopMatrix();
    mr_ObjectFog(pObj, false);
    rd_PopAttrib();
}

// ---- the sky ----------------------------------------------------------------------

// A sky world model: its BSP as authored (sky brushes do not move), front-
// facing polys only, node poly before its far side (r_DrawSkyWorldModel).
static void mr_DrawSkyBsp(Node* pNode, const LTVector& vCam, const float* pTint, uint8_t nAlpha, int nDepth)
{
    if (!pNode || (pNode->m_Flags & 3) || nDepth > 1024 || !pNode->m_pPoly)
        return;
    WorldPoly* pPoly = pNode->m_pPoly;
    const LTPlane* pPlane = pPoly->m_pSurface->m_pPlane;
    int bFront = (pPlane->m_Normal.x * vCam.x + pPlane->m_Normal.y * vCam.y +
                  pPlane->m_Normal.z * vCam.z - pPlane->m_Dist) > 0.0f;
    if (bFront && !(pPoly->m_pSurface->m_Flags & SURF_INVISIBLE))
    {
        MRTexture* pTex = mr_SurfaceTexture(pPoly->m_pSurface);
        mr_SetBaseTexture(pTex);
        mr_DrawPoly(pPoly, pTex, false, pTint, nAlpha);
    }
    mr_DrawSkyBsp(pNode->m_Sides[!bFront], vCam, pTint, nAlpha, nDepth + 1);
    mr_DrawSkyBsp(pNode->m_Sides[bFront], vCam, pTint, nAlpha, nDepth + 1);
}

// r_DrawSky: the sky objects in their list order, depth off, seen with the
// main camera's rotation from the camera's place squashed into the sky box
// (r_SetupSkyStuff).
static void mr_DrawSky(SceneDesc* pScene, MainWorld* pWorld)
{
    if (!cvi(CV_DrawSky) || pScene->m_nSkyObjects <= 0 || !g_Frame.m_nSkyPolys)
        return;
    MRCamera sky = g_Frame.m_Cam;
    LTVector t = { 0.5f, 0.5f, 0.5f };
    LTVector ext = { pWorld->m_ExtentsMax.x - pWorld->m_ExtentsMin.x,
                     pWorld->m_ExtentsMax.y - pWorld->m_ExtentsMin.y,
                     pWorld->m_ExtentsMax.z - pWorld->m_ExtentsMin.z };
    if (ext.x != 0.0f && ext.y != 0.0f && ext.z != 0.0f)
    {
        t.x = (sky.m_Pos.x - pWorld->m_ExtentsMin.x) / ext.x;
        t.y = (sky.m_Pos.y - pWorld->m_ExtentsMin.y) / ext.y;
        t.z = (sky.m_Pos.z - pWorld->m_ExtentsMin.z) / ext.z;
    }
    const SkyDef& def = pScene->m_SkyDef;
    sky.m_Pos.x = def.m_ViewMin.x + (def.m_ViewMax.x - def.m_ViewMin.x) * t.x;
    sky.m_Pos.y = def.m_ViewMin.y + (def.m_ViewMax.y - def.m_ViewMin.y) * t.y;
    sky.m_Pos.z = def.m_ViewMin.z + (def.m_ViewMax.z - def.m_ViewMin.z) * t.z;
    sky.m_fNear = 0.01f;
    sky.m_fScale = cvf(CV_SkyScale) > 0.0f ? cvf(CV_SkyScale) : 1.0f;
    mr_LoadCamera(&sky);

    rd_PushAttrib(RD_ATTR_ALL);
    rd_Disable(RD_DEPTH_TEST);
    rd_DepthMask(false);
    rd_Disable(RD_ALPHA_TEST);
    mr_SetFog(true);
    LTVector vSkyCam = sky.m_Pos;
    for (int i = 0; i < pScene->m_nSkyObjects; i++)
    {
        LTObject* pObj = pScene->m_SkyObjects[i];
        if (!pObj || !(pObj->m_Flags & FLAG_VISIBLE))
            continue;
        if (pObj->m_ObjectType == OT_WORLDMODEL)
        {
            WorldModelInstance* pWM = (WorldModelInstance*)pObj;
            if (!pWM->m_pWorldBsp)
                continue;
            // Only a translucent sky model blends (r_SetTranslucentObjectStates);
            // the others are opaque (r_UnsetTranslucentObjectStates), whatever
            // alpha their textures or the object carry (d3d.ren 0x10030a60).
            uint32_t nFlags = mr_FirstSurfaceFlags(pWM);
            if (nFlags & SURF_TRANSLUCENT)
            {
                rd_Enable(RD_BLEND);
                if (nFlags & SURF_ADDITIVE)
                    rd_BlendFunc(RD_ONE, RD_ONE);
                else
                    rd_BlendFunc(RD_SRC_ALPHA, RD_ONE_MINUS_SRC_ALPHA);
            }
            else
                rd_Disable(RD_BLEND);
            float tint[3];
            mr_ObjectTint(pObj, tint);
            mr_ObjectFog(pObj, true);
            mr_ForgetBindings();
            mr_DrawSkyBsp((Node*)pWM->m_pWorldBsp->m_RootNode, vSkyCam, tint, pObj->m_ColorA, 0);
            mr_ObjectFog(pObj, false);
        }
        else
        {
            g_Frame.m_SkyCam = sky;
            mr_DrawSkyObject(pObj);
        }
    }
    rd_PopAttrib();
    mr_LoadCamera(&g_Frame.m_Cam);
}

// ---- screen passes --------------------------------------------------------------------

static void mr_ScreenQuad(void)
{
    rd_MatrixMode(RD_PROJECTION);
    rd_PushMatrix();
    rd_LoadIdentity();
    rd_MatrixMode(RD_MODELVIEW);
    rd_PushMatrix();
    rd_LoadIdentity();
    rd_Begin(RD_QUADS);
    rd_Vertex2f(-1, -1); rd_Vertex2f(1, -1); rd_Vertex2f(1, 1); rd_Vertex2f(-1, 1);
    rd_End();
    rd_PopMatrix();
    rd_MatrixMode(RD_PROJECTION);
    rd_PopMatrix();
    rd_MatrixMode(RD_MODELVIEW);
}

// The global light scale darkens the view (dst * scale, as dst*src + src*dst
// with src = scale/2), and the camera's light add brightens it.
static void mr_ScreenPasses(SceneDesc* pScene)
{
    const LTVector& s = pScene->m_GlobalLightScale;
    const LTVector& a = pScene->m_GlobalLightAdd;
    bool bScale = s.x < 0.99f || s.x > 1.01f || s.y < 0.99f || s.y > 1.01f || s.z < 0.99f || s.z > 1.01f;
    bool bAdd = cvi(CV_LightAddPoly) && (a.x >= 0.001f || a.y >= 0.001f || a.z >= 0.001f);
    if (!bScale && !bAdd)
        return;
    rd_PushAttrib(RD_ATTR_ALL);
    rd_Disable(RD_TEXTURE_2D);
    rd_Disable(RD_FOG);
    rd_Disable(RD_DEPTH_TEST);
    rd_Disable(RD_ALPHA_TEST);
    rd_DepthMask(false);
    rd_Enable(RD_BLEND);
    if (bScale)
    {
        rd_BlendFunc(RD_DST_COLOR, RD_SRC_COLOR);
        rd_Color4f(s.x * 0.5f, s.y * 0.5f, s.z * 0.5f, 1.0f);
        mr_ScreenQuad();
    }
    if (bAdd)
    {
        rd_BlendFunc(RD_ONE, RD_ONE);
        rd_Color4f(a.x, a.y, a.z, 1.0f);
        mr_ScreenQuad();
    }
    rd_PopAttrib();
}

// ---- RenderScene ---------------------------------------------------------------------

LTBOOL mr_RenderScene(SceneDesc* pScene)
{
    if (!g.bContext || !pScene)
        return 0;
    if (g.bInOptimized2D)
    {
        mr_Print("Error: tried to render 3D while in optimized 2D mode.");
        return 0;
    }
    mr_ReadConsoleVariables();         // the Mac re-reads them every frame

    MRContext* pContext = pScene->m_DrawMode == DRAWMODE_NORMAL ? (MRContext*)pScene->m_pRenderContext : NULL;
    if (pScene->m_DrawMode == DRAWMODE_NORMAL && (!pContext || !pContext->m_pWorld))
        return 0;
    MainWorld* pWorld = pContext ? pContext->m_pWorld : NULL;

    memset(&g_Frame, 0, sizeof(g_Frame));
    g_Frame.m_pScene = pScene;
    g_Frame.m_pContext = pContext;
    mr_SetupCamera(pScene, &g_Frame.m_Cam);
    mr_SetupFrustum(&g_Frame.m_Cam);
    if (pContext)
    {
        mr_IncrementFrameCode(pContext);
        g_Frame.m_WorldFrameCode = pContext->m_FrameCode;
        g_Frame.m_fLMGrid = pWorld->m_LMGridSize > 0.0f ? pWorld->m_LMGridSize : 1.0f;
        g_Frame.m_bLightmaps = g_RDCaps.nTextureUnits >= 2 && pContext->m_pAtlases && cvi(CV_LightMap) &&
                               pContext->m_nContextGen == g.nContextGen;
    }
    // The object frame code stamps LTObject+0x58: the engine's visibility
    // walk and the attachment pass process each object once a frame.
    g_Frame.m_ObjectFrameCode = g.pStruct->IncObjectFrameCode();
    if (g.pStruct->IncCurTextureFrameCode)
        g.pStruct->IncCurTextureFrameCode();
    mr_BeginObjects();

    // What is visible.
    if (pContext)
    {
        if ((pWorld->m_Flags & MAINWORLD_HASVIS) && !cvi(CV_DrawAll))
        {
            VisQueryRequest req;
            memset(&req, 0, sizeof(req));
            req.m_Viewpoint = g_Frame.m_Cam.m_Pos;
            req.m_ViewRadius = 10000.0f;
            req.m_ObjectCB = cb_Object;
            req.m_AddObjects = cb_AddObjects;
            req.m_LeafCB = cb_Leaf;
            req.m_PortalTest = cb_PortalTest;
            req.m_NodeFilter = cb_NodeFilter;
            mw_DoVisQuery(pWorld, &req);
        }
        else
            mr_VisitAll(pWorld);
    }
    else
    {
        for (int i = 0; i < pScene->m_ObjectListSize; i++)
        {
            LTObject* pObj = pScene->m_pObjectList ? pScene->m_pObjectList[i] : NULL;
            if (pObj && pObj->m_ObjectType > OT_NORMAL && pObj->m_ObjectType < NUM_OBJECTTYPES)
                mr_QueueObject(pObj);
        }
    }

    // Draw it.
    rd_PushAttrib(RD_ATTR_ALL);
    rd_MatrixMode(RD_TEXTURE);
    rd_PushMatrix();
    rd_LoadIdentity();
    rd_MatrixMode(RD_PROJECTION);
    rd_PushMatrix();
    rd_MatrixMode(RD_MODELVIEW);
    rd_PushMatrix();
    mr_LoadCamera(&g_Frame.m_Cam);

    rd_Disable(RD_BLEND);
    // Texels are punched out only in the chromakey sets (d3d.ren's
    // sub_10023eb0/sub_10023f40 around them): solid textures' alpha means
    // nothing, and many 5551 ones (window shutters, the sky) carry 0. The
    // Mac renderer leaves r_Init's alpha test on everywhere.
    rd_Disable(RD_ALPHA_TEST);
    rd_AlphaGreater(0.1666f);
    rd_TexEnv(RD_ENV_MODULATE);

    if (pContext)
        mr_DrawSky(pScene, pWorld);

    rd_Enable(RD_DEPTH_TEST);
    rd_DepthFunc(RD_LEQUAL);
    rd_DepthMask(true);
    mr_SetFog(false);
    if (pContext)
    {
        if (g_Frame.m_bLightmaps)
            mr_UpdateLightmaps(pContext);
        mr_FlushPolys(g_White, 255, NULL);
    }
    mr_FlushObjects();

    rd_MatrixMode(RD_MODELVIEW);
    rd_PopMatrix();
    rd_MatrixMode(RD_PROJECTION);
    rd_PopMatrix();
    rd_MatrixMode(RD_TEXTURE);
    rd_PopMatrix();
    rd_MatrixMode(RD_MODELVIEW);
    rd_PopAttrib();

    // The screen passes cover the camera's rect.
    rd_PushAttrib(RD_ATTR_VIEWPORT);
    rd_Viewport(g_Frame.m_Cam.m_nViewX, g_Frame.m_Cam.m_nViewY, g_Frame.m_Cam.m_nViewW, g_Frame.m_Cam.m_nViewH);
    mr_ScreenPasses(pScene);
    rd_PopAttrib();
    return 0;
}
