#include "pch.h"
#include "BackendD2D.h"

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4127)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

BackendD2D::BackendD2D(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
}

void BackendD2D::Render(const RenderingPayload& p)
{
    _swapChainManager.UpdateSwapChainSettings(
        p,
        _device.get(),
        [this]() {
            _d2dRenderTarget.reset();
            _deviceContext->ClearState();
        },
        [this]() {
            _d2dRenderTarget.reset();
            _deviceContext->ClearState();
            _deviceContext->Flush();
        });

    if (_generation != p.s.generation())
    {
        if (!_d2dRenderTarget)
        {
            {
                const auto surface = _swapChainManager.GetBuffer().query<IDXGISurface>();

                const D2D1_RENDER_TARGET_PROPERTIES props{
                    .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
                    .dpiX = static_cast<float>(p.s->font->dpi),
                    .dpiY = static_cast<float>(p.s->font->dpi),
                };
                wil::com_ptr<ID2D1RenderTarget> renderTarget;
                THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
                _d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
                _d2dRenderTarget4 = renderTarget.query<ID2D1DeviceContext4>();
                _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->misc->antialiasingMode));
            }
            {
                static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
                THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
                _brushColor = 0xffffffff;
            }
        }

        if (_fontGeneration != p.s->font.generation())
        {
            const auto dpi = p.s->font->dpi;
            _d2dRenderTarget->SetDpi(dpi, dpi);
        }

        if (_fontGeneration != p.s->font.generation() || _cellCount != p.s->cellCount)
        {
            const D2D1_BITMAP_PROPERTIES props{
                .pixelFormat = { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
                .dpiX = static_cast<float>(p.s->font->dpi),
                .dpiY = static_cast<float>(p.s->font->dpi),
            };
            const D2D1_SIZE_U size{ p.s->cellCount.x, p.s->cellCount.y };
            const D2D1_MATRIX_3X2_F transform {
                ._11 = static_cast<float>(p.s->font->cellSize.x),
                ._22 = static_cast<float>(p.s->font->cellSize.y),
            };
            
            {
                THROW_IF_FAILED(_d2dRenderTarget->CreateBitmap(size, nullptr, 0, &props, _d2dBackgroundBitmap.put()));
                THROW_IF_FAILED(_d2dRenderTarget->CreateBitmapBrush(_d2dBackgroundBitmap.get(), _d2dBackgroundBrush.put()));
                _d2dBackgroundBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                _d2dBackgroundBrush->SetTransform(&transform);
            }
            {
                THROW_IF_FAILED(_d2dRenderTarget->CreateBitmap(size, nullptr, 0, &props, _d2dForegroundBitmap.put()));
                THROW_IF_FAILED(_d2dRenderTarget->CreateBitmapBrush(_d2dForegroundBitmap.get(), _d2dForegroundBrush.put()));
                _d2dForegroundBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                _d2dForegroundBrush->SetTransform(&transform);
            }
        }

        _generation = p.s.generation();
        _fontGeneration = p.s->font.generation();
        _cellCount = p.s->cellCount;
    }

    _d2dRenderTarget->BeginDraw();
    {
        _d2dRenderTarget->Clear();

        // Let's say the terminal is 120x30 cells and 1200x600 pixels large respectively.
        // This draws the background color by upscaling a 120x30 pixel bitmap to fill the 1200x600 pixel render target.
        {
            const D2D1_RECT_F rect{ 0, 0, p.s->cellCount.x * p.d.font.cellSizeDIP.x, p.s->cellCount.y * p.d.font.cellSizeDIP.y };
            _d2dBackgroundBitmap->CopyFromMemory(nullptr, p.backgroundBitmap.data(), p.s->cellCount.x * 4);
            _d2dRenderTarget->FillRectangle(&rect, _d2dBackgroundBrush.get());
        }

        {
            const D2D1_RECT_F rect{ 0, 0, p.s->cellCount.x * p.d.font.cellSizeDIP.x, p.s->cellCount.y * p.d.font.cellSizeDIP.y };
            _d2dForegroundBitmap->CopyFromMemory(nullptr, p.foregroundBitmap.data(), p.s->cellCount.x * 4);
        }

        size_t y = 0;
        for (const auto& row : p.rows)
        {
            for (const auto& m : row.mappings)
            {
                const DWRITE_GLYPH_RUN glyphRun{
                    .fontFace = m.fontFace.get(),
                    .fontEmSize = m.fontEmSize,
                    .glyphCount = m.glyphsTo - m.glyphsFrom,
                    .glyphIndices = &row.glyphIndices[m.glyphsFrom],
                    .glyphAdvances = &row.glyphAdvances[m.glyphsFrom],
                    .glyphOffsets = &row.glyphOffsets[m.glyphsFrom],
                };
                const D2D1_POINT_2F baseline{
                    .y = p.d.font.cellSizeDIP.y * y + p.s->font->baselineInDIP,
                };
                _drawGlyphRun(p.dwriteFactory4.get(), _d2dRenderTarget.get(), _d2dRenderTarget4.get(), baseline, &glyphRun, _d2dForegroundBrush.get());
            }

            y++;
        }
    }
    THROW_IF_FAILED(_d2dRenderTarget->EndDraw());

    _swapChainManager.Present(p);
}

bool BackendD2D::RequiresContinuousRedraw() noexcept
{
    return false;
}

void BackendD2D::WaitUntilCanRender() noexcept
{
    _swapChainManager.WaitUntilCanRender();
}

ID2D1Brush* BackendD2D::_brushWithColor(u32 color)
{
    if (_brushColor != color)
    {
        const auto d2dColor = colorFromU32(color);
        THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&d2dColor, nullptr, _brush.put()));
        _brushColor = color;
    }
    return _brush.get();
}

void BackendD2D::_d2dDrawLine(const RenderingPayload& p, u16r rect, u16 pos, u16 width, u32 color, ID2D1StrokeStyle* strokeStyle)
{
    const auto w = static_cast<float>(width) * p.d.font.dipPerPixel;
    const auto y1 = static_cast<float>(rect.top) * p.d.font.cellSizeDIP.y + static_cast<float>(pos) * p.d.font.dipPerPixel + w * 0.5f;
    const auto x1 = static_cast<float>(rect.left) * p.d.font.cellSizeDIP.x;
    const auto x2 = static_cast<float>(rect.right) * p.d.font.cellSizeDIP.x;
    const auto brush = _brushWithColor(color);
    _d2dRenderTarget->DrawLine({ x1, y1 }, { x2, y1 }, brush, w, strokeStyle);
}

void BackendD2D::_d2dFillRectangle(const RenderingPayload& p, u16r rect, u32 color)
{
    const D2D1_RECT_F r{
        .left = static_cast<float>(rect.left) * p.d.font.cellSizeDIP.x,
        .top = static_cast<float>(rect.top) * p.d.font.cellSizeDIP.y,
        .right = static_cast<float>(rect.right) * p.d.font.cellSizeDIP.x,
        .bottom = static_cast<float>(rect.bottom) * p.d.font.cellSizeDIP.y,
    };
    const auto brush = _brushWithColor(color);
    _d2dRenderTarget->FillRectangle(r, brush);
}

void BackendD2D::_d2dCellFlagRendererCursor(const RenderingPayload& p, u16r rect, u32 color)
{
    //_drawCursor(p, _d2dRenderTarget.get(), rect, _brushWithColor(p.s->cursor->cursorColor), false);
}

void BackendD2D::_d2dCellFlagRendererSelected(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dFillRectangle(p, rect, p.s->misc->selectionColor);
}

void BackendD2D::_d2dCellFlagRendererUnderline(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dDrawLine(p, rect, p.s->font->underlinePos, p.s->font->underlineWidth, color, nullptr);
}

void BackendD2D::_d2dCellFlagRendererUnderlineDotted(const RenderingPayload& p, u16r rect, u32 color)
{
    if (!_dottedStrokeStyle)
    {
        static constexpr D2D1_STROKE_STYLE_PROPERTIES props{ .dashStyle = D2D1_DASH_STYLE_CUSTOM };
        static constexpr FLOAT dashes[2]{ 1, 2 };
        THROW_IF_FAILED(p.d2dFactory->CreateStrokeStyle(&props, &dashes[0], 2, _dottedStrokeStyle.addressof()));
    }

    _d2dDrawLine(p, rect, p.s->font->underlinePos, p.s->font->underlineWidth, color, _dottedStrokeStyle.get());
}

void BackendD2D::_d2dCellFlagRendererUnderlineDouble(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dDrawLine(p, rect, p.s->font->doubleUnderlinePos.x, p.s->font->thinLineWidth, color, nullptr);
    _d2dDrawLine(p, rect, p.s->font->doubleUnderlinePos.y, p.s->font->thinLineWidth, color, nullptr);
}

void BackendD2D::_d2dCellFlagRendererStrikethrough(const RenderingPayload& p, u16r rect, u32 color)
{
    _d2dDrawLine(p, rect, p.s->font->strikethroughPos, p.s->font->strikethroughWidth, color, nullptr);
}
