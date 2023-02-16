// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AtlasEngine.h"

#include "Backend.h"
#include "BackendD2D.h"
#include "BackendD3D11.h"
#include "dwrite.h"

// #### NOTE ####
// If you see any code in here that contains "_api." you might be seeing a race condition.
// The AtlasEngine::Present() method is called on a background thread without any locks,
// while any of the API methods (like AtlasEngine::Invalidate) might be called concurrently.
// The usage of the _r field is safe as its members are in practice
// only ever written to by the caller of Present() (the "Renderer" class).
// The _api fields on the other hand are concurrently written to by others.

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4127)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

// https://en.wikipedia.org/wiki/Inversion_list
template<size_t N>
constexpr bool isInInversionList(const std::array<wchar_t, N>& ranges, wchar_t needle)
{
    const auto beg = ranges.begin();
    const auto end = ranges.end();
    decltype(ranges.begin()) it;

    // Linear search is faster than binary search for short inputs.
    if constexpr (N < 16)
    {
        it = std::find_if(beg, end, [=](wchar_t v) { return needle < v; });
    }
    else
    {
        it = std::upper_bound(beg, end, needle);
    }

    const auto idx = it - beg;
    return (idx & 1) != 0;
}

#pragma region IRenderEngine

// Present() is called without the console buffer lock being held.
// --> Put as much in here as possible.
[[nodiscard]] HRESULT AtlasEngine::Present() noexcept
try
{
    if (!_b)
    {
        _recreateBackend();
    }

    const til::rect fullRect{ 0, 0, _p.s->cellCount.x, _p.s->cellCount.y };

    _b->Render(_p);

    if (!_p.dxgiFactory->IsCurrent())
    {
        _b.reset();
    }

    return S_OK;
}
catch (const wil::ResultException& exception)
{
    // TODO: this writes to _api.
    return _handleException(exception);
}
CATCH_RETURN()

[[nodiscard]] bool AtlasEngine::RequiresContinuousRedraw() noexcept
{
    return debugGeneralPerformance || (_b && _b->RequiresContinuousRedraw());
}

void AtlasEngine::WaitUntilCanRender() noexcept
{
    if (_b)
    {
        _b->WaitUntilCanRender();
    }
}

#pragma endregion

void AtlasEngine::_recreateBackend()
{
#if !defined(NDEBUG)
    // DXGIGetDebugInterface1 returns E_NOINTERFACE on systems without the Windows SDK installed.
    if (wil::com_ptr<IDXGIInfoQueue> infoQueue; SUCCEEDED_LOG(DXGIGetDebugInterface1(0, IID_PPV_ARGS(infoQueue.addressof()))))
    {
        // I didn't want to link with dxguid.lib just for getting DXGI_DEBUG_ALL. This GUID is publicly documented.
        static constexpr GUID dxgiDebugAll{ 0xe48ae283, 0xda80, 0x490b, { 0x87, 0xe6, 0x43, 0xe9, 0xa9, 0xcf, 0xda, 0x8 } };
        for (const auto severity : std::array{ DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING })
        {
            infoQueue->SetBreakOnSeverity(dxgiDebugAll, severity, true);
        }
    }
#endif

#if defined(NDEBUG)
    static constexpr UINT flags = 0;
#else
    static constexpr UINT flags = DXGI_CREATE_FACTORY_DEBUG;
#endif

    THROW_IF_FAILED(CreateDXGIFactory2(flags, __uuidof(IDXGIFactory3), _p.dxgiFactory.put_void()));

    auto d2dMode = debugForceD2DMode;
    auto deviceFlags = D3D11_CREATE_DEVICE_SINGLETHREADED
#if !defined(NDEBUG)
                       | D3D11_CREATE_DEVICE_DEBUG
#endif
                       // This flag prevents the driver from creating a large thread pool for things like shader computations
                       // that would be advantageous for games. For us this has only a minimal performance benefit,
                       // but comes with a large memory usage overhead. At the time of writing the Nvidia
                       // driver launches $cpu_thread_count more worker threads without this flag.
                       | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS
                       // Direct2D support.
                       | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    wil::com_ptr<IDXGIAdapter1> dxgiAdapter;
    THROW_IF_FAILED(_p.dxgiFactory->EnumAdapters1(0, dxgiAdapter.addressof()));

    {
        auto findSoftwareAdapter = _p.s->target->useSoftwareRendering;
        auto adapter = dxgiAdapter;
        UINT i = 0;

        for (;;)
        {
            DXGI_ADAPTER_DESC1 desc;
            THROW_IF_FAILED(adapter->GetDesc1(&desc));

            // Switch to D2D mode if any adapter is a remote adapter (RDP).
            d2dMode |= WI_IsFlagSet(desc.Flags, DXGI_ADAPTER_FLAG_REMOTE);

            // If useSoftwareRendering is true we search for the first WARP adapter.
            if (findSoftwareAdapter && WI_IsFlagSet(desc.Flags, DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                WI_ClearFlag(deviceFlags, D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS);
                dxgiAdapter = std::move(adapter);
                findSoftwareAdapter = false;
            }

            ++i;
            if (_p.dxgiFactory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
        }
    }

    wil::com_ptr<ID3D11Device> device0;
    wil::com_ptr<ID3D11DeviceContext> deviceContext0;
    D3D_FEATURE_LEVEL featureLevel{};

    static constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };

    THROW_IF_FAILED(D3D11CreateDevice(
        /* pAdapter */ dxgiAdapter.get(),
        /* DriverType */ D3D_DRIVER_TYPE_UNKNOWN,
        /* Software */ nullptr,
        /* Flags */ deviceFlags,
        /* pFeatureLevels */ featureLevels.data(),
        /* FeatureLevels */ gsl::narrow_cast<UINT>(featureLevels.size()),
        /* SDKVersion */ D3D11_SDK_VERSION,
        /* ppDevice */ device0.put(),
        /* pFeatureLevel */ &featureLevel,
        /* ppImmediateContext */ deviceContext0.put()));

    const auto device = device0.query<ID3D11Device2>();
    const auto deviceContext = deviceContext0.query<ID3D11DeviceContext2>();

    if (featureLevel < D3D_FEATURE_LEVEL_10_0)
    {
        d2dMode = true;
    }
    else if (featureLevel < D3D_FEATURE_LEVEL_11_0)
    {
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS options;
        THROW_IF_FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &options, sizeof(options)));
        if (!options.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
        {
            d2dMode = true;
        }
    }

    if (d2dMode)
    {
        _b = std::make_unique<BackendD2D>(std::move(device), std::move(deviceContext));
    }
    else
    {
        _b = std::make_unique<BackendD3D11>(std::move(device), std::move(deviceContext));
    }
}
