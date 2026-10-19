#include "wxgui/canvas/D3D12Canvas.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "interface/WindowSystem.h"
#include "wxgui/helpers/wxHelpers.h"

#include <wx/msgdlg.h>
#include <wx/dcclient.h>

D3D12Canvas::D3D12Canvas(wxWindow* parent, const wxSize& size, bool is_main_window)
	: IRenderCanvas(is_main_window), wxWindow(parent, wxID_ANY, wxDefaultPosition, size, wxNO_FULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
{
	Bind(wxEVT_PAINT, &D3D12Canvas::OnPaint, this);
	Bind(wxEVT_SIZE, &D3D12Canvas::OnResize, this);
#if __WXMSW__
	MSWDisableComposited();
#endif

	auto& canvas = is_main_window ? WindowSystem::GetWindowInfo().canvas_main : WindowSystem::GetWindowInfo().canvas_pad;
	canvas = initHandleContextFromWxWidgetsWindow(this);

	cemu_assert(g_d3d12_available);
	try
	{
		if (is_main_window)
			g_renderer = std::make_unique<D3D12Renderer>();
		auto d3d12Renderer = D3D12Renderer::GetInstance();
		d3d12Renderer->InitializeSurface({ size.x, size.y }, is_main_window);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Error when initializing Direct3D 12 renderer: {}", ex.what());
		auto msg = formatWxString(_("Error when initializing Direct3D 12 renderer:\n{}"), ex.what());
		wxMessageDialog dialog(this, msg, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		dialog.ShowModal();
		_exit(0);
	}

	wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);
}

D3D12Canvas::~D3D12Canvas()
{
	Unbind(wxEVT_PAINT, &D3D12Canvas::OnPaint, this);
	Unbind(wxEVT_SIZE, &D3D12Canvas::OnResize, this);

	if (!m_is_main_window)
	{
		if (auto* d3d12Renderer = D3D12Renderer::GetInstance())
			d3d12Renderer->StopUsingPadAndWait();
	}
}

void D3D12Canvas::OnPaint(wxPaintEvent& event)
{
	(void)event;
	wxPaintDC dc(this);
}

void D3D12Canvas::OnResize(wxSizeEvent& event)
{
	(void)event;
	const wxSize size = GetSize();
	if (size.GetWidth() == 0 || size.GetHeight() == 0)
		return;

	const wxRect refreshRect(size);
	RefreshRect(refreshRect, false);

	if (auto* d3d12Renderer = D3D12Renderer::GetInstance())
		d3d12Renderer->ResizeSurface({ size.x, size.y }, m_is_main_window);
}
