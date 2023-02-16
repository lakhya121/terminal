#pragma once

#include "DWriteTextAnalysis.h"
#include "common.h"

namespace Microsoft::Console::Render::Atlas
{
    struct SwapChainManager
    {
        void UpdateSwapChainSettings(const RenderingPayload& p, IUnknown* device, auto&& prepareResize, auto&& prepareRecreate)
        {
            if (_targetGeneration != p.s->target.generation())
            {
                if (_swapChain)
                {
                    prepareRecreate();
                }
                _createSwapChain(p, device);
            }
            else if (_targetSize != p.s->targetSize)
            {
                _targetSize = p.s->targetSize;
                prepareResize();
                THROW_IF_FAILED(_swapChain->ResizeBuffers(0, _targetSize.x, _targetSize.y, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
            }

            // XAML's SwapChainPanel combines the worst of both worlds and applies a transform to the
            // swap chain to match the display scale and not just if it got a perspective transform, etc.
            // This if condition undoes the damage no one asked for. (Seriously though: Why?)
            if (_fontGeneration != p.s->font.generation() && !p.s->target->hwnd)
            {
                const DXGI_MATRIX_3X2_F matrix{
                    ._11 = p.d.font.dipPerPixel,
                    ._22 = p.d.font.dipPerPixel,
                };
                THROW_IF_FAILED(_swapChain->SetMatrixTransform(&matrix));
            }
        }

        wil::com_ptr<ID3D11Texture2D> GetBuffer() const
        {
            wil::com_ptr<ID3D11Texture2D> buffer;
            THROW_IF_FAILED(_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
            return buffer;
        }

        void Present(const RenderingPayload& p)
        {
            const til::rect fullRect{ 0, 0, p.s->cellCount.x, p.s->cellCount.y };

            if (!p.dirtyRect)
            {
                return;
            }

            if (p.dirtyRect != fullRect)
            {
                auto dirtyRectInPx = p.dirtyRect;
                dirtyRectInPx.left *= p.s->font->cellSize.x;
                dirtyRectInPx.top *= p.s->font->cellSize.y;
                dirtyRectInPx.right *= p.s->font->cellSize.x;
                dirtyRectInPx.bottom *= p.s->font->cellSize.y;

                RECT scrollRect{};
                POINT scrollOffset{};
                DXGI_PRESENT_PARAMETERS params{
                    .DirtyRectsCount = 1,
                    .pDirtyRects = dirtyRectInPx.as_win32_rect(),
                };

                if (p.scrollOffset)
                {
                    scrollRect = {
                        0,
                        std::max<til::CoordType>(0, p.scrollOffset),
                        p.s->cellCount.x,
                        p.s->cellCount.y + std::min<til::CoordType>(0, p.scrollOffset),
                    };
                    scrollOffset = {
                        0,
                        p.scrollOffset,
                    };

                    scrollRect.top *= p.s->font->cellSize.y;
                    scrollRect.right *= p.s->font->cellSize.x;
                    scrollRect.bottom *= p.s->font->cellSize.y;

                    scrollOffset.y *= p.s->font->cellSize.y;

                    params.pScrollRect = &scrollRect;
                    params.pScrollOffset = &scrollOffset;
                }

                THROW_IF_FAILED(_swapChain->Present1(1, 0, &params));
            }
            else
            {
                THROW_IF_FAILED(_swapChain->Present(1, 0));
            }

            _waitForPresentation = true;
        }

        void WaitUntilCanRender() noexcept
        {
            // IDXGISwapChain2::GetFrameLatencyWaitableObject returns an auto-reset event.
            // Once we've waited on the event, waiting on it again will block until the timeout elapses.
            // _waitForPresentation guards against this.
            if (_waitForPresentation)
            {
                WaitForSingleObjectEx(_frameLatencyWaitableObject.get(), 100, true);
                _waitForPresentation = false;
            }
        }

    private:
        void _createSwapChain(const RenderingPayload& p, IUnknown* device)
        {
            _swapChain.reset();
            _frameLatencyWaitableObject.reset();

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = p.s->targetSize.x;
            desc.Height = p.s->targetSize.y;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            // Sometimes up to 2 buffers are locked, for instance during screen capture or when moving the window.
            // 3 buffers seems to guarantee a stable framerate at display frequency at all times.
            desc.BufferCount = 3;
            desc.Scaling = DXGI_SCALING_NONE;
            // DXGI_SWAP_EFFECT_FLIP_DISCARD is a mode that was created at a time were display drivers
            // lacked support for Multiplane Overlays (MPO) and were copying buffers was expensive.
            // This allowed DWM to quickly draw overlays (like gamebars) on top of rendered content.
            // With faster GPU memory in general and with support for MPO in particular this isn't
            // really an advantage anymore. Instead DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL allows for a
            // more "intelligent" composition and display updates to occur like Panel Self Refresh
            // (PSR) which requires dirty rectangles (Present1 API) to work correctly.
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            // If our background is opaque we can enable "independent" flips by setting DXGI_ALPHA_MODE_IGNORE.
            // As our swap chain won't have to compose with DWM anymore it reduces the display latency dramatically.
            desc.AlphaMode = p.s->target->enableTransparentBackground ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

            wil::com_ptr<IDXGISwapChain1> swapChain0;

            if (p.s->target->hwnd)
            {
                THROW_IF_FAILED(p.dxgiFactory->CreateSwapChainForHwnd(device, p.s->target->hwnd, &desc, nullptr, nullptr, swapChain0.addressof()));
            }
            else
            {
                const wil::unique_hmodule module{ LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) };
                THROW_LAST_ERROR_IF(!module);
                const auto DCompositionCreateSurfaceHandle = GetProcAddressByFunctionDeclaration(module.get(), DCompositionCreateSurfaceHandle);
                THROW_LAST_ERROR_IF(!DCompositionCreateSurfaceHandle);

                // As per: https://docs.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-dcompositioncreatesurfacehandle
                static constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;
                THROW_IF_FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, _swapChainHandle.addressof()));
                THROW_IF_FAILED(p.dxgiFactory.query<IDXGIFactoryMedia>()->CreateSwapChainForCompositionSurfaceHandle(device, _swapChainHandle.get(), &desc, nullptr, swapChain0.addressof()));
            }

            _swapChain = swapChain0.query<IDXGISwapChain2>();
            _targetGeneration = p.s->target.generation();
            _targetSize = p.s->targetSize;
            _waitForPresentation = true;

            WaitUntilCanRender();

            if (p.swapChainChangedCallback)
            {
                try
                {
                    p.swapChainChangedCallback(_swapChainHandle.get());
                }
                CATCH_LOG()
            }
        }

        wil::com_ptr<IDXGISwapChain2> _swapChain;
        wil::unique_handle _swapChainHandle;
        wil::unique_handle _frameLatencyWaitableObject;
        til::generation_t _targetGeneration;
        til::generation_t _fontGeneration;
        u16x2 _targetSize;
        bool _waitForPresentation = false;
    };

    template<typename T = D2D1_COLOR_F>
    constexpr T colorFromU32(uint32_t rgba)
    {
        const auto r = static_cast<float>((rgba >> 0) & 0xff) / 255.0f;
        const auto g = static_cast<float>((rgba >> 8) & 0xff) / 255.0f;
        const auto b = static_cast<float>((rgba >> 16) & 0xff) / 255.0f;
        const auto a = static_cast<float>((rgba >> 24) & 0xff) / 255.0f;
        return { r, g, b, a };
    }

    inline f32r getGlyphRunBlackBox(const DWRITE_GLYPH_RUN& glyphRun, float baselineX, float baselineY)
    {
        DWRITE_FONT_METRICS fontMetrics;
        glyphRun.fontFace->GetMetrics(&fontMetrics);

        std::unique_ptr<DWRITE_GLYPH_METRICS[]> glyphRunMetricsHeap;
        std::array<DWRITE_GLYPH_METRICS, 8> glyphRunMetricsStack;
        DWRITE_GLYPH_METRICS* glyphRunMetrics = glyphRunMetricsStack.data();

        if (glyphRun.glyphCount > glyphRunMetricsStack.size())
        {
            glyphRunMetricsHeap = std::make_unique_for_overwrite<DWRITE_GLYPH_METRICS[]>(glyphRun.glyphCount);
            glyphRunMetrics = glyphRunMetricsHeap.get();
        }

        glyphRun.fontFace->GetDesignGlyphMetrics(glyphRun.glyphIndices, glyphRun.glyphCount, glyphRunMetrics, false);

        float const fontScale = glyphRun.fontEmSize / fontMetrics.designUnitsPerEm;
        f32r accumulatedBounds{
            FLT_MAX,
            FLT_MAX,
            FLT_MIN,
            FLT_MIN,
        };

        for (uint32_t i = 0; i < glyphRun.glyphCount; ++i)
        {
            const auto& glyphMetrics = glyphRunMetrics[i];
            const auto glyphAdvance = glyphRun.glyphAdvances ? glyphRun.glyphAdvances[i] : glyphMetrics.advanceWidth * fontScale;

            const auto left = static_cast<float>(glyphMetrics.leftSideBearing) * fontScale;
            const auto top = static_cast<float>(glyphMetrics.topSideBearing - glyphMetrics.verticalOriginY) * fontScale;
            const auto right = static_cast<float>(gsl::narrow_cast<INT32>(glyphMetrics.advanceWidth) - glyphMetrics.rightSideBearing) * fontScale;
            const auto bottom = static_cast<float>(gsl::narrow_cast<INT32>(glyphMetrics.advanceHeight) - glyphMetrics.bottomSideBearing - glyphMetrics.verticalOriginY) * fontScale;

            if (left < right && top < bottom)
            {
                auto glyphX = baselineX;
                auto glyphY = baselineY;
                if (glyphRun.glyphOffsets)
                {
                    glyphX += glyphRun.glyphOffsets[i].advanceOffset;
                    glyphY -= glyphRun.glyphOffsets[i].ascenderOffset;
                }

                accumulatedBounds.left = std::min(accumulatedBounds.left, left + glyphX);
                accumulatedBounds.top = std::min(accumulatedBounds.top, top + glyphY);
                accumulatedBounds.right = std::max(accumulatedBounds.right, right + glyphX);
                accumulatedBounds.bottom = std::max(accumulatedBounds.bottom, bottom + glyphY);
            }

            baselineX += glyphAdvance;
        }

        return accumulatedBounds;
    }

    inline bool _drawGlyphRun(IDWriteFactory4* dwriteFactory4, ID2D1DeviceContext* d2dRenderTarget, ID2D1DeviceContext4* d2dRenderTarget4, D2D_POINT_2F baselineOrigin, const DWRITE_GLYPH_RUN* glyphRun, ID2D1SolidColorBrush* foregroundBrush) noexcept
    {
        static constexpr auto measuringMode = DWRITE_MEASURING_MODE_NATURAL;
        static constexpr auto formats =
            DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
            DWRITE_GLYPH_IMAGE_FORMATS_CFF |
            DWRITE_GLYPH_IMAGE_FORMATS_COLR |
            DWRITE_GLYPH_IMAGE_FORMATS_SVG |
            DWRITE_GLYPH_IMAGE_FORMATS_PNG |
            DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
            DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
            DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;

        wil::com_ptr<IDWriteColorGlyphRunEnumerator1> enumerator;

        // If ID2D1DeviceContext4 isn't supported, we'll exit early below.
        auto hr = DWRITE_E_NOCOLOR;

        if (d2dRenderTarget4)
        {
            D2D_MATRIX_3X2_F transform;
            d2dRenderTarget4->GetTransform(&transform);
            float dpiX, dpiY;
            d2dRenderTarget4->GetDpi(&dpiX, &dpiY);
            transform = transform * D2D1::Matrix3x2F::Scale(dpiX, dpiY);

            // Support for ID2D1DeviceContext4 implies support for IDWriteFactory4.
            // ID2D1DeviceContext4 is required for drawing below.
            hr = dwriteFactory4->TranslateColorGlyphRun(baselineOrigin, glyphRun, nullptr, formats, measuringMode, nullptr, 0, &enumerator);
        }

        if (hr == DWRITE_E_NOCOLOR)
        {
            d2dRenderTarget->DrawGlyphRun(baselineOrigin, glyphRun, foregroundBrush, measuringMode);
            return false;
        }

        THROW_IF_FAILED(hr);

        const auto previousAntialiasingMode = d2dRenderTarget4->GetTextAntialiasMode();
        d2dRenderTarget4->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto cleanup = wil::scope_exit([&]() {
            d2dRenderTarget4->SetTextAntialiasMode(previousAntialiasingMode);
        });

        wil::com_ptr<ID2D1SolidColorBrush> solidBrush;

        for (;;)
        {
            BOOL hasRun;
            THROW_IF_FAILED(enumerator->MoveNext(&hasRun));
            if (!hasRun)
            {
                break;
            }

            const DWRITE_COLOR_GLYPH_RUN1* colorGlyphRun;
            THROW_IF_FAILED(enumerator->GetCurrentRun(&colorGlyphRun));

            ID2D1Brush* runBrush;
            if (colorGlyphRun->paletteIndex == /*DWRITE_NO_PALETTE_INDEX*/ 0xffff)
            {
                runBrush = foregroundBrush;
            }
            else
            {
                if (!solidBrush)
                {
                    THROW_IF_FAILED(d2dRenderTarget4->CreateSolidColorBrush(colorGlyphRun->runColor, &solidBrush));
                }
                else
                {
                    solidBrush->SetColor(colorGlyphRun->runColor);
                }
                runBrush = solidBrush.get();
            }

            switch (colorGlyphRun->glyphImageFormat)
            {
            case DWRITE_GLYPH_IMAGE_FORMATS_NONE:
                break;
            case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
            case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
            case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
            case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
                d2dRenderTarget4->DrawColorBitmapGlyphRun(colorGlyphRun->glyphImageFormat, baselineOrigin, &colorGlyphRun->glyphRun, colorGlyphRun->measuringMode, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
                break;
            case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
                d2dRenderTarget4->DrawSvgGlyphRun(baselineOrigin, &colorGlyphRun->glyphRun, runBrush, nullptr, 0, colorGlyphRun->measuringMode);
                break;
            default:
                d2dRenderTarget4->DrawGlyphRun(baselineOrigin, &colorGlyphRun->glyphRun, colorGlyphRun->glyphRunDescription, runBrush, colorGlyphRun->measuringMode);
                break;
            }
        }

        return true;
    }

}
