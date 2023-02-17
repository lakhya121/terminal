// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AtlasEngine.h"

#include "Backend.h"
#include "../../interactivity/win32/CustomWindowMessages.h"

// #### NOTE ####
// This file should only contain methods that are only accessed by the caller of Present() (the "Renderer" class).
// Basically this file poses the "synchronization" point between the concurrently running
// general IRenderEngine API (like the Invalidate*() methods) and the Present() method
// and thus may access both _r and _api.

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4189)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

#pragma warning(suppress : 26455) // Default constructor may not throw. Declare it 'noexcept' (f.6).
AtlasEngine::AtlasEngine()
{
#ifdef NDEBUG
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, _p.d2dFactory.addressof()));
#else
    static constexpr D2D1_FACTORY_OPTIONS options{ D2D1_DEBUG_LEVEL_INFORMATION };
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, _p.d2dFactory.addressof()));
#endif

    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(_p.dwriteFactory), reinterpret_cast<::IUnknown**>(_p.dwriteFactory.addressof())));
    _p.dwriteFactory4 = _p.dwriteFactory.try_query<IDWriteFactory4>();
    THROW_IF_FAILED(_p.dwriteFactory->GetSystemFontFallback(_p.systemFontFallback.addressof()));
    {
        wil::com_ptr<IDWriteTextAnalyzer> textAnalyzer;
        THROW_IF_FAILED(_p.dwriteFactory->CreateTextAnalyzer(textAnalyzer.addressof()));
        _p.textAnalyzer = textAnalyzer.query<IDWriteTextAnalyzer1>();
    }
}

#pragma region IRenderEngine

// StartPaint() is called while the console buffer lock is being held.
// --> Put as little in here as possible.
[[nodiscard]] HRESULT AtlasEngine::StartPaint() noexcept
try
{
    if (const auto hwnd = _api.s->target->hwnd)
    {
        RECT rect;
        LOG_IF_WIN32_BOOL_FALSE(GetClientRect(hwnd, &rect));
        std::ignore = SetWindowSize({ rect.right - rect.left, rect.bottom - rect.top });

        if (_api.invalidatedTitle)
        {
            LOG_IF_WIN32_BOOL_FALSE(PostMessageW(hwnd, CM_UPDATE_TITLE, 0, 0));
            _api.invalidatedTitle = false;
        }
    }

    if (_p.s != _api.s)
    {
        const auto targetChanged = _p.s->target != _api.s->target;
        const auto fontChanged = _p.s->font != _api.s->font;
        const auto cellCountChanged = _p.s->cellCount != _api.s->cellCount;

        _p.s = _api.s;

        if (targetChanged)
        {
            _b.reset();
        }
        if (fontChanged)
        {
            _recreateFontDependentResources();
        }
        if (cellCountChanged)
        {
            _recreateCellCountDependentResources();
        }

        _api.invalidatedRows = invalidatedRowsAll;
    }

    if constexpr (debugGlyphGenerationPerformance)
    {
    }
    if constexpr (debugTextParsingPerformance)
    {
        _api.invalidatedRows = invalidatedRowsAll;
        _api.scrollOffset = 0;
    }

    // Clamp invalidation rects into valid value ranges.
    {
        _api.invalidatedCursorArea.left = std::min(_api.invalidatedCursorArea.left, _p.s->cellCount.x);
        _api.invalidatedCursorArea.top = std::min(_api.invalidatedCursorArea.top, _p.s->cellCount.y);
        _api.invalidatedCursorArea.right = clamp(_api.invalidatedCursorArea.right, _api.invalidatedCursorArea.left, _p.s->cellCount.x);
        _api.invalidatedCursorArea.bottom = clamp(_api.invalidatedCursorArea.bottom, _api.invalidatedCursorArea.top, _p.s->cellCount.y);
    }
    {
        _api.invalidatedRows.x = std::min(_api.invalidatedRows.x, _p.s->cellCount.y);
        _api.invalidatedRows.y = clamp(_api.invalidatedRows.y, _api.invalidatedRows.x, _p.s->cellCount.y);
    }
    {
        const auto limit = gsl::narrow_cast<i16>(_p.s->cellCount.y & 0x7fff);
        _api.scrollOffset = gsl::narrow_cast<i16>(clamp<int>(_api.scrollOffset, -limit, limit));
    }

    // Scroll the buffer by the given offset and mark the newly uncovered rows as "invalid".
    if (_api.scrollOffset != 0)
    {
        const auto nothingInvalid = _api.invalidatedRows.x == _api.invalidatedRows.y;
        const auto offset = _api.scrollOffset;

        if (_api.scrollOffset < 0)
        {
            // Scroll up (for instance when new text is being written at the end of the buffer).
            const u16 endRow = _p.s->cellCount.y + _api.scrollOffset;
            _api.invalidatedRows.x = nothingInvalid ? endRow : std::min<u16>(_api.invalidatedRows.x, endRow);
            _api.invalidatedRows.y = _p.s->cellCount.y;

            // scrollOffset/offset = -1
            // +----------+    +----------+
            // |          |    | xxxxxxxxx|         + dst  < beg
            // | xxxxxxxxx| -> |xxxxxxx   |  + src  |      < beg - offset
            // |xxxxxxx   |    |          |  |      v
            // +----------+    +----------+  v             < end
            {
                const auto beg = _p.rows.begin();
                const auto end = _p.rows.end();
                std::move(beg - offset, end, beg);
            }
        }
        else
        {
            // Scroll down.
            _api.invalidatedRows.x = 0;
            _api.invalidatedRows.y = nothingInvalid ? _api.scrollOffset : std::max<u16>(_api.invalidatedRows.y, _api.scrollOffset);

            // scrollOffset/offset = 1
            // +----------+    +----------+
            // | xxxxxxxxx|    |          |  + src         < beg
            // |xxxxxxx   | -> | xxxxxxxxx|  |      ^
            // |          |    |xxxxxxx   |  v      |      < end - offset
            // +----------+    +----------+         + dst  < end
            {
                const auto beg = _p.rows.begin();
                const auto end = _p.rows.end();
                std::move_backward(beg, end - offset, end);
            }
        }
    }

    for (auto y = _api.invalidatedRows.x; y < _api.invalidatedRows.y; ++y)
    {
        _p.rows[y].clear();
    }

    _api.dirtyRect = til::rect{ 0, 0, _p.s->cellCount.x, _p.s->cellCount.y }; //til::rect{ 0, _api.invalidatedRows.x, _p.s->cellCount.x, _api.invalidatedRows.y };
    _p.dirtyRect = _api.dirtyRect;
    _p.scrollOffset = _api.scrollOffset;

    return S_OK;
}
catch (const wil::ResultException& exception)
{
    return _handleException(exception);
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::EndPaint() noexcept
try
{
    _flushBufferLine();

    _api.invalidatedCursorArea = invalidatedAreaNone;
    _api.invalidatedRows = invalidatedRowsNone;
    _api.scrollOffset = 0;
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::PrepareForTeardown(_Out_ bool* const pForcePaint) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pForcePaint);
    *pForcePaint = false;
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::ScrollFrame() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PrepareRenderInfo(const RenderFrameInfo& info) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::ResetLineTransform() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PrepareLineTransform(const LineRendition lineRendition, const til::CoordType targetRow, const til::CoordType viewportLeft) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PaintBackground() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PaintBufferLine(std::span<const Cluster> clusters, til::point coord, const bool fTrimLeft, const bool lineWrapped) noexcept
try
{
    const auto y = gsl::narrow_cast<u16>(clamp<int>(coord.y, 0, _p.s->cellCount.y));

    if (_api.lastPaintBufferLineCoord.y != y)
    {
        _flushBufferLine();
    }

    // _api.bufferLineColumn contains 1 more item than _api.bufferLine, as it represents the
    // past-the-end index. It'll get appended again later once we built our new _api.bufferLine.
    if (!_api.bufferLineColumn.empty())
    {
        _api.bufferLineColumn.pop_back();
    }

    const auto x = gsl::narrow_cast<u16>(clamp<int>(coord.x, 0, _p.s->cellCount.x));

    // Due to the current IRenderEngine interface (that wasn't refactored yet) we need to assemble
    // the current buffer line first as the remaining function operates on whole lines of text.
    {
        auto column = x;
        for (const auto& cluster : clusters)
        {
            for (const auto& ch : cluster.GetText())
            {
                _api.bufferLine.emplace_back(ch);
                _api.bufferLineColumn.emplace_back(column);
            }

            column += gsl::narrow_cast<u16>(cluster.GetColumns());
        }

        _api.bufferLineColumn.emplace_back(column);

        std::fill(_api.colorsForeground.begin() + x, _api.colorsForeground.begin() + column, _api.currentColor.x);
        std::fill_n(_p.backgroundBitmap.begin() + (y * _p.s->cellCount.x + x), column - x, _api.currentColor.y);
        std::fill_n(_p.foregroundBitmap.begin() + (y * _p.s->cellCount.x + x), column - x, _api.currentColor.x);
    }

    _api.lastPaintBufferLineCoord = { x, y };
    _api.bufferLineWasHyperlinked = false;

    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::PaintBufferGridLines(const GridLineSet lines, const COLORREF color, const size_t cchLine, const til::point coordTarget) noexcept
try
{
    if (!_api.bufferLineWasHyperlinked && lines.test(GridLines::Underline) && WI_IsFlagClear(_api.flags, CellFlags::Underline))
    {
        _api.bufferLineWasHyperlinked = true;

        WI_UpdateFlagsInMask(_api.flags, CellFlags::Underline | CellFlags::UnderlineDotted | CellFlags::UnderlineDouble, CellFlags::Underline);

        //const BufferLineMetadata metadata{ _api.currentColor, _api.flags };
        //const auto y = _api.lastPaintBufferLineCoord.y;
        //const auto x = _api.lastPaintBufferLineCoord.x;
        //std::fill_n(_getBufferLineMetadata(x, y), _r.metadata.size() - x, metadata);
    }
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::PaintSelection(const til::rect& rect) noexcept
try
{
    // Unfortunately there's no step after Renderer::_PaintBufferOutput that
    // would inform us that it's done with the last AtlasEngine::PaintBufferLine.
    // As such we got to call _flushBufferLine() here just to be sure.
    _flushBufferLine();

    const u16r u16rect{
        rect.narrow_left<u16>(),
        rect.narrow_top<u16>(),
        rect.narrow_right<u16>(),
        rect.narrow_bottom<u16>(),
    };

    const auto row = gsl::narrow_cast<u16>(clamp<til::CoordType>(rect.top, 0, _p.s->cellCount.y));
    const auto from = gsl::narrow_cast<u16>(clamp<til::CoordType>(rect.left, 0, _p.s->cellCount.x - 1));
    const auto to = gsl::narrow_cast<u16>(clamp<til::CoordType>(rect.right, from, _p.s->cellCount.x));

    _p.rows[row].selectionFrom = from;
    _p.rows[row].selectionTo = to;
    _p.dirtyRect |= rect;
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::PaintCursor(const CursorOptions& options) noexcept
try
{
    // Unfortunately there's no step after Renderer::_PaintBufferOutput that
    // would inform us that it's done with the last AtlasEngine::PaintBufferLine.
    // As such we got to call _flushBufferLine() here just to be sure.
    _flushBufferLine();

    {
        const CursorSettings cachedOptions{
            .cursorColor = gsl::narrow_cast<u32>(options.fUseColor ? options.cursorColor | 0xff000000 : INVALID_COLOR),
            .cursorType = gsl::narrow_cast<u16>(options.cursorType),
            .heightPercentage = gsl::narrow_cast<u8>(options.ulCursorHeightPercent),
        };
        if (*_p.s->cursor != cachedOptions)
        {
            *_p.s.write()->cursor.write() = cachedOptions;
        }
    }

    // Clear the previous cursor
    if (const auto r = _api.invalidatedCursorArea; r.non_empty())
    {
        _p.cursorRect = {};
        _p.dirtyRect |= til::rect{ r.left, r.top, r.right, r.bottom };
    }

    if (options.isOn)
    {
        const auto point = options.coordCursor;
        // TODO: options.coordCursor can contain invalid out of bounds coordinates when
        // the window is being resized and the cursor is on the last line of the viewport.
        const auto x = gsl::narrow_cast<uint16_t>(clamp(point.x, 0, _p.s->cellCount.x - 1));
        const auto y = gsl::narrow_cast<uint16_t>(clamp(point.y, 0, _p.s->cellCount.y - 1));
        const auto cursorWidth = 1 + (options.fIsDoubleWidth & (options.cursorType != CursorType::VerticalBar));
        const auto right = gsl::narrow_cast<uint16_t>(clamp(x + cursorWidth, 0, _p.s->cellCount.x - 0));
        const auto bottom = gsl::narrow_cast<uint16_t>(y + 1);
        _p.cursorRect = { x, y, right, bottom };
        _p.dirtyRect |= til::rect{ x, y, right, bottom };
    }

    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::UpdateDrawingBrushes(const TextAttribute& textAttributes, const RenderSettings& renderSettings, const gsl::not_null<IRenderData*> /*pData*/, const bool usingSoftFont, const bool isSettingDefaultBrushes) noexcept
try
{
    auto [fg, bg] = renderSettings.GetAttributeColorsWithAlpha(textAttributes);
    fg |= 0xff000000;
    bg |= _api.backgroundOpaqueMixin;

    if (!isSettingDefaultBrushes)
    {
        const auto hyperlinkId = textAttributes.GetHyperlinkId();

        auto flags = CellFlags::None;
        WI_SetFlagIf(flags, CellFlags::BorderLeft, textAttributes.IsLeftVerticalDisplayed());
        WI_SetFlagIf(flags, CellFlags::BorderTop, textAttributes.IsTopHorizontalDisplayed());
        WI_SetFlagIf(flags, CellFlags::BorderRight, textAttributes.IsRightVerticalDisplayed());
        WI_SetFlagIf(flags, CellFlags::BorderBottom, textAttributes.IsBottomHorizontalDisplayed());
        WI_SetFlagIf(flags, CellFlags::Underline, textAttributes.IsUnderlined());
        WI_SetFlagIf(flags, CellFlags::UnderlineDotted, hyperlinkId != 0);
        WI_SetFlagIf(flags, CellFlags::UnderlineDouble, textAttributes.IsDoublyUnderlined());
        WI_SetFlagIf(flags, CellFlags::Strikethrough, textAttributes.IsCrossedOut());

        if (_api.hyperlinkHoveredId && _api.hyperlinkHoveredId == hyperlinkId)
        {
            WI_SetFlag(flags, CellFlags::Underline);
            WI_ClearAllFlags(flags, CellFlags::UnderlineDotted | CellFlags::UnderlineDouble);
        }

        const u32x2 newColors{ gsl::narrow_cast<u32>(fg), gsl::narrow_cast<u32>(bg) };
        const AtlasKeyAttributes attributes{
            .bold = textAttributes.IsIntense() && renderSettings.GetRenderMode(RenderSettings::Mode::IntenseIsBold),
            .italic = textAttributes.IsItalic()
        };

        if (_api.attributes != attributes)
        {
            _flushBufferLine();
        }

        _api.currentColor = newColors;
        _api.attributes = attributes;
        _api.flags = flags;
    }
    else if (textAttributes.BackgroundIsDefault() && bg != _p.s->misc->backgroundColor)
    {
        _p.s.write()->misc.write()->backgroundColor = bg;
        _api.s.write()->misc.write()->backgroundColor = bg;
    }

    return S_OK;
}
CATCH_RETURN()

#pragma endregion

[[nodiscard]] HRESULT AtlasEngine::_handleException(const wil::ResultException& exception) noexcept
{
    const auto hr = exception.GetErrorCode();
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET)
    {
        _p.dxgiFactory.reset();
        _b.reset();
        return E_PENDING; // Indicate a retry to the renderer
    }

    if (_p.warningCallback)
    {
        try
        {
            _p.warningCallback(hr);
        }
        CATCH_LOG()
    }

    return hr;
}

void AtlasEngine::_recreateFontDependentResources()
{
    if (!_p.s->font->fontAxisValues.empty())
    {
        // See AtlasEngine::UpdateFont.
        // It hardcodes indices 0/1/2 in fontAxisValues to the weight/italic/slant axes.
        // If they're -1.0f they haven't been set by the user and must be filled by us.
        // When we call SetFontAxisValues() we basically override (disable) DirectWrite's internal font axes,
        // and if either of the 3 aren't set we'd make it impossible for the user to see bold/italic text.
        const auto& standardAxes = _p.s->font->fontAxisValues;
        auto fontAxisValues = _p.s->font->fontAxisValues;

        for (auto italic = 0; italic < 2; ++italic)
        {
            for (auto bold = 0; bold < 2; ++bold)
            {
                const auto fontWeight = bold ? DWRITE_FONT_WEIGHT_BOLD : static_cast<DWRITE_FONT_WEIGHT>(_p.s->font->fontWeight);

                // The wght axis defaults to the font weight.
                fontAxisValues[0].value = bold || standardAxes[0].value == -1.0f ? static_cast<float>(fontWeight) : standardAxes[0].value;
                // The ital axis defaults to 1 if this is italic and 0 otherwise.
                fontAxisValues[1].value = italic ? 1.0f : (standardAxes[1].value == -1.0f ? 0.0f : standardAxes[1].value);
                // The slnt axis defaults to -12 if this is italic and 0 otherwise.
                fontAxisValues[2].value = italic ? -12.0f : (standardAxes[2].value == -1.0f ? 0.0f : standardAxes[2].value);
                _p.d.font.textFormatAxes[italic][bold] = { fontAxisValues.data(), fontAxisValues.size() };
            }
        }
    }
    
    _p.d.font.dipPerPixel = static_cast<float>(USER_DEFAULT_SCREEN_DPI) / static_cast<float>(_p.s->font->dpi);
    _p.d.font.pixelPerDIP = static_cast<float>(_p.s->font->dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    _p.d.font.cellSizeDIP.x = static_cast<float>(_p.s->font->cellSize.x) * _p.d.font.dipPerPixel;
    _p.d.font.cellSizeDIP.y = static_cast<float>(_p.s->font->cellSize.y) * _p.d.font.dipPerPixel;
}

void AtlasEngine::_recreateCellCountDependentResources()
{
    // Let's guess that every cell consists of a surrogate pair.
    const auto projectedTextSize = static_cast<size_t>(_p.s->cellCount.x) * 2;
    // IDWriteTextAnalyzer::GetGlyphs says:
    //   The recommended estimate for the per-glyph output buffers is (3 * textLength / 2 + 16).
    const auto projectedGlyphSize = 3 * projectedTextSize / 2 + 16;

    _api.bufferLine = std::vector<wchar_t>{};
    _api.bufferLine.reserve(projectedTextSize);
    _api.bufferLineColumn.reserve(projectedTextSize + 1);
    _api.colorsForeground = Buffer<u32>(_p.s->cellCount.x);

    _api.analysisResults = std::vector<TextAnalysisSinkResult>{};
    _api.clusterMap = Buffer<u16>{ projectedTextSize };
    _api.textProps = Buffer<DWRITE_SHAPING_TEXT_PROPERTIES>{ projectedTextSize };
    _api.glyphIndices = Buffer<u16>{ projectedGlyphSize };
    _api.glyphProps = Buffer<DWRITE_SHAPING_GLYPH_PROPERTIES>{ projectedGlyphSize };
    _api.glyphAdvances = Buffer<f32>{ projectedGlyphSize };
    _api.glyphOffsets = Buffer<DWRITE_GLYPH_OFFSET>{ projectedGlyphSize };

    _p.rows = std::vector<ShapedRow>(_p.s->cellCount.y);
    _p.backgroundBitmap = std::vector<u32>(static_cast<size_t>(_p.s->cellCount.x) * _p.s->cellCount.y);
    _p.foregroundBitmap = std::vector<u32>(static_cast<size_t>(_p.s->cellCount.x) * _p.s->cellCount.y);
}

const Buffer<DWRITE_FONT_AXIS_VALUE>& AtlasEngine::_getTextFormatAxis(bool bold, bool italic) const noexcept
{
    return _p.d.font.textFormatAxes[italic][bold];
}

void AtlasEngine::_flushBufferLine()
{
    if (_api.bufferLine.empty())
    {
        return;
    }

    const auto cleanup = wil::scope_exit([this]() noexcept {
        _api.bufferLine.clear();
        _api.bufferLineColumn.clear();
    });

    // This would seriously blow us up otherwise.
    Expects(_api.bufferLineColumn.size() == _api.bufferLine.size() + 1);

    auto& row = _p.rows[_api.lastPaintBufferLineCoord.y];
    const auto& textFormatAxis = _getTextFormatAxis(_api.attributes.bold, _api.attributes.italic);

    TextAnalysisSource analysisSource{ _api.bufferLine.data(), gsl::narrow<UINT32>(_api.bufferLine.size()) };
    TextAnalysisSink analysisSink{ _api.analysisResults };

    wil::com_ptr<IDWriteFontFace> mappedFontFace;

#pragma warning(suppress : 26494) // Variable 'mappedEnd' is uninitialized. Always initialize an object (type.5).
    for (u32 idx = 0, mappedEnd; idx < _api.bufferLine.size(); idx = mappedEnd)
    {
        auto scale = 1.0f;
        u32 mappedLength = 0;

        if (textFormatAxis)
        {
            wil::com_ptr<IDWriteFontFace5> fontFace5;
            THROW_IF_FAILED(_p.systemFontFallback.query<IDWriteFontFallback1>()->MapCharacters(
                /* analysisSource */ &analysisSource,
                /* textPosition */ idx,
                /* textLength */ gsl::narrow_cast<u32>(_api.bufferLine.size()) - idx,
                /* baseFontCollection */ _p.s->font->fontCollection.get(),
                /* baseFamilyName */ _p.s->font->fontName.c_str(),
                /* fontAxisValues */ textFormatAxis.data(),
                /* fontAxisValueCount */ gsl::narrow_cast<u32>(textFormatAxis.size()),
                /* mappedLength */ &mappedLength,
                /* scale */ &scale,
                /* mappedFontFace */ fontFace5.put()));
            mappedFontFace = std::move(fontFace5);
        }
        else
        {
            const auto baseWeight = _api.attributes.bold ? DWRITE_FONT_WEIGHT_BOLD : static_cast<DWRITE_FONT_WEIGHT>(_p.s->font->fontWeight);
            const auto baseStyle = _api.attributes.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
            wil::com_ptr<IDWriteFont> font;

            THROW_IF_FAILED(_p.systemFontFallback->MapCharacters(
                /* analysisSource     */ &analysisSource,
                /* textPosition       */ idx,
                /* textLength         */ gsl::narrow_cast<u32>(_api.bufferLine.size()) - idx,
                /* baseFontCollection */ _p.s->font->fontCollection.get(),
                /* baseFamilyName     */ _p.s->font->fontName.c_str(),
                /* baseWeight         */ baseWeight,
                /* baseStyle          */ baseStyle,
                /* baseStretch        */ DWRITE_FONT_STRETCH_NORMAL,
                /* mappedLength       */ &mappedLength,
                /* mappedFont         */ font.addressof(),
                /* scale              */ &scale));

            mappedFontFace.reset();
            if (font)
            {
                THROW_IF_FAILED(font->CreateFontFace(mappedFontFace.addressof()));
            }
        }

        mappedEnd = idx + mappedLength;

        if (!mappedFontFace)
        {
            continue;
        }

        const auto initialIndicesCount = row.glyphIndices.size();

        if (mappedLength > initialIndicesCount)
        {
            auto size = initialIndicesCount;
            size = size + (size >> 1);
            size = std::max<size_t>(size, mappedLength);
            _api.glyphIndices = Buffer<u16>{ size };
        }

        // We can reuse idx here, as it'll be reset to "idx = mappedEnd" in the outer loop anyways.
        for (u32 complexityLength = 0; idx < mappedEnd; idx += complexityLength)
        {
            BOOL isTextSimple;
            THROW_IF_FAILED(_p.textAnalyzer->GetTextComplexity(_api.bufferLine.data() + idx, mappedEnd - idx, mappedFontFace.get(), &isTextSimple, &complexityLength, _api.glyphIndices.data()));

            if (isTextSimple)
            {
                for (size_t i = 0; i < complexityLength; ++i)
                {
                    const auto col = _api.bufferLineColumn[idx + i];
                    const auto fg = _api.colorsForeground[col];
                    f32 glyphAdvance;

                    if constexpr (!debugProportionalText)
                    {
                        for (size_t j = 1;; ++j)
                        {
                            if (col != _api.bufferLineColumn[idx + i + j])
                            {
                                glyphAdvance = j * _p.d.font.cellSizeDIP.x;
                                break;
                            }
                        }
                    }
                    else
                    {
                        DWRITE_FONT_METRICS metrics{};
                        mappedFontFace->GetMetrics(&metrics);

                        DWRITE_GLYPH_METRICS glyphMetrics{};
                        THROW_IF_FAILED(mappedFontFace->GetDesignGlyphMetrics(&_api.glyphIndices[i], 1, &glyphMetrics, false));
                        const auto designUnitsPerDIP = _p.s->font->fontSizeInDIP / static_cast<float>(metrics.designUnitsPerEm);
                        glyphAdvance = static_cast<f32>(glyphMetrics.advanceWidth) * designUnitsPerDIP;
                    }

                    row.glyphIndices.emplace_back(_api.glyphIndices[i]);
                    row.glyphAdvances.emplace_back(glyphAdvance);
                    row.glyphOffsets.emplace_back();
                    row.colors.emplace_back(fg);
                }
            }
            else
            {
                _api.analysisResults.clear();
                THROW_IF_FAILED(_p.textAnalyzer->AnalyzeScript(&analysisSource, idx, complexityLength, &analysisSink));
                //_p.textAnalyzer->AnalyzeBidi(&atlasAnalyzer, idx, complexityLength, &atlasAnalyzer);

                for (const auto& a : _api.analysisResults)
                {
                    DWRITE_SCRIPT_ANALYSIS scriptAnalysis{ a.script, static_cast<DWRITE_SCRIPT_SHAPES>(a.shapes) };
                    u32 actualGlyphCount = 0;

#pragma warning(push)
#pragma warning(disable : 26494) // Variable '...' is uninitialized. Always initialize an object (type.5).
                    // None of these variables need to be initialized.
                    // features/featureRangeLengths are marked _In_reads_opt_(featureRanges).
                    // featureRanges is only > 0 when we also initialize all these variables.
                    DWRITE_TYPOGRAPHIC_FEATURES feature;
                    const DWRITE_TYPOGRAPHIC_FEATURES* features;
                    u32 featureRangeLengths;
#pragma warning(pop)
                    u32 featureRanges = 0;

                    if (!_p.s->font->fontFeatures.empty())
                    {
                        // Direct2D, why is this mutable?         Why?
                        feature.features = const_cast<DWRITE_FONT_FEATURE*>(_p.s->font->fontFeatures.data());
                        feature.featureCount = gsl::narrow_cast<u32>(_p.s->font->fontFeatures.size());
                        features = &feature;
                        featureRangeLengths = a.textLength;
                        featureRanges = 1;
                    }

                    if (_api.clusterMap.size() <= a.textLength)
                    {
                        _api.clusterMap = Buffer<u16>{ a.textLength + 1 };
                        _api.textProps = Buffer<DWRITE_SHAPING_TEXT_PROPERTIES>{ a.textLength };
                    }

                    for (auto retry = 0;;)
                    {
                        const auto hr = _p.textAnalyzer->GetGlyphs(
                            /* textString          */ _api.bufferLine.data() + a.textPosition,
                            /* textLength          */ a.textLength,
                            /* fontFace            */ mappedFontFace.get(),
                            /* isSideways          */ false,
                            /* isRightToLeft       */ a.bidiLevel & 1,
                            /* scriptAnalysis      */ &scriptAnalysis,
                            /* localeName          */ nullptr,
                            /* numberSubstitution  */ nullptr,
                            /* features            */ &features,
                            /* featureRangeLengths */ &featureRangeLengths,
                            /* featureRanges       */ featureRanges,
                            /* maxGlyphCount       */ gsl::narrow_cast<u32>(_api.glyphProps.size()),
                            /* clusterMap          */ _api.clusterMap.data(),
                            /* textProps           */ _api.textProps.data(),
                            /* glyphIndices        */ _api.glyphIndices.data(),
                            /* glyphProps          */ _api.glyphProps.data(),
                            /* actualGlyphCount    */ &actualGlyphCount);

                        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && ++retry < 8)
                        {
                            // Grow factor 1.5x.
                            auto size = _api.glyphProps.size();
                            size = size + (size >> 1);
                            // Overflow check.
                            Expects(size > _api.glyphProps.size());
                            _api.glyphIndices = Buffer<u16>{ size };
                            _api.glyphProps = Buffer<DWRITE_SHAPING_GLYPH_PROPERTIES>(size);
                            continue;
                        }

                        THROW_IF_FAILED(hr);
                        break;
                    }

                    if (_api.glyphAdvances.size() < actualGlyphCount)
                    {
                        // Grow the buffer by at least 1.5x and at least of `actualGlyphCount` items.
                        // The 1.5x growth ensures we don't reallocate every time we need 1 more slot.
                        auto size = _api.glyphAdvances.size();
                        size = size + (size >> 1);
                        size = std::max<size_t>(size, actualGlyphCount);
                        _api.glyphAdvances = Buffer<f32>{ size };
                        _api.glyphOffsets = Buffer<DWRITE_GLYPH_OFFSET>{ size };
                    }

                    THROW_IF_FAILED(_p.textAnalyzer->GetGlyphPlacements(
                        /* textString          */ _api.bufferLine.data() + a.textPosition,
                        /* clusterMap          */ _api.clusterMap.data(),
                        /* textProps           */ _api.textProps.data(),
                        /* textLength          */ a.textLength,
                        /* glyphIndices        */ _api.glyphIndices.data(),
                        /* glyphProps          */ _api.glyphProps.data(),
                        /* glyphCount          */ actualGlyphCount,
                        /* fontFace            */ mappedFontFace.get(),
                        /* fontEmSize          */ _p.s->font->fontSizeInDIP,
                        /* isSideways          */ false,
                        /* isRightToLeft       */ a.bidiLevel & 1,
                        /* scriptAnalysis      */ &scriptAnalysis,
                        /* localeName          */ nullptr,
                        /* features            */ &features,
                        /* featureRangeLengths */ &featureRangeLengths,
                        /* featureRanges       */ featureRanges,
                        /* glyphAdvances       */ _api.glyphAdvances.data(),
                        /* glyphOffsets        */ _api.glyphOffsets.data()));

                    _api.clusterMap[a.textLength] = gsl::narrow_cast<u16>(actualGlyphCount);

                    auto prevCluster = _api.clusterMap[0];
                    size_t beg = 0;

                    for (size_t i = 1; i <= a.textLength; ++i)
                    {
                        const auto nextCluster = _api.clusterMap[i];
                        if (prevCluster == nextCluster)
                        {
                            continue;
                        }

                        const auto col1 = _api.bufferLineColumn[a.textPosition + beg];
                        const auto col2 = _api.bufferLineColumn[a.textPosition + i];
                        const auto fg = _api.colorsForeground[col1];

                        if constexpr (!debugProportionalText)
                        {
                            const auto expectedAdvance = (col2 - col1) * _p.d.font.cellSizeDIP.x;
                            f32 actualAdvance = 0;
                            for (auto j = prevCluster; j < nextCluster; ++j)
                            {
                                actualAdvance += _api.glyphAdvances[j];
                            }
                            _api.glyphAdvances[nextCluster - 1] += expectedAdvance - actualAdvance;
                        }

                        row.colors.insert(row.colors.end(), nextCluster - prevCluster, fg);

                        prevCluster = nextCluster;
                        beg = i;
                    }

                    row.glyphIndices.insert(row.glyphIndices.end(), _api.glyphIndices.begin(), _api.glyphIndices.begin() + actualGlyphCount);
                    row.glyphAdvances.insert(row.glyphAdvances.end(), _api.glyphAdvances.begin(), _api.glyphAdvances.begin() + actualGlyphCount);
                    row.glyphOffsets.insert(row.glyphOffsets.end(), _api.glyphOffsets.begin(), _api.glyphOffsets.begin() + actualGlyphCount);
                }
            }
        }

        const auto indicesCount = row.glyphIndices.size();
        if (indicesCount > initialIndicesCount)
        {
            row.mappings.emplace_back(std::move(mappedFontFace), _p.s->font->fontSizeInDIP * scale, gsl::narrow_cast<u32>(initialIndicesCount), gsl::narrow_cast<u32>(indicesCount));
        }
    }
}
