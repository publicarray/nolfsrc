// ----------------------------------------------------------------------- //
//
// MODULE  : LTGUIFont.cpp
//
// PURPOSE : Font wrapper around LithFont for the GUI controls
//
// ----------------------------------------------------------------------- //

#include "ltguimgr.h"
#include "ltguifont.h"

LTBOOL CLTGUIFont::s_bWrapUseSpaces = LTTRUE;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CLTGUIFont::CLTGUIFont()
{
	m_pLithFont = LTNULL;
}

CLTGUIFont::~CLTGUIFont()
{
	Term();
}

// Initialize a bitmap font
LTBOOL CLTGUIFont::Init(ILTClient *pClientDE, LITHFONTCREATESTRUCT *lfCS)
{
	m_pClientDE = pClientDE;
	if (!pClientDE)
		return LTFALSE;

	m_pLithFont = CreateLithFont(pClientDE, lfCS, LTTRUE);

	return (m_pLithFont != LTNULL);
}

// Termination
void CLTGUIFont::Term()
{
	if (m_pLithFont)
	{
		FreeLithFont(m_pLithFont);
		m_pLithFont = LTNULL;
	}
}

void CLTGUIFont::Draw(char *szString, HSURFACE hDest, int x, int y, int justify, HLTCOLOR color)
{
	if (!m_pClientDE || !szString || !m_pLithFont)
		return;

	LITHFONTDRAWDATA dd;
	dd.dwFlags = LTF_DRAW_NORMAL;
	dd.hColor = color;
	dd.bForceFormat = LTTRUE;

	switch (justify)
	{
	case 0:
		dd.byJustify = LTF_JUSTIFY_LEFT;
		break;
	case 1:
		dd.byJustify = LTF_JUSTIFY_RIGHT;
		break;
	case 2:
		dd.byJustify = LTF_JUSTIFY_CENTER;
		break;
	}

	if (!m_pLithFont->IsChromaKeyed() && !m_pLithFont->IsSystemFont())
	{
		if (color == SETRGB(255, 255, 255))
		{
			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_ADD);
			m_pLithFont->Draw(hDest, szString, &dd, x, y);
		}
		else if (color == LTNULL)
		{
			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_MASK);
			m_pLithFont->Draw(hDest, szString, &dd, x, y);
		}
		else
		{
			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_MASK);
			m_pLithFont->Draw(hDest, szString, &dd, x, y);

			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_ADD);
			m_pClientDE->SetOptimized2DColor(color);
			m_pLithFont->Draw(hDest, szString, &dd, x, y, LTNULL);
		}

		m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_ALPHA);
		m_pClientDE->SetOptimized2DColor(SETRGB(255, 255, 255));
	}
	else
	{
		m_pLithFont->Draw(hDest, szString, &dd, x, y);
	}
}

void CLTGUIFont::Draw(HSTRING hString, HSURFACE hDest, int x, int y, int justify, HLTCOLOR color)
{
	if (!m_pClientDE || !hString)
		return;

	char *pString = m_pClientDE->GetStringData(hString);
	Draw(pString, hDest, x, y, justify, color);
}

void CLTGUIFont::DrawFormat(char *szString, HSURFACE hDest, int x, int y, int nWidth, HLTCOLOR color)
{
	if (!m_pClientDE || !szString || !m_pLithFont)
		return;

	LITHFONTDRAWDATA dd;
	dd.dwFlags = LTF_DRAW_FORMATTED;
	dd.dwFormatWidth = nWidth;
	dd.byJustify = LTF_JUSTIFY_LEFT;
	dd.bForceFormat = LTTRUE;
	dd.hColor = color;

	if (!m_pLithFont->IsChromaKeyed() && !m_pLithFont->IsSystemFont())
	{
		if (color == SETRGB(255, 255, 255))
		{
			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_ADD);
			m_pLithFont->Draw(hDest, szString, &dd, x, y);
		}
		else if (color == LTNULL)
		{
			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_MASK);
			m_pLithFont->Draw(hDest, szString, &dd, x, y);
		}
		else
		{
			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_MASK);
			m_pLithFont->Draw(hDest, szString, &dd, x, y);

			m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_ADD);
			m_pClientDE->SetOptimized2DColor(color);
			m_pLithFont->Draw(hDest, szString, &dd, x, y, LTNULL);
		}

		m_pClientDE->SetOptimized2DBlend(LTSURFACEBLEND_ALPHA);
		m_pClientDE->SetOptimized2DColor(SETRGB(255, 255, 255));
	}
	else
	{
		m_pLithFont->Draw(hDest, szString, &dd, x, y);
	}
}

void CLTGUIFont::DrawFormat(HSTRING hString, HSURFACE hDest, int x, int y, int nWidth, HLTCOLOR color)
{
	if (!m_pClientDE || !hString)
		return;

	char *pString = m_pClientDE->GetStringData(hString);
	DrawFormat(pString, hDest, x, y, nWidth, color);
}

void CLTGUIFont::Draw(char *szString, HSURFACE hDest, LITHFONTDRAWDATA *lfDD, int x, int y, LITHFONTSAVEDATA *lfSD)
{
	if (!m_pClientDE || !szString || !m_pLithFont)
		return;

	m_pLithFont->Draw(hDest, szString, lfDD, x, y, lfSD);
}

void CLTGUIFont::Draw(HSTRING hString, HSURFACE hDest, LITHFONTDRAWDATA *lfDD, int x, int y, LITHFONTSAVEDATA *lfSD)
{
	if (!m_pClientDE || !hString || !m_pLithFont)
		return;

	m_pLithFont->Draw(hDest, hString, lfDD, x, y, lfSD);
}

// Returns the height and width of a text string
LTIntPt CLTGUIFont::GetTextExtents(HSTRING hString)
{
	LTIntPt pt(0, 0);

	if (!m_pClientDE || !hString)
		return pt;

	if (m_pLithFont)
	{
		LITHFONTDRAWDATA dd;
		dd.dwFlags = LTF_DRAW_NORMAL;
		dd.byJustify = LTF_JUSTIFY_LEFT;

		uint32 x = 0, y = 0;
		m_pLithFont->GetTextExtents(hString, &dd, x, y);

		pt.x = x;
		pt.y = y;
	}

	return pt;
}

LTIntPt CLTGUIFont::GetTextExtentsFormat(HSTRING hString, int nWidth)
{
	LTIntPt pt(0, 0);

	if (!m_pClientDE || !hString)
		return pt;

	if (m_pLithFont)
	{
		LITHFONTDRAWDATA dd;
		dd.dwFlags = LTF_DRAW_FORMATTED;
		dd.dwFormatWidth = nWidth;
		dd.byJustify = LTF_JUSTIFY_LEFT;
		dd.bForceFormat = LTTRUE;

		uint32 x = 0, y = 0;
		m_pLithFont->GetTextExtents(hString, &dd, x, y);

		pt.x = x;
		pt.y = y;
	}

	return pt;
}

LTIntPt CLTGUIFont::GetPositionFromIndex(HSTRING hString, int nIndex, int nWidth)
{
	LTIntPt pt(0, 0);

	if (!m_pClientDE || !hString)
		return pt;

	if (m_pLithFont)
	{
		LITHFONTDRAWDATA dd;
		if (nWidth > 0)
		{
			dd.dwFlags = LTF_DRAW_FORMATTED;
			dd.dwFormatWidth = nWidth;
		}
		else
		{
			dd.dwFlags = LTF_DRAW_NORMAL;
		}
		dd.bForceFormat = LTTRUE;
		dd.byJustify = LTF_JUSTIFY_LEFT;

		uint32 x = 0, y = 0;
		m_pLithFont->GetPositionFromIndex(hString, &dd, nIndex, x, y);

		pt.x = x;
		pt.y = y;
	}

	return pt;
}

// Returns the height of the font
int CLTGUIFont::GetHeight()
{
	if (m_pLithFont)
		return m_pLithFont->Height();
	return 0;
}

LTBOOL CLTGUIFont::IsChromaKeyed()
{
	if (!m_pLithFont)
		return LTFALSE;
	return m_pLithFont->IsChromaKeyed();
}

#ifdef NOLFSRC_MODERN
#include "..\lithfontmgr\lithbitmapfont.h"

// M-P3-23
void CLTGUIFont::SetScale(LTFLOAT fScale)
{
	if (m_pLithFont && !m_pLithFont->IsSystemFont())
		((LithBitmapFont*)m_pLithFont)->SetScale(fScale);
}

LTFLOAT CLTGUIFont::GetScale()
{
	if (m_pLithFont && !m_pLithFont->IsSystemFont())
		return ((LithBitmapFont*)m_pLithFont)->GetScale();
	return 1.0f;
}
#endif
