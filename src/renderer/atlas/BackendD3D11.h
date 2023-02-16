#pragma once

#include <d3d11_2.h>

#include <stb_rect_pack.h>
#include <til/hash.h>

#include "Backend.h"

namespace Microsoft::Console::Render::Atlas
{
    struct BackendD3D11 : IBackend
    {
        BackendD3D11(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext);

        void Render(const RenderingPayload& payload) override;
        bool RequiresContinuousRedraw() noexcept override;
        void WaitUntilCanRender() noexcept override;

    private:
        // NOTE: D3D constant buffers sizes must be a multiple of 16 bytes.
        struct alignas(16) ConstBuffer
        {
            // WARNING: Modify this carefully after understanding how HLSL struct packing works.
            // The gist is:
            // * Minimum alignment is 4 bytes (like `#pragma pack 4`)
            // * Members cannot straddle 16 byte boundaries
            //   This means a structure like {u32; u32; u32; u32x2} would require
            //   padding so that it is {u32; u32; u32; <4 byte padding>; u32x2}.
            // * bool will probably not work the way you want it to,
            //   because HLSL uses 32-bit bools and C++ doesn't.
            alignas(sizeof(f32x4)) f32x4 positionScale;
            alignas(sizeof(f32x4)) f32 gammaRatios[4]{};
            alignas(sizeof(f32)) f32 cleartypeEnhancedContrast = 0;
            alignas(sizeof(f32)) f32 grayscaleEnhancedContrast = 0;
#pragma warning(suppress : 4324) // 'ConstBuffer': structure was padded due to alignment specifier
        };

        struct alignas(16) CustomConstBuffer
        {
            // WARNING: Same rules as for ConstBuffer above apply.
            alignas(sizeof(f32)) f32 time = 0;
            alignas(sizeof(f32)) f32 scale = 0;
            alignas(sizeof(f32x2)) f32x2 resolution;
            alignas(sizeof(f32x4)) f32x4 background;
#pragma warning(suppress : 4324) // 'CustomConstBuffer': structure was padded due to alignment specifier
        };

        struct GlyphCacheEntry
        {
            // BODGY: The IDWriteFontFace results from us calling IDWriteFontFallback::MapCharacters
            // which at the time of writing returns the same IDWriteFontFace as long as someone is
            // holding a reference / the reference count doesn't drop to 0 (see ActiveFaceCache).
            IDWriteFontFace* fontFace = nullptr;
            u16 glyphIndex = 0;

            u16x2 xy;
            u16x2 wh;
            i16x2 offset;
            bool colorGlyph = false;
        };
        static_assert(sizeof(GlyphCacheEntry) == 24);

        struct GlyphCacheMap
        {
            GlyphCacheMap() = default;

            GlyphCacheMap& operator=(GlyphCacheMap&& other) noexcept
            {
                _map = std::exchange(other._map, {});
                _mapMask = std::exchange(other._mapMask, 0);
                _capacity = std::exchange(other._capacity, 0);
                _size = std::exchange(other._size, 0);
                return *this;
            }

            ~GlyphCacheMap()
            {
                Clear();
            }

            void Clear() noexcept
            {
                for (auto& entry : _map)
                {
                    if (entry.fontFace)
                    {
                        entry.fontFace->Release();
                        entry.fontFace = nullptr;
                    }
                }
            }

            GlyphCacheEntry& FindOrInsert(IDWriteFontFace* fontFace, u16 glyphIndex, bool& inserted)
            {
                const auto hash = _hash(fontFace, glyphIndex);

                for (auto i = hash;; ++i)
                {
                    auto& entry = _map[i & _mapMask];
                    if (entry.fontFace == fontFace && entry.glyphIndex == glyphIndex)
                    {
                        inserted = false;
                        return entry;
                    }
                    if (!entry.fontFace)
                    {
                        inserted = true;
                        return _insert(fontFace, glyphIndex, hash);
                    }
                }
            }

        private:
            static size_t _hash(IDWriteFontFace* fontFace, u16 glyphIndex) noexcept
            {
                // MSVC 19.33 produces surprisingly good assembly for this without stack allocation.
                const uintptr_t data[2]{ std::bit_cast<uintptr_t>(fontFace), glyphIndex };
                return til::hash(&data[0], sizeof(data));
            }

            GlyphCacheEntry& _insert(IDWriteFontFace* fontFace, u16 glyphIndex, size_t hash)
            {
                if (_size >= _capacity)
                {
                    _bumpSize();
                }

                ++_size;

                for (auto i = hash;; ++i)
                {
                    auto& entry = _map[i & _mapMask];
                    if (!entry.fontFace)
                    {
                        entry.fontFace = fontFace;
                        entry.glyphIndex = glyphIndex;
                        entry.fontFace->AddRef();
                        return entry;
                    }
                }
            }

            void _bumpSize()
            {
                const auto newMapSize = _map.size() << 1;
                const auto newMapMask = newMapSize - 1;
                FAIL_FAST_IF(newMapSize >= INT32_MAX); // overflow/truncation protection

                auto newMap = Buffer<GlyphCacheEntry>(newMapSize);

                for (const auto& entry : _map)
                {
                    const auto newHash = _hash(entry.fontFace, entry.glyphIndex);
                    newMap[newHash & newMapMask] = entry;
                }

                _map = std::move(newMap);
                _mapMask = newMapMask;
                _capacity = newMapMask / 2;
            }

            static constexpr u32 initialSize = 256;

            Buffer<GlyphCacheEntry> _map{ initialSize };
            size_t _mapMask = initialSize - 1;
            size_t _capacity = _mapMask / 2;
            size_t _size = 0;
        };

        struct alignas(16) VertexInstanceData
        {
            f32x4 rect;
            f32x4 tex;
            u32 color = 0;
            u32 shadingType = 0;
#pragma warning(suppress : 4324) // 'CustomConstBuffer': structure was padded due to alignment specifier
        };

        void _drawGlyph(const RenderingPayload& p, GlyphCacheEntry& entry, f32 fontEmSize);

        static constexpr bool debugNvidiaQuadFill = false;
        
        SwapChainManager _swapChainManager;

        wil::com_ptr<ID3D11Device1> _device;
        wil::com_ptr<ID3D11DeviceContext1> _deviceContext;
        wil::com_ptr<IDXGISwapChain1> _swapChain;
        wil::unique_handle _frameLatencyWaitableObject;
        wil::com_ptr<ID3D11RenderTargetView> _renderTargetView;
        wil::com_ptr<ID3D11RenderTargetView> _renderTargetViewUInt;

        wil::com_ptr<ID3D11VertexShader> _vertexShader;
        wil::com_ptr<ID3D11PixelShader> _cleartypePixelShader;
        wil::com_ptr<ID3D11PixelShader> _grayscalePixelShader;
        wil::com_ptr<ID3D11PixelShader> _invertCursorPixelShader;
        wil::com_ptr<ID3D11BlendState1> _cleartypeBlendState;
        wil::com_ptr<ID3D11BlendState1> _alphaBlendState;
        wil::com_ptr<ID3D11BlendState1> _invertCursorBlendState;

        wil::com_ptr<ID3D11RasterizerState> _rasterizerState;
        wil::com_ptr<ID3D11PixelShader> _textPixelShader;
        wil::com_ptr<ID3D11BlendState> _textBlendState;

        wil::com_ptr<ID3D11PixelShader> _wireframePixelShader;
        wil::com_ptr<ID3D11RasterizerState> _wireframeRasterizerState;

        wil::com_ptr<ID3D11Buffer> _constantBuffer;
        wil::com_ptr<ID3D11InputLayout> _textInputLayout;
        wil::com_ptr<ID3D11Buffer> _vertexBuffers[2];
        size_t _vertexBuffers1Size = 0;

        wil::com_ptr<ID3D11Texture2D> _perCellColor;
        wil::com_ptr<ID3D11ShaderResourceView> _perCellColorView;

        wil::com_ptr<ID3D11Texture2D> _customOffscreenTexture;
        wil::com_ptr<ID3D11ShaderResourceView> _customOffscreenTextureView;
        wil::com_ptr<ID3D11RenderTargetView> _customOffscreenTextureTargetView;
        wil::com_ptr<ID3D11VertexShader> _customVertexShader;
        wil::com_ptr<ID3D11PixelShader> _customPixelShader;
        wil::com_ptr<ID3D11Buffer> _customShaderConstantBuffer;
        wil::com_ptr<ID3D11SamplerState> _customShaderSamplerState;
        std::chrono::steady_clock::time_point _customShaderStartTime;

        // D2D resources
        wil::com_ptr<ID3D11Texture2D> _atlasBuffer;
        wil::com_ptr<ID3D11ShaderResourceView> _atlasView;
        wil::com_ptr<ID2D1DeviceContext> _d2dRenderTarget;
        wil::com_ptr<ID2D1DeviceContext4> _d2dRenderTarget4; // Optional. Supported since Windows 10 14393.
        wil::com_ptr<ID2D1SolidColorBrush> _brush;
        Buffer<DWRITE_FONT_AXIS_VALUE> _textFormatAxes[2][2];
        wil::com_ptr<ID2D1StrokeStyle> _dottedStrokeStyle;

        wil::com_ptr<ID2D1Bitmap> _d2dBackgroundBitmap;
        wil::com_ptr<ID2D1BitmapBrush> _d2dBackgroundBrush;

        // D3D resources
        GlyphCacheMap _glyphCache;
        std::vector<stbrp_node> _rectPackerData;
        stbrp_context _rectPacker{};
        std::vector<ShapedRow> _rows;
        std::vector<VertexInstanceData> _vertexInstanceData;
        u32 _instanceCount = 6;

        bool _requiresContinuousRedraw = false;

        til::generation_t _generation;
        til::generation_t _fontGeneration;
        til::generation_t _miscGeneration;

#ifndef NDEBUG
        std::filesystem::path _sourceDirectory;
        wil::unique_folder_change_reader_nothrow _sourceCodeWatcher;
        std::atomic<int64_t> _sourceCodeInvalidationTime{ INT64_MAX };
        float _gamma = 0;
        float _cleartypeEnhancedContrast = 0;
        float _grayscaleEnhancedContrast = 0;
        u32 _brushColor = 0;
        u16x2 _cellCount;
#endif
    };
}
