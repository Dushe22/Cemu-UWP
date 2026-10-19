#pragma once

#include "wxgui/canvas/IRenderCanvas.h"

#include <wx/window.h>

class D3D12Canvas : public IRenderCanvas, public wxWindow
{
public:
	D3D12Canvas(wxWindow* parent, const wxSize& size, bool is_main_window);
	~D3D12Canvas();

private:
	void OnPaint(wxPaintEvent& event);
	void OnResize(wxSizeEvent& event);
};
