// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../../renderer/inc/IRenderEngine.hpp"
#include "common.h"
#include "DWriteTextAnalysis.h"

namespace Microsoft::Console::Render::Atlas
{
    struct TextAnalysisSinkResult;

    class AtlasEngine final : public IRenderEngine
    {
    public:
        explicit AtlasEngine();

        AtlasEngine(const AtlasEngine&) = delete;
        AtlasEngine& operator=(const AtlasEngine&) = delete;

        // IRenderEngine
        [[nodiscard]] HRESULT StartPaint() noexcept override;
        [[nodiscard]] HRESULT EndPaint() noexcept override;
        [[nodiscard]] bool RequiresContinuousRedraw() noexcept override;
        void WaitUntilCanRender() noexcept override;
        void _recreateBackend();
        [[nodiscard]] HRESULT Present() noexcept override;
        [[nodiscard]] HRESULT PrepareForTeardown(_Out_ bool* pForcePaint) noexcept override;
        [[nodiscard]] HRESULT ScrollFrame() noexcept override;
        [[nodiscard]] HRESULT Invalidate(const til::rect* psrRegion) noexcept override;
        [[nodiscard]] HRESULT InvalidateCursor(const til::rect* psrRegion) noexcept override;
        [[nodiscard]] HRESULT InvalidateSystem(const til::rect* prcDirtyClient) noexcept override;
        [[nodiscard]] HRESULT InvalidateSelection(const std::vector<til::rect>& rectangles) noexcept override;
        [[nodiscard]] HRESULT InvalidateScroll(const til::point* pcoordDelta) noexcept override;
        [[nodiscard]] HRESULT InvalidateAll() noexcept override;
        [[nodiscard]] HRESULT InvalidateFlush(_In_ const bool circled, _Out_ bool* const pForcePaint) noexcept override;
        [[nodiscard]] HRESULT InvalidateTitle(std::wstring_view proposedTitle) noexcept override;
        [[nodiscard]] HRESULT NotifyNewText(const std::wstring_view newText) noexcept override;
        [[nodiscard]] HRESULT PrepareRenderInfo(const RenderFrameInfo& info) noexcept override;
        [[nodiscard]] HRESULT ResetLineTransform() noexcept override;
        [[nodiscard]] HRESULT PrepareLineTransform(LineRendition lineRendition, til::CoordType targetRow, til::CoordType viewportLeft) noexcept override;
        [[nodiscard]] HRESULT PaintBackground() noexcept override;
        [[nodiscard]] HRESULT PaintBufferLine(std::span<const Cluster> clusters, til::point coord, bool fTrimLeft, bool lineWrapped) noexcept override;
        [[nodiscard]] HRESULT PaintBufferGridLines(GridLineSet lines, COLORREF color, size_t cchLine, til::point coordTarget) noexcept override;
        [[nodiscard]] HRESULT PaintSelection(const til::rect& rect) noexcept override;
        [[nodiscard]] HRESULT PaintCursor(const CursorOptions& options) noexcept override;
        [[nodiscard]] HRESULT UpdateDrawingBrushes(const TextAttribute& textAttributes, const RenderSettings& renderSettings, gsl::not_null<IRenderData*> pData, bool usingSoftFont, bool isSettingDefaultBrushes) noexcept override;
        [[nodiscard]] HRESULT UpdateFont(const FontInfoDesired& FontInfoDesired, _Out_ FontInfo& FontInfo) noexcept override;
        [[nodiscard]] HRESULT UpdateSoftFont(std::span<const uint16_t> bitPattern, til::size cellSize, size_t centeringHint) noexcept override;
        [[nodiscard]] HRESULT UpdateDpi(int iDpi) noexcept override;
        [[nodiscard]] HRESULT UpdateViewport(const til::inclusive_rect& srNewViewport) noexcept override;
        [[nodiscard]] HRESULT GetProposedFont(const FontInfoDesired& FontInfoDesired, _Out_ FontInfo& FontInfo, int iDpi) noexcept override;
        [[nodiscard]] HRESULT GetDirtyArea(std::span<const til::rect>& area) noexcept override;
        [[nodiscard]] HRESULT GetFontSize(_Out_ til::size* pFontSize) noexcept override;
        [[nodiscard]] HRESULT IsGlyphWideByFont(std::wstring_view glyph, _Out_ bool* pResult) noexcept override;
        [[nodiscard]] HRESULT UpdateTitle(std::wstring_view newTitle) noexcept override;

        // DxRenderer - getter
        HRESULT Enable() noexcept override;
        [[nodiscard]] std::wstring_view GetPixelShaderPath() noexcept override;
        [[nodiscard]] bool GetRetroTerminalEffect() const noexcept override;
        [[nodiscard]] float GetScaling() const noexcept override;
        [[nodiscard]] Types::Viewport GetViewportInCharacters(const Types::Viewport& viewInPixels) const noexcept override;
        [[nodiscard]] Types::Viewport GetViewportInPixels(const Types::Viewport& viewInCharacters) const noexcept override;
        // DxRenderer - setter
        void SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE antialiasingMode) noexcept override;
        void SetCallback(std::function<void(HANDLE)> pfn) noexcept override;
        void EnableTransparentBackground(const bool isTransparent) noexcept override;
        void SetForceFullRepaintRendering(bool enable) noexcept override;
        [[nodiscard]] HRESULT SetHwnd(HWND hwnd) noexcept override;
        void SetPixelShaderPath(std::wstring_view value) noexcept override;
        void SetRetroTerminalEffect(bool enable) noexcept override;
        void SetSelectionBackground(COLORREF color, float alpha = 0.5f) noexcept override;
        void SetSoftwareRendering(bool enable) noexcept override;
        void SetWarningCallback(std::function<void(HRESULT)> pfn) noexcept override;
        [[nodiscard]] HRESULT SetWindowSize(til::size pixels) noexcept override;
        [[nodiscard]] HRESULT UpdateFont(const FontInfoDesired& pfiFontInfoDesired, FontInfo& fiFontInfo, const std::unordered_map<std::wstring_view, uint32_t>& features, const std::unordered_map<std::wstring_view, float>& axes) noexcept override;
        void UpdateHyperlinkHoveredId(uint16_t hoveredId) noexcept override;

    private:
        // These flags are shared with shader_ps.hlsl.
        // If you change this be sure to copy it over to shader_ps.hlsl.
        //
        // clang-format off
        enum class CellFlags : u32
        {
            None            = 0x00000000,

            BorderLeft      = 0x00000020,
            BorderTop       = 0x00000040,
            BorderRight     = 0x00000080,
            BorderBottom    = 0x00000100,
            Underline       = 0x00000200,
            UnderlineDotted = 0x00000400,
            UnderlineDouble = 0x00000800,
            Strikethrough   = 0x00001000,
        };
        // clang-format on
        ATLAS_FLAG_OPS(CellFlags, u32)

        struct AtlasKeyAttributes
        {
            bool bold = false;
            bool italic = false;

            ATLAS_POD_OPS(AtlasKeyAttributes)
        };
        
        // AtlasEngine.cpp
        [[nodiscard]] HRESULT _handleException(const wil::ResultException& exception) noexcept;
        __declspec(noinline) void _recreateFontDependentResources();
        __declspec(noinline) void _recreateCellCountDependentResources();
        const Buffer<DWRITE_FONT_AXIS_VALUE>& _getTextFormatAxis(bool bold, bool italic) const noexcept;
        void _flushBufferLine();

        // AtlasEngine.api.cpp
        void _resolveTransparencySettings() noexcept;
        void _updateFont(const wchar_t* faceName, const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo, const std::unordered_map<std::wstring_view, uint32_t>& features, const std::unordered_map<std::wstring_view, float>& axes);
        void _resolveFontMetrics(const wchar_t* faceName, const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo, FontSettings* fontMetrics = nullptr) const;

        // AtlasEngine.r.cpp

        static constexpr bool debugProportionalText = false;
        static constexpr bool debugForceD2DMode = true;
        static constexpr bool debugGlyphGenerationPerformance = false;
        static constexpr bool debugTextParsingPerformance = false || debugGlyphGenerationPerformance;
        static constexpr bool debugGeneralPerformance = false || debugTextParsingPerformance;

        static constexpr u16 u16min = 0x0000;
        static constexpr u16 u16max = 0xffff;
        static constexpr i16 i16min = -0x8000;
        static constexpr i16 i16max = 0x7fff;
        static constexpr u16r invalidatedAreaNone = { u16max, u16max, u16min, u16min };
        static constexpr u16x2 invalidatedRowsNone{ u16max, u16min };
        static constexpr u16x2 invalidatedRowsAll{ u16min, u16max };

        std::unique_ptr<IBackend> _b;
        RenderingPayload _p;

        struct ApiState
        {
            til::generational<Settings> s = Settings::invalidated();

            // This structure is loosely sorted in chunks from "very often accessed together"
            // to seldom accessed and/or usually not together.
            
            bool invalidatedTitle = false;
            // These two are redundant with TargetSettings/MiscellaneousSettings, but that's because _resolveTransparencySettings()
            // turns the given settings into potentially different actual settings (which are then written into the Settings).
            bool enableTransparentBackground = false;
            u8 antialiasingMode = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;

            std::vector<wchar_t> bufferLine;
            std::vector<u16> bufferLineColumn;
            Buffer<u32> colorsForeground;

            std::vector<TextAnalysisSinkResult> analysisResults;
            Buffer<u16> clusterMap;
            Buffer<DWRITE_SHAPING_TEXT_PROPERTIES> textProps;
            Buffer<u16> glyphIndices;
            Buffer<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps;
            Buffer<f32> glyphAdvances;
            Buffer<DWRITE_GLYPH_OFFSET> glyphOffsets;

            // UpdateDrawingBrushes()
            u32 backgroundOpaqueMixin = 0xff000000;
            u32x2 currentColor;
            AtlasKeyAttributes attributes{};
            u16x2 lastPaintBufferLineCoord;
            CellFlags flags = CellFlags::None;
            // UpdateHyperlinkHoveredId()
            u16 hyperlinkHoveredId = 0;
            bool bufferLineWasHyperlinked = false;

            // dirtyRect is a computed value based on invalidatedRows.
            til::rect dirtyRect;
            // These "invalidation" fields are reset in EndPaint()
            u16r invalidatedCursorArea = invalidatedAreaNone;
            u16x2 invalidatedRows = invalidatedRowsNone; // x is treated as "top" and y as "bottom"
            i16 scrollOffset = 0;
        } _api;

#undef ATLAS_POD_OPS
#undef ATLAS_FLAG_OPS
    };
}

namespace Microsoft::Console::Render
{
    using AtlasEngine = Atlas::AtlasEngine;
}
