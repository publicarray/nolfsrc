// gl_device.cpp -- rdev.h on OpenGL: opengl.ren's backend.
//
// The device is OpenGL 1.x fixed function with a few extensions
// (ARB_multitexture, ARB_texture_env_combine, EXT_texture_compression_s3tc,
// EXT_texture_filter_anisotropic, ARB/NV_depth_clamp, display lists for
// rd_NearDepth), which is what every
// OpenGL a Wine prefix can reach provides; rdev.h is shaped after it, so
// most calls here are one GL call. The context is WGL's, on the window
// mr_window.cpp sized. Antialiasing draws into a multisampled framebuffer
// object (the window's own pixel format is fixed for its life, and the
// engine keeps one window across renderer restarts), resolved into the
// window's back buffer before a swap or a read.

#include "../mr.h"
#include <GL/gl.h>

const char g_RDDll[] = "opengl.ren";
const char g_RDLog[] = "opengl.log";
const char g_RDCard[] = "OpenGL";
RDCaps g_RDCaps;

// GL 1.2+ and extension tokens the mingw-w64 GL 1.1 header lacks.
#ifndef GL_BGRA
#define GL_BGRA                         0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                0x812F
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5         0x8363
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL            0x813D
#endif
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT  0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_TEXTURE_MAX_ANISOTROPY_EXT   0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_DEPTH_CLAMP                  0x864F
#define GL_TEXTURE0_ARB                 0x84C0
#define GL_MAX_TEXTURE_UNITS_ARB        0x84E2
#define GL_COMBINE_ARB                  0x8570
#define GL_COMBINE_RGB_ARB              0x8571
#define GL_COMBINE_ALPHA_ARB            0x8572
#define GL_RGB_SCALE_ARB                0x8573
#define GL_ADD_SIGNED_ARB               0x8574
#define GL_PRIMARY_COLOR_ARB            0x8577
#define GL_PREVIOUS_ARB                 0x8578
#define GL_SOURCE0_RGB_ARB              0x8580
#define GL_SOURCE1_RGB_ARB              0x8581
#define GL_SOURCE0_ALPHA_ARB            0x8588
#define GL_SOURCE1_ALPHA_ARB            0x8589
#define GL_OPERAND0_RGB_ARB             0x8590
#define GL_OPERAND1_RGB_ARB             0x8591
#define GL_OPERAND0_ALPHA_ARB           0x8598
#define GL_OPERAND1_ALPHA_ARB           0x8599
#define GL_FRAMEBUFFER                  0x8D40
#define GL_READ_FRAMEBUFFER             0x8CA8
#define GL_DRAW_FRAMEBUFFER             0x8CA9
#define GL_RENDERBUFFER                 0x8D41
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT     0x821A
#define GL_DEPTH24_STENCIL8             0x88F0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_MAX_SAMPLES                  0x8D57

typedef void (APIENTRY* PFN_glActiveTexture)(GLenum);
typedef void (APIENTRY* PFN_glMultiTexCoord2f)(GLenum, GLfloat, GLfloat);
typedef void (APIENTRY* PFN_glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei,
                                                     GLsizei, GLint, GLsizei, const void*);
static PFN_glActiveTexture        s_glActiveTexture;
static PFN_glMultiTexCoord2f      s_glMultiTexCoord2f;
static PFN_glCompressedTexImage2D s_glCompressedTexImage2D;

// Framebuffer objects (GL 3.0 / ARB_framebuffer_object), for antialiasing.
typedef void (APIENTRY* PFN_glGenObjects)(GLsizei, GLuint*);
typedef void (APIENTRY* PFN_glDeleteObjects)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFN_glBindObject)(GLenum, GLuint);
typedef void (APIENTRY* PFN_glRenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY* PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY* PFN_glCheckFramebufferStatus)(GLenum);
typedef void (APIENTRY* PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                              GLbitfield, GLenum);
static struct {
    PFN_glGenObjects                    GenFramebuffers, GenRenderbuffers;
    PFN_glDeleteObjects                 DeleteFramebuffers, DeleteRenderbuffers;
    PFN_glBindObject                    BindFramebuffer, BindRenderbuffer;
    PFN_glRenderbufferStorageMultisample RenderbufferStorageMultisample;
    PFN_glFramebufferRenderbuffer       FramebufferRenderbuffer;
    PFN_glCheckFramebufferStatus        CheckFramebufferStatus;
    PFN_glBlitFramebuffer               BlitFramebuffer;
} s_fbo;
static GLuint s_nFBO, s_nColorRB, s_nDepthRB;   // the multisampled target, 0 without antialiasing
static GLuint s_nNearList;                       // rd_NearDepth's batch, made at its first use
static int    s_nWidth;

static HWND  s_hWnd;
static HDC   s_hDC;
static HGLRC s_hRC;
static int   s_nHeight;                 // rdev.h's rects are from the top, GL's from the bottom

// ---- the device -------------------------------------------------------------------

static void gl_DestroyContext(void)
{
    if (s_hRC)
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(s_hRC);
        s_hRC = NULL;
        s_nNearList = 0;                // went with the context
    }
    if (s_hDC)
    {
        ReleaseDC(s_hWnd, s_hDC);
        s_hDC = NULL;
    }
}

// A double-buffered RGBA context with depth and stencil, made current.
static bool gl_CreateContext(void)
{
    s_hDC = GetDC(s_hWnd);
    if (!s_hDC)
        return false;

    // A window keeps its pixel format for life, and the engine reloads the
    // renderer on every focus change: reuse the one an earlier Init set.
    int nFormat = GetPixelFormat(s_hDC);
    if (!nFormat)
    {
        PIXELFORMATDESCRIPTOR pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;
        nFormat = ChoosePixelFormat(s_hDC, &pfd);
        if (!nFormat || !SetPixelFormat(s_hDC, nFormat, &pfd))
        {
            mr_Print("opengl.ren: no usable OpenGL pixel format");
            return false;
        }
    }
    s_hRC = wglCreateContext(s_hDC);
    if (!s_hRC || !wglMakeCurrent(s_hDC, s_hRC))
    {
        mr_Print("opengl.ren: could not create an OpenGL context");
        return false;
    }
    return true;
}

static void gl_DestroyTarget(void)
{
    if (s_nFBO)
    {
        s_fbo.BindFramebuffer(GL_FRAMEBUFFER, 0);
        s_fbo.DeleteFramebuffers(1, &s_nFBO);
        s_fbo.DeleteRenderbuffers(1, &s_nColorRB);
        s_fbo.DeleteRenderbuffers(1, &s_nDepthRB);
        s_nFBO = s_nColorRB = s_nDepthRB = 0;
    }
}

// The multisampled colour and depth/stencil target everything draws into.
static int gl_CreateTarget(int nSamples)
{
#define GL_FBO_PROC(name) (s_fbo.name = (decltype(s_fbo.name))wglGetProcAddress("gl" #name))
    if (!GL_FBO_PROC(GenFramebuffers) || !GL_FBO_PROC(GenRenderbuffers) || !GL_FBO_PROC(DeleteFramebuffers) ||
        !GL_FBO_PROC(DeleteRenderbuffers) || !GL_FBO_PROC(BindFramebuffer) || !GL_FBO_PROC(BindRenderbuffer) ||
        !GL_FBO_PROC(RenderbufferStorageMultisample) || !GL_FBO_PROC(FramebufferRenderbuffer) ||
        !GL_FBO_PROC(CheckFramebufferStatus) || !GL_FBO_PROC(BlitFramebuffer))
        return 1;
#undef GL_FBO_PROC
    GLint nMax = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &nMax);
    if (nSamples > nMax)
        nSamples = nMax;
    if (nSamples < 2)
        return 1;
    s_fbo.GenRenderbuffers(1, &s_nColorRB);
    s_fbo.BindRenderbuffer(GL_RENDERBUFFER, s_nColorRB);
    s_fbo.RenderbufferStorageMultisample(GL_RENDERBUFFER, nSamples, GL_RGBA8, s_nWidth, s_nHeight);
    s_fbo.GenRenderbuffers(1, &s_nDepthRB);
    s_fbo.BindRenderbuffer(GL_RENDERBUFFER, s_nDepthRB);
    s_fbo.RenderbufferStorageMultisample(GL_RENDERBUFFER, nSamples, GL_DEPTH24_STENCIL8, s_nWidth, s_nHeight);
    s_fbo.BindRenderbuffer(GL_RENDERBUFFER, 0);
    s_fbo.GenFramebuffers(1, &s_nFBO);
    s_fbo.BindFramebuffer(GL_FRAMEBUFFER, s_nFBO);
    s_fbo.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_nColorRB);
    s_fbo.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_nDepthRB);
    if (s_fbo.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        mr_Print("opengl.ren: no %dx multisampled target; antialiasing off", nSamples);
        gl_DestroyTarget();
        return 1;
    }
    return nSamples;
}

// The multisampled target into the window's back buffer. The scissor test
// applies to the blit, so it is off for it.
static void gl_Resolve(void)
{
    if (!s_nFBO)
        return;
    GLboolean bScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    s_fbo.BindFramebuffer(GL_READ_FRAMEBUFFER, s_nFBO);
    s_fbo.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    s_fbo.BlitFramebuffer(0, 0, s_nWidth, s_nHeight, 0, 0, s_nWidth, s_nHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    s_fbo.BindFramebuffer(GL_FRAMEBUFFER, s_nFBO);
    if (bScissor)
        glEnable(GL_SCISSOR_TEST);
}

bool rd_CreateDevice(void* hWnd, uint32_t nWidth, uint32_t nHeight, bool bWindowed, int nSwapInterval,
                     int nSamples)
{
    (void)bWindowed;
    s_hWnd = (HWND)hWnd;
    s_nWidth = (int)nWidth;
    s_nHeight = (int)nHeight;
    if (!gl_CreateContext())
    {
        gl_DestroyContext();
        return false;
    }

    const char* pExt = (const char*)glGetString(GL_EXTENSIONS);
    if (!pExt)
        pExt = "";
    memset(&g_RDCaps, 0, sizeof(g_RDCaps));
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_RDCaps.nMaxTextureSize);
    s_glActiveTexture = (PFN_glActiveTexture)wglGetProcAddress("glActiveTextureARB");
    s_glMultiTexCoord2f = (PFN_glMultiTexCoord2f)wglGetProcAddress("glMultiTexCoord2fARB");
    s_glCompressedTexImage2D = (PFN_glCompressedTexImage2D)wglGetProcAddress("glCompressedTexImage2DARB");
    GLint nUnits = 1;
    if (s_glActiveTexture && s_glMultiTexCoord2f)
        glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &nUnits);
    g_RDCaps.nTextureUnits = nUnits < 1 ? 1 : nUnits > 4 ? 4 : nUnits;
    g_RDCaps.bCombine = strstr(pExt, "GL_ARB_texture_env_combine") || strstr(pExt, "GL_EXT_texture_env_combine");
    g_RDCaps.bS3TC = strstr(pExt, "GL_EXT_texture_compression_s3tc") && s_glCompressedTexImage2D;
    g_RDCaps.bAniso = strstr(pExt, "GL_EXT_texture_filter_anisotropic") != NULL;
    g_RDCaps.fMaxAniso = 1.0f;
    if (g_RDCaps.bAniso)
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_RDCaps.fMaxAniso);
    g_RDCaps.bDepthClamp = strstr(pExt, "GL_ARB_depth_clamp") || strstr(pExt, "GL_NV_depth_clamp");
    g_RDCaps.bNearDepth = g_RDCaps.bDepthClamp;

    typedef BOOL (APIENTRY* PFN_wglSwapInterval)(int);
    PFN_wglSwapInterval pSwapInterval = (PFN_wglSwapInterval)wglGetProcAddress("wglSwapIntervalEXT");
    if (pSwapInterval)
        pSwapInterval(nSwapInterval);

    g_RDCaps.nSamples = nSamples > 1 ? gl_CreateTarget(nSamples) : 1;

    // The state every draw starts from (r_SetStartingRenderStates).
    glViewport(0, 0, nWidth, nHeight);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glHint(GL_FOG_HINT, GL_NICEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    return true;
}

void rd_DestroyDevice(void)
{
    if (s_hRC)
    {
        gl_DestroyTarget();
        glFlush();
        glFinish();
    }
    gl_DestroyContext();
}

void rd_Present(void)
{
    if (!s_hDC)
        return;
    gl_Resolve();
    SwapBuffers(s_hDC);
}

const char* rd_Describe(void)
{
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s (%s)", (const char*)glGetString(GL_RENDERER),
             (const char*)glGetString(GL_VERSION));
    return buf;
}

void rd_PrintInfo(void (*pPrint)(const char* pFormat, ...))
{
    pPrint("GL_VENDOR: %s", (const char*)glGetString(GL_VENDOR));
    pPrint("GL_RENDERER: %s", (const char*)glGetString(GL_RENDERER));
    pPrint("GL_VERSION: %s", (const char*)glGetString(GL_VERSION));
}

// ---- state ----------------------------------------------------------------------------

static const GLenum s_Caps[RD_CAP_COUNT] = {
    GL_BLEND, GL_ALPHA_TEST, GL_DEPTH_TEST, GL_FOG, GL_SCISSOR_TEST, GL_TEXTURE_2D, GL_DEPTH_CLAMP
};
void rd_Enable(RDCap eCap)  { glEnable(s_Caps[eCap]); }
void rd_Disable(RDCap eCap) { glDisable(s_Caps[eCap]); }
void rd_CullBack(bool bOn) { if (bOn) { glCullFace(GL_BACK); glFrontFace(GL_CW); glEnable(GL_CULL_FACE); } else glDisable(GL_CULL_FACE); }

void rd_BlendFunc(RDBlend eSrc, RDBlend eDst)
{
    static const GLenum blend[] = {
        GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE
    };
    glBlendFunc(blend[eSrc], blend[eDst]);
}

void rd_AlphaGreater(float fRef) { glAlphaFunc(GL_GREATER, fRef); }

void rd_DepthFunc(RDDepthFunc eFunc)
{
    static const GLenum func[] = { GL_LESS, GL_LEQUAL, GL_ALWAYS };
    glDepthFunc(func[eFunc]);
}

void rd_DepthMask(bool bWrite) { glDepthMask(bWrite ? GL_TRUE : GL_FALSE); }

void rd_ColorMask(bool bWrite)
{
    GLboolean b = bWrite ? GL_TRUE : GL_FALSE;
    glColorMask(b, b, b, b);
}

void rd_FogColor(float r, float g, float b)
{
    const float c[4] = { r, g, b, 1.0f };
    glFogfv(GL_FOG_COLOR, c);
}

void rd_FogRange(float fStart, float fEnd)
{
    glFogf(GL_FOG_START, fStart);
    glFogf(GL_FOG_END, fEnd);
}

void rd_Viewport(int x, int y, int w, int h) { glViewport(x, s_nHeight - y - h, w, h); }
void rd_DepthRange(float fNear, float fFar)  { glDepthRange(fNear, fFar); }

// rd_NearDepth: each batch drawn while it and depth clamping are on is
// recorded (a display list) and drawn twice, unclamped: as it is, which
// clips it at the projection's near plane and keeps what lies beyond in
// the scene's depth; then under the projection with its depth from fClip
// to that near plane, into window depth fDepth0..fDepth1, which clips it
// at both. Each part's depth is the projection's own, exact; the parts
// meet at the near plane.
static float  s_NearDepth[4];           // clip, near, depth0, depth1; clip 0: off
static bool   s_bNearRecording;

void rd_NearDepth(float fClip, float fNear, float fDepth0, float fDepth1)
{
    s_NearDepth[0] = fClip; s_NearDepth[1] = fNear;
    s_NearDepth[2] = fDepth0; s_NearDepth[3] = fDepth1;
}

static void gl_DrawNear(void)
{
    glDisable(GL_DEPTH_CLAMP);
    glCallList(s_nNearList);

    // The projection's depth row for fClip..fNear (mr_LoadCamera's form:
    // w is the eye depth, z = A*depth + B).
    GLfloat P[16], range[2];
    GLint nMode;
    glGetFloatv(GL_PROJECTION_MATRIX, P);
    glGetFloatv(GL_DEPTH_RANGE, range);
    glGetIntegerv(GL_MATRIX_MODE, &nMode);
    float n = s_NearDepth[0], f = s_NearDepth[1];
    P[10] = (f + n) / (f - n);
    P[14] = -2.0f * f * n / (f - n);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(P);
    glDepthRange(s_NearDepth[2], s_NearDepth[3]);
    glCallList(s_nNearList);
    glPopMatrix();
    glMatrixMode(nMode);
    glDepthRange(range[0], range[1]);
    glEnable(GL_DEPTH_CLAMP);
}
void rd_Scissor(int x, int y, int w, int h)  { glScissor(x, s_nHeight - y - h, w, h); }

void rd_PushAttrib(uint32_t nMask)
{
    if (nMask == RD_ATTR_ALL)
    {
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        return;
    }
    GLbitfield bits = 0;
    if (nMask & RD_ATTR_COLOR)    bits |= GL_COLOR_BUFFER_BIT;
    if (nMask & RD_ATTR_DEPTH)    bits |= GL_DEPTH_BUFFER_BIT;
    if (nMask & RD_ATTR_FOG)      bits |= GL_FOG_BIT;
    if (nMask & RD_ATTR_ENABLE)   bits |= GL_ENABLE_BIT;
    if (nMask & RD_ATTR_TEXTURE)  bits |= GL_TEXTURE_BIT;
    if (nMask & RD_ATTR_SCISSOR)  bits |= GL_SCISSOR_BIT;
    if (nMask & RD_ATTR_VIEWPORT) bits |= GL_VIEWPORT_BIT;
    glPushAttrib(bits);
}

void rd_PopAttrib(void) { glPopAttrib(); }

// ---- texture units ------------------------------------------------------------------

void rd_ActiveTexture(int nUnit)
{
    if (s_glActiveTexture)
        s_glActiveTexture(GL_TEXTURE0_ARB + nUnit);
}

void rd_TexEnv(RDTexEnv eEnv)
{
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, eEnv == RD_ENV_REPLACE ? GL_REPLACE : GL_MODULATE);
}

void rd_TexCombine(const RDCombine& c)
{
    static const GLenum op[] = { GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED_ARB };
    static const GLenum arg[] = { GL_TEXTURE, GL_PRIMARY_COLOR_ARB, GL_PREVIOUS_ARB };
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, op[c.eColorOp]);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, arg[c.eColor0]);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_ARB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, arg[c.eColor1]);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_ARB, GL_SRC_COLOR);
    glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, c.fScale);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, op[c.eAlphaOp]);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, arg[c.eAlpha0]);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA_ARB, GL_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_ARB, arg[c.eAlpha1]);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA_ARB, GL_SRC_ALPHA);
}

// ---- textures -----------------------------------------------------------------------

uint32_t rd_GenTexture(void)
{
    GLuint n = 0;
    glGenTextures(1, &n);
    return n;
}

void rd_DeleteTexture(uint32_t nTex)
{
    GLuint n = nTex;
    glDeleteTextures(1, &n);
}

void rd_BindTexture(uint32_t nTex) { glBindTexture(GL_TEXTURE_2D, nTex); }

static void gl_Pixels(RDPixels ePixels, GLenum* pFormat, GLenum* pType)
{
    switch (ePixels)
    {
    case RD_PIX_BGRA: *pFormat = GL_BGRA; *pType = GL_UNSIGNED_BYTE; break;
    case RD_PIX_RGBA: *pFormat = GL_RGBA; *pType = GL_UNSIGNED_BYTE; break;
    case RD_PIX_RGB:  *pFormat = GL_RGB;  *pType = GL_UNSIGNED_BYTE; break;
    case RD_PIX_565:  *pFormat = GL_RGB;  *pType = GL_UNSIGNED_SHORT_5_6_5; break;
    }
}

void rd_TexImage(int nLevel, RDFormat eFormat, int w, int h, RDPixels ePixels, const void* pData,
                 int nRowPixels)
{
    GLint nInternal = eFormat == RD_FMT_RGB ? GL_RGB : eFormat == RD_FMT_RGB8 ? GL_RGB8 : GL_RGBA8;
    GLenum nFormat, nType;
    gl_Pixels(ePixels, &nFormat, &nType);
    if (nRowPixels)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, nRowPixels);
    glTexImage2D(GL_TEXTURE_2D, nLevel, nInternal, w, h, 0, nFormat, nType, pData);
    if (nRowPixels)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void rd_TexSubImage(int nLevel, int x, int y, int w, int h, RDPixels ePixels, const void* pData)
{
    GLenum nFormat, nType;
    gl_Pixels(ePixels, &nFormat, &nType);
    glTexSubImage2D(GL_TEXTURE_2D, nLevel, x, y, w, h, nFormat, nType, pData);
}

void rd_CompressedTexImage(int nLevel, RDFormat eFormat, int w, int h, uint32_t nBytes, const void* pData)
{
    GLenum nFormat = eFormat == RD_FMT_DXT1  ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT :
                     eFormat == RD_FMT_DXT1A ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT :
                     eFormat == RD_FMT_DXT3  ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT :
                                               GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    s_glCompressedTexImage2D(GL_TEXTURE_2D, nLevel, nFormat, w, h, 0, nBytes, pData);
}

void rd_TexFilter(RDFilter eMin, RDFilter eMag)
{
    static const GLint filter[] = {
        GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
        GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR
    };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter[eMin]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter[eMag]);
}

void rd_TexWrap(bool bRepeatS, bool bRepeatT)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, bRepeatS ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, bRepeatT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

void rd_TexMaxLevel(int nLevel) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nLevel); }
void rd_TexAniso(float fAniso)  { glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, fAniso); }

// ---- matrices ---------------------------------------------------------------------------

void rd_MatrixMode(RDMatrixMode eMode)
{
    static const GLenum mode[] = { GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE };
    glMatrixMode(mode[eMode]);
}

void rd_LoadIdentity(void)          { glLoadIdentity(); }
void rd_LoadMatrix(const float* m)  { glLoadMatrixf(m); }
void rd_MultMatrix(const float* m)  { glMultMatrixf(m); }
void rd_PushMatrix(void)            { glPushMatrix(); }
void rd_PopMatrix(void)             { glPopMatrix(); }
void rd_Ortho(double l, double r, double b, double t, double n, double f) { glOrtho(l, r, b, t, n, f); }

// ---- drawing ------------------------------------------------------------------------------

void rd_Begin(RDPrim ePrim)
{
    static const GLenum prim[] = { GL_TRIANGLES, GL_TRIANGLE_FAN, GL_QUADS, GL_LINES };
    s_bNearRecording = s_NearDepth[0] > 0.0f && s_NearDepth[1] > s_NearDepth[0] && glIsEnabled(GL_DEPTH_CLAMP);
    if (s_bNearRecording)
    {
        if (!s_nNearList)
            s_nNearList = glGenLists(1);
        glNewList(s_nNearList, GL_COMPILE);
    }
    glBegin(prim[ePrim]);
}

void rd_End(void)
{
    glEnd();
    if (s_bNearRecording)
    {
        glEndList();
        s_bNearRecording = false;
        gl_DrawNear();
    }
}
void rd_Color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { glColor4ub(r, g, b, a); }
void rd_Color4f(float r, float g, float b, float a)         { glColor4f(r, g, b, a); }
void rd_TexCoord2f(float s, float t)                        { glTexCoord2f(s, t); }
void rd_TexCoord4f(float s, float t, float r, float q)      { glTexCoord4f(s, t, r, q); }
void rd_MultiTexCoord2f(int nUnit, float s, float t)        { s_glMultiTexCoord2f(GL_TEXTURE0_ARB + nUnit, s, t); }
void rd_Vertex2f(float x, float y)                          { glVertex2f(x, y); }
void rd_Vertex3f(float x, float y, float z)                 { glVertex3f(x, y, z); }

// ---- the frame buffer ---------------------------------------------------------------------

void rd_ClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }

void rd_Clear(uint32_t nWhat)
{
    GLbitfield bits = 0;
    if (nWhat & RD_CLEAR_COLOR)
        bits |= GL_COLOR_BUFFER_BIT;
    if (nWhat & RD_CLEAR_DEPTH)
        bits |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    glClear(bits);
}

// GL reads bottom-up: read, then turn the rows over in place.
void rd_ReadPixels(int x, int y, int w, int h, RDPixels ePixels, void* pDest, bool bFront)
{
    GLenum nFormat, nType;
    gl_Pixels(ePixels, &nFormat, &nType);
    if (s_nFBO)
    {
        // From the window's buffers: the back one resolved first.
        if (!bFront)
            gl_Resolve();
        s_fbo.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    glReadBuffer(bFront ? GL_FRONT : GL_BACK);
    glReadPixels(x, s_nHeight - y - h, w, h, nFormat, nType, pDest);
    glReadBuffer(GL_BACK);
    if (s_nFBO)
        s_fbo.BindFramebuffer(GL_READ_FRAMEBUFFER, s_nFBO);
    uint32_t nPitch = w * (ePixels == RD_PIX_565 ? 2 : 4);
    uint8_t* pRow = (uint8_t*)malloc(nPitch);
    if (!pRow)
        return;
    uint8_t* p = (uint8_t*)pDest;
    for (int i = 0; i < h / 2; i++)
    {
        memcpy(pRow, p + i * nPitch, nPitch);
        memcpy(p + i * nPitch, p + (h - 1 - i) * nPitch, nPitch);
        memcpy(p + (h - 1 - i) * nPitch, pRow, nPitch);
    }
    free(pRow);
}
