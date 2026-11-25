#include "pch.h"
#include "RuliTissue.h"
#include "StorageDlg.h"
#include "VideoLibrary.h"
#include "DarkControls.h"
#include "VectorIcons.h"
#include "Theme.h"
#include "afxdialogex.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdlib>   // std::abs
#include <set>
#include <string>   // std::wstring (SHFileOperation 이중 널 목록)

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	// 창 아이콘: 프로그램 강조색 바탕 위에 흰 파이 차트 (VectorIcon::PieChart) - size × size 32비트 아이콘
	HICON MakeStorageIcon(int size)
	{
		if (size <= 0)
			return nullptr;
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = size;
		bi.bmiHeader.biHeight = -size;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		HBITMAP color = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!color)
			return nullptr;
		{
			CDC mem;
			mem.CreateCompatibleDC(nullptr);
			HGDIOBJ old = ::SelectObject(mem.GetSafeHdc(), color);
			const CRect rc(0, 0, size, size);
			mem.FillSolidRect(rc, TCOL(TC_ACCENT));
			VectorIcon::PieChart(&mem, rc, RGB(255, 255, 255), 0.86);
			::GdiFlush();
			::SelectObject(mem.GetSafeHdc(), old);
		}
		DWORD* px = static_cast<DWORD*>(bits);
		for (int i = 0; i < size * size; ++i)
			px[i] |= 0xFF000000;   // 불투명
		HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
		ICONINFO ii = {};
		ii.fIcon = TRUE;
		ii.hbmColor = color;
		ii.hbmMask = mask;
		HICON icon = ::CreateIconIndirect(&ii);
		::DeleteObject(color);
		::DeleteObject(mask);
		return icon;
	}

	CString FormatBytes(ULONGLONG b)
	{
		const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
		double v = static_cast<double>(b);
		int u = 0;
		while (v >= 1024.0 && u < 4)
		{
			v /= 1024.0;
			++u;
		}
		CString s;
		if (u == 0)
			s.Format(L"%llu B", b);
		else
			s.Format(L"%.1f %s", v, units[u]);
		return s;
	}

	CString FormatDate(ULONGLONG ft)
	{
		if (ft == 0)
			return CString();
		FILETIME f;
		f.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF);
		f.dwHighDateTime = static_cast<DWORD>(ft >> 32);
		FILETIME lf;
		SYSTEMTIME st;
		if (!::FileTimeToLocalFileTime(&f, &lf) || !::FileTimeToSystemTime(&lf, &st))
			return CString();
		CString s;
		s.Format(L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
		return s;
	}

	bool IsImageName(const CString& name)
	{
		const CString ext = CString(::PathFindExtensionW(name)).MakeLower();
		return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".webp" || ext == L".bmp" || ext == L".gif";
	}
}

// ---------------------------------------------------------------------------
// 트리

BEGIN_MESSAGE_MAP(CStgDragLabel, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_NCHITTEST()
END_MESSAGE_MAP()

void CStgDragLabel::OnPaint()
{
	CPaintDC pdc(this);
	CRect rc;
	GetClientRect(&rc);
	CDC mem;
	mem.CreateCompatibleDC(&pdc);
	CBitmap bmp;
	bmp.CreateCompatibleBitmap(&pdc, rc.Width(), rc.Height());
	CBitmap* ob = mem.SelectObject(&bmp);
	if (m_tree)
		m_tree->PaintDragLabel(&mem, rc);
	pdc.BitBlt(0, 0, rc.Width(), rc.Height(), &mem, 0, 0, SRCCOPY);
	mem.SelectObject(ob);
}

BEGIN_MESSAGE_MAP(CStorageTree, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_SIZE()
	ON_WM_VSCROLL()
	ON_WM_MOUSEWHEEL()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_RBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_MESSAGE(WM_MOUSELEAVE, &CStorageTree::OnMouseLeave)
	ON_WM_KEYDOWN()
	ON_WM_GETDLGCODE()
	ON_WM_LBUTTONUP()
	ON_WM_CAPTURECHANGED()
	ON_WM_TIMER()
	ON_WM_SETCURSOR()
END_MESSAGE_MAP()

int CStorageTree::S(int px) const
{
	UINT dpi = GetSafeHwnd() ? ::GetDpiForWindow(m_hWnd) : 96;
	if (dpi == 0) dpi = 96;
	return ::MulDiv(px, static_cast<int>(dpi), 96);
}

bool CStorageTree::Create(CWnd* parent, const CRect& rc, UINT id)
{
	const CString cls = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW), nullptr);
	if (!CWnd::CreateEx(WS_EX_CLIENTEDGE, cls, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, rc, parent, id))
		return false;
	LOGFONT lf = {};
	if (CFont* f = parent->GetFont())
		f->GetLogFont(&lf);
	else
		::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
	m_font.CreateFontIndirect(&lf);
	lf.lfWeight = FW_BOLD;
	m_bold.CreateFontIndirect(&lf);
	CClientDC dc(this);
	CFont* old = dc.SelectObject(&m_font);
	const int th = dc.GetTextExtent(L"가Ag").cy;
	dc.SelectObject(old);
	m_rowH = th + S(8);
	m_headH = th + S(10);
	CDarkDialogTheme::ThemeChild(GetSafeHwnd());   // 어두운 스크롤바
	return true;
}

void CStorageTree::SetNodes(std::vector<StgNode>&& nodes, const std::vector<int>& roots, bool fresh)
{
	// 다시 스캔: 펼쳐 둔 폴더 · 선택은 경로(배우별 보기는 key)로 이어 줌, fresh(보기를 바꿈)면 처음처럼
	std::set<CString> open;
	CString selKey;
	const bool first = m_nodes.empty() || fresh;
	if (!first)
	{
		for (const StgNode& n : m_nodes)
			if (n.expanded)
				open.insert(KeyOf(n).MakeLower());
		if (SelectedNode() >= 0)
			selKey = KeyOf(m_nodes[SelectedNode()]);
	}
	m_nodes = std::move(nodes);
	m_roots = roots;
	for (StgNode& n : m_nodes)
		if (!first)
			n.expanded = open.count(KeyOf(n).MakeLower()) > 0;
	for (int r : m_roots)
		if (first && !m_actorMode)
			m_nodes[r].expanded = true;   // 처음에는 등록 폴더 한 단계만 펼침 (배우별 보기는 배우 목록만)
	m_rootTotal = 0;
	for (int r : m_roots)
		m_rootTotal += m_nodes[r].size;
	if (first)
		m_scroll = 0;
	SortKids();
	BuildRows();
	m_sel = -1;
	for (size_t i = 0; i < m_rows.size(); ++i)
		if (!selKey.IsEmpty() && KeyOf(m_nodes[m_rows[i]]).CompareNoCase(selKey) == 0)
			m_sel = static_cast<int>(i);
	if (m_sel < 0 && !m_rows.empty())
		m_sel = 0;
	UpdateScroll();
	Invalidate(FALSE);
	if (m_onSelChanged) m_onSelChanged();
}

void CStorageTree::SortKids()
{
	auto avg = [](const StgNode& n) { return n.ratingCnt > 0 ? static_cast<double>(n.ratingSum) / n.ratingCnt : -1.0; };
	auto less = [&](int a, int b)
	{
		const StgNode& x = m_nodes[a];
		const StgNode& y = m_nodes[b];
		int c = 0;
		switch (m_sortCol)
		{
		case C_NAME:
			if ((x.kind == 0) != (y.kind == 0))
				return x.kind == 0;   // 이름순: 폴더 먼저
			c = ::StrCmpLogicalW(x.name, y.name);
			break;
		case C_SIZE: case C_PCT: c = (x.size < y.size) ? -1 : (x.size > y.size ? 1 : 0); break;
		case C_FILES:     c = x.files - y.files; break;
		case C_FOLDERS:   c = x.folders - y.folders; break;
		case C_VIDEOS:    c = x.videos - y.videos; break;
		case C_UNREG:     c = x.unreg - y.unreg; break;
		case C_UNWATCHED: c = x.unwatched - y.unwatched; break;
		case C_PLAYS:     c = x.plays - y.plays; break;
		case C_LASTPLAY:  c = x.lastPlayed.Compare(y.lastPlayed); break;
		case C_RATING:    { const double ax = avg(x), ay = avg(y); c = (ax < ay) ? -1 : (ax > ay ? 1 : 0); break; }
		case C_MODIFIED:  c = (x.modified < y.modified) ? -1 : (x.modified > y.modified ? 1 : 0); break;
		}
		if (c == 0)
			return ::StrCmpLogicalW(x.name, y.name) < 0;
		return m_sortAsc ? c < 0 : c > 0;
	};
	for (StgNode& n : m_nodes)
		if (n.kids.size() > 1)
			std::sort(n.kids.begin(), n.kids.end(), less);
	if (m_actorMode && m_roots.size() > 1)
		std::sort(m_roots.begin(), m_roots.end(), less);   // 배우별 보기: 배우 줄도 정렬 (폴더별은 등록 순서 그대로)
}

void CStorageTree::BuildRows()
{
	m_rows.clear();
	std::vector<int> stack(m_roots.rbegin(), m_roots.rend());
	while (!stack.empty())
	{
		const int i = stack.back();
		stack.pop_back();
		m_rows.push_back(i);
		if (m_nodes[i].expanded)
			for (auto it = m_nodes[i].kids.rbegin(); it != m_nodes[i].kids.rend(); ++it)
				stack.push_back(*it);
	}
}

void CStorageTree::Toggle(int node)
{
	if (node < 0 || node >= static_cast<int>(m_nodes.size()) || m_nodes[node].kids.empty())
		return;
	const int selNode = SelectedNode();
	m_nodes[node].expanded = !m_nodes[node].expanded;
	BuildRows();
	m_sel = -1;
	for (size_t i = 0; i < m_rows.size(); ++i)
		if (m_rows[i] == selNode)
			m_sel = static_cast<int>(i);
	if (m_sel < 0)   // 접은 폴더 안을 고르고 있었으면 그 폴더로
		for (size_t i = 0; i < m_rows.size(); ++i)
			if (m_rows[i] == node)
				m_sel = static_cast<int>(i);
	UpdateScroll();
	Invalidate(FALSE);
}

void CStorageTree::LayoutCols(int width)
{
	const int w[C_COUNT] = { 0, S(78), S(92), S(52), S(48), S(48), S(54), S(64), S(44), S(108), S(44), S(88) };
	int sum = 0;
	for (int i = 1; i < C_COUNT; ++i)
	{
		m_colW[i] = w[i];
		sum += w[i];
	}
	m_colW[C_NAME] = (std::max)(S(200), width - sum);
}

void CStorageTree::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	LayoutCols(cx);
	UpdateScroll();
	Invalidate(FALSE);
}

void CStorageTree::UpdateScroll()
{
	if (!GetSafeHwnd())
		return;
	CRect rc;
	GetClientRect(&rc);
	const int page = (std::max)(1, (static_cast<int>(rc.Height()) - m_headH) / m_rowH);
	const int maxScroll = (std::max)(0, static_cast<int>(m_rows.size()) - page);
	m_scroll = (std::max)(0, (std::min)(m_scroll, maxScroll));
	SCROLLINFO si = { sizeof(si) };
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = (std::max)(0, static_cast<int>(m_rows.size()) - 1);
	si.nPage = static_cast<UINT>(page);
	si.nPos = m_scroll;
	SetScrollInfo(SB_VERT, &si, TRUE);
}

void CStorageTree::SelectKey(const CString& key, bool expand)
{
	for (size_t i = 0; i < m_nodes.size(); ++i)
	{
		if (KeyOf(m_nodes[i]).CompareNoCase(key) != 0)
			continue;
		if (expand && !m_nodes[i].kids.empty())
			m_nodes[i].expanded = true;
		for (int p = m_nodes[i].parent; p >= 0; p = m_nodes[p].parent)
			m_nodes[p].expanded = true;   // 위 단계도 펼쳐서 보이게
		BuildRows();
		for (size_t r = 0; r < m_rows.size(); ++r)
			if (m_rows[r] == static_cast<int>(i))
			{
				m_sel = static_cast<int>(r);
				// 맨 위로 (펼친 아래 줄이 보이게)
				m_scroll = m_sel;
				UpdateScroll();
			}
		Invalidate(FALSE);
		if (m_onSelChanged) m_onSelChanged();
		return;
	}
}

void CStorageTree::EnsureVisible(int row)
{
	if (row < 0)
		return;
	CRect rc;
	GetClientRect(&rc);
	const int page = (std::max)(1, (static_cast<int>(rc.Height()) - m_headH) / m_rowH);
	if (row < m_scroll)
		m_scroll = row;
	else if (row >= m_scroll + page)
		m_scroll = row - page + 1;
	UpdateScroll();
}

int CStorageTree::RowAt(CPoint pt) const
{
	if (pt.y < m_headH)
		return -1;
	const int r = m_scroll + (pt.y - m_headH) / m_rowH;
	return (r >= 0 && r < static_cast<int>(m_rows.size())) ? r : -1;
}

int CStorageTree::ColAt(int x) const
{
	int cx = 0;
	for (int c = 0; c < C_COUNT; ++c)
	{
		if (x >= cx && x < cx + m_colW[c])
			return c;
		cx += m_colW[c];
	}
	return -1;
}

CRect CStorageTree::ExpanderRect(int row) const
{
	const StgNode& n = m_nodes[m_rows[row]];
	const int y = m_headH + (row - m_scroll) * m_rowH;
	const int x = S(4) + n.depth * S(16);
	return CRect(x, y, x + S(14), y + m_rowH);
}

CString CStorageTree::CellText(const StgNode& n, int col) const
{
	CString s;
	const bool dir = (n.kind == 0 || n.kind == 4 || n.kind == 5 || n.kind == 6);
	switch (col)
	{
	case C_SIZE:      s = FormatBytes(n.size); break;
	case C_FILES:     if (dir) s.Format(L"%d", n.files); break;
	case C_FOLDERS:   if (dir && n.kind != 6) s.Format(L"%d", n.folders); break;   // 배우 줄 = 위치 수
	case C_VIDEOS:    if (n.videos > 0 && dir) s.Format(L"%d", n.videos); break;
	case C_UNREG:     if (n.unreg > 0 && !dir) s = L"미등록"; break;
	case C_UNWATCHED: if (n.unwatched > 0 && !dir) s = L"안 봄"; break;
	case C_PLAYS:     if (n.plays > 0) s.Format(L"%d", n.plays); break;
	case C_LASTPLAY:  s = n.lastPlayed.Left(16); break;
	case C_RATING:    if (n.ratingCnt > 0) s.Format(L"%.1f", static_cast<double>(n.ratingSum) / n.ratingCnt / 2.0); break;
	case C_MODIFIED:  s = FormatDate(n.modified); break;
	}
	// 폴더의 미등록 · 안 본 영상은 개수
	if (dir && col == C_UNREG && n.unreg > 0)
		s.Format(L"%d", n.unreg);
	if (dir && col == C_UNWATCHED && n.unwatched > 0)
		s.Format(L"%d", n.unwatched);
	return s;
}

void CStorageTree::DrawIcon(CDC* dc, const StgNode& n, const CRect& rc)
{
	switch (n.kind)
	{
	case 0: VectorIcon::Folder(dc, rc, RGB(0xF2, 0xB8, 0x24)); break;
	case 4: VectorIcon::FolderWarn(dc, rc, TCOL(TC_DANGER)); break;
	case 5: VectorIcon::User(dc, rc, RGB(0xE8, 0x8F, 0xB8)); break;          // 배우 (배우별 보기)
	case 6: VectorIcon::FolderPlay(dc, rc, RGB(0xF2, 0xB8, 0x24)); break;    // 배우 아래 등록 폴더
	case 1:
	{
		// 영상: 파란 칸 + 흰 삼각형
		CRect r = rc;
		r.DeflateRect(1, 2);
		dc->FillSolidRect(r, RGB(0x13, 0x7C, 0xBD));
		const int cx = r.CenterPoint().x, cy = r.CenterPoint().y, t = (std::max)(2, static_cast<int>(r.Height()) / 3);
		POINT pts[3] = { { cx - t + 1, cy - t }, { cx + t, cy }, { cx - t + 1, cy + t } };
		CBrush br(RGB(255, 255, 255));
		CPen pen(PS_SOLID, 1, RGB(255, 255, 255));
		CBrush* ob = dc->SelectObject(&br);
		CPen* op = dc->SelectObject(&pen);
		dc->Polygon(pts, 3);
		dc->SelectObject(ob);
		dc->SelectObject(op);
		break;
	}
	case 2:
	{
		CRect r = rc;
		r.DeflateRect(2, 2);
		dc->FillSolidRect(r, RGB(0x0F, 0x99, 0x60));   // 이미지: 초록 칸
		dc->FillSolidRect(r.left + 2, r.bottom - 4, r.Width() - 4, 2, RGB(255, 255, 255));
		break;
	}
	default:
	{
		CRect r = rc;
		r.DeflateRect(3, 1);
		CBrush br(TCOL(TC_SUB2));
		dc->FrameRect(r, &br);   // 그 밖의 파일: 회색 테두리
		break;
	}
	}
}

void CStorageTree::OnPaint()
{
	CPaintDC pdc(this);
	CRect client;
	GetClientRect(&client);
	CDC mem;
	mem.CreateCompatibleDC(&pdc);
	CBitmap bmp;
	bmp.CreateCompatibleBitmap(&pdc, (std::max)(1, client.Width()), (std::max)(1, client.Height()));
	CBitmap* oldBmp = mem.SelectObject(&bmp);
	CDC* dc = &mem;
	dc->FillSolidRect(client, TCOL(TC_EDIT));
	dc->SetBkMode(TRANSPARENT);
	CFont* oldFont = dc->SelectObject(&m_font);

	// 머리줄 (누르면 정렬, ▲▼)
	static const wchar_t* const kHead[C_COUNT] = { L"이름", L"크기", L"% 상위 폴더", L"파일", L"폴더", L"영상", L"미등록",
		L"안 본 영상", L"재생", L"마지막 재생", L"별점", L"수정한 날짜" };
	dc->FillSolidRect(0, 0, client.Width(), m_headH, TCOL(TC_COMBO));
	int x = 0;
	for (int c = 0; c < C_COUNT; ++c)
	{
		CRect hr(x + S(6), 0, x + m_colW[c] - S(6), m_headH);
		CString t = kHead[c];
		if (m_actorMode && c == C_PCT)
			t = L"% 상위";            // 배우 줄 = 전체 대비, 등록 폴더 줄 = 그 배우 대비
		else if (m_actorMode && c == C_FOLDERS)
			t = L"위치";              // 배우 줄 = 영상이 있는 등록 폴더 수
		if (c == m_sortCol)
			t += m_sortAsc ? L" ▲" : L" ▼";
		dc->SetTextColor(c == m_sortCol ? TCOL(TC_BRIGHT) : TCOL(TC_TEXT));
		dc->DrawText(t, hr, (c == C_NAME ? DT_LEFT : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		x += m_colW[c];
		if (c + 1 < C_COUNT)
			dc->FillSolidRect(x - 1, S(5), 1, m_headH - S(10), TCOL(TC_BORDER));
	}

	const int visible = (static_cast<int>(client.Height()) - m_headH) / m_rowH + 1;
	for (int r = m_scroll; r < static_cast<int>(m_rows.size()) && r < m_scroll + visible; ++r)
	{
		const StgNode& n = m_nodes[m_rows[r]];
		const int y = m_headH + (r - m_scroll) * m_rowH;
		const CRect row(0, y, client.right, y + m_rowH);
		if (m_dragging && r == m_dropRow)
		{
			// 끌어다 놓을 폴더: 강조 테두리
			dc->FillSolidRect(row, TCOL(TC_HILITE));
			CBrush br(TCOL(TC_ACCENT));
			CRect fr = row;
			dc->FrameRect(fr, &br);
			fr.DeflateRect(1, 1);
			dc->FrameRect(fr, &br);
		}
		else if (r == m_sel)
			dc->FillSolidRect(row, (GetFocus() == this) ? TCOL(TC_SEL) : TCOL(TC_SELNOFOCUS));
		else if (r == m_hot && !m_dragging)
			dc->FillSolidRect(row, TCOL(TC_HILITE));
		// 이름: 들여쓰기 + ▶/▼ + 아이콘 + 이름
		const CRect ex = ExpanderRect(r);
		if (!n.kids.empty())
		{
			const int cx = ex.CenterPoint().x, cy = ex.CenterPoint().y, a = S(4);
			POINT pts[3];
			if (n.expanded)
			{
				pts[0] = { cx - a, cy - a / 2 }; pts[1] = { cx + a, cy - a / 2 }; pts[2] = { cx, cy + a / 2 + 1 };
			}
			else
			{
				pts[0] = { cx - a / 2, cy - a }; pts[1] = { cx + a / 2 + 1, cy }; pts[2] = { cx - a / 2, cy + a };
			}
			CBrush br(TCOL(TC_SUB));
			CPen pen(PS_SOLID, 1, TCOL(TC_SUB));
			CBrush* ob = dc->SelectObject(&br);
			CPen* op = dc->SelectObject(&pen);
			dc->Polygon(pts, 3);
			dc->SelectObject(ob);
			dc->SelectObject(op);
		}
		const int iconS = (std::min)(S(16), m_rowH - S(4));
		const CRect icon(ex.right + S(2), y + (m_rowH - iconS) / 2, ex.right + S(2) + iconS, y + (m_rowH - iconS) / 2 + iconS);
		DrawIcon(dc, n, icon);
		{
			CRect tr(icon.right + S(6), y, m_colW[C_NAME] - S(4), y + m_rowH);
			const bool root = (n.parent < 0);
			dc->SelectObject(root ? &m_bold : &m_font);
			dc->SetTextColor(n.kind == 4 ? TCOL(TC_SUB) : TCOL(TC_TEXT));
			CString name = n.name;
			if (n.kind == 4)
				name += L"  (찾을 수 없음 - 연결 안 된 디스크?)";
			dc->DrawText(name, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			if ((root || n.kind == 6) && n.diskTotal > 0)
			{
				// 등록 폴더 이름 뒤: 디스크 남은 공간 (10% 미만이면 빨강)
				CRect calc = tr;
				dc->DrawText(name, calc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
				CRect sr(calc.right + S(12), y, tr.right, y + m_rowH);
				if (sr.right > sr.left + S(40))
				{
					dc->SelectObject(&m_font);
					CString t;
					t.Format(L"디스크 남은 공간 %s / %s", static_cast<LPCWSTR>(FormatBytes(n.diskFree)), static_cast<LPCWSTR>(FormatBytes(n.diskTotal)));
					const bool low = n.diskFree * 10 < n.diskTotal;
					if (n.otherActor)
						t = L"다른 배우 폴더  ·  " + t;   // 배우별 보기: 이 배우 영상이 다른 배우 이름의 폴더 아래에 있음
					dc->SetTextColor(low ? TCOL(TC_DANGER) : (n.otherActor ? RGB(0xE8, 0x9A, 0x3C) : TCOL(TC_SUB)));
					dc->DrawText(t, sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
				}
			}
			if (m_actorMode && n.kind == 1 && n.parent >= 0)
			{
				// 배우별 보기 영상 줄: 이름 뒤에 들어 있는 폴더 (등록 폴더 아래 경로, 길면 가운데를 … 로)
				CString root = m_nodes[n.parent].path;
				if (root.Right(1) != L"\\")
					root += L"\\";
				CString dir = n.path.Left(n.path.GetLength() - n.name.GetLength());
				if (dir.GetLength() >= root.GetLength() && dir.Left(root.GetLength()).CompareNoCase(root) == 0)
					dir = dir.Mid(root.GetLength());
				dir.TrimRight(L'\\');
				if (dir.IsEmpty())
					dir = L"(등록 폴더 바로 아래)";
				CRect calc = tr;
				dc->DrawText(name, calc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
				CRect sr(calc.right + S(12), y, tr.right, y + m_rowH);
				if (sr.right > sr.left + S(40))
				{
					dc->SelectObject(&m_font);
					dc->SetTextColor(TCOL(TC_SUB));
					dc->DrawText(dir, sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
				}
			}
			dc->SelectObject(&m_font);
		}
		// 나머지 열
		x = m_colW[C_NAME];
		const ULONGLONG parentSize = (n.parent >= 0) ? m_nodes[n.parent].size : (m_actorMode ? m_rootTotal : n.size);
		for (int c = 1; c < C_COUNT; ++c)
		{
			CRect cr(x + S(6), y, x + m_colW[c] - S(6), y + m_rowH);
			if (c == C_PCT && n.parent < 0 && n.diskTotal > 0)
			{
				// 등록 폴더 줄: 디스크 사용량 막대 (사용한 비율, 남은 공간 10% 미만이면 빨강)
				const double used = 100.0 * static_cast<double>(n.diskTotal - (std::min)(n.diskFree, n.diskTotal)) / static_cast<double>(n.diskTotal);
				const bool low = n.diskFree * 10 < n.diskTotal;
				CRect bar(x + S(4), y + S(3), x + m_colW[c] - S(4), y + m_rowH - S(3));
				dc->FillSolidRect(bar, TCOL(TC_BACK));
				CRect fill = bar;
				fill.right = fill.left + static_cast<int>(bar.Width() * (std::min)(100.0, used) / 100.0);
				if (fill.right > fill.left)
					dc->FillSolidRect(fill, low ? RGB(0xA3, 0x2D, 0x2D) : RGB(0x2B, 0x5F, 0x8C));
				CBrush br(TCOL(TC_BORDER));
				dc->FrameRect(bar, &br);
				CString t;
				t.Format(L"사용 %.0f %%", used);
				dc->SetTextColor(TCOL(TC_TEXT));
				dc->DrawText(t, bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
			else if (c == C_PCT)
			{
				// 상위 폴더 대비 % 막대
				const double pct = parentSize > 0 ? 100.0 * static_cast<double>(n.size) / static_cast<double>(parentSize) : 0.0;
				CRect bar(x + S(4), y + S(3), x + m_colW[c] - S(4), y + m_rowH - S(3));
				dc->FillSolidRect(bar, TCOL(TC_BACK));
				CRect fill = bar;
				fill.right = fill.left + static_cast<int>(bar.Width() * (std::min)(100.0, pct) / 100.0);
				if (fill.right > fill.left)
					dc->FillSolidRect(fill, RGB(0x2B, 0x5F, 0x8C));
				CBrush br(TCOL(TC_BORDER));
				dc->FrameRect(bar, &br);
				CString t;
				t.Format(L"%.1f %%", pct);
				dc->SetTextColor(TCOL(TC_TEXT));
				dc->DrawText(t, bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
			else
			{
				const CString t = CellText(n, c);
				if (!t.IsEmpty())
				{
					COLORREF col = TCOL(TC_TEXT);
					if (c == C_UNREG || c == C_UNWATCHED || c == C_LASTPLAY || c == C_MODIFIED)
						col = TCOL(TC_SUB);
					if (c == C_SIZE && n.parent < 0)
						dc->SelectObject(&m_bold);
					dc->SetTextColor(col);
					dc->DrawText(t, cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
					dc->SelectObject(&m_font);
				}
			}
			x += m_colW[c];
		}
	}
	if (m_rows.empty())
	{
		dc->SetTextColor(TCOL(TC_SUB));
		CRect tr(client.left, m_headH, client.right, client.bottom);
		dc->DrawText(m_emptyText, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	dc->SelectObject(oldFont);
	pdc.BitBlt(0, 0, client.Width(), client.Height(), &mem, 0, 0, SRCCOPY);
	mem.SelectObject(oldBmp);
}

void CStorageTree::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	CRect rc;
	GetClientRect(&rc);
	const int page = (std::max)(1, (static_cast<int>(rc.Height()) - m_headH) / m_rowH);
	switch (nSBCode)
	{
	case SB_LINEUP:   m_scroll -= 1; break;
	case SB_LINEDOWN: m_scroll += 1; break;
	case SB_PAGEUP:   m_scroll -= page; break;
	case SB_PAGEDOWN: m_scroll += page; break;
	case SB_THUMBTRACK: case SB_THUMBPOSITION:
	{
		SCROLLINFO si = { sizeof(si), SIF_TRACKPOS };
		GetScrollInfo(SB_VERT, &si);
		m_scroll = si.nTrackPos;
		break;
	}
	case SB_TOP:    m_scroll = 0; break;
	case SB_BOTTOM: m_scroll = static_cast<int>(m_rows.size()); break;
	}
	(void)nPos; (void)pScrollBar;
	UpdateScroll();
	Invalidate(FALSE);
}

BOOL CStorageTree::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
	(void)nFlags; (void)pt;
	m_scroll -= (zDelta / WHEEL_DELTA) * 3;
	UpdateScroll();
	Invalidate(FALSE);
	return TRUE;
}

void CStorageTree::OnLButtonDown(UINT nFlags, CPoint point)
{
	CWnd::OnLButtonDown(nFlags, point);
	SetFocus();
	if (point.y < m_headH)
	{
		// 머리줄: 정렬 (같은 열 = 방향 바꿈, 처음에는 이름 = 가나다, 나머지 = 큰 값부터)
		const int c = ColAt(point.x);
		if (c < 0)
			return;
		if (c == m_sortCol || (c == C_PCT && m_sortCol == C_SIZE) || (c == C_SIZE && m_sortCol == C_PCT))
			m_sortAsc = !m_sortAsc;
		else
			m_sortAsc = (c == C_NAME);
		m_sortCol = (c == C_PCT) ? C_SIZE : c;
		const int selNode = SelectedNode();
		SortKids();
		BuildRows();
		for (size_t i = 0; i < m_rows.size(); ++i)
			if (m_rows[i] == selNode)
				m_sel = static_cast<int>(i);
		Invalidate(FALSE);
		return;
	}
	const int r = RowAt(point);
	if (r < 0)
		return;
	if (ExpanderRect(r).PtInRect(point) && !m_nodes[m_rows[r]].kids.empty())
	{
		m_sel = r;
		Toggle(m_rows[r]);
		if (m_onSelChanged) m_onSelChanged();
		return;
	}
	m_sel = r;
	// 끌기 후보 (등록 폴더 자체 · 찾을 수 없는 폴더는 옮기지 않음)
	const StgNode& dn = m_nodes[m_rows[r]];
	if (m_actorMode)
		m_dragNode = (dn.kind == 1 || dn.kind == 5 || dn.kind == 6) ? m_rows[r] : -1;   // 배우별: 영상 · 배우 · 배우 아래 등록 폴더
	else
		m_dragNode = (dn.parent >= 0 && dn.kind != 4) ? m_rows[r] : -1;
	m_dragStart = point;
	Invalidate(FALSE);
	if (m_onSelChanged) m_onSelChanged();
}

bool CStorageTree::IsInside(int node, int ancestor) const
{
	for (int p = node; p >= 0; p = m_nodes[p].parent)
		if (p == ancestor)
			return true;
	return false;
}

int CStorageTree::DropFolderAt(CPoint pt, int* row) const
{
	if (row) *row = -1;
	if (m_dragNode < 0)
		return -1;
	const int r = RowAt(pt);
	if (r < 0)
		return -1;
	const int node = m_rows[r];
	if (m_actorMode)
	{
		// 배우별 보기: 놓을 곳 = 배우 아래 등록 폴더 줄 (영상 줄 위면 그 등록 폴더) → 그 등록 폴더로 옮김
		int loc = -1;
		if (m_nodes[node].kind == 6)
			loc = node;
		else if (m_nodes[node].kind == 1)
			loc = m_nodes[node].parent;
		if (loc < 0 || m_nodes[loc].kind != 6 || loc == m_dragNode)
			return -1;
		const StgNode& s = m_nodes[m_dragNode];
		if (s.kind == 1 && m_nodes[s.parent].path.CompareNoCase(m_nodes[loc].path) == 0)
			return -1;   // 이미 그 등록 폴더에 있음
		if (s.kind == 6 && s.path.CompareNoCase(m_nodes[loc].path) == 0)
			return -1;
		if (s.kind == 5 && m_nodes[loc].parent == m_dragNode && s.kids.size() < 2)
			return -1;   // 이미 한곳에 모여 있음
		if (row)
			for (size_t i = 0; i < m_rows.size(); ++i)
				if (m_rows[i] == loc)
					*row = static_cast<int>(i);
		return loc;
	}
	const int folder = (m_nodes[node].kind == 0) ? node : m_nodes[node].parent;   // 파일 위에 놓으면 그 파일의 폴더
	if (folder < 0 || m_nodes[folder].kind != 0)
		return -1;
	if (IsInside(folder, m_dragNode))
		return -1;   // 자기 자신 · 자기 안으로는 못 옮김
	if (m_nodes[m_dragNode].parent == folder)
		return -1;   // 이미 그 폴더에 있음
	if (row)
		for (size_t i = 0; i < m_rows.size(); ++i)
			if (m_rows[i] == folder)
				*row = static_cast<int>(i);
	return folder;
}

CString CStorageTree::DragLabelLine2() const
{
	if (m_dragNode < 0)
		return CString();
	CString s = FormatBytes(m_nodes[m_dragNode].size);
	if (m_dropRow >= 0 && m_dropRow < static_cast<int>(m_rows.size()))
		s += L"   →  " + m_nodes[m_rows[m_dropRow]].name;
	else
		s += L"   ·  여기에 놓을 수 없음";
	return s;
}

void CStorageTree::PaintDragLabel(CDC* dc, const CRect& rc)
{
	dc->FillSolidRect(rc, TCOL(TC_EDIT));
	CBrush br(TCOL(TC_ACCENT));
	dc->FrameRect(rc, &br);
	if (m_dragNode < 0)
		return;
	const StgNode& n = m_nodes[m_dragNode];
	const int pad = S(8), ic = S(16);
	const int lineH = (rc.Height() - pad * 2) / 2;
	dc->SetBkMode(TRANSPARENT);
	DrawIcon(dc, n, CRect(rc.left + pad, rc.top + pad + (lineH - ic) / 2, rc.left + pad + ic, rc.top + pad + (lineH - ic) / 2 + ic));
	CFont* of = dc->SelectObject(&m_bold);
	dc->SetTextColor(TCOL(TC_BRIGHT));
	CRect t(rc.left + pad + ic + S(6), rc.top + pad, rc.right - pad, rc.top + pad + lineH);
	dc->DrawText(n.name, t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	dc->SelectObject(&m_font);
	dc->SetTextColor(m_dropRow >= 0 ? TCOL(TC_SUB) : TCOL(TC_DANGER));
	t.OffsetRect(0, lineH);
	dc->DrawText(DragLabelLine2(), t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	dc->SelectObject(of);
}

void CStorageTree::MoveDragLabel()
{
	if (!m_dragging || m_dragNode < 0)
		return;
	if (!m_dragLabel.GetSafeHwnd())
	{
		m_dragLabel.m_tree = this;
		const CString cls = AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW));
		if (!m_dragLabel.CreateEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, cls, L"", WS_POPUP, CRect(0, 0, 1, 1), this, 0))
			return;
		::SetLayeredWindowAttributes(m_dragLabel.m_hWnd, 0, 235, LWA_ALPHA);
	}
	// 크기: 두 줄 중 긴 쪽 (너무 길면 … 으로 자름)
	CClientDC dc(this);
	CFont* of = dc.SelectObject(&m_bold);
	const int w1 = dc.GetTextExtent(m_nodes[m_dragNode].name).cx;
	const int h1 = dc.GetTextExtent(L"가").cy;
	dc.SelectObject(&m_font);
	const int w2 = dc.GetTextExtent(DragLabelLine2()).cx;
	dc.SelectObject(of);
	const int pad = S(8), ic = S(16);
	const int lineH = (std::max)(h1, ic) + S(2);
	const int w = (std::min)(pad * 2 + ic + S(6) + (std::max)(w1, w2) + S(2), S(460));
	const int h = pad * 2 + lineH * 2;
	CPoint pt;
	::GetCursorPos(&pt);
	int x = static_cast<int>(pt.x) + S(18), y = static_cast<int>(pt.y) + S(20);
	// 화면 밖으로 나가지 않게 (오른쪽 · 아래 끝이면 커서 반대편으로)
	MONITORINFO mi = { sizeof(mi) };
	if (::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi))
	{
		if (x + w > mi.rcWork.right)
			x = (std::max)(static_cast<int>(mi.rcWork.left), static_cast<int>(pt.x) - S(8) - w);
		if (y + h > mi.rcWork.bottom)
			y = (std::max)(static_cast<int>(mi.rcWork.top), static_cast<int>(pt.y) - S(8) - h);
	}
	m_dragLabel.SetWindowPos(&CWnd::wndTop, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	m_dragLabel.Invalidate(FALSE);
}

void CStorageTree::CancelDrag()
{
	const bool was = m_dragging;
	m_dragging = false;
	m_dragNode = -1;
	m_dropRow = -1;
	KillTimer(1);
	if (m_dragLabel.GetSafeHwnd())
		m_dragLabel.DestroyWindow();
	if (GetCapture() == this)
		ReleaseCapture();
	if (was)
		Invalidate(FALSE);
}

void CStorageTree::OnLButtonUp(UINT nFlags, CPoint point)
{
	CWnd::OnLButtonUp(nFlags, point);
	if (!m_dragging)
	{
		m_dragNode = -1;
		return;
	}
	const int src = m_dragNode;
	const int dst = DropFolderAt(point, nullptr);
	CancelDrag();
	if (dst >= 0 && m_onDrop)
		m_onDrop(src, dst);
}

void CStorageTree::OnCaptureChanged(CWnd* pWnd)
{
	CWnd::OnCaptureChanged(pWnd);
	if (m_dragging && pWnd != this)
		CancelDrag();   // 다른 창이 마우스를 가져감 (Alt+Tab 등) = 끌기 취소
}

void CStorageTree::OnTimer(UINT_PTR id)
{
	if (id != 1 || !m_dragging)
	{
		CWnd::OnTimer(id);
		return;
	}
	// 끄는 중 위 · 아래 끝에 마우스가 있으면 스크롤
	CPoint pt;
	::GetCursorPos(&pt);
	ScreenToClient(&pt);
	CRect rc;
	GetClientRect(&rc);
	int d = 0;
	if (pt.y < m_headH + m_rowH / 2)
		d = -1;
	else if (pt.y > rc.bottom - m_rowH / 2)
		d = 1;
	if (d != 0)
	{
		m_scroll += d;
		UpdateScroll();
		DropFolderAt(pt, &m_dropRow);
		Invalidate(FALSE);
		MoveDragLabel();
	}
}

BOOL CStorageTree::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (m_dragging)
	{
		::SetCursor(::LoadCursor(nullptr, m_dropRow >= 0 ? IDC_ARROW : IDC_NO));
		return TRUE;
	}
	return CWnd::OnSetCursor(pWnd, nHitTest, message);
}

void CStorageTree::OnLButtonDblClk(UINT nFlags, CPoint point)
{
	CWnd::OnLButtonDblClk(nFlags, point);
	const int r = RowAt(point);
	if (r < 0)
		return;
	const int node = m_rows[r];
	if (!m_nodes[node].kids.empty())
		Toggle(node);   // 폴더 = 펼치기 / 접기
	else if (m_onOpen)
		m_onOpen(node);
}

void CStorageTree::OnRButtonDown(UINT nFlags, CPoint point)
{
	CWnd::OnRButtonDown(nFlags, point);
	SetFocus();
	const int r = RowAt(point);
	if (r >= 0)
	{
		m_sel = r;
		Invalidate(FALSE);
		if (m_onSelChanged) m_onSelChanged();
	}
	CPoint s = point;
	ClientToScreen(&s);
	if (m_onMenu)
		m_onMenu(r >= 0 ? m_rows[r] : -1, s);
}

void CStorageTree::OnMouseMove(UINT nFlags, CPoint point)
{
	CWnd::OnMouseMove(nFlags, point);
	if (!m_tracking)
	{
		TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
		m_tracking = ::TrackMouseEvent(&tme) != FALSE;
	}
	if ((nFlags & MK_LBUTTON) && m_dragNode >= 0)
	{
		if (!m_dragging && (std::abs(point.x - m_dragStart.x) > ::GetSystemMetrics(SM_CXDRAG)
			|| std::abs(point.y - m_dragStart.y) > ::GetSystemMetrics(SM_CYDRAG)))
		{
			m_dragging = true;   // 끌기 시작
			SetCapture();
			SetTimer(1, 60, nullptr);
		}
		if (m_dragging)
		{
			int row = -1;
			DropFolderAt(point, &row);
			// 마우스를 잡고 있으면 WM_SETCURSOR 가 오지 않음 → 여기서 커서 (놓을 수 있음 = 화살표, 없음 = 금지)
			::SetCursor(::LoadCursor(nullptr, row >= 0 ? IDC_ARROW : IDC_NO));
			if (row != m_dropRow)
			{
				m_dropRow = row;
				Invalidate(FALSE);
			}
			MoveDragLabel();
			return;
		}
	}
	const int r = RowAt(point);
	if (r != m_hot)
	{
		m_hot = r;
		Invalidate(FALSE);
	}
}

LRESULT CStorageTree::OnMouseLeave(WPARAM, LPARAM)
{
	m_tracking = false;
	if (m_hot >= 0)
	{
		m_hot = -1;
		Invalidate(FALSE);
	}
	return 0;
}

void CStorageTree::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	if (m_rows.empty())
		return;
	CRect rc;
	GetClientRect(&rc);
	const int page = (std::max)(1, (static_cast<int>(rc.Height()) - m_headH) / m_rowH);
	int sel = (std::max)(0, m_sel);
	const int node = m_rows[sel];
	if (nChar == VK_DELETE)
	{
		if (m_sel >= 0 && m_onDelete)
			m_onDelete(node);
		return;
	}
	switch (nChar)
	{
	case VK_UP:    sel -= 1; break;
	case VK_DOWN:  sel += 1; break;
	case VK_PRIOR: sel -= page; break;
	case VK_NEXT:  sel += page; break;
	case VK_HOME:  sel = 0; break;
	case VK_END:   sel = static_cast<int>(m_rows.size()) - 1; break;
	case VK_LEFT:
		if (m_nodes[node].expanded && !m_nodes[node].kids.empty())
		{
			Toggle(node);
			return;
		}
		for (size_t i = 0; i < m_rows.size(); ++i)   // 상위 폴더로
			if (m_rows[i] == m_nodes[node].parent)
				sel = static_cast<int>(i);
		break;
	case VK_RIGHT:
		if (!m_nodes[node].expanded && !m_nodes[node].kids.empty())
		{
			Toggle(node);
			return;
		}
		if (!m_nodes[node].kids.empty())
			sel += 1;
		break;
	case VK_RETURN:
		if (!m_nodes[node].kids.empty())
			Toggle(node);
		else if (m_onOpen)
			m_onOpen(node);
		return;
	default:
		CWnd::OnKeyDown(nChar, nRepCnt, nFlags);
		return;
	}
	m_sel = (std::max)(0, (std::min)(sel, static_cast<int>(m_rows.size()) - 1));
	EnsureVisible(m_sel);
	Invalidate(FALSE);
	if (m_onSelChanged) m_onSelChanged();
}

// ---------------------------------------------------------------------------
// 창

BEGIN_MESSAGE_MAP(CStorageDlg, CDialogEx)
	ON_WM_CTLCOLOR()
	ON_WM_SIZE()
	ON_WM_GETMINMAXINFO()
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_BN_CLICKED(IDC_STG_RESCAN, &CStorageDlg::OnRescan)
	ON_BN_CLICKED(IDC_STG_VIEW_FOLDER, &CStorageDlg::OnViewFolder)
	ON_BN_CLICKED(IDC_STG_VIEW_ACTOR, &CStorageDlg::OnViewActor)
	ON_BN_CLICKED(IDC_STG_SCATTERED, &CStorageDlg::OnScattered)
	ON_MESSAGE(WM_STG_DONE, &CStorageDlg::OnScanDone)
END_MESSAGE_MAP()

CStorageDlg::CStorageDlg(CVideoLibrary& lib, CWnd* pParent)
	: CDialogEx(IDD_STORAGE, pParent), m_lib(lib)
{
}

CStorageDlg::~CStorageDlg()
{
	StopScan();
}

BOOL CStorageDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();
	m_theme.Apply(this);
	m_iconSmall = MakeStorageIcon(::GetSystemMetrics(SM_CXSMICON));
	m_iconBig = MakeStorageIcon(::GetSystemMetrics(SM_CXICON));
	if (m_iconSmall) SetIcon(m_iconSmall, FALSE);
	if (m_iconBig) SetIcon(m_iconBig, TRUE);
	CRect frame;
	GetDlgItem(IDC_STG_TREE)->GetWindowRect(&frame);
	ScreenToClient(&frame);
	GetDlgItem(IDC_STG_TREE)->ShowWindow(SW_HIDE);   // 자리 표시용
	m_tree.Create(this, frame, IDC_STG_TREE + 1000);
	m_tree.m_onOpen = [this](int node)
	{
		const StgNode& n = m_tree.Nodes()[node];
		if (n.kind == 1 && m_onPlay)
			m_onPlay(n.path);
	};
	m_tree.m_onMenu = [this](int node, CPoint s) { ShowMenu(node, s); };
	m_tree.m_onSelChanged = [this]() { UpdateStatus(); };
	m_tree.m_onDrop = [this](int src, int dst)
	{
		if (m_actorView)
			ConsolidateDrop(src, dst);   // 배우별 보기: 다른 등록 폴더로
		else
			MoveNode(src, dst);
	};
	m_tree.m_onDelete = [this](int node) { DeleteNode(node); };
	CRect wr;
	GetWindowRect(&wr);
	m_minSize = wr.Size();
	// 처음은 크게 (화면의 80%)
	CRect work;
	::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int w = (std::max)(static_cast<int>(wr.Width()), static_cast<int>(work.Width()) * 4 / 5);
	const int h = (std::max)(static_cast<int>(wr.Height()), static_cast<int>(work.Height()) * 4 / 5);
	SetWindowPos(nullptr, work.left + (work.Width() - w) / 2, work.top + (work.Height() - h) / 2, w, h, SWP_NOZORDER);
	// 보기: 배우 상세 "저장:" 줄에서 열면 배우별
	m_actorView = !m_startActor.IsEmpty();
	m_tree.m_actorMode = m_actorView;
	m_tree.SetSortState(m_actorView ? CStorageTree::C_FOLDERS : CStorageTree::C_SIZE, false);
	CheckRadioButton(IDC_STG_VIEW_FOLDER, IDC_STG_VIEW_ACTOR, m_actorView ? IDC_STG_VIEW_ACTOR : IDC_STG_VIEW_FOLDER);
	if (CWnd* c = GetDlgItem(IDC_STG_SCATTERED))
		c->EnableWindow(m_actorView);
	StartScan();
	m_tree.SetFocus();
	return FALSE;
}

BOOL CStorageDlg::PreTranslateMessage(MSG* pMsg)
{
	// 끄는 중 Esc = 끌기 취소 (창을 닫지 않음)
	if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_ESCAPE && m_tree.GetSafeHwnd() && m_tree.IsDragging())
	{
		m_tree.CancelDrag();
		return TRUE;
	}
	// Enter 는 트리로 (폴더 펼치기 · 영상 재생) - 대화 상자 기본 버튼으로 가지 않게
	if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_RETURN && m_tree.GetSafeHwnd() && ::GetFocus() == m_tree.GetSafeHwnd())
	{
		m_tree.SendMessage(WM_KEYDOWN, VK_RETURN, pMsg->lParam);
		return TRUE;
	}
	return CDialogEx::PreTranslateMessage(pMsg);
}

void CStorageDlg::OnCancel()
{
	StopScan();
	CDialogEx::OnCancel();
}

void CStorageDlg::OnDestroy()
{
	StopScan();
	KillTimer(1);
	CDialogEx::OnDestroy();
	if (m_iconSmall) { ::DestroyIcon(m_iconSmall); m_iconSmall = nullptr; }
	if (m_iconBig) { ::DestroyIcon(m_iconBig); m_iconBig = nullptr; }
}

HBRUSH CStorageDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	if (HBRUSH br = m_theme.CtlColor(pDC, pWnd, nCtlColor))
		return br;
	return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}

void CStorageDlg::OnGetMinMaxInfo(MINMAXINFO* mmi)
{
	if (m_minSize.cx > 0)
	{
		mmi->ptMinTrackSize.x = m_minSize.cx * 3 / 4;
		mmi->ptMinTrackSize.y = m_minSize.cy * 2 / 3;
	}
	CDialogEx::OnGetMinMaxInfo(mmi);
}

void CStorageDlg::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	if (m_tree.GetSafeHwnd() && nType != SIZE_MINIMIZED)
		LayoutControls(cx, cy);
}

void CStorageDlg::LayoutControls(int cx, int cy)
{
	CRect m(7, 7, 7, 7);
	MapDialogRect(&m);
	auto rectOf = [this](UINT id) { CRect r; GetDlgItem(id)->GetWindowRect(&r); ScreenToClient(&r); return r; };
	if (CWnd* close = GetDlgItem(IDCANCEL))
	{
		const CRect r = rectOf(IDCANCEL);
		close->SetWindowPos(nullptr, cx - m.left - r.Width(), r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	const CRect st = rectOf(IDC_STG_STATUS);
	const int stY = cy - m.top - st.Height();
	GetDlgItem(IDC_STG_STATUS)->SetWindowPos(nullptr, m.left, stY, cx - m.left * 2, st.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
	if (CWnd* info = GetDlgItem(IDC_STG_INFO))
	{
		const CRect r = rectOf(IDC_STG_INFO);
		const CRect c = rectOf(IDCANCEL);
		info->SetWindowPos(nullptr, 0, 0, (std::max)(10, static_cast<int>(c.left) - static_cast<int>(r.left) - static_cast<int>(m.left)), r.Height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	CRect g;
	m_tree.GetWindowRect(&g);
	ScreenToClient(&g);
	const int mx = static_cast<int>(m.left), my = static_cast<int>(m.top), gTop = static_cast<int>(g.top);
	m_tree.SetWindowPos(nullptr, mx, gTop, cx - mx * 2, (std::max)(40, stY - my / 2 - gTop), SWP_NOZORDER | SWP_NOACTIVATE);
	Invalidate();
}

void CStorageDlg::StartScan()
{
	StopScan();
	// DB 정보 스냅숏 (스레드는 라이브러리를 직접 보지 않음)
	std::map<CString, VInfo> lib;
	for (size_t i = 0; i < m_lib.items.size(); ++i)
	{
		const VideoItem& v = m_lib.items[i];
		VInfo vi;
		vi.item = static_cast<int>(i);
		vi.pending = v.pending;
		vi.plays = v.playCount;
		vi.last = v.lastPlayed;
		vi.rating = v.rating;
		CString key = v.path;
		key.MakeLower();
		lib[key] = vi;
	}
	std::vector<CString> roots = m_lib.folders;
	m_abort = false;
	m_scanCount = 0;
	m_scanning = true;
	m_scanStart = ::GetTickCount64();
	if (CWnd* b = GetDlgItem(IDC_STG_RESCAN))
		b->EnableWindow(FALSE);
	m_thread = std::thread(&CStorageDlg::ScanWorker, std::move(roots), std::move(lib), &m_abort, &m_scanCount, GetSafeHwnd());
	SetTimer(1, 200, nullptr);
	UpdateStatus();
}

void CStorageDlg::StopScan()
{
	if (m_thread.joinable())
	{
		m_abort = true;
		m_thread.join();
	}
	m_abort = false;
	if (GetSafeHwnd())
	{
		// 끝났는데 아직 받지 못한 결과는 버림
		MSG msg;
		while (::PeekMessageW(&msg, GetSafeHwnd(), WM_STG_DONE, WM_STG_DONE, PM_REMOVE))
			delete reinterpret_cast<ScanResult*>(msg.lParam);
	}
	m_scanning = false;
}

void CStorageDlg::ScanWorker(std::vector<CString> roots, std::map<CString, VInfo> lib, std::atomic<bool>* abort,
	std::atomic<int>* count, HWND notify)
{
	std::unique_ptr<ScanResult> res(new ScanResult);
	std::vector<StgNode>& nodes = res->nodes;
	for (const CString& rootPath : roots)
	{
		StgNode root;
		root.name = rootPath;
		root.path = rootPath;
		root.kind = ::PathIsDirectoryW(rootPath) ? 0 : 4;
		if (root.kind == 0)
		{
			// 디스크 남은 공간 (등록 폴더가 있는 드라이브 · 네트워크 공유 기준)
			CString dirPath = rootPath;
			if (dirPath.Right(1) != L"\\")
				dirPath += L"\\";
			ULARGE_INTEGER freeB = {}, totalB = {};
			if (::GetDiskFreeSpaceExW(dirPath, &freeB, &totalB, nullptr))
			{
				root.diskFree = freeB.QuadPart;
				root.diskTotal = totalB.QuadPart;
			}
		}
		const int ri = static_cast<int>(nodes.size());
		nodes.push_back(root);
		res->roots.push_back(ri);
		if (nodes[ri].kind == 4)
			continue;
		std::vector<int> stack(1, ri);
		while (!stack.empty() && !*abort)
		{
			const int di = stack.back();
			stack.pop_back();
			CString base = nodes[di].path;
			if (base.Right(1) != L"\\")
				base += L"\\";
			WIN32_FIND_DATAW fd;
			HANDLE h = ::FindFirstFileExW(base + L"*", FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
			if (h == INVALID_HANDLE_VALUE)
				continue;
			do
			{
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
					continue;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
					continue;   // 정션 · 심볼릭 링크는 건너뜀 (같은 파일을 두 번 세지 않게)
				StgNode n;
				n.name = fd.cFileName;
				n.path = base + fd.cFileName;
				n.parent = di;
				n.depth = nodes[di].depth + 1;
				n.modified = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
				const int ni = static_cast<int>(nodes.size());
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					n.kind = 0;
					nodes.push_back(n);
					nodes[di].kids.push_back(ni);
					stack.push_back(ni);
					continue;
				}
				n.size = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
				n.files = 1;
				if (CVideoLibrary::IsVideoFile(n.path))
				{
					n.kind = 1;
					n.videos = 1;
					CString key = n.path;
					key.MakeLower();
					auto it = lib.find(key);
					if (it == lib.end() || it->second.pending)
						n.unreg = 1;   // DB 에 없거나 정보 저장 전 (임시)
					if (it != lib.end())
					{
						n.item = it->second.item;
						n.plays = it->second.plays;
						n.lastPlayed = it->second.last;
						if (it->second.rating > 0)
						{
							n.ratingSum = it->second.rating;
							n.ratingCnt = 1;
						}
					}
					if (n.plays == 0)
						n.unwatched = 1;
				}
				else
					n.kind = IsImageName(n.name) ? 2 : 3;
				nodes.push_back(n);
				nodes[di].kids.push_back(ni);
				count->fetch_add(1);
			} while (!*abort && ::FindNextFileW(h, &fd));
			::FindClose(h);
		}
		if (*abort)
			return;
	}
	// 아래에서 위로 합계 (자식 번호 > 부모 번호)
	for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i)
	{
		const int p = nodes[i].parent;
		if (p < 0)
			continue;
		StgNode& c = nodes[i];
		StgNode& pn = nodes[p];
		pn.size += c.size;
		pn.files += c.files;
		pn.folders += c.folders + (c.kind == 0 ? 1 : 0);
		pn.videos += c.videos;
		pn.unreg += c.unreg;
		pn.unwatched += c.unwatched;
		pn.plays += c.plays;
		pn.ratingSum += c.ratingSum;
		pn.ratingCnt += c.ratingCnt;
		if (c.lastPlayed.Compare(pn.lastPlayed) > 0)
			pn.lastPlayed = c.lastPlayed;
		if (c.modified > pn.modified)
			pn.modified = c.modified;
	}
	if (*abort)
		return;
	ScanResult* raw = res.release();
	if (!::PostMessageW(notify, WM_STG_DONE, 0, reinterpret_cast<LPARAM>(raw)))
		delete raw;
}

LRESULT CStorageDlg::OnScanDone(WPARAM, LPARAM lParam)
{
	std::unique_ptr<ScanResult> res(reinterpret_cast<ScanResult*>(lParam));
	if (m_thread.joinable())
		m_thread.join();
	m_scanning = false;
	m_scanMs = ::GetTickCount64() - m_scanStart;
	KillTimer(1);
	if (CWnd* b = GetDlgItem(IDC_STG_RESCAN))
		b->EnableWindow(TRUE);
	if (res)
	{
		m_scan = res->nodes;
		m_scanRoots = res->roots;
		if (m_actorView)
			BuildActorView(false);
		else
		{
			m_tree.m_emptyText = L"등록 폴더가 없습니다 (폴더 관리에서 추가)";
			m_tree.SetNodes(std::move(res->nodes), res->roots);
		}
	}
	if (!m_startActor.IsEmpty() && m_actorView)
	{
		// 배우 상세 "저장:" 줄에서 열었음 → 그 배우를 펼쳐서 고름
		CString key = m_lib.ActorKeyOf(m_lib.ActorNameIndex(), m_startActor);
		m_tree.SelectKey(L"a|" + key, true);
	}
	m_startActor.Empty();
	UpdateStatus();
	return 0;
}

int CStorageDlg::ScanIdx(int treeNode) const
{
	std::vector<StgNode>& nodes = const_cast<CStorageTree&>(m_tree).Nodes();
	if (treeNode < 0 || treeNode >= static_cast<int>(nodes.size()))
		return -1;
	return m_actorView ? nodes[treeNode].src : treeNode;
}

int CStorageDlg::RootOf(int scanIdx) const
{
	int r = scanIdx;
	while (r >= 0 && m_scan[r].parent >= 0)
		r = m_scan[r].parent;
	return r;
}

void CStorageDlg::ShowView(bool actor)
{
	// [폴더별] / [배우별] 보기 바꿈 (스캔 결과는 그대로, 트리만 다시)
	if (m_tree.IsDragging())
		m_tree.CancelDrag();
	m_actorView = actor;
	m_tree.m_actorMode = actor;
	m_tree.SetSortState(actor ? CStorageTree::C_FOLDERS : CStorageTree::C_SIZE, false);   // 배우별 = 위치 수가 많은 배우부터
	CheckRadioButton(IDC_STG_VIEW_FOLDER, IDC_STG_VIEW_ACTOR, actor ? IDC_STG_VIEW_ACTOR : IDC_STG_VIEW_FOLDER);
	if (CWnd* c = GetDlgItem(IDC_STG_SCATTERED))
		c->EnableWindow(actor);
	if (m_scanning)
	{
		m_tree.m_emptyText = L"등록 폴더를 훑는 중입니다…";
		m_tree.SetNodes(std::vector<StgNode>(), std::vector<int>(), true);
		UpdateStatus();
		return;
	}
	if (actor)
		BuildActorView(true);
	else
	{
		m_tree.m_emptyText = L"등록 폴더가 없습니다 (폴더 관리에서 추가)";
		std::vector<StgNode> copy = m_scan;
		m_tree.SetNodes(std::move(copy), m_scanRoots, true);
	}
	UpdateStatus();
	m_tree.SetFocus();
}

void CStorageDlg::OnViewFolder()
{
	if (m_actorView)
		ShowView(false);
}

void CStorageDlg::OnViewActor()
{
	if (!m_actorView)
		ShowView(true);
}

void CStorageDlg::OnScattered()
{
	m_scatteredOnly = (IsDlgButtonChecked(IDC_STG_SCATTERED) == BST_CHECKED);
	if (m_actorView && !m_scanning)
		BuildActorView(false);   // 펼친 배우 · 선택은 그대로
	UpdateStatus();
}

void CStorageDlg::BuildActorView(bool fresh)
{
	// 배우별 보기: 배우 → (그 배우 영상이 있는) 등록 폴더 → 영상
	//  - 배우는 DB 배우 정보로 묶음 (별칭은 본래 배우로, 폴더 이름과 관계없음), 여러 배우가 나오는 영상은 배우마다
	//  - DB 에 없는 영상(배우 정보 없음)은 빠짐
	const std::map<CString, int> nameIndex = m_lib.ActorNameIndex();
	//  - 위치 = 등록 폴더 아래 첫 단계 폴더(배우 폴더 - 등록 폴더\배우\[날짜]품번\영상), 그 폴더가 배우 폴더가 아니면(영상이 바로 아래 · 품번 폴더) 등록 폴더
	//    → 같은 드라이브여도 다른 배우 폴더 아래에 들어간 영상은 다른 위치로 나뉨 (흩어짐)
	std::map<CString, std::map<int, std::vector<int>>> groups;   // 배우 키 → (위치 폴더 스캔 번호 → 영상 스캔 번호)
	std::map<CString, CString> display;
	m_videoLoc.assign(m_scan.size(), -1);
	for (size_t i = 0; i < m_scan.size(); ++i)
	{
		const StgNode& v = m_scan[i];
		if (v.kind != 1 || v.item < 0 || v.item >= static_cast<int>(m_lib.items.size()))
			continue;
		int root = RootOf(static_cast<int>(i));
		{
			// 등록 폴더 바로 아래 폴더 (배우 폴더 후보)
			int first = static_cast<int>(i);
			while (first >= 0 && m_scan[first].parent >= 0 && m_scan[m_scan[first].parent].parent >= 0)
				first = m_scan[first].parent;
			if (first >= 0 && first != static_cast<int>(i) && m_scan[first].kind == 0)
			{
				// 배우 폴더로 봄: 영상이 2단계 이상 아래(배우\품번\영상) 이거나, 폴더 이름이 알려진 배우 이름 · 별칭
				CString fk = m_scan[first].name;
				fk.MakeLower();
				if (v.depth >= 3 || nameIndex.find(fk) != nameIndex.end())
					root = first;
			}
		}
		m_videoLoc[i] = root;
		std::set<CString> seen;
		for (const CString& a : CVideoLibrary::SplitList(m_lib.items[v.item].actors))
		{
			const CString key = m_lib.ActorKeyOf(nameIndex, a);
			if (key.IsEmpty() || !seen.insert(key).second)
				continue;
			if (display.find(key) == display.end())
			{
				auto it = nameIndex.find(key);
				display[key] = (it != nameIndex.end() && it->second >= 0 && it->second < static_cast<int>(m_lib.actors.size()))
					? m_lib.actors[it->second].name : a;
			}
			groups[key][root].push_back(static_cast<int>(i));
		}
	}
	int scattered = 0;
	std::vector<StgNode> nodes;
	std::vector<int> roots;
	for (const auto& g : groups)
	{
		if (g.second.size() >= 2)
			++scattered;
		if (m_scatteredOnly && g.second.size() < 2)
			continue;
		StgNode an;
		an.kind = 5;
		an.name = display[g.first];
		an.key = L"a|" + g.first;
		an.folders = static_cast<int>(g.second.size());   // 위치 수
		const int ai = static_cast<int>(nodes.size());
		nodes.push_back(an);
		roots.push_back(ai);
		for (const auto& loc : g.second)
		{
			const StgNode& r = m_scan[loc.first];
			const StgNode& rootNode = m_scan[RootOf(loc.first)];
			StgNode ln;
			ln.kind = 6;
			ln.name = r.path;
			ln.path = r.path;
			ln.key = an.key + L"|" + CString(r.path).MakeLower();
			ln.parent = ai;
			ln.depth = 1;
			ln.src = loc.first;
			ln.diskFree = rootNode.diskFree;
			ln.diskTotal = rootNode.diskTotal;
			if (r.parent >= 0)
				ln.otherActor = (m_lib.ActorKeyOf(nameIndex, r.name) != g.first);   // 이 배우 이름 · 별칭이 아닌 폴더
			const int li = static_cast<int>(nodes.size());
			nodes.push_back(ln);
			nodes[ai].kids.push_back(li);
			for (int vi : loc.second)
			{
				StgNode vn = m_scan[vi];
				vn.kids.clear();
				vn.parent = li;
				vn.depth = 2;
				vn.src = vi;
				vn.expanded = false;
				vn.key = ln.key + L"|" + CString(vn.path).MakeLower();
				const int ni = static_cast<int>(nodes.size());
				nodes.push_back(vn);
				nodes[li].kids.push_back(ni);
			}
		}
	}
	// 합계: 영상 → 등록 폴더 → 배우 (자식 번호 > 부모 번호)
	for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i)
	{
		const int p = nodes[i].parent;
		if (p < 0)
			continue;
		StgNode& c = nodes[i];
		StgNode& pn = nodes[p];
		pn.size += c.size;
		pn.files += c.files;
		pn.videos += c.videos;
		pn.unreg += c.unreg;
		pn.unwatched += c.unwatched;
		pn.plays += c.plays;
		pn.ratingSum += c.ratingSum;
		pn.ratingCnt += c.ratingCnt;
		if (c.lastPlayed.Compare(pn.lastPlayed) > 0)
			pn.lastPlayed = c.lastPlayed;
		if (c.modified > pn.modified)
			pn.modified = c.modified;
	}
	m_actorCount = static_cast<int>(groups.size());
	m_scatteredCount = scattered;
	if (groups.empty())
		m_tree.m_emptyText = L"배우 정보가 있는 영상이 없습니다";
	else
		m_tree.m_emptyText = L"여러 등록 폴더에 흩어진 배우가 없습니다 (모두 한곳에 모여 있음)";
	m_tree.SetNodes(std::move(nodes), roots, fresh);
}

void CStorageDlg::CollectVideos(int treeNode, std::vector<int>& out) const
{
	// 배우별 보기: 영상 줄 = 그 영상, 등록 폴더 줄 = 그 아래 영상, 배우 줄 = 그 배우 영상 전부 (스캔 노드 번호, 중복 없이)
	out.clear();
	std::vector<StgNode>& nodes = const_cast<CStorageTree&>(m_tree).Nodes();
	if (treeNode < 0 || treeNode >= static_cast<int>(nodes.size()))
		return;
	std::set<int> seen;
	std::vector<int> stack(1, treeNode);
	while (!stack.empty())
	{
		const int i = stack.back();
		stack.pop_back();
		if (nodes[i].kind == 1)
		{
			if (nodes[i].src >= 0 && seen.insert(nodes[i].src).second)
				out.push_back(nodes[i].src);
			continue;
		}
		for (int k : nodes[i].kids)
			stack.push_back(k);
	}
}

CString CStorageDlg::WhatOf(int treeNode) const
{
	std::vector<StgNode>& nodes = const_cast<CStorageTree&>(m_tree).Nodes();
	if (treeNode < 0 || treeNode >= static_cast<int>(nodes.size()))
		return CString();
	const StgNode& n = nodes[treeNode];
	if (n.kind == 6 && n.parent >= 0)
		return nodes[n.parent].name + L" (" + n.path + L")";
	return n.name;
}

void CStorageDlg::ConsolidateDrop(int src, int dstLocation)
{
	// 배우별 보기 끌어다 놓기: 놓은 줄의 등록 폴더로
	std::vector<StgNode>& nodes = m_tree.Nodes();
	if (dstLocation < 0 || nodes[dstLocation].kind != 6)
		return;
	std::vector<int> vids;
	CollectVideos(src, vids);
	MoveVideosToFolder(vids, nodes[dstLocation].path, RootOf(nodes[dstLocation].src), WhatOf(src));
}

namespace
{
	CString JoinPath(CString base, CString rel)
	{
		rel.TrimLeft(L'\\');
		if (base.Right(1) != L"\\")
			base += L"\\";
		return base + rel;
	}

	CString RelOf(const CString& path, CString root)
	{
		// 등록 폴더 아래 경로 (앞의 \ 없이)
		if (root.Right(1) != L"\\")
			root += L"\\";
		return (path.GetLength() > root.GetLength() && path.Left(root.GetLength()).CompareNoCase(root) == 0) ? path.Mid(root.GetLength()) : CString(::PathFindFileNameW(path));
	}

	CString VolumeOf(const CString& path)
	{
		wchar_t v[MAX_PATH] = {};
		if (!::GetVolumePathNameW(path, v, MAX_PATH))
			return CString();
		return CString(v).MakeLower();
	}
}

void CStorageDlg::MoveVideosToFolder(const std::vector<int>& videos, const CString& targetFolder, int targetRoot, const CString& what)
{
	// 영상(스캔 노드)들을 위치 폴더(targetFolder - 배우 폴더나 등록 폴더, 없으면 만듦)로 - 지금 위치 폴더 아래 경로 그대로
	//   (D:\영상\배우B\[날짜]품번 → E:\보관\배우A\[날짜]품번, targetRoot = 그 폴더가 있는 등록 폴더 - 남은 공간)
	//  - 영상의 폴더가 그 영상 하나만 든 폴더(위치 폴더 아래 품번 폴더)면 폴더째, 아니면 영상 + 같은 이름 커버 · 자막 등
	//  - 이미 그 위치 폴더에 있는 영상은 빼고, 옮길 곳에 같은 이름이 있거나 작업 중인 영상은 건너뜀
	//  - 다른 드라이브로 가는 크기가 남은 공간보다 크면 옮기지 않음
	if (m_scanning)
	{
		MessageBox(L"스캔이 끝난 뒤에 옮겨 주세요.", L"저장 공간", MB_ICONINFORMATION);
		return;
	}
	if (targetRoot < 0 || targetRoot >= static_cast<int>(m_scan.size()) || m_scan[targetRoot].kind != 0)
		return;
	const StgNode& tr = m_scan[targetRoot];
	CString tf = targetFolder;
	if (tf.GetLength() > 3)
		tf.TrimRight(L'\\');
	const CString tVol = VolumeOf(tr.path);
	struct Item { CString from, to; bool dir; };
	std::vector<Item> items;
	std::set<CString> seenFrom;
	CString skipped, example;
	int skipCount = 0, videoCount = 0, already = 0;
	ULONGLONG total = 0, need = 0;
	auto skip = [&](const CString& path, const wchar_t* why)
	{
		++skipCount;
		if (skipCount <= 8)
			skipped += L"\n  · " + CString(::PathFindFileNameW(path)) + L" - " + why;
	};
	for (int v : videos)
	{
		if (v < 0 || v >= static_cast<int>(m_scan.size()))
			continue;
		const int loc = (v < static_cast<int>(m_videoLoc.size()) && m_videoLoc[v] >= 0) ? m_videoLoc[v] : RootOf(v);
		CString locPath = m_scan[loc].path;
		if (locPath.GetLength() > 3)
			locPath.TrimRight(L'\\');
		if (locPath.CompareNoCase(tf) == 0)
		{
			++already;
			continue;
		}
		const StgNode& vn = m_scan[v];
		const StgNode& rn = m_scan[loc];   // 지금 위치 폴더 (이 아래 경로를 그대로 옮김)
		if (m_isBusy && m_isBusy(vn.path))
		{
			skip(vn.path, L"스크린샷 · 미리보기 작업 중");
			continue;
		}
		std::vector<Item> unit;
		ULONGLONG size = vn.size;
		const int p = vn.parent;
		if (p >= 0 && p != loc && m_scan[p].parent >= 0 && m_scan[p].videos == 1)
		{
			// 품번 폴더째 (위치 폴더 아래, 영상 하나만 든 폴더)
			unit.push_back({ m_scan[p].path, JoinPath(tf, RelOf(m_scan[p].path, rn.path)), true });
			size = m_scan[p].size;
		}
		else
		{
			std::vector<CString> from;
			CString extra;
			CollectWithSidecars(v, from, extra);
			for (const CString& f : from)
				unit.push_back({ f, JoinPath(tf, RelOf(f, rn.path)), ::PathIsDirectoryW(f) != FALSE });
		}
		if (unit.empty() || seenFrom.count(CString(unit[0].from).MakeLower()))
			continue;   // 다른 배우로 이미 넣음 (같은 영상)
		bool clash = false;
		for (const Item& it : unit)
			if (::PathFileExistsW(it.to))
				clash = true;
		if (clash)
		{
			skip(vn.path, L"옮길 곳에 같은 이름이 있음");
			continue;
		}
		for (const Item& it : unit)
		{
			seenFrom.insert(CString(it.from).MakeLower());
			items.push_back(it);
		}
		if (example.IsEmpty())
			example = unit[0].from + L"\n   → " + unit[0].to;
		++videoCount;
		total += size;
		if (VolumeOf(rn.path) != tVol)
			need += size;
	}
	if (items.empty())
	{
		CString m = (already > 0 && skipCount == 0) ? CString(L"이미 모두 그 폴더에 있습니다.") : CString(L"옮길 영상이 없습니다.");
		if (!skipped.IsEmpty())
			m += L"\n\n건너뜀:" + skipped;
		MessageBox(m, L"저장 공간", MB_ICONINFORMATION);
		return;
	}
	if (need > 0 && tr.diskTotal > 0 && need >= tr.diskFree)
	{
		CString m;
		m.Format(L"옮길 곳의 남은 공간이 모자랍니다.\n\n%s\n필요 %s · 남은 공간 %s", static_cast<LPCWSTR>(tr.path),
			static_cast<LPCWSTR>(FormatBytes(need)), static_cast<LPCWSTR>(FormatBytes(tr.diskFree)));
		MessageBox(m, L"저장 공간", MB_ICONWARNING);
		return;
	}
	CString msg;
	msg.Format(L"%s\n영상 %d개 (%s) 를\n→ %s%s\n로 옮길까요?\n\n지금 위치(배우 폴더 · 등록 폴더) 아래 경로 그대로 옮깁니다 (품번 폴더는 폴더째, 커버 · 자막도 함께).\n예) %s",
		static_cast<LPCWSTR>(what), videoCount, static_cast<LPCWSTR>(FormatBytes(total)), static_cast<LPCWSTR>(tf),
		::PathIsDirectoryW(tf) ? L"" : L"  (새 폴더)", static_cast<LPCWSTR>(example));
	if (tr.diskTotal > 0 && need > 0)
	{
		CString t;
		t.Format(L"\n\n남은 공간 %s → 옮긴 뒤 약 %s", static_cast<LPCWSTR>(FormatBytes(tr.diskFree)), static_cast<LPCWSTR>(FormatBytes(tr.diskFree - need)));
		msg += t;
	}
	if (already > 0)
	{
		CString t;
		t.Format(L"\n(이미 그 폴더에 있는 영상 %d개는 그대로)", already);
		msg += t;
	}
	if (skipCount > 0)
	{
		CString t;
		t.Format(L"\n\n건너뜀 %d개:", skipCount);
		msg += t + skipped + (skipCount > 8 ? L"\n  …" : L"");
	}
	if (MessageBox(msg, L"한곳으로 모으기", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
		return;
	std::wstring fromList, toList;
	for (const Item& it : items)
	{
		fromList += it.from.GetString();
		fromList.push_back(L'\0');
		toList += it.to.GetString();
		toList.push_back(L'\0');
	}
	fromList.push_back(L'\0');
	toList.push_back(L'\0');
	SHFILEOPSTRUCTW op = {};
	op.hwnd = GetSafeHwnd();
	op.wFunc = FO_MOVE;
	op.pFrom = fromList.c_str();
	op.pTo = toList.c_str();
	op.fFlags = FOF_MULTIDESTFILES | FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;   // 하나하나 정확한 대상 경로로 (없는 폴더는 만듦)
	const int rc = ::SHFileOperationW(&op);
	int moved = 0;
	for (const Item& it : items)
	{
		if (::PathFileExistsW(it.to) && !::PathFileExistsW(it.from))
		{
			++moved;
			m_moved = true;
			if (m_onMoved)
				m_onMoved(it.from, it.to, it.dir);
		}
	}
	if (rc != 0 || op.fAnyOperationsAborted || moved < static_cast<int>(items.size()))
	{
		CString e;
		e.Format(L"모두 옮기지 못했습니다 (%d / %d개 옮김%s).", moved, static_cast<int>(items.size()), op.fAnyOperationsAborted ? L", 취소됨" : L"");
		MessageBox(e, L"저장 공간", MB_ICONWARNING);
	}
	StartScan();
}

void CStorageDlg::OnTimer(UINT_PTR id)
{
	if (id == 1)
	{
		UpdateStatus();
		return;
	}
	CDialogEx::OnTimer(id);
}

void CStorageDlg::OnRescan()
{
	StartScan();
}

void CStorageDlg::UpdateStatus()
{
	CString s;
	if (m_scanning)
	{
		s.Format(L"등록 폴더를 훑는 중… 파일 %d개", m_scanCount.load());
		SetDlgItemText(IDC_STG_STATUS, s);
		return;
	}
	// 전체 합계 + 드라이브 남은 공간 + 고른 줄
	const std::vector<StgNode>& nodes = m_scan;
	ULONGLONG total = 0;
	int files = 0, videos = 0, unreg = 0;
	std::set<CString> drives;
	for (const StgNode& n : nodes)
		if (n.parent < 0)
		{
			total += n.size;
			files += n.files;
			videos += n.videos;
			unreg += n.unreg;
			wchar_t root[MAX_PATH] = {};
			wcsncpy_s(root, n.path, _TRUNCATE);
			if (::PathStripToRootW(root))
				drives.insert(CString(root).MakeUpper());
		}
	s.Format(L"합계 %s · 파일 %d개 · 영상 %d개 (미등록 %d개) · %.1f초", static_cast<LPCWSTR>(FormatBytes(total)), files, videos, unreg, m_scanMs / 1000.0);
	if (m_actorView)
	{
		CString t;
		t.Format(L"   |   배우 %d명 · 2곳 이상에 흩어진 배우 %d명", m_actorCount, m_scatteredCount);
		s += t;
	}
	for (const CString& d : drives)
	{
		ULARGE_INTEGER freeB = {}, totalB = {};
		if (::GetDiskFreeSpaceExW(d, &freeB, &totalB, nullptr))
		{
			CString t;
			t.Format(L"   |   %s 남은 공간 %s / %s", static_cast<LPCWSTR>(d), static_cast<LPCWSTR>(FormatBytes(freeB.QuadPart)), static_cast<LPCWSTR>(FormatBytes(totalB.QuadPart)));
			s += t;
		}
	}
	SetDlgItemText(IDC_STG_STATUS, s);
}

void CStorageDlg::CollectWithSidecars(int src, std::vector<CString>& from, CString& extraNames) const
{
	// src(스캔 노드) 자신 + (영상이면) 같은 폴더에서 같은 이름으로 시작하는 파일 · 폴더 (이름 뒤가 . _ 공백 - 커버 · 세로 커버 · 자막 · 스크린샷 폴더, 다른 영상은 빼고)
	const std::vector<StgNode>& nodes = m_scan;
	from.clear();
	extraNames.Empty();
	if (src < 0 || src >= static_cast<int>(nodes.size()))
		return;
	const StgNode& s = nodes[src];
	from.assign(1, s.path);
	extraNames.Empty();
	if (s.kind != 1 || s.parent < 0)
		return;
	const int dot = s.name.ReverseFind(L'.');
	const CString stem = (dot > 0) ? s.name.Left(dot) : s.name;
	for (int k : nodes[s.parent].kids)
	{
		const StgNode& c = nodes[k];
		if (k == src || c.kind == 1 || c.name.GetLength() <= stem.GetLength())
			continue;
		if (c.name.Left(stem.GetLength()).CompareNoCase(stem) != 0)
			continue;
		const wchar_t ch = c.name[stem.GetLength()];
		if (ch == L'.' || ch == L'_' || ch == L' ')
		{
			from.push_back(c.path);
			extraNames += L"\n  · " + c.name;
		}
	}
}

bool CStorageDlg::CanDelete(int node) const
{
	// 등록 폴더 자체 · 찾을 수 없는 폴더는 지우지 않음 (등록 폴더는 폴더 관리에서), 배우별 보기는 영상 줄만
	const int si = (node >= 0) ? ScanIdx(node) : -1;
	if (si < 0 || si >= static_cast<int>(m_scan.size()))
		return false;
	const StgNode& n = m_scan[si];
	if (m_actorView && n.kind != 1)
		return false;
	return n.parent >= 0 && n.kind != 4;
}

void CStorageDlg::DeleteNode(int node)
{
	// 휴지통으로 삭제 (오른쪽 클릭 [삭제] · Delete 키)
	//  - 영상이면 같은 이름 커버 · 자막 · 스크린샷 폴더도 함께
	//  - DB 정보는 그대로 둠 → 영상은 "파일 없음" 으로 보이고, 휴지통에서 되살리면 별점 · 배우 · 재생 기록이 그대로 돌아옴
	//  - 스크린샷 추출 · 미리보기 만들기 중(대기 포함)인 영상이 있으면 지우지 않음
	if (!CanDelete(node))
		return;
	if (m_scanning)
	{
		MessageBox(L"스캔이 끝난 뒤에 삭제해 주세요.", L"저장 공간", MB_ICONINFORMATION);
		return;
	}
	const int si = ScanIdx(node);
	const StgNode s = m_scan[si];
	const bool dir = (s.kind == 0);
	std::vector<CString> from;
	CString extraNames;
	CollectWithSidecars(si, from, extraNames);
	int dbCount = 0;
	for (const VideoItem& v : m_lib.items)
	{
		const bool hit = dir ? CVideoLibrary::IsUnder(v.path, s.path) : (v.path.CompareNoCase(s.path) == 0);
		if (!hit)
			continue;
		++dbCount;
		if (m_isBusy && m_isBusy(v.path))
		{
			MessageBox(L"스크린샷 추출 · 미리보기 만들기 중(또는 대기 중)인 영상이 있어서 삭제하지 않았습니다.\n" + v.path, L"저장 공간", MB_ICONINFORMATION);
			return;
		}
	}
	CString msg;
	if (dir)
		msg.Format(L"%s\n\n이 폴더를 휴지통으로 옮길까요?\n(안의 파일 %d개 · %s)", static_cast<LPCWSTR>(s.path), s.files, static_cast<LPCWSTR>(FormatBytes(s.size)));
	else
		msg.Format(L"%s\n\n이 파일을 휴지통으로 옮길까요? (%s)", static_cast<LPCWSTR>(s.path), static_cast<LPCWSTR>(FormatBytes(s.size)));
	if (!extraNames.IsEmpty())
		msg += L"\n\n함께 삭제:" + extraNames;
	if (dbCount > 0)
	{
		CString t;
		t.Format(L"\n\nDB 에 등록된 영상 %d개는 정보가 그대로 남고 \"파일 없음\" 으로 보입니다.\n(휴지통에서 되살리면 그대로 돌아옵니다)", dbCount);
		msg += t;
	}
	if (MessageBox(msg, L"삭제", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
		return;
	std::wstring list;
	for (const CString& f : from)
	{
		list += f.GetString();
		list.push_back(L'\0');
	}
	list.push_back(L'\0');
	SHFILEOPSTRUCTW op = {};
	op.hwnd = GetSafeHwnd();
	op.wFunc = FO_DELETE;
	op.pFrom = list.c_str();
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING;   // 휴지통으로 (휴지통에 못 넣는 드라이브면 Windows 가 영구 삭제 경고)
	const int rc = ::SHFileOperationW(&op);
	int gone = 0;
	for (const CString& f : from)
		if (!::PathFileExistsW(f))
			++gone;
	if (gone > 0)
		m_moved = true;   // 닫은 뒤 영상 목록 새로 고침 (파일 없음 표시)
	if (rc != 0 || op.fAnyOperationsAborted || gone < static_cast<int>(from.size()))
	{
		CString e;
		e.Format(L"모두 삭제하지 못했습니다 (%d / %d개 삭제%s).", gone, static_cast<int>(from.size()), op.fAnyOperationsAborted ? L", 취소됨" : L"");
		MessageBox(e, L"저장 공간", MB_ICONWARNING);
	}
	StartScan();
}

void CStorageDlg::MoveNode(int src, int dstFolder)
{
	// 끌어다 놓기: src (폴더 또는 파일) 를 dstFolder 안으로 옮김
	//  - 영상이면 같은 이름으로 시작하는 커버 · 세로 커버(_V) · 자막 · 스크린샷 폴더도 함께 (영상 파일은 빼고)
	//  - 같은 이름이 이미 있으면 옮기지 않음, 옮기기 전에 확인, Windows 파일 옮기기(휴지통 되돌리기 가능 · 다른 드라이브면 복사 후 삭제 + 진행 창)
	//  - 옮긴 뒤 DB 영상 경로 · 미리보기 파일 이름을 바꾸고 저장 (m_onMoved) → 다시 스캔
	if (m_scanning)
	{
		MessageBox(L"스캔이 끝난 뒤에 옮겨 주세요.", L"저장 공간", MB_ICONINFORMATION);
		return;
	}
	const std::vector<StgNode>& nodes = m_tree.Nodes();
	const StgNode s = nodes[src];
	const StgNode d = nodes[dstFolder];
	const bool dir = (s.kind == 0);
	CString dstDir = d.path;
	if (dstDir.Right(1) != L"\\")
		dstDir += L"\\";
	std::vector<CString> from;
	CString extraNames;
	CollectWithSidecars(src, from, extraNames);
	for (const CString& f : from)
	{
		const CString to = dstDir + ::PathFindFileNameW(f);
		if (::PathFileExistsW(to))
		{
			MessageBox(L"옮길 폴더에 같은 이름이 이미 있어서 옮기지 않았습니다.\n" + to, L"저장 공간", MB_ICONWARNING);
			return;
		}
	}
	// DB 영상 (옮기면 경로가 바뀌는 것) · 작업 중인 영상
	int dbCount = 0;
	for (const VideoItem& v : m_lib.items)
	{
		const bool hit = dir ? CVideoLibrary::IsUnder(v.path, s.path) : (v.path.CompareNoCase(s.path) == 0);
		if (!hit)
			continue;
		++dbCount;
		if (m_isBusy && m_isBusy(v.path))
		{
			MessageBox(L"스크린샷 추출 · 미리보기 만들기 중(또는 대기 중)인 영상이 있어서 옮기지 않았습니다.\n" + v.path, L"저장 공간", MB_ICONINFORMATION);
			return;
		}
	}
	CString msg;
	msg.Format(L"%s\n\n→ %s\n\n이 %s을(를) 옮길까요?", static_cast<LPCWSTR>(s.path), static_cast<LPCWSTR>(d.path), dir ? L"폴더" : L"파일");
	if (!extraNames.IsEmpty())
		msg += L"\n\n함께 옮김:" + extraNames;
	if (dbCount > 0)
	{
		CString t;
		t.Format(L"\n\nDB 에 등록된 영상 %d개의 경로도 같이 바꿉니다.", dbCount);
		msg += t;
	}
	if (MessageBox(msg, L"옮기기", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
		return;
	std::wstring list;
	for (const CString& f : from)
	{
		list += f.GetString();
		list.push_back(L'\0');
	}
	list.push_back(L'\0');
	std::wstring toList(d.path.GetString());
	toList.push_back(L'\0');
	toList.push_back(L'\0');
	SHFILEOPSTRUCTW op = {};
	op.hwnd = GetSafeHwnd();
	op.wFunc = FO_MOVE;
	op.pFrom = list.c_str();
	op.pTo = toList.c_str();
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
	const int rc = ::SHFileOperationW(&op);
	// 실제로 옮겨진 것만 DB 에 반영 (중간에 취소 · 실패해도 옮겨진 것은 맞춤)
	int moved = 0;
	for (const CString& f : from)
	{
		const CString to = dstDir + ::PathFindFileNameW(f);
		if (::PathFileExistsW(to) && !::PathFileExistsW(f))
		{
			++moved;
			m_moved = true;
			if (m_onMoved)
				m_onMoved(f, to, ::PathIsDirectoryW(to) != FALSE);
		}
	}
	if (rc != 0 || op.fAnyOperationsAborted || moved < static_cast<int>(from.size()))
	{
		CString e;
		e.Format(L"모두 옮기지 못했습니다 (%d / %d개 옮김%s).", moved, static_cast<int>(from.size()), op.fAnyOperationsAborted ? L", 취소됨" : L"");
		MessageBox(e, L"저장 공간", MB_ICONWARNING);
	}
	StartScan();   // 트리 다시 (펼친 폴더는 그대로)
}

void CStorageDlg::ShowMenu(int node, CPoint screen)
{
	enum { M_EXPLORE = 1, M_PLAY, M_GOTO, M_TOGGLE, M_RESCAN, M_DELETE, M_TARGET = 100 };
	CMenu menu;
	menu.CreatePopupMenu();
	const StgNode* n = (node >= 0) ? &m_tree.Nodes()[node] : nullptr;
	// 배우별 보기: 다른 위치로 (배우 = 한곳으로 모으기) - 대상 = 그 배우의 위치들 + 등록 폴더마다 "등록 폴더\배우 이름" (없으면 새 폴더)
	//  위치마다 [그 배우 영상이 이미 몇 개 있는지 · 남은 공간], 다른 배우 이름의 폴더는 표시
	struct Target { CString folder; int root; };
	CMenu sub;
	std::vector<Target> targets;
	std::vector<int> vids;
	CString subLabel;
	if (m_actorView && n && (n->kind == 1 || n->kind == 5 || n->kind == 6))
	{
		CollectVideos(node, vids);
		std::vector<StgNode>& tn = m_tree.Nodes();
		int actor = node;
		while (actor >= 0 && tn[actor].kind != 5)
			actor = tn[actor].parent;
		sub.CreatePopupMenu();
		auto countThere = [&](const CString& folder)
		{
			int there = 0;
			for (int v : vids)
			{
				const int loc = (v < static_cast<int>(m_videoLoc.size()) && m_videoLoc[v] >= 0) ? m_videoLoc[v] : RootOf(v);
				if (m_scan[loc].path.CompareNoCase(folder) == 0)
					++there;
			}
			return there;
		};
		auto add = [&](const CString& folder, int root, bool other, bool isNew)
		{
			for (const Target& t : targets)
				if (t.folder.CompareNoCase(folder) == 0)
					return;
			const int there = countThere(folder);
			CString label = folder;
			if (other)
				label += L"  (다른 배우 폴더)";
			if (isNew)
				label += L"  (새 폴더)";
			CString tail;
			if (there > 0)
				tail.Format(L"\t영상 %d개 있음 · 남은 %s", there, static_cast<LPCWSTR>(FormatBytes(m_scan[root].diskFree)));
			else
				tail.Format(L"\t남은 %s", static_cast<LPCWSTR>(FormatBytes(m_scan[root].diskFree)));
			const bool all = (there == static_cast<int>(vids.size()));
			sub.AppendMenu(MF_STRING | ((all || m_scanning) ? MF_GRAYED : 0), M_TARGET + targets.size(), label + tail);
			targets.push_back({ folder, root });
		};
		CString actorName;
		if (actor >= 0)
		{
			actorName = tn[actor].name;
			for (int k : tn[actor].kids)   // 그 배우의 위치들 (자기 이름 폴더 먼저)
				if (tn[k].kind == 6 && !tn[k].otherActor)
					add(tn[k].path, RootOf(tn[k].src), false, false);
		}
		// 등록 폴더마다 "등록 폴더\배우 이름" (폴더 이름에 쓸 수 없는 글자는 _)
		CString folderName = actorName;
		for (int i = 0; i < folderName.GetLength(); ++i)
			if (wcschr(L"\\/:*?\"<>|", folderName[i]))
				folderName.SetAt(i, L'_');
		folderName.TrimRight(L". ");
		if (!folderName.IsEmpty())
			for (int r : m_scanRoots)
				if (r >= 0 && r < static_cast<int>(m_scan.size()) && m_scan[r].kind == 0)
				{
					const CString f = JoinPath(m_scan[r].path, folderName);
					add(f, r, false, !::PathIsDirectoryW(f));
				}
		if (actor >= 0)
			for (int k : tn[actor].kids)   // 다른 배우 이름 폴더 · 등록 폴더 바로 아래 (맨 뒤)
				if (tn[k].kind == 6 && tn[k].otherActor)
					add(tn[k].path, RootOf(tn[k].src), true, false);
		subLabel = (n->kind == 5) ? L"한곳으로 모으기" : (n->kind == 6 ? L"이 위치의 영상을 다른 위치로" : L"다른 위치로 옮기기");
	}
	if (n)
	{
		if (n->kind == 1)
		{
			menu.AppendMenu(MF_STRING, M_PLAY, L"재생\t더블클릭");
			menu.AppendMenu(MF_STRING | (n->item >= 0 ? 0 : MF_GRAYED), M_GOTO, L"영상 탭에서 보기 (이 창을 닫음)");
		}
		if (!n->kids.empty())
			menu.AppendMenu(MF_STRING, M_TOGGLE, n->expanded ? L"접기" : L"펼치기");
		menu.AppendMenu(MF_STRING | ((n->kind == 4 || n->kind == 5) ? MF_GRAYED : 0), M_EXPLORE, L"탐색기에서 열기");
		if (sub.GetSafeHmenu() && !targets.empty())
		{
			menu.AppendMenu(MF_SEPARATOR);
			menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(sub.GetSafeHmenu()), subLabel);
		}
		menu.AppendMenu(MF_SEPARATOR);
		if (!m_actorView || n->kind == 1)
		{
			menu.AppendMenu(MF_STRING | ((CanDelete(node) && !m_scanning) ? 0 : MF_GRAYED), M_DELETE, n->kind == 0 ? L"폴더 삭제 (휴지통)\tDel" : L"파일 삭제 (휴지통)\tDel");
			menu.AppendMenu(MF_SEPARATOR);
		}
	}
	menu.AppendMenu(MF_STRING | (m_scanning ? MF_GRAYED : 0), M_RESCAN, L"다시 스캔");
	const UINT cmd = DarkMenu::Track(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, this);
	if (!n && cmd != M_RESCAN)
		return;
	const int ti = static_cast<int>(cmd) - M_TARGET;
	if (ti >= 0 && ti < static_cast<int>(targets.size()))
	{
		MoveVideosToFolder(vids, targets[ti].folder, targets[ti].root, WhatOf(node));
		return;
	}
	switch (cmd)
	{
	case M_PLAY:
		if (m_onPlay) m_onPlay(n->path);
		break;
	case M_GOTO:
		m_gotoPath = n->path;
		StopScan();
		EndDialog(IDOK);
		break;
	case M_TOGGLE:
		m_tree.Toggle(node);
		break;
	case M_EXPLORE:
		if ((n->kind == 0 && n->parent < 0) || n->kind == 6)
			::ShellExecuteW(GetSafeHwnd(), L"open", n->path, nullptr, nullptr, SW_SHOWNORMAL);
		else
			::ShellExecuteW(GetSafeHwnd(), L"open", L"explorer.exe", L"/select,\"" + n->path + L"\"", nullptr, SW_SHOWNORMAL);
		break;
	case M_RESCAN:
		StartScan();
		break;
	case M_DELETE:
		DeleteNode(node);
		break;
	}
}
