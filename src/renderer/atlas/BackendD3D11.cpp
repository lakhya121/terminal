#include "pch.h"
#include "BackendD3D11.h"

#include <custom_shader_ps.h>
#include <custom_shader_vs.h>
#include <shader_text_cleartype_ps.h>
#include <shader_text_grayscale_ps.h>
#include <shader_invert_cursor_ps.h>
#include <shader_wireframe.h>
#include <shader_vs.h>

#include "dwrite.h"

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4127)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

BackendD3D11::BackendD3D11(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
    // Our constant buffer will never get resized
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(ConstBuffer);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _constantBuffer.put()));
    }

    THROW_IF_FAILED(_device->CreateVertexShader(&shader_vs[0], sizeof(shader_vs), nullptr, _vertexShader.put()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_text_cleartype_ps[0], sizeof(shader_text_cleartype_ps), nullptr, _cleartypePixelShader.put()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_text_grayscale_ps[0], sizeof(shader_text_grayscale_ps), nullptr, _grayscalePixelShader.put()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_invert_cursor_ps[0], sizeof(shader_invert_cursor_ps), nullptr, _invertCursorPixelShader.put()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_wireframe[0], sizeof(shader_wireframe), nullptr, _wireframePixelShader.put()));

    {
        D3D11_BLEND_DESC1 desc{};
        desc.RenderTarget[0] = {
            .BlendEnable = true,
            .SrcBlend = D3D11_BLEND_ONE,
            .DestBlend = D3D11_BLEND_INV_SRC1_COLOR,
            .BlendOp = D3D11_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D11_BLEND_ONE,
            .DestBlendAlpha = D3D11_BLEND_ZERO,
            .BlendOpAlpha = D3D11_BLEND_OP_ADD,
            .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
        };
        THROW_IF_FAILED(_device->CreateBlendState1(&desc, _cleartypeBlendState.put()));
    }
    {
        D3D11_BLEND_DESC1 desc{};
        desc.RenderTarget[0] = {
            .BlendEnable = true,
            .SrcBlend = D3D11_BLEND_ONE,
            .DestBlend = D3D11_BLEND_INV_SRC_ALPHA,
            .BlendOp = D3D11_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D11_BLEND_ONE,
            .DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA,
            .BlendOpAlpha = D3D11_BLEND_OP_ADD,
            .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
        };
        THROW_IF_FAILED(_device->CreateBlendState1(&desc, _alphaBlendState.put()));
    }
    {
        D3D11_BLEND_DESC1 desc{};
        desc.RenderTarget[0] = {
            .LogicOpEnable = true,
            .LogicOp = D3D11_LOGIC_OP_XOR,
            .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE,
        };
        THROW_IF_FAILED(_device->CreateBlendState1(&desc, _invertCursorBlendState.put()));
    }

    if constexpr (debugNvidiaQuadFill)
    {
        // Quick hack to test NVAPI_QUAD_FILLMODE_BBOX if needed.
        struct NvAPI_D3D11_RASTERIZER_DESC_EX : D3D11_RASTERIZER_DESC
        {
            UINT8 _padding1[40];
            INT QuadFillMode; // Set to 1 for NVAPI_QUAD_FILLMODE_BBOX
            UINT8 _padding2[67];
        };

        using NvAPI_QueryInterface_t = PVOID(__cdecl*)(UINT);
        using NvAPI_Initialize_t = INT(__cdecl*)();
        using NvAPI_D3D11_CreateRasterizerState_t = INT(__cdecl*)(ID3D11Device*, const NvAPI_D3D11_RASTERIZER_DESC_EX*, ID3D11RasterizerState**);

        static const auto NvAPI_D3D11_CreateRasterizerState = []() -> NvAPI_D3D11_CreateRasterizerState_t {
            const auto module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
            {
                return nullptr;
            }
            const auto NvAPI_QueryInterface = reinterpret_cast<NvAPI_QueryInterface_t>(GetProcAddress(module, "nvapi_QueryInterface"));
            if (!NvAPI_QueryInterface)
            {
                return nullptr;
            }
            const auto NvAPI_Initialize = reinterpret_cast<NvAPI_Initialize_t>(NvAPI_QueryInterface(0x0150E828));
            if (!NvAPI_Initialize)
            {
                return nullptr;
            }
            const auto func = reinterpret_cast<NvAPI_D3D11_CreateRasterizerState_t>(NvAPI_QueryInterface(0xDB8D28AF));
            if (!NvAPI_Initialize)
            {
                return nullptr;
            }
            if (NvAPI_Initialize())
            {
                return nullptr;
            }
            return func;
        }();

        if (NvAPI_D3D11_CreateRasterizerState)
        {
            NvAPI_D3D11_RASTERIZER_DESC_EX desc{};
            desc.FillMode = D3D11_FILL_SOLID;
            desc.CullMode = D3D11_CULL_NONE;
            desc.QuadFillMode = 1;
            if (const auto status = NvAPI_D3D11_CreateRasterizerState(_device.get(), &desc, _rasterizerState.put()))
            {
                LOG_HR_MSG(E_UNEXPECTED, "failed to set QuadFillMode with: %d", status);
                THROW_IF_FAILED(_device->CreateRasterizerState(&desc, _rasterizerState.put()));
                _instanceCount = 6;
            }
            else
            {
                _instanceCount = 3;
            }
        }
    }
    else
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        THROW_IF_FAILED(_device->CreateRasterizerState(&desc, _rasterizerState.put()));
    }

    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_WIREFRAME;
        desc.CullMode = D3D11_CULL_NONE;
        wil::com_ptr<ID3D11RasterizerState> state;
        THROW_IF_FAILED(_device->CreateRasterizerState(&desc, _wireframeRasterizerState.put()));
    }

    {
        static constexpr D3D11_INPUT_ELEMENT_DESC layout[]{
            { "SV_Position", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "Rect", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(VertexInstanceData, rect), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "Tex", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(VertexInstanceData, tex), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "Color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, offsetof(VertexInstanceData, color), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "ShadingType", 0, DXGI_FORMAT_R32_UINT, 1, offsetof(VertexInstanceData, shadingType), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        };
        THROW_IF_FAILED(_device->CreateInputLayout(&layout[0], gsl::narrow_cast<UINT>(std::size(layout)), &shader_vs[0], sizeof(shader_vs), _textInputLayout.put()));
    }

    {
        static constexpr f32x2 vertices[]{
            { 0, 0 },
            { 1, 0 },
            { 1, 1 },
            { 1, 1 },
            { 0, 1 },
            { 0, 0 },
        };

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(vertices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA initialData{ &vertices[0] };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, &initialData, _vertexBuffers[0].put()));
    }

#ifndef NDEBUG
    _sourceDirectory = std::filesystem::path{ __FILE__ }.parent_path();
    _sourceCodeWatcher = wil::make_folder_change_reader_nothrow(_sourceDirectory.c_str(), false, wil::FolderChangeEvents::FileName | wil::FolderChangeEvents::LastWriteTime, [this](wil::FolderChangeEvent, PCWSTR path) {
        if (til::ends_with(path, L".hlsl"))
        {
            auto expected = INT64_MAX;
            const auto invalidationTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            _sourceCodeInvalidationTime.compare_exchange_strong(expected, invalidationTime.time_since_epoch().count(), std::memory_order_relaxed);
        }
    });
#endif
}

void BackendD3D11::Render(const RenderingPayload& p)
{
#ifndef NDEBUG
    if (const auto invalidationTime = _sourceCodeInvalidationTime.load(std::memory_order_relaxed); invalidationTime != INT64_MAX && invalidationTime <= std::chrono::steady_clock::now().time_since_epoch().count())
    {
        _sourceCodeInvalidationTime.store(INT64_MAX, std::memory_order_relaxed);

        try
        {
            static const auto compile = [](const std::filesystem::path& path, const char* target) {
                wil::com_ptr<ID3DBlob> error;
                wil::com_ptr<ID3DBlob> blob;
                const auto hr = D3DCompileFromFile(
                    /* pFileName   */ path.c_str(),
                    /* pDefines    */ nullptr,
                    /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
                    /* pEntrypoint */ "main",
                    /* pTarget     */ target,
                    /* Flags1      */ D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                    /* Flags2      */ 0,
                    /* ppCode      */ blob.addressof(),
                    /* ppErrorMsgs */ error.addressof());

                if (error)
                {
                    std::thread t{ [error = std::move(error)]() noexcept {
                        MessageBoxA(nullptr, static_cast<const char*>(error->GetBufferPointer()), "Compilation error", MB_ICONERROR | MB_OK);
                    } };
                    t.detach();
                }

                THROW_IF_FAILED(hr);
                return blob;
            };

            struct FileVS
            {
                std::wstring_view filename;
                wil::com_ptr<ID3D11VertexShader> BackendD3D11::*target;
            };
            struct FilePS
            {
                std::wstring_view filename;
                wil::com_ptr<ID3D11PixelShader> BackendD3D11::*target;
            };

            static std::array filesVS{
                FileVS{ L"shader_vs.hlsl", &BackendD3D11::_vertexShader },
            };
            static std::array filesPS{
                FilePS{ L"shader_text_cleartype_ps.hlsl", &BackendD3D11::_cleartypePixelShader },
                FilePS{ L"shader_text_grayscale_ps.hlsl", &BackendD3D11::_grayscalePixelShader },
                FilePS{ L"shader_invert_cursor_ps.hlsl", &BackendD3D11::_invertCursorPixelShader },
            };

            std::array<wil::com_ptr<ID3D11VertexShader>, filesVS.size()> compiledVS;
            std::array<wil::com_ptr<ID3D11PixelShader>, filesPS.size()> compiledPS;

            // Compile our files before moving them into `this` below to ensure we're
            // always in a consistent state where all shaders are seemingly valid.
            for (size_t i = 0; i < filesVS.size(); ++i)
            {
                const auto blob = compile(_sourceDirectory / filesVS[i].filename, "vs_4_0");
                THROW_IF_FAILED(_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledVS[i].addressof()));
            }
            for (size_t i = 0; i < filesPS.size(); ++i)
            {
                const auto blob = compile(_sourceDirectory / filesPS[i].filename, "ps_4_0");
                THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledPS[i].addressof()));
            }

            for (size_t i = 0; i < filesVS.size(); ++i)
            {
                this->*filesVS[i].target = std::move(compiledVS[i]);
            }
            for (size_t i = 0; i < filesPS.size(); ++i)
            {
                this->*filesPS[i].target = std::move(compiledPS[i]);
            }
        }
        CATCH_LOG()
    }
#endif

    if (!p.s->misc->customPixelShaderPath.empty())
    {
        const char* target = nullptr;
        switch (_device->GetFeatureLevel())
        {
        case D3D_FEATURE_LEVEL_10_0:
            target = "ps_4_0";
            break;
        case D3D_FEATURE_LEVEL_10_1:
            target = "ps_4_1";
            break;
        default:
            target = "ps_5_0";
            break;
        }

        static constexpr auto flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR
#ifdef NDEBUG
            | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
            // Only enable strictness and warnings in DEBUG mode
            //  as these settings makes it very difficult to develop
            //  shaders as windows terminal is not telling the user
            //  what's wrong, windows terminal just fails.
            //  Keep it in DEBUG mode to catch errors in shaders
            //  shipped with windows terminal
            | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ p.s->misc->customPixelShaderPath.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ flags,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        // Unless we can determine otherwise, assume this shader requires evaluation every frame
        _requiresContinuousRedraw = true;

        if (SUCCEEDED(hr))
        {
            THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _customPixelShader.put()));

            // Try to determine whether the shader uses the Time variable
            wil::com_ptr<ID3D11ShaderReflection> reflector;
            if (SUCCEEDED_LOG(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(reflector.put()))))
            {
                if (ID3D11ShaderReflectionConstantBuffer* constantBufferReflector = reflector->GetConstantBufferByIndex(0)) // shader buffer
                {
                    if (ID3D11ShaderReflectionVariable* variableReflector = constantBufferReflector->GetVariableByIndex(0)) // time
                    {
                        D3D11_SHADER_VARIABLE_DESC variableDescriptor;
                        if (SUCCEEDED_LOG(variableReflector->GetDesc(&variableDescriptor)))
                        {
                            // only if time is used
                            _requiresContinuousRedraw = WI_IsFlagSet(variableDescriptor.uFlags, D3D_SVF_USED);
                        }
                    }
                }
            }
        }
        else
        {
            if (error)
            {
                LOG_HR_MSG(hr, "%*hs", error->GetBufferSize(), error->GetBufferPointer());
            }
            else
            {
                LOG_HR(hr);
            }
            if (p.warningCallback)
            {
                p.warningCallback(D2DERR_SHADER_COMPILE_FAILED);
            }
        }
    }
    else if (p.s->misc->useRetroTerminalEffect)
    {
        THROW_IF_FAILED(_device->CreatePixelShader(&custom_shader_ps[0], sizeof(custom_shader_ps), nullptr, _customPixelShader.put()));
        // We know the built-in retro shader doesn't require continuous redraw.
        _requiresContinuousRedraw = false;
    }

    if (_customPixelShader)
    {
        THROW_IF_FAILED(_device->CreateVertexShader(&custom_shader_vs[0], sizeof(custom_shader_vs), nullptr, _customVertexShader.put()));

        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(CustomConstBuffer);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _customShaderConstantBuffer.put()));
        }

        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.MaxAnisotropy = 1;
            desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            THROW_IF_FAILED(_device->CreateSamplerState(&desc, _customShaderSamplerState.put()));
        }

        _customShaderStartTime = std::chrono::steady_clock::now();
    }

    if (_generation != p.s.generation())
    {
        _swapChainManager.UpdateSwapChainSettings(
            p,
            _device.get(),
            [this]() {
                _renderTargetView.reset();
                _deviceContext->ClearState();
            },
            [this]() {
                _renderTargetView.reset();
                _deviceContext->ClearState();
                _deviceContext->Flush();
            });

        if (!_renderTargetView)
        {
            const auto buffer = _swapChainManager.GetBuffer();
            THROW_IF_FAILED(_device->CreateRenderTargetView(buffer.get(), nullptr, _renderTargetView.put()));
        }

        if (_fontGeneration != p.s->font.generation())
        {
            _d2dRenderTarget.reset();
            _fontGeneration = p.s->font.generation();
        }

        if (_miscGeneration != p.s->misc.generation())
        {
            _miscGeneration = p.s->misc.generation();
        }

        if (_cellCount != p.s->cellCount)
        {
            if (_customPixelShader)
            {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = p.s->targetSize.x;
                desc.Height = p.s->targetSize.y;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc = { 1, 0 };
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _customOffscreenTexture.addressof()));
                THROW_IF_FAILED(_device->CreateShaderResourceView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureView.addressof()));
                THROW_IF_FAILED(_device->CreateRenderTargetView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureTargetView.addressof()));
            }

            {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = p.s->cellCount.x;
                desc.Height = p.s->cellCount.y;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc = { 1, 0 };
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _perCellColor.addressof()));
                THROW_IF_FAILED(_device->CreateShaderResourceView(_perCellColor.get(), nullptr, _perCellColorView.addressof()));
            }

            _cellCount = p.s->cellCount;
        }

        _generation = p.s.generation();
    }

    if (!_atlasBuffer)
    {
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 1024;
            desc.Height = 1024;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = { 1, 0 };
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _atlasBuffer.addressof()));
            THROW_IF_FAILED(_device->CreateShaderResourceView(_atlasBuffer.get(), nullptr, _atlasView.addressof()));
        }

        {
            _glyphCache.Clear();
            _rectPackerData.resize(1024);
            stbrp_init_target(&_rectPacker, 1024, 1024, _rectPackerData.data(), gsl::narrow_cast<int>(_rectPackerData.size()));
        }

        {
            const auto surface = _atlasBuffer.query<IDXGISurface>();

            wil::com_ptr<IDWriteRenderingParams1> renderingParams;
            DWrite_GetRenderParams(p.dwriteFactory.get(), &_gamma, &_cleartypeEnhancedContrast, &_grayscaleEnhancedContrast, renderingParams.addressof());

            D2D1_RENDER_TARGET_PROPERTIES props{};
            props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.dpiX = static_cast<float>(p.s->font->dpi);
            props.dpiY = static_cast<float>(p.s->font->dpi);
            wil::com_ptr<ID2D1RenderTarget> renderTarget;
            THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
            _d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
            _d2dRenderTarget4 = renderTarget.query<ID2D1DeviceContext4>();

            // We don't really use D2D for anything except DWrite, but it
            // can't hurt to ensure that everything it does is pixel aligned.
            _d2dRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            // In case p.s->misc->antialiasingMode is D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE we'll
            // continuously adjust it in BackendD3D11::_drawGlyph. See _drawGlyph.
            _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->misc->antialiasingMode));
            // Ensure that D2D uses the exact same gamma as our shader uses.
            _d2dRenderTarget->SetTextRenderingParams(renderingParams.get());
        }
        {
            static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
            THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
            _brushColor = 0xffffffff;
        }

        switch (p.s->misc->antialiasingMode)
        {
        case D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE:
            _textPixelShader = _cleartypePixelShader;
            _textBlendState = _cleartypeBlendState;
            break;
        default:
            _textPixelShader = _grayscalePixelShader;
            _textBlendState = _alphaBlendState;
            break;
        }
    }

    vec2<size_t> textRange;
    vec2<size_t> cursorRange;
    vec2<size_t> selectionRange;

    {
        _vertexInstanceData.clear();

        // Background
        {
            auto& ref = _vertexInstanceData.emplace_back();
            ref.rect = { 0.0f, 0.0f, static_cast<f32>(p.s->targetSize.x), static_cast<f32>(p.s->targetSize.y) };
            ref.tex = { 0.0f, 0.0f, static_cast<f32>(p.s->targetSize.x) / static_cast<f32>(p.s->font->cellSize.x), static_cast<f32>(p.s->targetSize.y) / static_cast<f32>(p.s->font->cellSize.y) };
            ref.color = 0;
            ref.shadingType = 1;
        }

        // Text
        {
            textRange.x = _vertexInstanceData.size();

            {
                bool beganDrawing = false;

                size_t y = 0;
                for (const auto& row : _rows)
                {
                    const auto baselineY = p.d.font.cellSizeDIP.y * y + p.s->font->baselineInDIP;
                    f32 cumulativeAdvance = 0;

                    for (const auto& m : row.mappings)
                    {
                        for (auto i = m.glyphsFrom; i < m.glyphsTo; ++i)
                        {
                            bool inserted = false;
                            auto& entry = _glyphCache.FindOrInsert(m.fontFace.get(), row.glyphIndices[i], inserted);
                            if (inserted)
                            {
                                if (!beganDrawing)
                                {
                                    beganDrawing = true;
                                    _d2dRenderTarget->BeginDraw();
                                }

                                _drawGlyph(p, entry, m.fontEmSize);
                            }

                            if (entry.wh != u16x2{})
                            {
                                auto& ref = _vertexInstanceData.emplace_back();
                                ref.rect = {
                                    (cumulativeAdvance + row.glyphOffsets[i].advanceOffset) * p.d.font.pixelPerDIP + entry.offset.x,
                                    (baselineY - row.glyphOffsets[i].ascenderOffset) * p.d.font.pixelPerDIP + entry.offset.y,
                                    static_cast<f32>(entry.wh.x),
                                    static_cast<f32>(entry.wh.y),
                                };
                                ref.tex = {
                                    static_cast<f32>(entry.xy.x),
                                    static_cast<f32>(entry.xy.y),
                                    static_cast<f32>(entry.wh.x),
                                    static_cast<f32>(entry.wh.y),
                                };
                                ref.color = row.colors[i];
                                ref.shadingType = entry.colorGlyph ? 1 : 0;
                            }

                            cumulativeAdvance += row.glyphAdvances[i];
                        }
                    }

                    y++;
                }

                if (beganDrawing)
                {
                    THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
                }
            }

            /*{
                auto it = _metadata.begin();

                for (u16 y = 0; y < p.s->cellCount.y; ++y)
                {
                    for (u16 x = 0; x < p.s->cellCount.x; ++x, ++it)
                    {
                        const auto meta = *it;
                        const auto flags = meta.flags;
                        if (flags == CellFlags::None)
                        {
                            continue;
                        }
                        
                        const auto top = p.s->font->cellSize.y * y;
                        const auto left = p.s->font->cellSize.x * x;

                        auto& ref = _vertexInstanceData.emplace_back();
                        ref.color = meta.colors.x;
                        ref.shadingType = 2;

                        if (WI_IsFlagSet(flags, CellFlags::BorderLeft))
                        {
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top),
                                static_cast<float>(p.s->font->thinLineWidth),
                                static_cast<float>(p.s->font->cellSize.y),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::BorderTop))
                        {
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->thinLineWidth),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::BorderRight))
                        {
                            ref.rect = {
                                static_cast<float>(left + p.s->font->cellSize.x - p.s->font->thinLineWidth),
                                static_cast<float>(top),
                                static_cast<float>(p.s->font->thinLineWidth),
                                static_cast<float>(p.s->font->cellSize.y),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::BorderBottom))
                        {
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top + p.s->font->cellSize.y - p.s->font->thinLineWidth),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->thinLineWidth),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::Underline))
                        {
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top + p.s->font->underlinePos),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->underlineWidth),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::UnderlineDotted))
                        {
                            // TODO
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top + p.s->font->underlinePos),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->underlineWidth),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::UnderlineDouble))
                        {
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top + p.s->font->doubleUnderlinePos.x),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->thinLineWidth),
                            };
                            auto& ref2 = _vertexInstanceData.emplace_back();
                            ref2.color = meta.colors.x;
                            ref2.shadingType = 2;
                            ref2.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top + p.s->font->doubleUnderlinePos.y),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->thinLineWidth),
                            };
                        }
                        if (WI_IsFlagSet(flags, CellFlags::Strikethrough))
                        {
                            ref.rect = {
                                static_cast<float>(left),
                                static_cast<float>(top + p.s->font->strikethroughPos),
                                static_cast<float>(p.s->font->cellSize.x),
                                static_cast<float>(p.s->font->strikethroughWidth),
                            };
                        }
                    }
                }
            }*/

            textRange.y = _vertexInstanceData.size() - textRange.x;
        }

        if (p.cursorRect.non_empty())
        {
            cursorRange.x = _vertexInstanceData.size();

            auto& ref = _vertexInstanceData.emplace_back();
            ref.rect = {
                static_cast<f32>(p.s->font->cellSize.x * p.cursorRect.left),
                static_cast<f32>(p.s->font->cellSize.y * p.cursorRect.top),
                static_cast<f32>(p.s->font->cellSize.x * (p.cursorRect.right - p.cursorRect.left)),
                static_cast<f32>(p.s->font->cellSize.y * (p.cursorRect.bottom - p.cursorRect.top)),
            };

            cursorRange.y = _vertexInstanceData.size() - cursorRange.x;
        }

        // Selection
        /*{
            selectionRange.x = _vertexInstanceData.size();

            size_t y = 0;
            for (const auto& row : _rows)
            {
                if (row.selectionTo > row.selectionFrom)
                {
                    auto& ref = _vertexInstanceData.emplace_back();
                    ref.rect = {
                        static_cast<f32>(p.s->font->cellSize.x * row.selectionFrom),
                        static_cast<f32>(p.s->font->cellSize.y * y),
                        static_cast<f32>(p.s->font->cellSize.x * (row.selectionTo - row.selectionFrom)),
                        static_cast<f32>(p.s->font->cellSize.y),
                    };
                    ref.color = _selectionColor;
                    ref.shadingType = 2;
                }

                y++;
            }

            selectionRange.y = _vertexInstanceData.size() - selectionRange.x;
        }*/
    }

    if constexpr (true)
    {
        ConstBuffer data;
        data.positionScale = { 2.0f / p.s->targetSize.x, -2.0f / p.s->targetSize.y, 1, 1 };
        DWrite_GetGammaRatios(_gamma, data.gammaRatios);
        data.cleartypeEnhancedContrast = _cleartypeEnhancedContrast;
        data.grayscaleEnhancedContrast = _grayscaleEnhancedContrast;
#pragma warning(suppress : 26447) // The function is declared 'noexcept' but calls function '...' which may throw exceptions (f.6).
        _deviceContext->UpdateSubresource(_constantBuffer.get(), 0, nullptr, &data, 0, 0);
    }

    if (_vertexInstanceData.size() > _vertexBuffers1Size)
    {
        const auto totalCellCount = static_cast<size_t>(p.s->cellCount.x) * static_cast<size_t>(p.s->cellCount.y);
        const auto growthSize = _vertexBuffers1Size + _vertexBuffers1Size / 2;
        const auto newSize = std::max(totalCellCount, growthSize);

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = gsl::narrow<UINT>(sizeof(VertexInstanceData) * newSize);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _vertexBuffers[1].put()));

        _vertexBuffers1Size = newSize;
    }

    {
#pragma warning(suppress : 26494) // Variable 'mapped' is uninitialized. Always initialize an object (type.5).
        D3D11_MAPPED_SUBRESOURCE mapped;
        THROW_IF_FAILED(_deviceContext->Map(_perCellColor.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        for (auto i = 0; i < p.s->cellCount.y; ++i)
        {
            memcpy(mapped.pData, p.backgroundBitmap.data() + i * p.s->cellCount.x, p.s->cellCount.x * sizeof(u32));
            mapped.pData = static_cast<void*>(static_cast<std::byte*>(mapped.pData) + mapped.RowPitch);
        }
        _deviceContext->Unmap(_perCellColor.get(), 0);
    }

    {
#pragma warning(suppress : 26494) // Variable 'mapped' is uninitialized. Always initialize an object (type.5).
        D3D11_MAPPED_SUBRESOURCE mapped;
        THROW_IF_FAILED(_deviceContext->Map(_vertexBuffers[1].get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, _vertexInstanceData.data(), _vertexInstanceData.size() * sizeof(VertexInstanceData));
        _deviceContext->Unmap(_vertexBuffers[1].get(), 0);
    }

    {
        {
            // IA: Input Assembler
            static constexpr UINT strides[2]{ sizeof(f32x2), sizeof(VertexInstanceData) };
            static constexpr UINT offsets[2]{ 0, 0 };
            _deviceContext->IASetInputLayout(_textInputLayout.get());
            _deviceContext->IASetVertexBuffers(0, 2, _vertexBuffers[0].addressof(), &strides[0], &offsets[0]);
            _deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // VS: Vertex Shader
            _deviceContext->VSSetShader(_vertexShader.get(), nullptr, 0);
            _deviceContext->VSSetConstantBuffers(0, 1, _constantBuffer.addressof());

            // RS: Rasterizer Stage
            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(p.s->targetSize.x);
            viewport.Height = static_cast<float>(p.s->targetSize.y);
            _deviceContext->RSSetViewports(1, &viewport);
            _deviceContext->RSSetState(_rasterizerState.get());

            // PS: Pixel Shader
            _deviceContext->PSSetShader(_textPixelShader.get(), nullptr, 0);
            _deviceContext->PSSetConstantBuffers(0, 1, _constantBuffer.addressof());
            _deviceContext->PSSetShaderResources(0, 1, _perCellColorView.addressof());

            // OM: Output Merger
            _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);
            _deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);

            _deviceContext->DrawInstanced(6, 1, 0, 0);
        }

        // Inverted cursors use D3D11 Logic Ops with D3D11_LOGIC_OP_XOR (see GH#).
        // But unfortunately this poses two problems:
        // * Cursors are drawn "in between" text and selection
        // * all RenderTargets bound must have a UINT format
        // --> We have to draw in 3 passes.
        if (cursorRange.y)
        {
            _deviceContext->PSSetShader(_textPixelShader.get(), nullptr, 0);
            _deviceContext->PSSetShaderResources(0, 1, _atlasView.addressof());
            _deviceContext->OMSetBlendState(_textBlendState.get(), nullptr, 0xffffffff);
            _deviceContext->DrawInstanced(_instanceCount, gsl::narrow_cast<UINT>(textRange.y), 0, gsl::narrow_cast<UINT>(textRange.x));

            _deviceContext->PSSetShader(_invertCursorPixelShader.get(), nullptr, 0);
            _deviceContext->OMSetRenderTargets(1, _renderTargetViewUInt.addressof(), nullptr);
            _deviceContext->OMSetBlendState(_invertCursorBlendState.get(), nullptr, 0xffffffff);
            _deviceContext->DrawInstanced(_instanceCount, gsl::narrow_cast<UINT>(cursorRange.y), 0, gsl::narrow_cast<UINT>(cursorRange.x));

            if (selectionRange.y)
            {
                _deviceContext->PSSetShader(_textPixelShader.get(), nullptr, 0);
                _deviceContext->PSSetShaderResources(0, 1, _atlasView.addressof());
                _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);
                _deviceContext->OMSetBlendState(_textBlendState.get(), nullptr, 0xffffffff);
                _deviceContext->DrawInstanced(_instanceCount, gsl::narrow_cast<UINT>(selectionRange.y), 0, gsl::narrow_cast<UINT>(selectionRange.x));
            }
        }
        else
        {
            _deviceContext->PSSetShader(_textPixelShader.get(), nullptr, 0);
            _deviceContext->PSSetShaderResources(0, 1, _atlasView.addressof());
            _deviceContext->OMSetBlendState(_textBlendState.get(), nullptr, 0xffffffff);
            _deviceContext->DrawInstanced(_instanceCount, gsl::narrow_cast<UINT>(textRange.y + selectionRange.y), 0, gsl::narrow_cast<UINT>(textRange.x));
        }
    }

    if constexpr (false)
    {
        _deviceContext->RSSetState(_wireframeRasterizerState.get());
        _deviceContext->PSSetShader(_wireframePixelShader.get(), nullptr, 0);
        _deviceContext->OMSetBlendState(_alphaBlendState.get(), nullptr, 0xffffffff);
        _deviceContext->DrawInstanced(_instanceCount, gsl::narrow<UINT>(_vertexInstanceData.size()), 0, 0);
    }

    _swapChainManager.Present(p);
}

bool BackendD3D11::RequiresContinuousRedraw() noexcept
{
    return _requiresContinuousRedraw;
}

void BackendD3D11::WaitUntilCanRender() noexcept
{
    _swapChainManager.WaitUntilCanRender();
}

void BackendD3D11::_drawGlyph(const RenderingPayload& p, GlyphCacheEntry& entry, f32 fontEmSize)
{
    DWRITE_GLYPH_RUN glyphRun{};
    glyphRun.fontFace = entry.fontFace;
    glyphRun.fontEmSize = fontEmSize;
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &entry.glyphIndex;

    auto box = getGlyphRunBlackBox(glyphRun, 0, 0);
    if (box.left >= box.right || box.top >= box.bottom)
    {
        return;
    }

    box.left = roundf(box.left * p.d.font.pixelPerDIP) - 1.0f;
    box.top = roundf(box.top * p.d.font.pixelPerDIP) - 1.0f;
    box.right = roundf(box.right * p.d.font.pixelPerDIP) + 1.0f;
    box.bottom = roundf(box.bottom * p.d.font.pixelPerDIP) + 1.0f;

    stbrp_rect rect{};
    rect.w = gsl::narrow_cast<int>(box.right - box.left);
    rect.h = gsl::narrow_cast<int>(box.bottom - box.top);
    if (!stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        __debugbreak();
        return;
    }

    const D2D1_POINT_2F baseline{
        (rect.x - box.left) * p.d.font.dipPerPixel,
        (rect.y - box.top) * p.d.font.dipPerPixel,
    };
    const auto colorGlyph = _drawGlyphRun(p.dwriteFactory4.get(), _d2dRenderTarget.get(), _d2dRenderTarget4.get(), baseline, &glyphRun, _brush.get());

    entry.xy.x = gsl::narrow_cast<u16>(rect.x);
    entry.xy.y = gsl::narrow_cast<u16>(rect.y);
    entry.wh.x = gsl::narrow_cast<u16>(rect.w);
    entry.wh.y = gsl::narrow_cast<u16>(rect.h);
    entry.offset.x = gsl::narrow_cast<u16>(box.left);
    entry.offset.y = gsl::narrow_cast<u16>(box.top);
    entry.colorGlyph = colorGlyph;
}
