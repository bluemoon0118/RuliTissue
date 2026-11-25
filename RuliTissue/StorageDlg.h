#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "DarkTheme.h"

class CVideoLibrary;

// ---------------------------------------------------------------------------
// 저장 공간 창 ([관리 도구 ▾] → [저장 공간...]) - TreeSize 처럼 폴더별 용량 트리
//  - 루트 = 등록 폴더 (폴더 관리), 실제 폴더를 백그라운드로 훑어 모든 파일 크기를 합침
//  - 열: 크기 · 상위 대비 % · 파일 · 폴더 · 영상 · 미등록 · 안 본 영상 · 재생 · 마지막 재생 · 별점 · 수정한 날짜
//    (영상 · 재생 기록 · 별점은 DB 정보 - 폴더는 그 안 전체 합계 / 평균)

struct StgNode
{
	CString   name, path;
	int       kind = 0;          // 0 = 폴더, 1 = 영상, 2 = 이미지, 3 = 그 밖의 파일, 4 = 찾을 수 없는 등록 폴더, 5 = 배우 (배우별 보기), 6 = 배우 아래 등록 폴더 (배우별 보기)
	int       parent = -1;
	std::vector<int> kids;
	int       depth = 0;
	ULONGLONG size = 0;
	ULONGLONG modified = 0;      // FILETIME (폴더 = 안의 가장 최근 값)
	int       files = 0, folders = 0;
	int       videos = 0, unreg = 0, unwatched = 0, plays = 0;
	CString   lastPlayed;        // "YYYY-MM-DD HH:MM" (가장 최근)
	int       ratingSum = 0, ratingCnt = 0;   // 별점 0 ~ 10 (반 별 단위)
	int       item = -1;         // 영상이면 라이브러리 항목 번호 (등록된 것만)
	bool      expanded = false;
	ULONGLONG diskFree = 0, diskTotal = 0;   // 등록 폴더(루트)만: 그 폴더가 있는 디스크의 남은 공간 / 전체 (스캔할 때 읽음)
	CString   key;               // 펼침 · 선택을 이어 줄 이름 (비어 있으면 path) - 배우별 보기는 같은 경로가 여러 배우 아래 있어서 따로
	int       src = -1;          // 배우별 보기: 스캔 결과(폴더별 트리)의 노드 번호 (영상 = 그 영상, 위치 = 그 위치 폴더)
	bool      otherActor = false; // 배우별 보기 위치 줄: 다른 배우 이름의 폴더 (이 배우 영상이 다른 배우 폴더 아래에 있음)
};

// 트리 + 열 (직접 그림, 세로 스크롤)
class CStorageTree;

// 끄는 중 커서 옆을 따라다니는 이름표 (끄는 것 아이콘 · 이름, 아래 줄 = 크기 · 놓을 폴더)
//  - 마우스는 통과 (WS_EX_TRANSPARENT · HTTRANSPARENT), 그리기는 트리가 함
class CStgDragLabel : public CWnd
{
public:
	CStorageTree* m_tree = nullptr;
protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC*) { return TRUE; }
	afx_msg LRESULT OnNcHitTest(CPoint) { return HTTRANSPARENT; }
	DECLARE_MESSAGE_MAP()
};

class CStorageTree : public CWnd
{
public:
	enum Col { C_NAME, C_SIZE, C_PCT, C_FILES, C_FOLDERS, C_VIDEOS, C_UNREG, C_UNWATCHED, C_PLAYS, C_LASTPLAY, C_RATING, C_MODIFIED, C_COUNT };

	std::function<void(int node)> m_onOpen;                    // 영상 더블클릭 / Enter
	std::function<void(int node, CPoint screen)> m_onMenu;     // 오른쪽 클릭 (node = -1 이면 빈 곳)
	std::function<void()> m_onSelChanged;
	std::function<void(int src, int dstFolder)> m_onDrop;     // 끌어다 놓기: src 를 dstFolder 폴더 안으로 옮김
	std::function<void(int node)> m_onDelete;                  // Delete 키: 휴지통으로
	bool IsDragging() const { return m_dragging; }
	void CancelDrag();
	void PaintDragLabel(CDC* dc, const CRect& rc);

	bool Create(CWnd* parent, const CRect& rc, UINT id);
	void SetNodes(std::vector<StgNode>&& nodes, const std::vector<int>& roots, bool fresh = false);   // fresh = 보기를 바꿈 (펼침 · 선택을 잇지 않음)
	void SetSortState(int col, bool asc) { m_sortCol = col; m_sortAsc = asc; }   // 다음 SetNodes 부터
	void SelectKey(const CString& key, bool expand);   // 그 줄을 고르고 (expand 면 펼쳐서) 보이게
	bool    m_actorMode = false;    // 배우별 보기 (배우 → 등록 폴더 → 영상, 끌어다 놓기 = 다른 등록 폴더로)
	CString m_emptyText = L"등록 폴더를 훑는 중입니다…";   // 줄이 없을 때 가운데 글자
	std::vector<StgNode>& Nodes() { return m_nodes; }
	int  SelectedNode() const { return (m_sel >= 0 && m_sel < static_cast<int>(m_rows.size())) ? m_rows[m_sel] : -1; }
	void Toggle(int node);

protected:
	std::vector<StgNode> m_nodes;
	std::vector<int> m_roots;
	std::vector<int> m_rows;           // 보이는 줄 (노드 번호)
	int  m_sel = -1, m_scroll = 0, m_rowH = 20, m_headH = 24, m_hot = -1;
	int  m_sortCol = C_SIZE;
	bool m_sortAsc = false;
	bool m_tracking = false;
	int  m_colW[C_COUNT] = {};
	ULONGLONG m_rootTotal = 0;         // 맨 위 줄 크기 합 (배우별 보기의 배우 줄 % 기준)
	static CString KeyOf(const StgNode& n) { return n.key.IsEmpty() ? n.path : n.key; }
	// 끌어다 놓기
	int    m_dragNode = -1;          // 누른 줄의 노드 (끌기 후보)
	CPoint m_dragStart;
	bool   m_dragging = false;
	int    m_dropRow = -1;           // 놓을 폴더 줄 (보이는 줄 번호, -1 = 놓을 수 없음)
	int    DropFolderAt(CPoint pt, int* row) const;   // 그 위치에 놓으면 들어갈 폴더 노드 (못 놓으면 -1)
	bool   IsInside(int node, int ancestor) const;
	CStgDragLabel m_dragLabel;
	void   MoveDragLabel();           // 이름표를 지금 커서 옆으로 (내용 · 크기도 다시)
	CString DragLabelLine2() const;
	CFont m_font, m_bold;

	int  S(int px) const;
	void SortKids();
	void BuildRows();
	void UpdateScroll();
	void EnsureVisible(int row);
	void LayoutCols(int width);
	int  RowAt(CPoint pt) const;
	int  ColAt(int x) const;
	CRect ExpanderRect(int row) const;
	void DrawIcon(CDC* dc, const StgNode& n, const CRect& rc);
	CString CellText(const StgNode& n, int col) const;

	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC*) { return TRUE; }
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
	afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg LRESULT OnMouseLeave(WPARAM, LPARAM);
	afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg UINT OnGetDlgCode() { return DLGC_WANTARROWS | DLGC_WANTCHARS; }
	DECLARE_MESSAGE_MAP()
};

class CStorageDlg : public CDialogEx
{
public:
	CStorageDlg(CVideoLibrary& lib, CWnd* pParent = nullptr);
	~CStorageDlg();

	// 주인 창이 채움
	std::function<void(const CString& path)> m_onPlay;           // 영상 재생 (외부 플레이어)
	std::function<bool(const CString& path)> m_isBusy;           // 스크린샷 추출 · 미리보기 만들기 중인 영상 (옮기지 않음)
	std::function<void(const CString& from, const CString& to, bool dir)> m_onMoved;   // 옮긴 뒤: DB 경로 · 미리보기 파일 이름 바꾸고 저장
	bool m_moved = false;                                         // 이 창에서 파일을 옮기거나 삭제했음 (닫은 뒤 주인 창이 목록 새로 고침)
	CString m_gotoPath;                                           // [영상 탭에서 보기] 로 닫으면 그 영상 경로 (주인 창이 고름)
	CString m_startActor;                                         // 배우별 보기로 열고 그 배우를 펼쳐 고름 (배우 상세 "저장:" 줄 더블클릭)

protected:
	virtual BOOL OnInitDialog();
	virtual void OnOK() {}
	virtual void OnCancel();
	virtual BOOL PreTranslateMessage(MSG* pMsg);

	struct VInfo { int item = -1; bool pending = false; int plays = 0; CString last; int rating = 0; };
	struct ScanResult { std::vector<StgNode> nodes; std::vector<int> roots; };
	static const UINT WM_STG_DONE = WM_APP + 0x3B1;

	CVideoLibrary& m_lib;
	CStorageTree m_tree;
	CDarkDialogTheme m_theme;
	CSize m_minSize;
	HICON m_iconSmall = nullptr;   // 창 아이콘 (파이 차트, 직접 그림)
	HICON m_iconBig = nullptr;
	std::thread m_thread;
	std::atomic<bool> m_abort{ false };
	std::atomic<int>  m_scanCount{ 0 };
	bool m_scanning = false;
	ULONGLONG m_scanStart = 0, m_scanMs = 0;
	std::vector<StgNode> m_scan;        // 마지막 스캔 결과 (폴더별 트리 - 폴더별 보기의 트리와 번호가 같음)
	std::vector<int>     m_scanRoots;
	bool m_actorView = false;           // 배우별 보기
	bool m_scatteredOnly = false;       // 배우별 보기: 2곳 이상에 흩어진 배우만
	int  m_actorCount = 0, m_scatteredCount = 0;   // 배우별 보기 아래 줄 (배우 수 · 흩어진 배우 수)

	void ShowView(bool actor);
	void BuildActorView(bool fresh);
	int  ScanIdx(int treeNode) const;   // 트리 노드 → 스캔 노드 (배우별 보기는 src, 배우 줄은 -1)
	int  RootOf(int scanIdx) const;     // 스캔 노드가 들어 있는 등록 폴더 (스캔 노드 번호)
	void CollectVideos(int treeNode, std::vector<int>& out) const;   // 배우별 보기: 그 줄 아래 영상들 (스캔 노드 번호)
	CString WhatOf(int treeNode) const;  // 확인 창에 쓸 이름 (배우 · 배우 + 위치 · 파일 이름)
	std::vector<int> m_videoLoc;        // 배우별 보기: 스캔 노드(영상) → 위치 폴더 스캔 노드 (등록 폴더 아래 배우 폴더, 없으면 등록 폴더)
	void MoveVideosToFolder(const std::vector<int>& videos, const CString& targetFolder, int targetRoot, const CString& what);   // 위치 폴더로 (위치 아래 경로 그대로)
	void ConsolidateDrop(int src, int dstLocation);

	void StartScan();
	void StopScan();
	static void ScanWorker(std::vector<CString> roots, std::map<CString, VInfo> lib, std::atomic<bool>* abort,
		std::atomic<int>* count, HWND notify);
	void UpdateStatus();
	void LayoutControls(int cx, int cy);
	void ShowMenu(int node, CPoint screen);
	void MoveNode(int src, int dstFolder);
	void DeleteNode(int node);
	void CollectWithSidecars(int src, std::vector<CString>& from, CString& extraNames) const;   // 영상이면 같은 이름 커버 · 자막 등도
	bool CanDelete(int node) const;

	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* mmi);
	afx_msg void OnRescan();
	afx_msg void OnViewFolder();
	afx_msg void OnViewActor();
	afx_msg void OnScattered();
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg void OnDestroy();
	afx_msg LRESULT OnScanDone(WPARAM, LPARAM);
	DECLARE_MESSAGE_MAP()
};
