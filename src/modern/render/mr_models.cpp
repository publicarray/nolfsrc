// mr_models.cpp -- models (OT_MODEL): animation, skinning, lighting.
//
// docs/objects.md part II. The engine has already advanced every
// animation tracker this frame; the renderer turns the trackers into one
// matrix per node with its own copy of the engine's transform maker (as
// d3d.ren does: CTransformMakerK, src/engine/lithtech/transform.cpp section
// B), skins the vertices of the chosen LOD on the CPU, lights them in model
// space (the world's light grid, the visible dynamic lights, the model
// hook) and draws each piece with its skin.
//
// Not done yet: chrome and
// specular passes, detail textures, LOD geomorphing, the distance-fade
// sprite and shadows.

#include <math.h>

#include "mrobjects.h"

// ---- the engine's model data (docs/objects.md II.1) -----------------------------

struct CModelXform { LTVector m_Pos; LTRotation m_Rot; };          // 0x1c
struct AnimNode { uint8_t pad[0x0c]; CModelXform* m_pXforms; };
struct ModelAnim { uint32_t m_nAllocedNodes; AnimNode** m_pAnimNodes; };
struct AnimInfo {                                                   // 0x20
    ModelAnim* m_pAnim;
    void*      m_pChildModel;           // +0x04: its relation xforms array is at its +0x04
    LTVector   m_vDims;
    LTVector   m_vTranslation;          // +0x14: added to node 0
};
struct ModelWeightSet { char m_Name[0x10]; uint32_t m_nAlloced; float* m_pWeights; };
struct ModelNode {
    uint8_t     pad00[4];
    LTVector    m_vOffset;              // 0x04 used when m_Flags & 2
    uint16_t    m_iNode;                // 0x10
    uint16_t    m_Flags;                // 0x12
    uint8_t     pad14[0x1c - 0x14];
    ModelNode** m_pChildren;            // 0x1c
    uint32_t    m_nChildren;            // 0x20
};
struct AnimTimeRef {                    // 0x1c
    void*    m_pModel;
    uint16_t m_PrevAnim, m_PrevFrame;
    uint32_t m_PrevTime;
    uint16_t m_CurAnim, m_CurFrame;
    uint32_t m_CurTime;
    float    m_Percent;
    uint32_t m_iWeightSet;
};
static_assert(sizeof(AnimTimeRef) == 0x1c, "AnimTimeRef");

typedef void (*NodeControlFn)(LTObject* hObj, uint32_t iNode, LTMatrix* pMat, void* pUser);

// What the instance's SetupTransformMaker (vtable +0x38) fills: the time
// refs of all its trackers and the node-control hook.
struct TMHeader {
    AnimTimeRef   m_TimeRefs[8];        // 0x000
    uint32_t      m_nTimeRefs;          // 0x0e0
    LTMatrix*     m_pBase;              // 0x0e4
    LTMatrix*     m_pTransforms;        // 0x0e8
    NodeControlFn m_NodeControlFn;      // 0x0ec
    void*         m_pNodeControlData;   // 0x0f0
    LTObject*     m_hObject;            // 0x0f4
    uint8_t       m_Rest[0x300 - 0xf8]; // the engine's scratch, whatever it writes
};
typedef void (__thiscall* SetupTMFn)(LTObject* pInst, TMHeader* pTM);

struct NewVertexWeight { float m_Vec[4]; uint32_t m_iNode; };       // 0x14
struct ModelVert {                                                  // 0x20
    NewVertexWeight* m_Weights;
    uint16_t m_nWeights, m_iReplacement;
    LTVector m_Vec, m_Normal;           // bind pose, model space
};
struct ModelTri { uint16_t m_Indices[3], pad; float m_UV[3][2]; };  // 0x20
struct PieceLOD {                                                   // 0x24
    uint32_t   m_00;
    ModelVert* m_Verts;
    uint32_t   m_nVerts;
    uint32_t   m_0c, m_10;
    ModelTri*  m_Tris;
    uint32_t   m_nTris;
    uint32_t   m_1c, m_20;
};
static_assert(sizeof(PieceLOD) == 0x24, "PieceLOD");
// A ModelPiece begins with its LOD 0.
struct ModelPiece : PieceLOD {
    uint32_t  m_iTexture;               // 0x24 skin index
    uint8_t   pad28[0x38 - 0x28];
    PieceLOD* m_pLODs;                  // 0x38 LODs 1..n
    uint32_t  m_nLODs;                  // 0x3c
};

template <typename T> static inline T rd(const void* p, uint32_t off) { T v; memcpy(&v, (const uint8_t*)p + off, sizeof(T)); return v; }

struct ModelView {                      // the Model fields read here
    void*        m_p;
    ModelNode**  Nodes() const       { return rd<ModelNode**>(m_p, 0x28); }
    uint32_t     nNodes() const      { return rd<uint32_t>(m_p, 0x2c); }
    ModelPiece** Pieces() const      { return rd<ModelPiece**>(m_p, 0x38); }
    uint32_t     nPieces() const     { return rd<uint32_t>(m_p, 0x3c); }
    ModelWeightSet** WeightSets() const { return rd<ModelWeightSet**>(m_p, 0x48); }
    uint32_t     nWeightSets() const { return rd<uint32_t>(m_p, 0x4c); }
    float*       LODDists() const    { return rd<float*>(m_p, 0x90); }
    uint32_t     nExtraLODs() const  { return rd<uint32_t>(m_p, 0x94); }
    float        LODDist0() const    { return rd<float>(m_p, 0xa0); }
    float        Radius() const      { return rd<float>(m_p, 0xa4); }
    float        VisRadius() const   { return rd<float>(m_p, 0xa8); }
    uint32_t     NormalRefNode() const { return rd<uint32_t>(m_p, 0xf4); }
    const LTMatrix& NormalRefMat() const { return *(const LTMatrix*)((const uint8_t*)m_p + 0xfc); }
    uint32_t     bNormalRef() const  { return rd<uint32_t>(m_p, 0x13c); }
    uint32_t     bFovOffset() const  { return rd<uint32_t>(m_p, 0x140); }
    float        FovXOffset() const  { return rd<float>(m_p, 0x144); }
    float        FovYOffset() const  { return rd<float>(m_p, 0x148); }
    AnimInfo*    Anims() const       { return rd<AnimInfo*>(m_p, 0x168); }
    uint32_t     nAnims() const      { return rd<uint32_t>(m_p, 0x16c); }
    ModelNode*   RootNode() const    { return rd<ModelNode*>(m_p, 0x2c0); }
};

// ---- matrices and quaternions (the SDK's) ------------------------------------------

static void mat_Mul(LTMatrix* pOut, const LTMatrix& a, const LTMatrix& b)
{
    LTMatrix r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    *pOut = r;
}

static void mat_FromQuat(LTMatrix* pOut, const LTRotation& q, const LTVector& t)
{
    float R[3][3];
    mr_RotationMatrix(q, R);
    for (int i = 0; i < 3; i++)
    {
        pOut->m[i][0] = R[i][0];
        pOut->m[i][1] = R[i][1];
        pOut->m[i][2] = R[i][2];
    }
    pOut->m[0][3] = t.x;
    pOut->m[1][3] = t.y;
    pOut->m[2][3] = t.z;
    pOut->m[3][0] = pOut->m[3][1] = pOut->m[3][2] = 0.0f;
    pOut->m[3][3] = 1.0f;
}

// The inverse of a rotation-scale-translation (affine) matrix.
void mr_InverseAffine(LTMatrix* pOut, const LTMatrix& M)
{
    float a = M.m[0][0], b = M.m[0][1], c = M.m[0][2];
    float d = M.m[1][0], e = M.m[1][1], f = M.m[1][2];
    float g = M.m[2][0], h = M.m[2][1], k = M.m[2][2];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    float inv = det != 0.0f ? 1.0f / det : 0.0f;
    LTMatrix r;
    r.m[0][0] = (e * k - f * h) * inv; r.m[0][1] = (c * h - b * k) * inv; r.m[0][2] = (b * f - c * e) * inv;
    r.m[1][0] = (f * g - d * k) * inv; r.m[1][1] = (a * k - c * g) * inv; r.m[1][2] = (c * d - a * f) * inv;
    r.m[2][0] = (d * h - e * g) * inv; r.m[2][1] = (b * g - a * h) * inv; r.m[2][2] = (a * e - b * d) * inv;
    for (int i = 0; i < 3; i++)
        r.m[i][3] = -(r.m[i][0] * M.m[0][3] + r.m[i][1] * M.m[1][3] + r.m[i][2] * M.m[2][3]);
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
    r.m[3][3] = 1.0f;
    *pOut = r;
}

static LTVector mat_Apply(const LTMatrix& M, const LTVector& v)
{
    LTVector r = { M.m[0][0] * v.x + M.m[0][1] * v.y + M.m[0][2] * v.z + M.m[0][3],
                   M.m[1][0] * v.x + M.m[1][1] * v.y + M.m[1][2] * v.z + M.m[1][3],
                   M.m[2][0] * v.x + M.m[2][1] * v.y + M.m[2][2] * v.z + M.m[2][3] };
    return r;
}

static void quat_Slerp(LTRotation* pOut, const LTRotation& q1, const LTRotation& q2, float t)
{
    float cosom = q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w;
    LTRotation b = q2;
    if (cosom < 0.0f)
    {
        cosom = -cosom;
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
    }
    float s0, s1;
    if (1.0f - cosom > 0.00001f)
    {
        float omega = acosf(cosom), sinom = sinf(omega);
        s0 = sinf((1.0f - t) * omega) / sinom;
        s1 = sinf(t * omega) / sinom;
    }
    else
    {
        s0 = 1.0f - t;
        s1 = t;
    }
    LTRotation r = { s0 * q1.x + s1 * b.x, s0 * q1.y + s1 * b.y, s0 * q1.z + s1 * b.z, s0 * q1.w + s1 * b.w };
    *pOut = r;
}

static LTRotation quat_Mul(const LTRotation& a, const LTRotation& b)
{
    LTRotation r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}

// ---- the transform maker (CTransformMakerK) -------------------------------------------

struct MRTransformMaker {
    TMHeader*        m_pTM;
    ModelView        m_Model;
    LTMatrix*        m_pOut;
    ModelWeightSet*  m_pWeightSets[8];
    ModelAnim*       m_pPrev[8];
    ModelAnim*       m_pCur[8];
    void*            m_pAnimOwner;
    LTRotation       m_rBlend;
    LTVector         m_vBlend;

    bool Prepare();
    void GetNodeTransform(uint32_t t, uint32_t iNode, LTRotation* pRot, LTVector* pPos, bool bRel);
    void ProcessNode(uint32_t iNode, const LTMatrix* pParent, int nDepth);
};

bool MRTransformMaker::Prepare()
{
    TMHeader* tm = m_pTM;
    if (tm->m_nTimeRefs == 0 || tm->m_nTimeRefs > 8)
        return false;
    void* pModel = tm->m_TimeRefs[0].m_pModel;
    if (!pModel)
        return false;
    m_Model.m_p = pModel;
    uint32_t nAnims = m_Model.nAnims();
    for (uint32_t i = 0; i < tm->m_nTimeRefs; i++)
    {
        const AnimTimeRef& r = tm->m_TimeRefs[i];
        if (r.m_pModel != pModel || r.m_PrevAnim >= nAnims || r.m_CurAnim >= nAnims)
            return false;
        if (i > 0)
        {
            if (r.m_iWeightSet >= m_Model.nWeightSets())
                return false;
            m_pWeightSets[i] = m_Model.WeightSets()[r.m_iWeightSet];
        }
        m_pPrev[i] = m_Model.Anims()[r.m_PrevAnim].m_pAnim;
        m_pCur[i] = m_Model.Anims()[r.m_CurAnim].m_pAnim;
        if (!m_pPrev[i] || !m_pCur[i])
            return false;
    }
    m_pAnimOwner = m_Model.Anims()[tm->m_TimeRefs[0].m_PrevAnim].m_pChildModel;
    return m_pAnimOwner != NULL;
}

// The keyframes either side, interpolated (GetNodeTransform), or relative
// to the animation's first keyframe (GetNodeTransformRel, additive tracks).
void MRTransformMaker::GetNodeTransform(uint32_t t, uint32_t iNode, LTRotation* pRot, LTVector* pPos, bool bRel)
{
    const AnimTimeRef& r = m_pTM->m_TimeRefs[t];
    CModelXform* pBase = m_pPrev[t]->m_pAnimNodes[iNode]->m_pXforms;
    const CModelXform& A = pBase[r.m_PrevFrame];
    const CModelXform& B = m_pCur[t]->m_pAnimNodes[iNode]->m_pXforms[r.m_CurFrame];
    float p = r.m_Percent;
    quat_Slerp(pRot, A.m_Rot, B.m_Rot, p);
    pPos->x = A.m_Pos.x + (B.m_Pos.x - A.m_Pos.x) * p;
    pPos->y = A.m_Pos.y + (B.m_Pos.y - A.m_Pos.y) * p;
    pPos->z = A.m_Pos.z + (B.m_Pos.z - A.m_Pos.z) * p;
    if (bRel)
    {
        LTRotation conj = { -pBase->m_Rot.x, -pBase->m_Rot.y, -pBase->m_Rot.z, pBase->m_Rot.w };
        *pRot = quat_Mul(conj, *pRot);
        pPos->x -= pBase->m_Pos.x;
        pPos->y -= pBase->m_Pos.y;
        pPos->z -= pBase->m_Pos.z;
    }
    else if (iNode == 0)
    {
        const LTVector& a = m_Model.Anims()[r.m_PrevAnim].m_vTranslation;
        const LTVector& b = m_Model.Anims()[r.m_CurAnim].m_vTranslation;
        pPos->x += a.x + (b.x - a.x) * p;
        pPos->y += a.y + (b.y - a.y) * p;
        pPos->z += a.z + (b.z - a.z) * p;
    }
}

void MRTransformMaker::ProcessNode(uint32_t iNode, const LTMatrix* pParent, int nDepth)
{
    if (nDepth > 64 || iNode >= m_Model.nNodes())
        return;
    GetNodeTransform(0, iNode, &m_rBlend, &m_vBlend, false);
    for (uint32_t t = 1; t < m_pTM->m_nTimeRefs; t++)
    {
        float w = m_pWeightSets[t]->m_pWeights[iNode];
        if (w == 0.0f)
            continue;
        LTRotation rot;
        LTVector pos;
        if (w == 2.0f)
        {
            // Additive: on top of what is there.
            GetNodeTransform(t, iNode, &rot, &pos, true);
            m_rBlend = quat_Mul(m_rBlend, rot);
            m_vBlend.x += pos.x; m_vBlend.y += pos.y; m_vBlend.z += pos.z;
        }
        else
        {
            GetNodeTransform(t, iNode, &rot, &pos, false);
            LTRotation old = m_rBlend;
            quat_Slerp(&m_rBlend, old, rot, w);
            m_vBlend.x += (pos.x - m_vBlend.x) * w;
            m_vBlend.y += (pos.y - m_vBlend.y) * w;
            m_vBlend.z += (pos.z - m_vBlend.z) * w;
        }
    }
    ModelNode* pNode = m_Model.Nodes()[iNode];
    LTMatrix local, tmp, rel;
    mat_FromQuat(&local, m_rBlend, (pNode->m_Flags & 2) ? pNode->m_vOffset : m_vBlend);
    mat_Mul(&tmp, *pParent, local);
    // The animation owner's per-node relation transform.
    CModelXform* pRel = &(*(CModelXform**)((uint8_t*)m_pAnimOwner + 4))[iNode];
    mat_FromQuat(&rel, pRel->m_Rot, pRel->m_Pos);
    LTMatrix* pOut = &m_pOut[iNode];
    mat_Mul(pOut, tmp, rel);
    if (m_pTM->m_NodeControlFn)
        m_pTM->m_NodeControlFn(m_pTM->m_hObject, iNode, pOut, m_pTM->m_pNodeControlData);
    for (uint32_t c = 0; c < pNode->m_nChildren; c++)
        if (pNode->m_pChildren[c])
            ProcessNode(pNode->m_pChildren[c]->m_iNode, pOut, nDepth + 1);
}

// ---- lighting (docs/objects.md II.5) -------------------------------------------

struct ModelLight {
    LTVector m_Pos;                     // model space
    LTVector m_Dir;                     // normalised m_Pos
    float    m_R2, m_InvR2;
    LTVector m_Color;                   // 0..255
};

struct ModelHookData {                  // ltbasedefs.h
    LTObject* m_hObject;
    uint32_t  m_Flags;
    uint32_t  m_ObjectFlags;
    LTVector* m_LightAdd;
    LTVector* m_ObjectColor;
};
typedef void (*ModelHookFn)(ModelHookData* pData, void* pUser);
#define MHF_USETEXTURE 1

struct ModelLighting {
    LTVector m_Ambient;                 // 0..255
    LTVector m_DirLight;                // towards the sun, model space
    float    m_fDirAmt;                 // how much of the model sees it, 0..1
    LTVector m_DirColor;                // 0..255
    LTVector m_Scale;                   // colour scale (0..1, x2 under Saturate)
    LTVector m_Add;                     // 0..255
    ModelLight m_Lights[16];
    int      m_nLights;
    bool     m_bNoLight;
    bool     m_bTexture;
    uint32_t m_Flags;                   // the object flags after the hook
};

// The world's static lights near a model (d3d.ren's 0x10019a70, a
// FindObjectsInBox callback): within reach of the model's sphere, and for
// a spot light inside its cone, coloured from the centre's colour to the
// edge's.
struct StaticLightCtx {
    ModelLighting*  m_pL;
    const LTMatrix* m_pW2M;
    LTVector        m_Pos;
    float           m_fVis;
    int             m_nMax;
};

static void mr_StaticLightCB(void* pObj, void* pUser)
{
    const StaticLight* S = (const StaticLight*)pObj;
    StaticLightCtx* c = (StaticLightCtx*)pUser;
    ModelLighting* pL = c->m_pL;
    if (pL->m_nLights >= c->m_nMax || S->m_fRadius <= 0.0f)
        return;
    float dx = c->m_Pos.x - S->m_Pos.x, dy = c->m_Pos.y - S->m_Pos.y, dz = c->m_Pos.z - S->m_Pos.z;
    float fDist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (fDist - c->m_fVis >= S->m_fRadius)
        return;
    LTVector col = S->m_Color;
    if (S->m_fCone > -1.0f && S->m_fCone < 1.0f && fDist != 0.0f)
    {
        float fDot = (S->m_Dir.x * dx + S->m_Dir.y * dy + S->m_Dir.z * dz) / fDist;
        if (fDot < S->m_fCone)
            return;
        float t = (1.0f - (0.5f * fDot + 0.5f)) / (1.0f - (0.5f * S->m_fCone + 0.5f));
        col.x = col.x * (1.0f - t) + S->m_OuterColor.x * t;
        col.y = col.y * (1.0f - t) + S->m_OuterColor.y * t;
        col.z = col.z * (1.0f - t) + S->m_OuterColor.z * t;
    }
    ModelLight& L = pL->m_Lights[pL->m_nLights++];
    L.m_Pos = mat_Apply(*c->m_pW2M, S->m_Pos);
    float len = sqrtf(L.m_Pos.x * L.m_Pos.x + L.m_Pos.y * L.m_Pos.y + L.m_Pos.z * L.m_Pos.z);
    float inv = len > 0.0f ? 1.0f / len : 0.0f;
    L.m_Dir.x = L.m_Pos.x * inv; L.m_Dir.y = L.m_Pos.y * inv; L.m_Dir.z = L.m_Pos.z * inv;
    L.m_R2 = S->m_fRadius * S->m_fRadius;
    L.m_InvR2 = 1.0f / L.m_R2;
    L.m_Color = col;
}

// d3d.ren's 0x1001bae4 (GetDirLightAmount): a ray twice the world's size
// towards the sun. With a variance, the model's two edges across the sun are
// tested, and where they disagree the boundary is bisected down to it.
static float mr_SunAmount(LTObject* pInst, const LTVector& vPos, const LTMatrix& world, float fVar)
{
    const LTVector& vDir = g.pStruct->m_GlobalLightDir;
    float k = mr_WorldSize() * -2.0f;
    LTVector vRay = { vDir.x * k, vDir.y * k, vDir.z * k };
    if (fVar == 0.0f)
        return mr_SeesSky(vPos, vRay) ? 1.0f : 0.0f;
    LTVector vRight = { world.m[0][0], world.m[1][0], world.m[2][0] };
    float r = pInst->m_Radius;
    // vDir.Cross(vRight), which in LithTech is vRight x vDir.
    LTVector vPerp = { (vRight.y * vDir.z - vRight.z * vDir.y) * r,
                       (vRight.z * vDir.x - vRight.x * vDir.z) * r,
                       (vRight.x * vDir.y - vRight.y * vDir.x) * r };
    LTVector vHi = { vPos.x + vPerp.x, vPos.y + vPerp.y, vPos.z + vPerp.z };
    LTVector vLo = { vPos.x - vPerp.x, vPos.y - vPerp.y, vPos.z - vPerp.z };
    bool bHi = mr_SeesSky(vHi, vRay), bLo = mr_SeesSky(vLo, vRay);
    if (bHi == bLo)
        return bHi ? 1.0f : 0.0f;
    float fStep = 1.0f, fHi = 1.0f, fLo = -1.0f, fResult = 1.0f;
    while (fStep > fVar && bLo != bHi)
    {
        float fMid = (fLo + fHi) * 0.5f;
        LTVector vTest = { vPos.x + vPerp.x * fMid, vPos.y + vPerp.y * fMid, vPos.z + vPerp.z * fMid };
        bool bHit = mr_SeesSky(vTest, vRay);
        fStep *= 0.5f;
        if (!bHit)
            fResult -= fStep;
        if (bHit == bHi)
        {
            fHi = fMid;
            bHi = bHit;
        }
        else
        {
            fLo = fMid;
            bLo = bHit;
        }
    }
    return fResult;
}

// world places the model; normWorld (world, or the NormalRef node's frame)
// is what the lights and the sun are brought into the vertices' space with.
static void mr_SetupModelLight(LTObject* pInst, const ModelView& model, const LTMatrix& world,
                                const LTMatrix& normWorld, ModelLighting* pL)
{
    SceneDesc* pScene = g_Frame.m_pScene;
    pL->m_Scale.x = pInst->m_ColorR;
    pL->m_Scale.y = pInst->m_ColorG;
    pL->m_Scale.z = pInst->m_ColorB;
    pL->m_Add = pScene->m_ModelAdd;
    pL->m_Flags = pInst->m_Flags;
    pL->m_bTexture = true;
    if (pScene->m_ModelHookFn)
    {
        ModelHookData d;
        d.m_hObject = pInst;
        d.m_Flags = MHF_USETEXTURE;
        d.m_ObjectFlags = pInst->m_Flags;
        d.m_LightAdd = &pL->m_Add;
        d.m_ObjectColor = &pL->m_Scale;
        ((ModelHookFn)pScene->m_ModelHookFn)(&d, pScene->m_ModelHookUser);
        pL->m_Flags = d.m_ObjectFlags;
        pL->m_bTexture = (d.m_Flags & MHF_USETEXTURE) != 0;
    }
    float k = 1.0f / 255.0f;
    if (cvi(CV_Saturate))
        k *= cvf(CV_ModelSaturation);
    pL->m_Scale.x *= k;
    pL->m_Scale.y *= k;
    pL->m_Scale.z *= k;
    pL->m_nLights = 0;
    pL->m_fDirAmt = 0.0f;
    pL->m_DirLight.x = pL->m_DirLight.y = pL->m_DirLight.z = 0.0f;
    pL->m_bNoLight = (pL->m_Flags & FLAG_NOLIGHT) != 0;
    if (pL->m_bNoLight)
    {
        pL->m_Ambient.x = pL->m_Ambient.y = pL->m_Ambient.z = 255.0f;
        return;
    }
    LTVector vPos = { world.m[0][3], world.m[1][3], world.m[2][3] };
    LTMatrix W2M;
    mr_InverseAffine(&W2M, normWorld);

    // The sun (RenderStruct m_GlobalLightDir, m_GlobalLightColor): how much
    // of the model sees the sky towards it, kept on the instance (+0x238
    // where it was worked out, +0x244 the amount, -1 until then) and worked
    // out again only for a FLAG2_DYNAMICDIRLIGHT model that has moved.
    const LTVector& vSunDir = g.pStruct->m_GlobalLightDir;
    pL->m_DirColor = g.pStruct->m_GlobalLightColor;
    if (g_Frame.m_pContext && cvi(CV_ModelApplySun) &&
        (pL->m_DirColor.x != 0.0f || pL->m_DirColor.y != 0.0f || pL->m_DirColor.z != 0.0f))
    {
        LTVector* pCachePos = (LTVector*)((uint8_t*)pInst + 0x238);
        float* pCacheAmt = (float*)((uint8_t*)pInst + 0x244);
        float fVar = cvf(CV_ModelSunVariance), fEps = 0.001f + fVar;
        if (*pCacheAmt < 0.0f ||
            ((pInst->m_Flags2 & FLAG2_DYNAMICDIRLIGHT) &&
             (fabsf(vPos.x - pCachePos->x) > fEps || fabsf(vPos.y - pCachePos->y) > fEps ||
              fabsf(vPos.z - pCachePos->z) > fEps)))
        {
            *pCachePos = vPos;
            *pCacheAmt = mr_SunAmount(pInst, vPos, world, fVar);
        }
        pL->m_fDirAmt = *pCacheAmt;
        pL->m_DirLight.x = -(W2M.m[0][0] * vSunDir.x + W2M.m[0][1] * vSunDir.y + W2M.m[0][2] * vSunDir.z);
        pL->m_DirLight.y = -(W2M.m[1][0] * vSunDir.x + W2M.m[1][1] * vSunDir.y + W2M.m[1][2] * vSunDir.z);
        pL->m_DirLight.z = -(W2M.m[2][0] * vSunDir.x + W2M.m[2][1] * vSunDir.y + W2M.m[2][2] * vSunDir.z);
    }
    float amb[3];
    mr_LightGridSample(vPos, amb);
    pL->m_Ambient.x = amb[0];
    pL->m_Ambient.y = amb[1];
    pL->m_Ambient.z = amb[2];

    // The frame's visible dynamic lights within reach, into model space.
    MRObjList* pLights = mr_ObjList(VS_LIGHTS);
    int nMax = cvi(CV_MaxModelLights);
    if (nMax > 16) nMax = 16;
    float fVis = model.VisRadius();
    for (uint32_t i = 0; i < pLights->m_nObjects && pL->m_nLights < nMax; i++)
    {
        LTObject* pLight = pLights->m_pObjects[i];
        if ((pLight->m_Flags & 0x20 /* FLAG_ONLYLIGHTWORLD */) || !cvi(CV_DynamicLight))
            continue;
        float r = rd<float>(pLight, 0x1a8);
        LTVector d = { pLight->m_Pos.x - vPos.x, pLight->m_Pos.y - vPos.y, pLight->m_Pos.z - vPos.z };
        if (sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) - fVis >= r || r <= 0.0f)
            continue;
        ModelLight& L = pL->m_Lights[pL->m_nLights++];
        L.m_Pos = mat_Apply(W2M, pLight->m_Pos);
        float len = sqrtf(L.m_Pos.x * L.m_Pos.x + L.m_Pos.y * L.m_Pos.y + L.m_Pos.z * L.m_Pos.z);
        float inv = len > 0.0f ? 1.0f / len : 0.0f;
        L.m_Dir.x = L.m_Pos.x * inv; L.m_Dir.y = L.m_Pos.y * inv; L.m_Dir.z = L.m_Pos.z * inv;
        L.m_R2 = r * r;
        L.m_InvR2 = 1.0f / L.m_R2;
        L.m_Color.x = pLight->m_ColorR;
        L.m_Color.y = pLight->m_ColorG;
        L.m_Color.z = pLight->m_ColorB;
    }

    // Then the world's static lights, from the world tree.
    MainWorld* pWorld = g_Frame.m_pContext ? g_Frame.m_pContext->m_pWorld : NULL;
    if (pL->m_nLights < nMax && pWorld)
    {
        StaticLightCtx ctx = { pL, &W2M, vPos, fVis, nMax };
        FindObjInfo info;
        memset(&info, 0, sizeof(info));
        info.m_iObjArray = 1;
        info.m_Min.x = vPos.x - fVis; info.m_Min.y = vPos.y - fVis; info.m_Min.z = vPos.z - fVis;
        info.m_Max.x = vPos.x + fVis; info.m_Max.y = vPos.y + fVis; info.m_Max.z = vPos.z + fVis;
        info.m_CB = mr_StaticLightCB;
        info.m_pUser = &ctx;
        mw_FindObjectsInBox(pWorld, &info);
    }
}

// SetupSkeletalVerts' vertex colour: bind-pose position and normal against
// the model-space lights.
static inline void mr_LightVertex(const ModelLighting& L, const ModelVert& v, uint8_t nAlpha, uint8_t* pOut)
{
    LTVector c = L.m_Ambient;
    float d = (v.m_Normal.x * L.m_DirLight.x + v.m_Normal.y * L.m_DirLight.y +
               v.m_Normal.z * L.m_DirLight.z) * L.m_fDirAmt;
    if (d > 0.0f)
    {
        c.x += d * (L.m_DirColor.x - c.x);
        c.y += d * (L.m_DirColor.y - c.y);
        c.z += d * (L.m_DirColor.z - c.z);
    }
    for (int i = 0; i < L.m_nLights; i++)
    {
        const ModelLight& l = L.m_Lights[i];
        float ndl = v.m_Normal.x * l.m_Dir.x + v.m_Normal.y * l.m_Dir.y + v.m_Normal.z * l.m_Dir.z;
        if (ndl <= 0.0f)
            continue;
        float dx = v.m_Vec.x - l.m_Pos.x, dy = v.m_Vec.y - l.m_Pos.y, dz = v.m_Vec.z - l.m_Pos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= l.m_R2)
            continue;
        float k = ndl * (1.0f - d2 * l.m_InvR2);
        c.x += l.m_Color.x * k;
        c.y += l.m_Color.y * k;
        c.z += l.m_Color.z * k;
    }
    float r = c.x * L.m_Scale.x + L.m_Add.x, gg = c.y * L.m_Scale.y + L.m_Add.y, b = c.z * L.m_Scale.z + L.m_Add.z;
    pOut[0] = (uint8_t)(r > 255.0f ? 255 : r < 0.0f ? 0 : r);
    pOut[1] = (uint8_t)(gg > 255.0f ? 255 : gg < 0.0f ? 0 : gg);
    pOut[2] = (uint8_t)(b > 255.0f ? 255 : b < 0.0f ? 0 : b);
    pOut[3] = nAlpha;
}

// ---- drawing --------------------------------------------------------------------

static PieceLOD* mr_GetLOD(ModelPiece* p, uint32_t i)
{
    if (i == 0)
        return p;
    return i - 1 < p->m_nLODs ? &p->m_pLODs[i - 1] : NULL;
}

// SelectLOD: the last level whose distance the model is beyond.
static uint32_t mr_SelectLOD(const ModelView& model, float d)
{
    uint32_t n = model.nExtraLODs();
    const float* pDists = model.LODDists();
    int nOff = cvi(CV_ModelLODOffset);
    if (nOff && pDists)
    {
        uint32_t o = (uint32_t)(nOff > 0 ? nOff : -nOff);
        if (o > n) o = n;
        float f = o == 0 ? model.LODDist0() : pDists[o - 1];
        d += nOff > 0 ? f : -f;
    }
    uint32_t lod = 0;
    for (uint32_t i = 1; i <= n && pDists; i++)
        if (d > pDists[i - 1])
            lod = i;
    return lod;
}

static float* g_pSkinned;               // xyz per vertex of one piece
static uint8_t* g_pColors;              // rgba per vertex
static uint32_t g_nSkinAlloc;

static bool mr_ReserveVerts(uint32_t n)
{
    if (n <= g_nSkinAlloc)
        return true;
    float* p = (float*)realloc(g_pSkinned, n * 3 * sizeof(float));
    if (!p)
        return false;
    g_pSkinned = p;
    uint8_t* c = (uint8_t*)realloc(g_pColors, n * 4);
    if (!c)
        return false;
    g_pColors = c;
    g_nSkinAlloc = n;
    return true;
}

static LTMatrix* g_pNodeXforms;
static uint32_t g_nNodeAlloc;

static void mr_DrawModelWith(LTObject* pInst, bool bReallyClose)
{
    ModelView model = { rd<void*>(pInst, 0x1dc) };
    if (!model.m_p || !cvi(CV_DrawModels))
        return;
    uint32_t nAnims = model.nAnims();
    if (rd<uint16_t>(pInst, 0x1e0) >= nAnims || rd<uint16_t>(pInst, 0x1e8) >= nAnims)
        return;
    if (bReallyClose && !cvi(CV_DrawGuns))
        return;

    // The object's matrix: T * R * S (a really-close model is placed relative
    // to the camera).
    LTRotation q = pInst->m_Rotation;
    if (fabsf(q.x) > 100000.0f || fabsf(q.y) > 100000.0f || fabsf(q.z) > 100000.0f || fabsf(q.w) > 100000.0f)
    {
        q.x = q.y = q.z = 0.0f;
        q.w = 1.0f;
    }
    LTMatrix world;
    mat_FromQuat(&world, q, pInst->m_Pos);
    for (int r = 0; r < 3; r++)
    {
        world.m[r][0] *= pInst->m_Scale.x;
        world.m[r][1] *= pInst->m_Scale.y;
        world.m[r][2] *= pInst->m_Scale.z;
    }
    const MRCamera& cam = g_Frame.m_Cam;
    // A really-close model (the view model) lives in camera space: its node
    // matrices are made there (d3d.ren's base is the camera-space object
    // matrix, so the game's node-control callback sees them so) and it is
    // drawn with the camera-space view. Only its lighting needs it in the
    // world.
    LTMatrix lightWorld = world;
    LTMatrix camToWorld;
    if (bReallyClose)
    {
        for (int r = 0; r < 3; r++)
        {
            camToWorld.m[r][0] = cam.m_R[r][0];
            camToWorld.m[r][1] = cam.m_R[r][1];
            camToWorld.m[r][2] = cam.m_R[r][2];
        }
        camToWorld.m[0][3] = cam.m_Pos.x;
        camToWorld.m[1][3] = cam.m_Pos.y;
        camToWorld.m[2][3] = cam.m_Pos.z;
        camToWorld.m[3][0] = camToWorld.m[3][1] = camToWorld.m[3][2] = 0.0f;
        camToWorld.m[3][3] = 1.0f;
        mat_Mul(&lightWorld, camToWorld, world);
    }
    LTVector vPos = { lightWorld.m[0][3], lightWorld.m[1][3], lightWorld.m[2][3] };
    float fScale = fabsf(pInst->m_Scale.x);
    if (fabsf(pInst->m_Scale.y) > fScale) fScale = fabsf(pInst->m_Scale.y);
    if (fabsf(pInst->m_Scale.z) > fScale) fScale = fabsf(pInst->m_Scale.z);
    if (!bReallyClose && !mr_SphereVisible(vPos, model.Radius() * fScale))
        return;

    // Node matrices in world space: the engine fills the time refs, our
    // transform maker does the rest.
    uint32_t nNodes = model.nNodes();
    if (!nNodes)
        return;
    if (nNodes > g_nNodeAlloc)
    {
        LTMatrix* p = (LTMatrix*)realloc(g_pNodeXforms, nNodes * sizeof(LTMatrix));
        if (!p)
            return;
        g_pNodeXforms = p;
        g_nNodeAlloc = nNodes;
    }
    TMHeader tm;
    memset(&tm, 0, sizeof(tm));
    ((*(SetupTMFn**)pInst)[0x38 / 4])(pInst, &tm);
    tm.m_hObject = pInst;
    MRTransformMaker maker;
    memset(&maker, 0, sizeof(maker));
    maker.m_pTM = &tm;
    maker.m_pOut = g_pNodeXforms;
    if (!maker.Prepare() || maker.m_Model.m_p != model.m_p || !model.RootNode())
        return;
    maker.ProcessNode(model.RootNode()->m_iNode, &world, 0);

    // Lighting, LOD. A model with a NormalRef node is lit (and chromed) in
    // that node's frame, so its light follows the node as it animates.
    LTMatrix normWorld = lightWorld;
    if (model.bNormalRef() && model.NormalRefNode() < nNodes)
    {
        LTMatrix nodeWorld = g_pNodeXforms[model.NormalRefNode()];
        if (bReallyClose)
            mat_Mul(&nodeWorld, camToWorld, nodeWorld);
        mat_Mul(&normWorld, nodeWorld, model.NormalRefMat());
    }
    ModelLighting L;
    mr_SetupModelLight(pInst, model, lightWorld, normWorld, &L);
    float dx = vPos.x - cam.m_Pos.x, dy = vPos.y - cam.m_Pos.y, dz = vPos.z - cam.m_Pos.z;
    uint32_t lod = mr_SelectLOD(model, sqrtf(dx * dx + dy * dy + dz * dz));
    uint8_t nAlpha = pInst->m_ColorA;
    uint32_t nHidden = rd<uint32_t>(pInst, 0x204);

    // Chrome (DoModelDrawing, docs/objects.md 5.4): the world's environment
    // map in a first pass, its coordinates from the bind-pose normals turned
    // by the view (and the NormalRef node), panned with the camera; the skin
    // then blends over it by its alpha. EnvMapAll draws the chrome alone.
    MRTexture* pEnv = NULL;
    bool bEnvAll = cvi(CV_EnvMapAll) != 0;
    float envRow[2][3], envScale = 0.0f, envPanU = 0.0f, envPanV = 0.0f;
    if ((bEnvAll || (L.m_Flags & FLAG_ENVIRONMENTMAP)) && cvi(CV_EnvMapEnable) && g.pStruct->m_pEnvMap)
    {
        mr_TextureId(g.pStruct->m_pEnvMap);
        pEnv = (MRTexture*)g.pStruct->m_pEnvMap->m_pRenderData;
    }
    if (pEnv)
    {
        // The view's right and up rows (world -> camera), times the normals'
        // frame when the model has a NormalRef node.
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
            {
                if (model.bNormalRef())
                    envRow[r][c] = cam.m_R[0][r] * normWorld.m[0][c] + cam.m_R[1][r] * normWorld.m[1][c] +
                                   cam.m_R[2][r] * normWorld.m[2][c];
                else
                    envRow[r][c] = cam.m_R[c][r];
            }
        float fEnvScale = cvf(CV_EnvScale);
        envScale = fEnvScale != 0.0f ? 0.5f / fEnvScale : 0.5f;
        envPanU = g_Frame.m_Cam.m_Pos.x * cvf(CV_EnvPanSpeed) + 0.5f;
        envPanV = g_Frame.m_Cam.m_Pos.z * cvf(CV_EnvPanSpeed) + 0.5f;
        rd_PushAttrib(RD_ATTR_COLOR);
    }

    mr_ObjectFog(pInst, true);
    if (nAlpha < 255)
        rd_Enable(RD_BLEND);
    if (bReallyClose)
    {
        rd_MatrixMode(RD_MODELVIEW);
        rd_PushMatrix();
        rd_LoadIdentity();
    }
    mr_ForgetBindings();
    // d3d.ren culls a model's back faces in software (TriFacing): without
    // it the inside of a modelled-thick glass bottle drew over its label.
    rd_CullBack(true);
    for (uint32_t i = 0; i < model.nPieces(); i++)
    {
        if (i < 32 && (nHidden & (1u << i)))
            continue;
        ModelPiece* pPiece = model.Pieces()[i];
        PieceLOD* pLOD = pPiece ? mr_GetLOD(pPiece, lod) : NULL;
        if (!pLOD || !pLOD->m_nTris || !mr_ReserveVerts(pLOD->m_nVerts))
            continue;

        // Skin: P = sum of node matrix x (weighted position, weight).
        for (uint32_t v = 0; v < pLOD->m_nVerts; v++)
        {
            const ModelVert& mv = pLOD->m_Verts[v];
            float px = 0, py = 0, pz = 0, pw = 0;
            for (uint32_t w = 0; w < mv.m_nWeights; w++)
            {
                const NewVertexWeight& nw = mv.m_Weights[w];
                if (nw.m_iNode >= nNodes)
                    continue;
                const LTMatrix& M = g_pNodeXforms[nw.m_iNode];
                px += M.m[0][0] * nw.m_Vec[0] + M.m[0][1] * nw.m_Vec[1] + M.m[0][2] * nw.m_Vec[2] + M.m[0][3] * nw.m_Vec[3];
                py += M.m[1][0] * nw.m_Vec[0] + M.m[1][1] * nw.m_Vec[1] + M.m[1][2] * nw.m_Vec[2] + M.m[1][3] * nw.m_Vec[3];
                pz += M.m[2][0] * nw.m_Vec[0] + M.m[2][1] * nw.m_Vec[1] + M.m[2][2] * nw.m_Vec[2] + M.m[2][3] * nw.m_Vec[3];
                pw += M.m[3][0] * nw.m_Vec[0] + M.m[3][1] * nw.m_Vec[1] + M.m[3][2] * nw.m_Vec[2] + M.m[3][3] * nw.m_Vec[3];
            }
            float inv = pw != 0.0f ? 1.0f / pw : 1.0f;
            g_pSkinned[v * 3 + 0] = px * inv;
            g_pSkinned[v * 3 + 1] = py * inv;
            g_pSkinned[v * 3 + 2] = pz * inv;
            mr_LightVertex(L, mv, nAlpha, &g_pColors[v * 4]);
        }

        // The piece's skin: ModelInstance::m_pSkins[piece.m_iTexture].
        MRTexture* pTex = NULL;
        if (pPiece->m_iTexture < 2 && L.m_bTexture && cvi(CV_TextureModels))
        {
            SharedTexture* pSkin = rd<SharedTexture*>(pInst, 0x1b8 + 4 * pPiece->m_iTexture);
            if (pSkin)
            {
                mr_TextureId(pSkin);
                pTex = (MRTexture*)pSkin->m_pRenderData;
            }
        }
        if (pEnv)
        {
            // The chrome pass is opaque: the skin pass of the piece before
            // left blending on, which drew every later piece's chrome by
            // the environment map's alpha and showed the scene through
            // (wine bottles, glasses).
            if (nAlpha == 255)
                rd_Disable(RD_BLEND);
            mr_SetBaseTexture(pEnv);
            rd_Begin(RD_TRIANGLES);
            for (uint32_t t = 0; t < pLOD->m_nTris; t++)
            {
                const ModelTri& tri = pLOD->m_Tris[t];
                for (int k = 0; k < 3; k++)
                {
                    uint32_t idx = tri.m_Indices[k];
                    if (idx >= pLOD->m_nVerts)
                        idx = 0;
                    const LTVector& N = pLOD->m_Verts[idx].m_Normal;
                    rd_Color4ubv(&g_pColors[idx * 4]);
                    rd_TexCoord2f((envRow[0][0] * N.x + envRow[0][1] * N.y + envRow[0][2] * N.z) * envScale + envPanU,
                                 (envRow[1][0] * N.x + envRow[1][1] * N.y + envRow[1][2] * N.z) * envScale + envPanV);
                    rd_Vertex3fv(&g_pSkinned[idx * 3]);
                }
            }
            rd_End();
            if (bEnvAll)
                continue;
            rd_Enable(RD_BLEND);
        }
        mr_SetBaseTexture(pTex);
        rd_Begin(RD_TRIANGLES);
        for (uint32_t t = 0; t < pLOD->m_nTris; t++)
        {
            const ModelTri& tri = pLOD->m_Tris[t];
            for (int k = 0; k < 3; k++)
            {
                uint32_t idx = tri.m_Indices[k];
                if (idx >= pLOD->m_nVerts)
                    idx = 0;
                rd_Color4ubv(&g_pColors[idx * 4]);
                rd_TexCoord2f(tri.m_UV[k][0], tri.m_UV[k][1]);
                rd_Vertex3fv(&g_pSkinned[idx * 3]);
            }
        }
        rd_End();
    }
    rd_CullBack(false);
    if (bReallyClose)
        rd_PopMatrix();
    if (pEnv)
        rd_PopAttrib();
    // The shadow, after the model (DoModelDrawing, on the hook's flags).
    if (!bReallyClose && (L.m_Flags & FLAG_SHADOW))
        mr_DrawModelShadow(pInst, model.m_p);
    mr_ObjectFog(pInst, false);
}

void mr_DrawModel(LTObject* pInst)
{
    bool bClose = (pInst->m_Flags & FLAG_REALLYCLOSE) != 0;
    if (bClose)
        mr_BeginReallyClose(pInst);
    mr_DrawModelWith(pInst, bClose);
    if (bClose)
        mr_EndReallyClose();
}

// A translucent model: its own blend, no depth writes (r_DrawTransparentModel).
void mr_DrawTranslucentModel(LTObject* pInst)
{
    rd_PushAttrib(RD_ATTR_COLOR | RD_ATTR_FOG);
    rd_Enable(RD_BLEND);
    mr_ObjectBlend(pInst);
    mr_DrawModel(pInst);
    rd_PopAttrib();
}

// Really-close objects (the player's weapon, the menus' 3D parts) are in
// camera space, clipped at 0.01 rather than the scene's near plane and,
// for a model that asks (FovXOffset / FovYOffset), at their own scale --
// corrected for a widened screen like the scene's (MODERN.md M-P0-01) so
// they look as at 4:3. They come after everything else, in the scene's
// depth buffer (mr_FlushObjects).
void mr_BeginReallyClose(LTObject* pObj)
{
    const MRCamera& cam = g_Frame.m_Cam;
    MRCamera close = cam;
    // d3d.ren clips these at 0.01 but projects them with the scene's depth
    // mapping into the one depth buffer: the scene's near plane, with depth
    // clamping so what is nearer still draws (in front of everything). The
    // main menu needs the scene's mapping: its flat 3D parts lie behind
    // Cate Archer, a scene model, and she hides them. d3d.ren's depth for
    // what is nearer than the near plane is below 0, still in order;
    // clamped, all of it has the one depth, and the weapon's back faces
    // drawn last show through its front (an inside-out gun). So where the
    // device can, that part goes into the front of the depth buffer, which
    // the scene leaves free (mr_LoadCamera), in order.
    if (g_RDCaps.bDepthClamp)
    {
        rd_Enable(RD_DEPTH_CLAMP);
        if (g_RDCaps.bNearDepth)
            rd_NearDepth(0.01f, close.m_fNear, 0.0f, MR_CLOSE_DEPTH);
    }
    else
        close.m_fNear = 0.01f;
    ModelView model = { pObj->m_ObjectType == OT_MODEL ? rd<void*>(pObj, 0x1dc) : NULL };
    if (model.m_p && model.bFovOffset())
    {
        SceneDesc* pScene = g_Frame.m_pScene;
        float fx = pScene->m_xFov + model.FovXOffset(), fy = pScene->m_yFov + model.FovYOffset();
        const float fMin = 10.0f * 3.14159265f / 180.0f, fMax = 170.0f * 3.14159265f / 180.0f;
        fx = fx < fMin ? fMin : fx > fMax ? fMax : fx;
        fy = fy < fMin ? fMin : fy > fMax ? fMax : fy;
        // d3d.ren (0x10017ff7) scales the model's camera space by
        // tan(f/2) under an unscaled projection, so a wider "field of
        // view" draws the model larger: the player's weapons are drawn
        // bigger than the scene's own projection would.
        float aspect = (float)cam.m_nViewW / (float)cam.m_nViewH;
        close.m_fTanX = aspect / (4.0f / 3.0f) / tanf(fx * 0.5f);
        close.m_fTanY = 1.0f / tanf(fy * 0.5f);
    }
    mr_LoadCamera(&close);
}

void mr_EndReallyClose(void)
{
    if (g_RDCaps.bDepthClamp)
        rd_Disable(RD_DEPTH_CLAMP);
    if (g_RDCaps.bNearDepth)
        rd_NearDepth(0.0f, 0.0f, 0.0f, 0.0f);
    mr_LoadCamera(&g_Frame.m_Cam);
}
