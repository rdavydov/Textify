#include "stdafx.h"
#include "resource.h"

#include "TextDlg.h"
#include "InjectAllowSetForegroundWindow.h"
#include "WebAppLaunch.h"

namespace
{
	void GetAccessibleInfoFromPoint(POINT pt, CWindow& window, CString& outString, CRect& outRc, std::vector<int>& indexes);
	BOOL UnadjustWindowRectEx(LPRECT prc, DWORD dwStyle, BOOL fMenu, DWORD dwExStyle);
	BOOL WndAdjustWindowRect(CWindow window, LPRECT prc);
	BOOL WndUnadjustWindowRect(CWindow window, LPRECT prc);
	CSize GetEditControlTextSize(CEdit window, LPCTSTR lpszString, int nMaxWidth = INT_MAX);
	CSize TextSizeToEditClientSize(CEdit editWnd, CSize textSize);
	CSize EditClientSizeToTextSize(CEdit editWnd, CSize editClientSize);
	BOOL SetClipboardText(const WCHAR* text);
}

BOOL CTextDlg::OnInitDialog(CWindow wndFocus, LPARAM lInitParam)
{
	CPoint& ptEvent = *reinterpret_cast<CPoint*>(lInitParam);

	CWindow wndAcc;
	CString strText;
	CRect rcAccObject;
	GetAccessibleInfoFromPoint(ptEvent, wndAcc, strText, rcAccObject, m_editIndexes);

	// Check whether the target window is another TextDlg.
	if(wndAcc)
	{
		CWindow wndAccRoot{ ::GetAncestor(wndAcc, GA_ROOT) };
		if(wndAccRoot)
		{
			WCHAR szBuffer[32];
			if(::GetClassName(wndAccRoot, szBuffer, _countof(szBuffer)) &&
				wcscmp(szBuffer, L"TextifyEditDlg") == 0)
			{
				wndAccRoot.SendMessage(WM_CLOSE);
				EndDialog(0);
				return FALSE;
			}
		}
	}

	if(!::SetForegroundWindow(m_hWnd))
	{
		InjectAllowSetForegroundWindow(GetCurrentProcessId(), 3000);

		if(!::SetForegroundWindow(m_hWnd))
		{
			//EndDialog(0);
			//return FALSE;
		}
	}

	InitWebAppButtons();

	CEdit editWnd = GetDlgItem(IDC_EDIT);
	editWnd.SetLimitText(0);
	editWnd.SetWindowText(strText);

	AdjustWindowLocationAndSize(ptEvent, rcAccObject, strText);

	m_lastSelStart = 0;
	m_lastSelEnd = strText.GetLength();

	if(m_autoCopySelection)
	{
		SetClipboardText(strText);
	}

	m_wndEdit.SubclassWindow(editWnd);

	return TRUE;
}

HBRUSH CTextDlg::OnCtlColorStatic(CDCHandle dc, CStatic wndStatic)
{
	if(wndStatic.GetDlgCtrlID() == IDC_EDIT)
	{
		return GetSysColorBrush(COLOR_WINDOW);
	}

	SetMsgHandled(FALSE);
	return NULL;
}

void CTextDlg::OnActivate(UINT nState, BOOL bMinimized, CWindow wndOther)
{
	if(nState == WA_INACTIVE && !m_showingModalBrowserHost)
		EndDialog(0);
}

void CTextDlg::OnCancel(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	EndDialog(nID);
}

void CTextDlg::OnCommand(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	int buttonIndex = nID - IDC_WEB_BUTTON_1;
	if(buttonIndex >= 0 && buttonIndex < static_cast<int>(m_webButtonInfos.size()))
	{
		CString selectedText;
		m_wndEdit.GetWindowText(selectedText);

		int start, end;
		m_wndEdit.GetSel(start, end);
		if(start < end)
		{
			selectedText = selectedText.Mid(start, end - start);
		}

		m_showingModalBrowserHost = true;
		ShowWindow(SW_HIDE);

		const auto& buttonInfo = m_webButtonInfos[buttonIndex];
		CString errorMessage;
		bool succeeded = WebAppLaunch(buttonInfo.url, buttonInfo.params, selectedText,
			buttonInfo.externalBrowser, buttonInfo.width, buttonInfo.height, &errorMessage);

		if(!succeeded)
		{
			MessageBox(errorMessage, L"Textify: could not open web page", MB_ICONERROR);
		}

		EndDialog(0);
	}
}

LRESULT CTextDlg::OnNcHitTest(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = ::DefWindowProc(m_hWnd, uMsg, wParam, lParam);
	if(result == HTCLIENT)
		result = HTCAPTION;

	return result;
}

void CTextDlg::OnKeyDown(UINT vk, UINT nRepCnt, UINT nFlags)
{
	if(vk == VK_TAB)
	{
		int start, end;
		m_wndEdit.GetSel(start, end);
		int length = m_wndEdit.SendMessage(WM_GETTEXTLENGTH);

		int newStart, newEnd;

		const int newlineSize = sizeof("\r\n") - 1;

		if(start == 0 && end == length) // all text is selected
		{
			newStart = 0;
			newEnd = m_editIndexes.empty() ? length : (m_editIndexes.front() - newlineSize);
		}
		else
		{
			newStart = 0;
			newEnd = length;

			for(size_t i = 0; i < m_editIndexes.size(); i++)
			{
				int from = (i == 0) ? 0 : m_editIndexes[i - 1];
				int to = m_editIndexes[i] - newlineSize;

				if(from == start && to == end)
				{
					newStart = m_editIndexes[i];
					newEnd = (i + 1 < m_editIndexes.size()) ? (m_editIndexes[i + 1] - newlineSize) : length;

					break;
				}
			}
		}

		m_wndEdit.SetSel(newStart, newEnd, TRUE);
	}
	else
	{
		SetMsgHandled(FALSE);
	}
}

void CTextDlg::OnKeyUp(UINT vk, UINT nRepCnt, UINT nFlags)
{
	OnSelectionMaybeChanged();
	SetMsgHandled(FALSE);
}

void CTextDlg::OnLButtonUp(UINT nFlags, CPoint point)
{
	OnSelectionMaybeChanged();
	SetMsgHandled(FALSE);
}

void CTextDlg::OnChar(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	if(nChar == 1) // Ctrl+A
	{
		m_wndEdit.SetSelAll(TRUE);
	}
	else
	{
		SetMsgHandled(FALSE);
	}
}

void CTextDlg::InitWebAppButtons()
{
	CButton firstButton = GetDlgItem(IDC_WEB_BUTTON_1);

	int numberOfButtons = static_cast<int>(m_webButtonInfos.size());
	if(numberOfButtons == 0)
	{
		firstButton.ShowWindow(SW_HIDE);
		return;
	}

	m_webButtonIcons.resize(numberOfButtons);

	HICON icon = (HICON)::LoadImage(nullptr, m_webButtonInfos[0].iconPath.m_strPath.GetString(),
		IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
	if(icon)
	{
		firstButton.SetIcon(icon);
		m_webButtonIcons[0] = icon;
	}

	CRect firstButtonRect;
	firstButton.GetWindowRect(firstButtonRect);

	HFONT firstButtonFont = firstButton.GetFont();

	for(int i = 1; i < numberOfButtons; i++)
	{
		CString buttonText;
		if(i + 1 < 10)
			buttonText.Format(L"&%d", i + 1);
		else if(i + 1 == 10)
			buttonText.Format(L"1&0");
		else
			buttonText.Format(L"%d", i + 1);

		CButton newButton;
		newButton.Create(m_hWnd, firstButtonRect, buttonText,
			BS_ICON | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, IDC_WEB_BUTTON_1 + i);
		newButton.SetFont(firstButtonFont);

		icon = (HICON)::LoadImage(nullptr, m_webButtonInfos[i].iconPath.m_strPath.GetString(),
			IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
		if(icon)
		{
			newButton.SetIcon(icon);
			m_webButtonIcons[i] = icon;
		}
	}
}

void CTextDlg::AdjustWindowLocationAndSize(CPoint ptEvent, CRect rcAccObject, CString strText)
{
	CEdit editWnd = GetDlgItem(IDC_EDIT);

	// A dirty, partial workaround.
	// http://stackoverflow.com/questions/35673347
	strText.Replace(L"!", L"! ");
	strText.Replace(L"|", L"| ");
	strText.Replace(L"?", L"? ");
	strText.Replace(L"-", L"- ");
	strText.Replace(L"}", L"} ");
	strText.Replace(L"{", L" {");
	strText.Replace(L"[", L" [");
	strText.Replace(L"(", L" (");
	strText.Replace(L"+", L" +");
	strText.Replace(L"%", L" %");
	strText.Replace(L"$", L" $");
	strText.Replace(L"\\", L" \\");

	CSize defTextSize = GetEditControlTextSize(editWnd, L"(no text could be retrieved)");
	CSize defTextSizeClient = TextSizeToEditClientSize(editWnd, defTextSize);

	int nMaxClientWidth = defTextSizeClient.cx > rcAccObject.Width() ? defTextSizeClient.cx : rcAccObject.Width();

	CSize textSize = GetEditControlTextSize(editWnd, strText, nMaxClientWidth);
	CSize textSizeClient = TextSizeToEditClientSize(editWnd, textSize);

	if(textSizeClient.cx < rcAccObject.Width())
	{
		// Perhaps it will look better if we won't shrink the control,
		// as it will fit perfectly above the control.
		// Let's see if the shrinking is small.

		int nMinClientWidth = 200;
		CDC hdc = ::GetDC(NULL);
		if(hdc)
		{
			nMinClientWidth = MulDiv(nMinClientWidth, hdc.GetDeviceCaps(LOGPIXELSX), 96);
			hdc.DeleteDC();
		}

		if(rcAccObject.Width() <= nMinClientWidth || textSizeClient.cx * 1.5 >= rcAccObject.Width())
		{
			int delta = rcAccObject.Width() - textSizeClient.cx;
			textSizeClient.cx = rcAccObject.Width();
			textSize.cx += delta;

			// Recalculate the height, which might be smaller now.
			//CSize newTextSize = GetEditControlTextSize(editWnd, strText, textSize.cx);
			//textSize.cy = newTextSize.cy;
		}
	}

	CRect rcClient{ rcAccObject.TopLeft(), textSizeClient };
	if(rcAccObject.IsRectEmpty() || !rcClient.PtInRect(ptEvent))
	{
		CPoint ptWindowLocation{ ptEvent };
		ptWindowLocation.Offset(-rcClient.Width() / 2, -rcClient.Height() / 2);
		rcClient.MoveToXY(ptWindowLocation);
	}

	int numberOfWebAppButtons = static_cast<int>(m_webButtonInfos.size());
	CSize webAppButtonSize;
	if(numberOfWebAppButtons > 0)
	{
		CButton webAppButton = GetDlgItem(IDC_WEB_BUTTON_1);
		CRect webAppButtonRect;
		webAppButton.GetWindowRect(webAppButtonRect);
		webAppButtonSize = webAppButtonRect.Size();

		if(rcClient.Width() < webAppButtonSize.cx * numberOfWebAppButtons)
			rcClient.right = rcClient.left + webAppButtonSize.cx * numberOfWebAppButtons;

		rcClient.bottom += webAppButtonSize.cy;
	}

	CRect rcWindow{ rcClient };
	WndAdjustWindowRect(m_hWnd, &rcWindow);

	HMONITOR hMonitor = MonitorFromPoint(ptEvent, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorinfo = { sizeof(MONITORINFO) };
	if(GetMonitorInfo(hMonitor, &monitorinfo))
	{
		CRect rcMonitor{ monitorinfo.rcMonitor };
		CRect rcWindowPrev{ rcWindow };

		if(rcWindow.Width() > rcMonitor.Width() ||
			rcWindow.Height() > rcMonitor.Height())
		{
			if(rcWindow.Height() > rcMonitor.Height())
			{
				rcWindow.top = 0;
				rcWindow.bottom = rcMonitor.Height();
			}

			editWnd.ShowScrollBar(SB_VERT);
			rcWindow.right += GetSystemMetrics(SM_CXVSCROLL);
			if(rcWindow.Width() > rcMonitor.Width())
			{
				rcWindow.left = 0;
				rcWindow.right = rcMonitor.Width();
			}
		}

		if(rcWindow.left < rcMonitor.left)
		{
			rcWindow.MoveToX(rcMonitor.left);
		}
		else if(rcWindow.right > rcMonitor.right)
		{
			rcWindow.MoveToX(rcMonitor.right - rcWindow.Width());
		}

		if(rcWindow.top < rcMonitor.top)
		{
			rcWindow.MoveToY(rcMonitor.top);
		}
		else if(rcWindow.bottom > rcMonitor.bottom)
		{
			rcWindow.MoveToY(rcMonitor.bottom - rcWindow.Height());
		}

		if(rcWindowPrev != rcWindow)
		{
			rcClient = rcWindow;
			WndUnadjustWindowRect(m_hWnd, &rcClient);
		}
	}

	if(numberOfWebAppButtons > 0)
	{
		if(rcClient.bottom - webAppButtonSize.cy > rcClient.top)
		{
			rcClient.bottom -= webAppButtonSize.cy;

			for(int i = 0; i < numberOfWebAppButtons; i++)
			{
				CButton webAppButton = GetDlgItem(IDC_WEB_BUTTON_1 + i);
				webAppButton.SetWindowPos(NULL, webAppButtonSize.cx * i, rcClient.Height(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
		}
		else
		{
			// Remove WebApp buttons.
			for(int i = 0; i < numberOfWebAppButtons; i++)
			{
				CButton webAppButton = GetDlgItem(IDC_WEB_BUTTON_1 + i);
				webAppButton.ShowWindow(SW_HIDE);
			}

			m_webButtonInfos.clear();
			numberOfWebAppButtons = 0;
		}
	}

	SetWindowPos(NULL, &rcWindow, SWP_NOZORDER | SWP_NOACTIVATE);
	editWnd.SetWindowPos(NULL, 0, 0, rcClient.Width(), rcClient.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void CTextDlg::OnSelectionMaybeChanged()
{
	if(m_autoCopySelection)
	{
		int start, end;
		m_wndEdit.GetSel(start, end);
		if(start < end && (start != m_lastSelStart || end != m_lastSelEnd))
		{
			CString str;
			m_wndEdit.GetWindowText(str);

			SetClipboardText(str.Mid(start, end - start));

			m_lastSelStart = start;
			m_lastSelEnd = end;
		}
	}
}

namespace
{
	void GetAccessibleInfoFromPoint(POINT pt, CWindow& window, CString& outString, CRect& outRc, std::vector<int>& indexes)
	{
		outString = L"(no text could be retrieved)";
		outRc = CRect{ pt, CSize{ 0, 0 } };

		HRESULT hr;

		CComPtr<IAccessible> pAcc;
		CComVariant vtChild;
		hr = AccessibleObjectFromPoint(pt, &pAcc, &vtChild);
		if(FAILED(hr))
		{
			return;
		}

		hr = WindowFromAccessibleObject(pAcc, &window.m_hWnd);
		if(FAILED(hr))
		{
			return;
		}

		CString string;

		CComBSTR bsName;
		hr = pAcc->get_accName(vtChild, &bsName);
		if(SUCCEEDED(hr) && bsName.Length() > 0)
		{
			string = bsName;
		}

		CComBSTR bsValue;
		hr = pAcc->get_accValue(vtChild, &bsValue);
		if(SUCCEEDED(hr) && bsValue.Length() > 0 &&
			bsValue != bsName)
		{
			if(!string.IsEmpty())
			{
				string += L"\r\n";
				indexes.push_back(string.GetLength());
			}

			string += bsValue;
		}

		CComVariant vtRole;
		hr = pAcc->get_accRole(CComVariant(CHILDID_SELF), &vtRole);
		if(FAILED(hr) || vtRole.lVal != ROLE_SYSTEM_TITLEBAR) // ignore description for the system title bar
		{
			CComBSTR bsDescription;
			hr = pAcc->get_accDescription(vtChild, &bsDescription);
			if(SUCCEEDED(hr) && bsDescription.Length() > 0 &&
				bsDescription != bsName && bsDescription != bsValue)
			{
				if(!string.IsEmpty())
				{
					string += L"\r\n";
					indexes.push_back(string.GetLength());
				}

				string += bsDescription;
			}
		}

		if(!string.IsEmpty())
		{
			// Normalize newlines.
			string.Replace(L"\r\n", L"\n");
			string.Replace(L"\r", L"\n");
			string.Replace(L"\n", L"\r\n");

			string.TrimRight();

			outString = string;
		}

		long pxLeft, pyTop, pcxWidth, pcyHeight;
		hr = pAcc->accLocation(&pxLeft, &pyTop, &pcxWidth, &pcyHeight, vtChild);
		if(SUCCEEDED(hr))
		{
			outRc = CRect{ CPoint{ pxLeft, pyTop }, CSize{ pcxWidth, pcyHeight } };
		}
	}

	BOOL UnadjustWindowRectEx(LPRECT prc, DWORD dwStyle, BOOL fMenu, DWORD dwExStyle)
	{
		RECT rc;
		SetRectEmpty(&rc);

		BOOL fRc = AdjustWindowRectEx(&rc, dwStyle, fMenu, dwExStyle);
		if(fRc)
		{
			prc->left -= rc.left;
			prc->top -= rc.top;
			prc->right -= rc.right;
			prc->bottom -= rc.bottom;
		}

		return fRc;
	}

	BOOL WndAdjustWindowRect(CWindow window, LPRECT prc)
	{
		DWORD dwStyle = window.GetStyle();
		DWORD dwExStyle = window.GetExStyle();
		BOOL bMenu = (!(dwStyle & WS_CHILD) && (window.GetMenu() != NULL));

		return AdjustWindowRectEx(prc, dwStyle, bMenu, dwExStyle);
	}

	BOOL WndUnadjustWindowRect(CWindow window, LPRECT prc)
	{
		DWORD dwStyle = window.GetStyle();
		DWORD dwExStyle = window.GetExStyle();
		BOOL bMenu = (!(dwStyle & WS_CHILD) && (window.GetMenu() != NULL));

		return UnadjustWindowRectEx(prc, dwStyle, bMenu, dwExStyle);
	}

	CSize GetEditControlTextSize(CEdit window, LPCTSTR lpszString, int nMaxWidth /*= INT_MAX*/)
	{
		CFontHandle pEdtFont = window.GetFont();
		if(!pEdtFont)
			return CSize{};

		CClientDC oDC{ window };
		CFontHandle pOldFont = oDC.SelectFont(pEdtFont);

		CRect rc{ 0, 0, nMaxWidth, 0 };
		oDC.DrawTextEx((LPTSTR)lpszString, -1, &rc, DT_CALCRECT | DT_EDITCONTROL | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | DT_TABSTOP);

		oDC.SelectFont(pOldFont);

		return rc.Size();
	}

	CSize TextSizeToEditClientSize(CEdit editWnd, CSize textSize)
	{
		CRect rc{ CPoint{ 0, 0 }, textSize };
		WndAdjustWindowRect(editWnd, &rc);

		UINT nLeftMargin, nRightMargin;
		editWnd.GetMargins(nLeftMargin, nRightMargin);

		CSize editClientSize;

		// Experiments show that this works kinda ok.
		editClientSize.cx = rc.Width() +
			nLeftMargin + nRightMargin +
			2 * GetSystemMetrics(SM_CXBORDER) + 2 * GetSystemMetrics(SM_CXDLGFRAME);

		editClientSize.cy = rc.Height() +
			2 * GetSystemMetrics(SM_CYBORDER)/* +
			2 * GetSystemMetrics(SM_CYDLGFRAME)*/;

		return editClientSize;
	}

	CSize EditClientSizeToTextSize(CEdit editWnd, CSize editClientSize)
	{
		UINT nLeftMargin, nRightMargin;
		editWnd.GetMargins(nLeftMargin, nRightMargin);

		editClientSize.cx -=
			nLeftMargin + nRightMargin +
			2 * GetSystemMetrics(SM_CXBORDER) + 2 * GetSystemMetrics(SM_CXDLGFRAME);

		editClientSize.cy -=
			2 * GetSystemMetrics(SM_CYBORDER)/* +
			2 * GetSystemMetrics(SM_CYDLGFRAME)*/;

		CRect rc{ CPoint{ 0, 0 }, editClientSize };
		WndUnadjustWindowRect(editWnd, &rc);

		return rc.Size();
	}

	BOOL SetClipboardText(const WCHAR* text)
	{
		if(!OpenClipboard(NULL))
			return FALSE;

		BOOL bSucceeded = FALSE;

		size_t size = sizeof(WCHAR) * (wcslen(text) + 1);
		HANDLE handle = GlobalAlloc(GHND, size);
		if(handle)
		{
			WCHAR* clipboardText = (WCHAR*)GlobalLock(handle);
			if(clipboardText)
			{
				memcpy(clipboardText, text, size);
				GlobalUnlock(handle);
				bSucceeded = EmptyClipboard() &&
					SetClipboardData(CF_UNICODETEXT, handle);
			}

			if(!bSucceeded)
				GlobalFree(handle);
		}

		CloseClipboard();
		return bSucceeded;
	}
}
