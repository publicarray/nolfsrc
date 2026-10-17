// rdev.h -- the device under the renderer core: what a backend provides.
//
// The core (everything in src/modern/render/) draws with a small
// fixed-function pipeline in the shape of OpenGL 1.x: a state machine with
// enables, blending, alpha test, depth, linear fog, up to four texture
// units with modulate/replace/combine environments, three matrix stacks,
// and immediate-mode primitives. gl/ implements it with OpenGL (opengl.ren),
// d3d9/ with Direct3D 9 (d3d9.ren). Anything one backend does that the
// other cannot is a capability in RDCaps, which the core checks.
//
// The conventions are OpenGL's: column vectors with matrices stored
// column-major, clip space z from -w to w, and texture coordinates with
// (0,0) at the first texel of the first row. Rectangles (viewport,
// scissor, read-back) are in pixels from the top left of the screen, and
// read-back rows come top-down.

#ifndef RDEV_H
#define RDEV_H

#include <stdint.h>

// ---- the device --------------------------------------------------------------

// Who the backend is: the DLL's file name ("opengl.ren"), its log file
// ("opengl.log") and the card name the mode list shows ("OpenGL"; no
// spaces).
extern const char g_RDDll[], g_RDLog[], g_RDCard[];

struct RDCaps {
    int      nMaxTextureSize;
    int      nTextureUnits;         // 1 when multitexture is missing
    bool     bCombine;              // rd_TexCombine works
    bool     bS3TC;                 // DXT1/3/5 textures
    bool     bAniso;
    float    fMaxAniso;
    bool     bDepthClamp;           // RD_DEPTH_CLAMP works
    bool     bNearDepth;            // and rd_NearDepth with it
    int      nSamples;              // the multisampling the device got (1: none)
};
extern RDCaps g_RDCaps;

// The device on the window plat_OpenWindow sized, in the state every draw
// starts from: depth test on (LEQUAL), everything else off, the viewport
// the whole screen, the screen cleared to black. Fills g_RDCaps. With
// nSamples above 1 it draws multisampled (antialiased) when it can,
// resolving to the screen when it presents or reads pixels.
bool rd_CreateDevice(void* hWnd, uint32_t nWidth, uint32_t nHeight, bool bWindowed, int nSwapInterval,
                     int nSamples);
void rd_DestroyDevice(void);        // waits for drawing to finish first
void rd_Present(void);
// What the device is ("AMD Radeon ... (4.6 Mesa 24.0)"), for the console,
// and its details for the "info" render command.
const char* rd_Describe(void);
void rd_PrintInfo(void (*pPrint)(const char* pFormat, ...));

// ---- state -------------------------------------------------------------------

enum RDCap {
    RD_BLEND, RD_ALPHA_TEST, RD_DEPTH_TEST, RD_FOG, RD_SCISSOR_TEST,
    RD_TEXTURE_2D,                  // on the active unit
    RD_DEPTH_CLAMP,                 // only when g_RDCaps.bDepthClamp
    RD_CAP_COUNT
};
void rd_Enable(RDCap eCap);
void rd_Disable(RDCap eCap);
// Back faces culled (screen-clockwise is the front, as d3d.ren's model
// renderer tests it) or not; off by default.
void rd_CullBack(bool bOn);

enum RDBlend {
    RD_ZERO, RD_ONE, RD_SRC_COLOR, RD_ONE_MINUS_SRC_COLOR, RD_DST_COLOR, RD_ONE_MINUS_DST_COLOR,
    RD_SRC_ALPHA, RD_ONE_MINUS_SRC_ALPHA, RD_DST_ALPHA, RD_ONE_MINUS_DST_ALPHA, RD_SRC_ALPHA_SATURATE
};
void rd_BlendFunc(RDBlend eSrc, RDBlend eDst);
// The alpha test passes fragments whose alpha is greater than fRef.
void rd_AlphaGreater(float fRef);

enum RDDepthFunc { RD_LESS, RD_LEQUAL, RD_ALWAYS };
void rd_DepthFunc(RDDepthFunc eFunc);
void rd_DepthMask(bool bWrite);
void rd_ColorMask(bool bWrite);

// Linear fog on eye-space depth.
void rd_FogColor(float r, float g, float b);
void rd_FogRange(float fStart, float fEnd);

void rd_Viewport(int x, int y, int w, int h);
// The part of the depth buffer the projection's near..far map to
// (glDepthRange; 0..1 to start with). Kept with the viewport.
void rd_DepthRange(float fNear, float fFar);
// With g_RDCaps.bNearDepth, while RD_DEPTH_CLAMP is on: what lies between
// eye depth fClip and the projection's near plane fNear is not clamped to
// the near plane's depth but drawn into window depth fDepth0..fDepth1 (by
// 1/z, as a projection with its near plane at fClip would), and what is
// nearer than fClip is clipped. fClip 0 turns it off.
void rd_NearDepth(float fClip, float fNear, float fDepth0, float fDepth1);
void rd_Scissor(int x, int y, int w, int h);

// What the push saves and the pop restores, as glPushAttrib's groups.
enum {
    RD_ATTR_COLOR    = 1,           // blend, blend func, alpha test, alpha ref, colour mask, clear colour
    RD_ATTR_DEPTH    = 2,           // depth test, depth func, depth mask
    RD_ATTR_FOG      = 4,           // fog enable, colour, range
    RD_ATTR_ENABLE   = 8,           // every RDCap, on every unit
    RD_ATTR_TEXTURE  = 16,          // the active unit, and each unit's texture, enable and environment
    RD_ATTR_SCISSOR  = 32,          // scissor enable and rect
    RD_ATTR_VIEWPORT = 64,
    RD_ATTR_ALL      = 127
};
void rd_PushAttrib(uint32_t nMask);
void rd_PopAttrib(void);

// ---- texture units -------------------------------------------------------------

void rd_ActiveTexture(int nUnit);

// The active unit's environment. MODULATE is texture x incoming colour;
// REPLACE is the texture's colour and alpha.
enum RDTexEnv { RD_ENV_MODULATE, RD_ENV_REPLACE };
void rd_TexEnv(RDTexEnv eEnv);

// A combine environment: each of colour and alpha is Op(Arg0, Arg1),
// colour times fScale (1, 2 or 4).
enum RDCombineOp { RD_OP_REPLACE, RD_OP_MODULATE, RD_OP_ADD, RD_OP_ADD_SIGNED };
enum RDCombineArg { RD_ARG_TEXTURE, RD_ARG_PRIMARY, RD_ARG_PREVIOUS };
struct RDCombine {
    RDCombineOp  eColorOp;
    RDCombineArg eColor0, eColor1;
    float        fScale;
    RDCombineOp  eAlphaOp;
    RDCombineArg eAlpha0, eAlpha1;
};
void rd_TexCombine(const RDCombine& c);

// ---- textures ------------------------------------------------------------------
//
// Texture names are the device's, never 0. Parameters and images go to the
// texture bound on the active unit.

uint32_t rd_GenTexture(void);
void     rd_DeleteTexture(uint32_t nTex);
void     rd_BindTexture(uint32_t nTex);         // 0 unbinds

enum RDFormat {
    RD_FMT_RGB,                     // the device's choice of RGB (the 16-bit screen copy)
    RD_FMT_RGB8, RD_FMT_RGBA8,
    RD_FMT_DXT1, RD_FMT_DXT1A, RD_FMT_DXT3, RD_FMT_DXT5
};
enum RDPixels {
    RD_PIX_BGRA,                    // 32-bit 0xAARRGGBB words
    RD_PIX_RGBA,                    // bytes R, G, B, A
    RD_PIX_RGB,                     // bytes R, G, B
    RD_PIX_565                      // 16-bit 5:6:5 words
};
// One level; pData may be NULL to size it. nRowPixels is the source's row
// length when it is not w.
void rd_TexImage(int nLevel, RDFormat eFormat, int w, int h, RDPixels ePixels, const void* pData,
                 int nRowPixels = 0);
void rd_TexSubImage(int nLevel, int x, int y, int w, int h, RDPixels ePixels, const void* pData);
void rd_CompressedTexImage(int nLevel, RDFormat eFormat, int w, int h, uint32_t nBytes, const void* pData);

enum RDFilter {
    RD_NEAREST, RD_LINEAR,
    RD_NEAREST_MIPMAP_NEAREST, RD_LINEAR_MIPMAP_NEAREST,
    RD_NEAREST_MIPMAP_LINEAR, RD_LINEAR_MIPMAP_LINEAR
};
void rd_TexFilter(RDFilter eMin, RDFilter eMag);
void rd_TexWrap(bool bRepeatS, bool bRepeatT);
void rd_TexMaxLevel(int nLevel);
void rd_TexAniso(float fAniso);

// ---- matrices --------------------------------------------------------------------

enum RDMatrixMode { RD_MODELVIEW, RD_PROJECTION, RD_TEXTURE };   // RD_TEXTURE: the active unit's
void rd_MatrixMode(RDMatrixMode eMode);
void rd_LoadIdentity(void);
void rd_LoadMatrix(const float* m);
void rd_MultMatrix(const float* m);
void rd_PushMatrix(void);
void rd_PopMatrix(void);
void rd_Ortho(double l, double r, double b, double t, double n, double f);

// ---- drawing -----------------------------------------------------------------------

enum RDPrim { RD_TRIANGLES, RD_TRIANGLE_FAN, RD_QUADS, RD_LINES };
void rd_Begin(RDPrim ePrim);
void rd_End(void);
void rd_Color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void rd_Color4f(float r, float g, float b, float a);
void rd_TexCoord2f(float s, float t);                       // unit 0
void rd_TexCoord4f(float s, float t, float r, float q);     // unit 0
void rd_MultiTexCoord2f(int nUnit, float s, float t);
void rd_Vertex2f(float x, float y);
void rd_Vertex3f(float x, float y, float z);
inline void rd_Color4ubv(const uint8_t* c) { rd_Color4ub(c[0], c[1], c[2], c[3]); }
inline void rd_Vertex3fv(const float* v) { rd_Vertex3f(v[0], v[1], v[2]); }

// ---- the frame buffer -----------------------------------------------------------------

enum { RD_CLEAR_COLOR = 1, RD_CLEAR_DEPTH = 2 };    // depth also clears stencil
void rd_ClearColor(float r, float g, float b, float a);
void rd_Clear(uint32_t nWhat);                      // within the scissor rect when it is on
// Pixels from the back buffer (or the front, for screenshots), top-down
// and tightly packed: RD_PIX_BGRA or RD_PIX_565.
void rd_ReadPixels(int x, int y, int w, int h, RDPixels ePixels, void* pDest, bool bFront = false);

#endif
