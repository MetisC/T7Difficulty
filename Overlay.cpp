#include "pch.h"
#include "Overlay.h"
#include "safetyhook.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <vector>

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // ========================================================
    // GLOBAL STATE
    // ========================================================

    HMODULE g_ownerModule = nullptr;

    const std::atomic<bool>* g_inOptions = nullptr;
    const std::atomic<int>* g_difficulty = nullptr;
    const std::atomic<int>* g_rounds = nullptr;
    const std::atomic<int>* g_specialFights = nullptr;
    const std::atomic<int>* g_selectedRow = nullptr;

    OverlayLogFn g_log = nullptr;

    safetyhook::InlineHook g_presentHook{};
    safetyhook::InlineHook g_resizeBuffersHook{};

    std::atomic<bool> g_initialized{ false };

    std::mutex g_renderMutex;

    HWND g_gameWindow = nullptr;
    IDXGISwapChain* g_boundSwapChain = nullptr;

    ComPtr<ID3D11Device> g_device;
    ComPtr<ID3D11DeviceContext> g_context;

    ComPtr<ID3D11RenderTargetView> g_backbufferRTV;

    ComPtr<ID3D11VertexShader> g_vertexShader;
    ComPtr<ID3D11PixelShader> g_pixelShader;
    ComPtr<ID3D11InputLayout> g_inputLayout;

    ComPtr<ID3D11BlendState> g_blendState;
    ComPtr<ID3D11RasterizerState> g_rasterizerState;
    ComPtr<ID3D11DepthStencilState> g_depthStencilState;

    ComPtr<ID3D11Buffer> g_vertexBuffer;
    UINT g_vertexCapacity = 0;

    UINT g_backbufferWidth = 0;
    UINT g_backbufferHeight = 0;

    bool g_rendererReadyLogged = false;
    bool g_failureLogged = false;

    // ========================================================
    // LOG
    // ========================================================

    void LogMessage(
        const wchar_t* text
    )
    {
        if (g_log != nullptr)
        {
            g_log(text);
        }
    }

    void LogHRESULT(
        const wchar_t* prefix,
        HRESULT hr
    )
    {
        if (g_log == nullptr)
        {
            return;
        }

        wchar_t buffer[256]{};

        swprintf_s(
            buffer,
            L"%s (HRESULT=0x%08X)",
            prefix,
            static_cast<unsigned int>(
                hr
                )
        );

        g_log(buffer);
    }

    // ========================================================
    // OVERLAY DATA
    // ========================================================

    struct Color
    {
        float r;
        float g;
        float b;
        float a;
    };

    struct Vertex
    {
        float x;
        float y;

        float r;
        float g;
        float b;
        float a;
    };

    struct Glyph
    {
        char c;
        uint8_t rows[7];
    };

    static constexpr Glyph kGlyphs[] =
    {
        { ' ', { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },

        { 'A', { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 } },
        { 'B', { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E } },
        { 'C', { 0x0F,0x10,0x10,0x10,0x10,0x10,0x0F } },
        { 'D', { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E } },
        { 'E', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F } },
        { 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } },
        { 'G', { 0x0F,0x10,0x10,0x17,0x11,0x11,0x0F } },
        { 'H', { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 } },
        { 'I', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x1F } },
        { 'J', { 0x01,0x01,0x01,0x01,0x11,0x11,0x0E } },
        { 'K', { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 } },
        { 'L', { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F } },
        { 'M', { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 } },
        { 'N', { 0x11,0x19,0x15,0x13,0x11,0x11,0x11 } },
        { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } },
        { 'P', { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 } },
        { 'Q', { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D } },
        { 'R', { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 } },
        { 'S', { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E } },
        { 'T', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 } },
        { 'U', { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E } },
        { 'V', { 0x11,0x11,0x11,0x11,0x0A,0x0A,0x04 } },
        { 'W', { 0x11,0x11,0x11,0x15,0x15,0x1B,0x11 } },
        { 'X', { 0x11,0x0A,0x04,0x04,0x04,0x0A,0x11 } },
        { 'Y', { 0x11,0x0A,0x04,0x04,0x04,0x04,0x04 } },
        { 'Z', { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F } },

        { '0', { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E } },
        { '1', { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E } },
        { '2', { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F } },
        { '3', { 0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E } },
        { '4', { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 } },
        { '5', { 0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E } },
        { '6', { 0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E } },
        { '7', { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 } },
        { '8', { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E } },
        { '9', { 0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E } },

        { '/', { 0x01,0x02,0x02,0x04,0x08,0x08,0x10 } },
        { '&', { 0x0C,0x12,0x14,0x08,0x15,0x12,0x0D } },
        { '+', { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 } },
        { '-', { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 } },
        { '<', { 0x02,0x04,0x08,0x10,0x08,0x04,0x02 } },
        { '>', { 0x08,0x04,0x02,0x01,0x02,0x04,0x08 } },
        { ':', { 0x00,0x04,0x04,0x00,0x04,0x04,0x00 } },
        { '?', { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 } }
    };

    const uint8_t* FindGlyph(
        char c
    )
    {
        for (const Glyph& glyph :
            kGlyphs)
        {
            if (glyph.c == c)
            {
                return glyph.rows;
            }
        }

        for (const Glyph& glyph :
            kGlyphs)
        {
            if (glyph.c == '?')
            {
                return glyph.rows;
            }
        }

        return nullptr;
    }

    const char* DifficultyName(
        int difficulty
    )
    {
        switch (difficulty)
        {
        case 0:
            return "DEFAULT";

        case 1:
            return "EASY";

        case 2:
            return "MEDIUM";

        case 3:
            return "HARD";

        case 4:
            return "VERY HARD";

        case 5:
            return "ULTRA HARD";

        default:
            return "UNKNOWN";
        }
    }

    Color DifficultyColor(
        int difficulty
    )
    {
        switch (difficulty)
        {
        case 0:
            return
            {
                0.72f,
                0.74f,
                0.78f,
                1.00f
            };

        case 1:
            return
            {
                0.35f,
                0.90f,
                0.52f,
                1.00f
            };

        case 2:
            return
            {
                0.35f,
                0.72f,
                1.00f,
                1.00f
            };

        case 3:
            return
            {
                1.00f,
                0.64f,
                0.25f,
                1.00f
            };

        case 4:
            return
            {
                1.00f,
                0.30f,
                0.27f,
                1.00f
            };

        case 5:
            return
            {
                1.00f,
                0.82f,
                0.27f,
                1.00f
            };

        default:
            return
            {
                1.00f,
                1.00f,
                1.00f,
                1.00f
            };
        }
    }

    // ========================================================
    // SWAPCHAIN FILTER
    // ========================================================

    bool IsWindowFromCurrentProcess(
        HWND hwnd
    )
    {
        if (hwnd == nullptr ||
            !IsWindow(
                hwnd
            ))
        {
            return false;
        }

        DWORD pid =
            0;

        GetWindowThreadProcessId(
            hwnd,
            &pid
        );

        return
            pid ==
            GetCurrentProcessId();
    }

    bool IsLikelyGameSwapChain(
        IDXGISwapChain* swapChain
    )
    {
        if (swapChain ==
            nullptr)
        {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};

        if (FAILED(
            swapChain->GetDesc(
                &desc
            )
        ))
        {
            return false;
        }

        const HWND hwnd =
            desc.OutputWindow;

        if (!IsWindowFromCurrentProcess(
            hwnd
        ))
        {
            return false;
        }

        if (g_gameWindow != nullptr &&
            IsWindow(
                g_gameWindow
            ))
        {
            return
                hwnd ==
                g_gameWindow;
        }

        if (!IsWindowVisible(
            hwnd
        ))
        {
            return false;
        }

        const LONG_PTR style =
            GetWindowLongPtrW(
                hwnd,
                GWL_STYLE
            );

        if ((style &
            WS_CHILD) != 0)
        {
            return false;
        }

        RECT client{};

        if (!GetClientRect(
            hwnd,
            &client
        ))
        {
            return false;
        }

        const LONG width =
            client.right -
            client.left;

        const LONG height =
            client.bottom -
            client.top;

        if (width < 640 ||
            height < 360)
        {
            return false;
        }

        g_gameWindow =
            hwnd;

        return true;
    }

    // ========================================================
    // DEVICE RESOURCES
    // ========================================================

    void ReleaseBackbufferResourcesLocked()
    {
        g_backbufferRTV.Reset();

        g_boundSwapChain =
            nullptr;

        g_backbufferWidth =
            0;

        g_backbufferHeight =
            0;
    }

    void ReleaseDeviceResourcesLocked()
    {
        ReleaseBackbufferResourcesLocked();

        g_vertexBuffer.Reset();

        g_vertexCapacity =
            0;

        g_depthStencilState.Reset();
        g_rasterizerState.Reset();
        g_blendState.Reset();

        g_inputLayout.Reset();
        g_pixelShader.Reset();
        g_vertexShader.Reset();

        g_context.Reset();
        g_device.Reset();
    }

    HRESULT CompileShader(
        const char* source,
        const char* entryPoint,
        const char* target,
        ID3DBlob** blob
    )
    {
        if (source == nullptr ||
            entryPoint == nullptr ||
            target == nullptr ||
            blob == nullptr)
        {
            return E_INVALIDARG;
        }

        *blob =
            nullptr;

        ComPtr<ID3DBlob> errors;

        const HRESULT hr =
            D3DCompile(
                source,
                std::strlen(
                    source
                ),
                nullptr,
                nullptr,
                nullptr,
                entryPoint,
                target,
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                blob,
                errors.GetAddressOf()
            );

        if (FAILED(
            hr
        ) &&
            errors != nullptr &&
            g_log != nullptr)
        {
            const char* errorText =
                static_cast<const char*>(
                    errors->GetBufferPointer()
                    );

            if (errorText !=
                nullptr)
            {
                wchar_t wide[512]{};

                MultiByteToWideChar(
                    CP_UTF8,
                    0,
                    errorText,
                    -1,
                    wide,
                    static_cast<int>(
                        std::size(
                            wide
                        )
                        )
                );

                g_log(
                    wide
                );
            }
        }

        return hr;
    }

    bool CreateFixedDeviceResourcesLocked()
    {
        if (g_device == nullptr ||
            g_context == nullptr)
        {
            return false;
        }

        static constexpr char shaderSource[] =
            "struct VSInput {"
            "  float2 pos : POSITION;"
            "  float4 color : COLOR0;"
            "};"
            "struct PSInput {"
            "  float4 pos : SV_POSITION;"
            "  float4 color : COLOR0;"
            "};"
            "PSInput VSMain(VSInput input) {"
            "  PSInput output;"
            "  output.pos = float4(input.pos, 0.0, 1.0);"
            "  output.color = input.color;"
            "  return output;"
            "}"
            "float4 PSMain(PSInput input) : SV_TARGET {"
            "  return input.color;"
            "}";

        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;

        HRESULT hr =
            CompileShader(
                shaderSource,
                "VSMain",
                "vs_4_0",
                vsBlob.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: D3D11 overlay vertex shader compile failed",
                hr
            );

            return false;
        }

        hr =
            CompileShader(
                shaderSource,
                "PSMain",
                "ps_4_0",
                psBlob.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: D3D11 overlay pixel shader compile failed",
                hr
            );

            return false;
        }

        hr =
            g_device->CreateVertexShader(
                vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(),
                nullptr,
                g_vertexShader.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: CreateVertexShader failed",
                hr
            );

            return false;
        }

        hr =
            g_device->CreatePixelShader(
                psBlob->GetBufferPointer(),
                psBlob->GetBufferSize(),
                nullptr,
                g_pixelShader.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: CreatePixelShader failed",
                hr
            );

            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC inputElements[] =
        {
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                0,
                D3D11_INPUT_PER_VERTEX_DATA,
                0
            },

            {
                "COLOR",
                0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                0,
                8,
                D3D11_INPUT_PER_VERTEX_DATA,
                0
            }
        };

        hr =
            g_device->CreateInputLayout(
                inputElements,
                static_cast<UINT>(
                    std::size(
                        inputElements
                    )
                    ),
                vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(),
                g_inputLayout.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: CreateInputLayout failed",
                hr
            );

            return false;
        }

        D3D11_BLEND_DESC blendDesc{};

        blendDesc.RenderTarget[0].BlendEnable =
            TRUE;

        blendDesc.RenderTarget[0].SrcBlend =
            D3D11_BLEND_SRC_ALPHA;

        blendDesc.RenderTarget[0].DestBlend =
            D3D11_BLEND_INV_SRC_ALPHA;

        blendDesc.RenderTarget[0].BlendOp =
            D3D11_BLEND_OP_ADD;

        blendDesc.RenderTarget[0].SrcBlendAlpha =
            D3D11_BLEND_ONE;

        blendDesc.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_INV_SRC_ALPHA;

        blendDesc.RenderTarget[0].BlendOpAlpha =
            D3D11_BLEND_OP_ADD;

        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;

        hr =
            g_device->CreateBlendState(
                &blendDesc,
                g_blendState.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: CreateBlendState failed",
                hr
            );

            return false;
        }

        D3D11_RASTERIZER_DESC rasterDesc{};

        rasterDesc.FillMode =
            D3D11_FILL_SOLID;

        rasterDesc.CullMode =
            D3D11_CULL_NONE;

        rasterDesc.FrontCounterClockwise =
            FALSE;

        rasterDesc.DepthBias =
            0;

        rasterDesc.SlopeScaledDepthBias =
            0.0f;

        rasterDesc.DepthBiasClamp =
            0.0f;

        rasterDesc.DepthClipEnable =
            TRUE;

        rasterDesc.ScissorEnable =
            FALSE;

        rasterDesc.MultisampleEnable =
            FALSE;

        rasterDesc.AntialiasedLineEnable =
            FALSE;

        hr =
            g_device->CreateRasterizerState(
                &rasterDesc,
                g_rasterizerState.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: CreateRasterizerState failed",
                hr
            );

            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depthDesc{};

        depthDesc.DepthEnable =
            FALSE;

        depthDesc.DepthWriteMask =
            D3D11_DEPTH_WRITE_MASK_ZERO;

        depthDesc.DepthFunc =
            D3D11_COMPARISON_ALWAYS;

        depthDesc.StencilEnable =
            FALSE;

        hr =
            g_device->CreateDepthStencilState(
                &depthDesc,
                g_depthStencilState.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: CreateDepthStencilState failed",
                hr
            );

            return false;
        }

        return true;
    }

    bool EnsureDeviceResourcesLocked(
        IDXGISwapChain* swapChain
    )
    {
        if (swapChain ==
            nullptr)
        {
            return false;
        }

        ComPtr<ID3D11Device>
            currentDevice;

        HRESULT hr =
            swapChain->GetDevice(
                IID_PPV_ARGS(
                    currentDevice.GetAddressOf()
                )
            );

        if (FAILED(
            hr
        ) ||
            currentDevice == nullptr)
        {
            if (!g_failureLogged)
            {
                LogHRESULT(
                    L"ERROR: Overlay GetDevice failed",
                    hr
                );

                g_failureLogged =
                    true;
            }

            return false;
        }

        if (g_device.Get() !=
            currentDevice.Get())
        {
            ReleaseDeviceResourcesLocked();

            g_device =
                currentDevice;

            g_device->GetImmediateContext(
                g_context.GetAddressOf()
            );

            if (g_context == nullptr ||
                !CreateFixedDeviceResourcesLocked())
            {
                ReleaseDeviceResourcesLocked();

                if (!g_failureLogged)
                {
                    LogMessage(
                        L"ERROR: D3D11 overlay fixed-resource creation failed."
                    );

                    g_failureLogged =
                        true;
                }

                return false;
            }
        }

        if (g_backbufferRTV != nullptr &&
            g_boundSwapChain ==
            swapChain &&
            g_backbufferWidth != 0 &&
            g_backbufferHeight != 0)
        {
            return true;
        }

        ReleaseBackbufferResourcesLocked();

        ComPtr<ID3D11Texture2D>
            backBuffer;

        hr =
            swapChain->GetBuffer(
                0,
                IID_PPV_ARGS(
                    backBuffer.GetAddressOf()
                )
            );

        if (FAILED(
            hr
        ) ||
            backBuffer == nullptr)
        {
            if (!g_failureLogged)
            {
                LogHRESULT(
                    L"ERROR: Overlay GetBuffer failed",
                    hr
                );

                g_failureLogged =
                    true;
            }

            return false;
        }

        D3D11_TEXTURE2D_DESC
            backDesc{};

        backBuffer->GetDesc(
            &backDesc
        );

        hr =
            g_device->CreateRenderTargetView(
                backBuffer.Get(),
                nullptr,
                g_backbufferRTV.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            D3D11_RENDER_TARGET_VIEW_DESC
                rtvDesc{};

            rtvDesc.ViewDimension =
                D3D11_RTV_DIMENSION_TEXTURE2D;

            rtvDesc.Texture2D.MipSlice =
                0;

            switch (backDesc.Format)
            {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:

                rtvDesc.Format =
                    DXGI_FORMAT_R8G8B8A8_UNORM;

                break;

            case DXGI_FORMAT_B8G8R8A8_TYPELESS:

                rtvDesc.Format =
                    DXGI_FORMAT_B8G8R8A8_UNORM;

                break;

            default:

                rtvDesc.Format =
                    backDesc.Format;

                break;
            }

            hr =
                g_device->CreateRenderTargetView(
                    backBuffer.Get(),
                    &rtvDesc,
                    g_backbufferRTV.GetAddressOf()
                );
        }

        if (FAILED(
            hr
        ) ||
            g_backbufferRTV == nullptr)
        {
            if (!g_failureLogged)
            {
                LogHRESULT(
                    L"ERROR: CreateRenderTargetView failed",
                    hr
                );

                g_failureLogged =
                    true;
            }

            return false;
        }

        g_boundSwapChain =
            swapChain;

        g_backbufferWidth =
            backDesc.Width;

        g_backbufferHeight =
            backDesc.Height;

        g_failureLogged =
            false;

        if (!g_rendererReadyLogged)
        {
#ifdef _DEBUG
            wchar_t buffer[256]{};

            swprintf_s(
                buffer,
                L"Overlay renderer ready | %ux%u | format=%u",
                g_backbufferWidth,
                g_backbufferHeight,
                static_cast<unsigned int>(
                    backDesc.Format
                    )
            );

            LogMessage(
                buffer
            );
#endif

            g_rendererReadyLogged =
                true;
        }

        return true;
    }

    bool EnsureVertexBufferLocked(
        UINT vertexCount
    )
    {
        if (g_device == nullptr ||
            vertexCount == 0)
        {
            return false;
        }

        if (g_vertexBuffer != nullptr &&
            g_vertexCapacity >=
            vertexCount)
        {
            return true;
        }

        g_vertexBuffer.Reset();

        UINT newCapacity =
            8192;

        while (newCapacity <
            vertexCount)
        {
            if (newCapacity >
                0x3FFFFFFF)
            {
                return false;
            }

            newCapacity *=
                2;
        }

        D3D11_BUFFER_DESC desc{};

        desc.ByteWidth =
            newCapacity *
            static_cast<UINT>(
                sizeof(
                    Vertex
                    )
                );

        desc.Usage =
            D3D11_USAGE_DYNAMIC;

        desc.BindFlags =
            D3D11_BIND_VERTEX_BUFFER;

        desc.CPUAccessFlags =
            D3D11_CPU_ACCESS_WRITE;

        HRESULT hr =
            g_device->CreateBuffer(
                &desc,
                nullptr,
                g_vertexBuffer.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            LogHRESULT(
                L"ERROR: Overlay CreateBuffer failed",
                hr
            );

            g_vertexCapacity =
                0;

            return false;
        }

        g_vertexCapacity =
            newCapacity;

        return true;
    }

    // ========================================================
    // GEOMETRY
    // ========================================================

    void AddRect(
        std::vector<Vertex>& vertices,
        float screenWidth,
        float screenHeight,
        float x,
        float y,
        float width,
        float height,
        const Color& color
    )
    {
        if (width <= 0.0f ||
            height <= 0.0f ||
            screenWidth <= 0.0f ||
            screenHeight <= 0.0f)
        {
            return;
        }

        const float left =
            (x /
                screenWidth) *
            2.0f -
            1.0f;

        const float right =
            ((x +
                width) /
                screenWidth) *
            2.0f -
            1.0f;

        const float top =
            1.0f -
            (y /
                screenHeight) *
            2.0f;

        const float bottom =
            1.0f -
            ((y +
                height) /
                screenHeight) *
            2.0f;

        const Vertex tl =
        {
            left,
            top,
            color.r,
            color.g,
            color.b,
            color.a
        };

        const Vertex tr =
        {
            right,
            top,
            color.r,
            color.g,
            color.b,
            color.a
        };

        const Vertex br =
        {
            right,
            bottom,
            color.r,
            color.g,
            color.b,
            color.a
        };

        const Vertex bl =
        {
            left,
            bottom,
            color.r,
            color.g,
            color.b,
            color.a
        };

        vertices.push_back(
            tl
        );

        vertices.push_back(
            tr
        );

        vertices.push_back(
            br
        );

        vertices.push_back(
            tl
        );

        vertices.push_back(
            br
        );

        vertices.push_back(
            bl
        );
    }

    void AddText(
        std::vector<Vertex>& vertices,
        float screenWidth,
        float screenHeight,
        float x,
        float y,
        const char* text,
        float pixelSize,
        const Color& color
    )
    {
        if (text == nullptr ||
            pixelSize <= 0.0f)
        {
            return;
        }

        float cursorX =
            x;

        for (const char* p = text;
            *p != '\0';
            ++p)
        {
            const uint8_t* rows =
                FindGlyph(
                    *p
                );

            if (rows != nullptr)
            {
                for (int row = 0;
                    row < 7;
                    ++row)
                {
                    const uint8_t bits =
                        rows[row];

                    for (int col = 0;
                        col < 5;
                        ++col)
                    {
                        const uint8_t mask =
                            static_cast<uint8_t>(
                                1u <<
                                (4 - col)
                                );

                        if ((bits &
                            mask) != 0)
                        {
                            AddRect(
                                vertices,
                                screenWidth,
                                screenHeight,
                                cursorX +
                                static_cast<float>(
                                    col
                                    ) *
                                pixelSize,
                                y +
                                static_cast<float>(
                                    row
                                    ) *
                                pixelSize,
                                pixelSize,
                                pixelSize,
                                color
                            );
                        }
                    }
                }
            }

            cursorX +=
                pixelSize *
                6.0f;
        }
    }

    float TextWidth(
        const char* text,
        float pixelSize
    )
    {
        if (text ==
            nullptr)
        {
            return 0.0f;
        }

        return
            static_cast<float>(
                std::strlen(
                    text
                )
                ) *
            pixelSize *
            6.0f;
    }

    void BuildOverlayVertices(
        std::vector<Vertex>& vertices,
        int difficulty
    )
    {
        vertices.clear();

        const float screenWidth =
            static_cast<float>(
                g_backbufferWidth
                );

        const float screenHeight =
            static_cast<float>(
                g_backbufferHeight
                );

        if (screenWidth < 500.0f ||
            screenHeight < 260.0f)
        {
            return;
        }

        const float margin =
            24.0f;

        const float boxWidth =
            (std::min)(
                540.0f,
                screenWidth -
                margin *
                2.0f
                );

        const float left = screenWidth - margin - boxWidth;
        const float top = margin;
        const Color background{0.035f,0.045f,0.065f,0.90f};
        const Color text{1,1,1,0.98f};
        const Color muted{0.7f,0.7f,0.7f,1};
        const Color accent = DifficultyColor(difficulty);
        AddRect(vertices, screenWidth, screenHeight, left, top, boxWidth, 205, background);
        const int selected = g_selectedRow->load();
        const int rounds = g_rounds->load();
        const int special = g_specialFights->load();
        const char* roundNames[] = {"DEFAULT","1","2","3","4","5"};
        const char* specialNames[] = {"VANILLA","NO TURBO","NO DOUBLE DAMAGE","NO TURBO & DOUBLE DAMAGE","DISABLED"};
        const char* labels[] = {"CPU DIFFICULTY", "ROUNDS", "SPECIAL BATTLES"};
        const char* values[] = {DifficultyName(difficulty), roundNames[rounds >= 0 && rounds <= 5 ? rounds : 0],
            specialNames[special >= 0 && special <= 4 ? special : 0]};
        for (int row = 0; row < 3; ++row)
        {
            const float y = top + 15 + row * 48.0f;
            if (row == selected) AddRect(vertices, screenWidth, screenHeight, left + 8, y, 4, 36, accent);
            AddText(vertices, screenWidth, screenHeight, left + 24, y, labels[row], 2, muted);
            AddText(vertices, screenWidth, screenHeight, left + 24, y + 18, values[row], 2, row == selected ? accent : text);
        }
        AddText(vertices, screenWidth, screenHeight, left + 24, top + 164, "L1 / Z < VALUE > R1 / X", 2, text);
        AddText(vertices, screenWidth, screenHeight, left + 24, top + 184, "L3 / TAB: SELECT ROW", 2, muted);
    }

    // ========================================================
    // PIPELINE STATE BACKUP
    // ========================================================

    struct PipelineBackup
    {
        // IMPORTANT:
        //
        // D3D11 has up to 8 simultaneous render targets.
        //
        // The previous overlay saved only RT0. Calling
        // OMSetRenderTargets(1, ...) for our overlay clears RT1..RT7.
        // Restoring only RT0 could therefore leave UE4's OM state
        // damaged after drawing.
        //
        // Save and restore all eight slots.

        ComPtr<ID3D11RenderTargetView>
            renderTargets[
                D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
            ];

        ComPtr<ID3D11DepthStencilView>
            depthStencilView;

        ComPtr<ID3D11BlendState>
            blendState;

        FLOAT blendFactor[4]{};

        UINT sampleMask =
            0xFFFFFFFFu;

        ComPtr<ID3D11DepthStencilState>
            depthStencilState;

        UINT stencilRef =
            0;

        ComPtr<ID3D11RasterizerState>
            rasterizerState;

        D3D11_VIEWPORT viewports[
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
        ]{};

            UINT viewportCount =
                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

            ComPtr<ID3D11InputLayout>
                inputLayout;

            ComPtr<ID3D11Buffer>
                vertexBuffer;

            UINT vertexStride =
                0;

            UINT vertexOffset =
                0;

            D3D11_PRIMITIVE_TOPOLOGY topology =
                D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

            ComPtr<ID3D11VertexShader>
                vs;

            ComPtr<ID3D11PixelShader>
                ps;

            ComPtr<ID3D11GeometryShader>
                gs;

            ComPtr<ID3D11HullShader>
                hs;

            ComPtr<ID3D11DomainShader>
                ds;

            void Capture(
                ID3D11DeviceContext* context
            )
            {
                ID3D11RenderTargetView*
                    currentRTVs[
                        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
                    ]{};

                        context->OMGetRenderTargets(
                            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                            currentRTVs,
                            depthStencilView.GetAddressOf()
                        );

                        for (UINT i = 0;
                            i <
                            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
                            ++i)
                        {
                            if (currentRTVs[i] !=
                                nullptr)
                            {
                                // OMGetRenderTargets returns an AddRef'd pointer.
                                // Attach adopts that reference without adding
                                // another one.
                                renderTargets[i].Attach(
                                    currentRTVs[i]
                                );
                            }
                        }

                        context->OMGetBlendState(
                            blendState.GetAddressOf(),
                            blendFactor,
                            &sampleMask
                        );

                        context->OMGetDepthStencilState(
                            depthStencilState.GetAddressOf(),
                            &stencilRef
                        );

                        context->RSGetState(
                            rasterizerState.GetAddressOf()
                        );

                        context->RSGetViewports(
                            &viewportCount,
                            viewports
                        );

                        context->IAGetInputLayout(
                            inputLayout.GetAddressOf()
                        );

                        context->IAGetVertexBuffers(
                            0,
                            1,
                            vertexBuffer.GetAddressOf(),
                            &vertexStride,
                            &vertexOffset
                        );

                        context->IAGetPrimitiveTopology(
                            &topology
                        );

                        context->VSGetShader(
                            vs.GetAddressOf(),
                            nullptr,
                            nullptr
                        );

                        context->PSGetShader(
                            ps.GetAddressOf(),
                            nullptr,
                            nullptr
                        );

                        context->GSGetShader(
                            gs.GetAddressOf(),
                            nullptr,
                            nullptr
                        );

                        context->HSGetShader(
                            hs.GetAddressOf(),
                            nullptr,
                            nullptr
                        );

                        context->DSGetShader(
                            ds.GetAddressOf(),
                            nullptr,
                            nullptr
                        );
            }

            void Restore(
                ID3D11DeviceContext* context
            )
            {
                ID3D11RenderTargetView*
                    restoredRTVs[
                        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
                    ]{};

                        for (UINT i = 0;
                            i <
                            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
                            ++i)
                        {
                            restoredRTVs[i] =
                                renderTargets[i].Get();
                        }

                        context->OMSetRenderTargets(
                            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                            restoredRTVs,
                            depthStencilView.Get()
                        );

                        context->OMSetBlendState(
                            blendState.Get(),
                            blendFactor,
                            sampleMask
                        );

                        context->OMSetDepthStencilState(
                            depthStencilState.Get(),
                            stencilRef
                        );

                        context->RSSetState(
                            rasterizerState.Get()
                        );

                        if (viewportCount >
                            0)
                        {
                            context->RSSetViewports(
                                viewportCount,
                                viewports
                            );
                        }

                        context->IASetInputLayout(
                            inputLayout.Get()
                        );

                        ID3D11Buffer* vb =
                            vertexBuffer.Get();

                        context->IASetVertexBuffers(
                            0,
                            1,
                            &vb,
                            &vertexStride,
                            &vertexOffset
                        );

                        context->IASetPrimitiveTopology(
                            topology
                        );

                        context->VSSetShader(
                            vs.Get(),
                            nullptr,
                            0
                        );

                        context->PSSetShader(
                            ps.Get(),
                            nullptr,
                            0
                        );

                        context->GSSetShader(
                            gs.Get(),
                            nullptr,
                            0
                        );

                        context->HSSetShader(
                            hs.Get(),
                            nullptr,
                            0
                        );

                        context->DSSetShader(
                            ds.Get(),
                            nullptr,
                            0
                        );
            }
    };

    // ========================================================
    // DRAW
    // ========================================================

    void DrawOverlayLocked(
        IDXGISwapChain* swapChain
    )
    {
        if (g_difficulty == nullptr ||
            !EnsureDeviceResourcesLocked(
                swapChain
            ))
        {
            return;
        }

        const int difficulty =
            g_difficulty->load(
                std::memory_order_relaxed
            );

        std::vector<Vertex>
            vertices;

        BuildOverlayVertices(
            vertices,
            difficulty
        );

        if (vertices.empty())
        {
            return;
        }

        if (vertices.size() >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
                ))
        {
            return;
        }

        const UINT vertexCount =
            static_cast<UINT>(
                vertices.size()
                );

        if (!EnsureVertexBufferLocked(
            vertexCount
        ))
        {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE
            mapped{};

        HRESULT hr =
            g_context->Map(
                g_vertexBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped
            );

        if (FAILED(
            hr
        ))
        {
            if (!g_failureLogged)
            {
                LogHRESULT(
                    L"ERROR: Overlay vertex-buffer Map failed",
                    hr
                );

                g_failureLogged =
                    true;
            }

            return;
        }

        std::memcpy(
            mapped.pData,
            vertices.data(),
            vertices.size() *
            sizeof(
                Vertex
                )
        );

        g_context->Unmap(
            g_vertexBuffer.Get(),
            0
        );

        PipelineBackup backup;

        backup.Capture(
            g_context.Get()
        );

        ID3D11RenderTargetView* rtv =
            g_backbufferRTV.Get();

        g_context->OMSetRenderTargets(
            1,
            &rtv,
            nullptr
        );

        const FLOAT blendFactor[4] =
        {
            0.0f,
            0.0f,
            0.0f,
            0.0f
        };

        g_context->OMSetBlendState(
            g_blendState.Get(),
            blendFactor,
            0xFFFFFFFFu
        );

        g_context->OMSetDepthStencilState(
            g_depthStencilState.Get(),
            0
        );

        g_context->RSSetState(
            g_rasterizerState.Get()
        );

        D3D11_VIEWPORT viewport{};

        viewport.TopLeftX =
            0.0f;

        viewport.TopLeftY =
            0.0f;

        viewport.Width =
            static_cast<float>(
                g_backbufferWidth
                );

        viewport.Height =
            static_cast<float>(
                g_backbufferHeight
                );

        viewport.MinDepth =
            0.0f;

        viewport.MaxDepth =
            1.0f;

        g_context->RSSetViewports(
            1,
            &viewport
        );

        const UINT stride =
            static_cast<UINT>(
                sizeof(
                    Vertex
                    )
                );

        const UINT offset =
            0;

        ID3D11Buffer* vb =
            g_vertexBuffer.Get();

        g_context->IASetInputLayout(
            g_inputLayout.Get()
        );

        g_context->IASetVertexBuffers(
            0,
            1,
            &vb,
            &stride,
            &offset
        );

        g_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
        );

        g_context->VSSetShader(
            g_vertexShader.Get(),
            nullptr,
            0
        );

        g_context->PSSetShader(
            g_pixelShader.Get(),
            nullptr,
            0
        );

        g_context->GSSetShader(
            nullptr,
            nullptr,
            0
        );

        g_context->HSSetShader(
            nullptr,
            nullptr,
            0
        );

        g_context->DSSetShader(
            nullptr,
            nullptr,
            0
        );

        g_context->Draw(
            vertexCount,
            0
        );

        backup.Restore(
            g_context.Get()
        );

        g_failureLogged =
            false;
    }

    // ========================================================
    // DXGI HOOKS
    // ========================================================

    HRESULT STDMETHODCALLTYPE PresentDetour(
        IDXGISwapChain* swapChain,
        UINT syncInterval,
        UINT flags
    )
    {
        if (g_initialized.load(
            std::memory_order_relaxed
        ) &&
            g_inOptions != nullptr &&
            g_inOptions->load(
                std::memory_order_relaxed
            ))
        {
            std::scoped_lock lock(
                g_renderMutex
            );

            if (IsLikelyGameSwapChain(
                swapChain
            ))
            {
                DrawOverlayLocked(
                    swapChain
                );
            }
        }

        return
            g_presentHook.call<HRESULT>(
                swapChain,
                syncInterval,
                flags
            );
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(
        IDXGISwapChain* swapChain,
        UINT bufferCount,
        UINT width,
        UINT height,
        DXGI_FORMAT newFormat,
        UINT swapChainFlags
    )
    {
        {
            std::scoped_lock lock(
                g_renderMutex
            );

            if (swapChain ==
                g_boundSwapChain)
            {
                ReleaseBackbufferResourcesLocked();
            }
        }

        return
            g_resizeBuffersHook.call<HRESULT>(
                swapChain,
                bufferCount,
                width,
                height,
                newFormat,
                swapChainFlags
            );
    }

    // ========================================================
    // RESOLVE GLOBAL DXGI METHODS
    // ========================================================

    bool ResolveDxgiHookTargets(
        void*& presentAddress,
        void*& resizeBuffersAddress
    )
    {
        presentAddress =
            nullptr;

        resizeBuffersAddress =
            nullptr;

        static constexpr wchar_t className[] =
            L"T7Difficulty_DummyDXGIWindow";

        WNDCLASSEXW wc{};

        wc.cbSize =
            sizeof(
                wc
                );

        wc.lpfnWndProc =
            DefWindowProcW;

        wc.hInstance =
            g_ownerModule;

        wc.lpszClassName =
            className;

        ATOM atom =
            RegisterClassExW(
                &wc
            );

        if (atom == 0 &&
            GetLastError() !=
            ERROR_CLASS_ALREADY_EXISTS)
        {
            LogMessage(
                L"ERROR: Overlay dummy-window class registration failed."
            );

            return false;
        }

        HWND dummyWindow =
            CreateWindowExW(
                0,
                className,
                L"",
                WS_OVERLAPPEDWINDOW,
                0,
                0,
                64,
                64,
                nullptr,
                nullptr,
                g_ownerModule,
                nullptr
            );

        if (dummyWindow ==
            nullptr)
        {
            LogMessage(
                L"ERROR: Overlay dummy-window creation failed."
            );

            if (atom !=
                0)
            {
                UnregisterClassW(
                    className,
                    g_ownerModule
                );
            }

            return false;
        }

        DXGI_SWAP_CHAIN_DESC
            swapDesc{};

        swapDesc.BufferDesc.Width =
            64;

        swapDesc.BufferDesc.Height =
            64;

        swapDesc.BufferDesc.RefreshRate.Numerator =
            60;

        swapDesc.BufferDesc.RefreshRate.Denominator =
            1;

        swapDesc.BufferDesc.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;

        swapDesc.SampleDesc.Count =
            1;

        swapDesc.SampleDesc.Quality =
            0;

        swapDesc.BufferUsage =
            DXGI_USAGE_RENDER_TARGET_OUTPUT;

        swapDesc.BufferCount =
            1;

        swapDesc.OutputWindow =
            dummyWindow;

        swapDesc.Windowed =
            TRUE;

        swapDesc.SwapEffect =
            DXGI_SWAP_EFFECT_DISCARD;

        static constexpr D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        ComPtr<IDXGISwapChain>
            dummySwapChain;

        ComPtr<ID3D11Device>
            dummyDevice;

        ComPtr<ID3D11DeviceContext>
            dummyContext;

        D3D_FEATURE_LEVEL
            createdFeatureLevel{};

        HRESULT hr =
            D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                0,
                featureLevels,
                static_cast<UINT>(
                    std::size(
                        featureLevels
                    )
                    ),
                D3D11_SDK_VERSION,
                &swapDesc,
                dummySwapChain.GetAddressOf(),
                dummyDevice.GetAddressOf(),
                &createdFeatureLevel,
                dummyContext.GetAddressOf()
            );

        if (FAILED(
            hr
        ))
        {
            hr =
                D3D11CreateDeviceAndSwapChain(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    0,
                    featureLevels,
                    static_cast<UINT>(
                        std::size(
                            featureLevels
                        )
                        ),
                    D3D11_SDK_VERSION,
                    &swapDesc,
                    dummySwapChain.GetAddressOf(),
                    dummyDevice.GetAddressOf(),
                    &createdFeatureLevel,
                    dummyContext.GetAddressOf()
                );
        }

        if (FAILED(
            hr
        ) ||
            dummySwapChain ==
            nullptr)
        {
            LogHRESULT(
                L"ERROR: Dummy D3D11 swapchain creation failed",
                hr
            );

            DestroyWindow(
                dummyWindow
            );

            if (atom !=
                0)
            {
                UnregisterClassW(
                    className,
                    g_ownerModule
                );
            }

            return false;
        }

        void** vtable =
            *reinterpret_cast<void***>(
                dummySwapChain.Get()
                );

        presentAddress =
            vtable[8];

        resizeBuffersAddress =
            vtable[13];

        dummyContext.Reset();
        dummyDevice.Reset();
        dummySwapChain.Reset();

        DestroyWindow(
            dummyWindow
        );

        if (atom !=
            0)
        {
            UnregisterClassW(
                className,
                g_ownerModule
            );
        }

        return
            presentAddress != nullptr &&
            resizeBuffersAddress != nullptr;
    }
}

// ============================================================
// PUBLIC API
// ============================================================

bool OverlayInitialize(
    HMODULE ownerModule,
    const std::atomic<bool>* inOptions,
    const std::atomic<int>* difficulty,
    const std::atomic<int>* rounds,
    const std::atomic<int>* specialFights,
    const std::atomic<int>* selectedRow,
    OverlayLogFn logFn
)
{
    if (g_initialized.load(
        std::memory_order_relaxed
    ))
    {
        return true;
    }

    g_ownerModule =
        ownerModule;

    g_inOptions =
        inOptions;

    g_difficulty =
        difficulty;

    g_rounds = rounds; g_specialFights = specialFights; g_selectedRow = selectedRow;
    g_log =
        logFn;

    if (g_ownerModule == nullptr ||
        g_inOptions == nullptr ||
        g_difficulty == nullptr || rounds == nullptr || specialFights == nullptr || selectedRow == nullptr)
    {
        LogMessage(
            L"ERROR: OverlayInitialize received invalid state."
        );

        return false;
    }

    void* presentAddress =
        nullptr;

    void* resizeBuffersAddress =
        nullptr;

    if (!ResolveDxgiHookTargets(
        presentAddress,
        resizeBuffersAddress
    ))
    {
        return false;
    }

    g_resizeBuffersHook =
        safetyhook::create_inline(
            resizeBuffersAddress,
            reinterpret_cast<void*>(
                ResizeBuffersDetour
                )
        );

    if (!g_resizeBuffersHook)
    {
        LogMessage(
            L"ERROR: ResizeBuffers hook FAILED."
        );

        return false;
    }

    g_presentHook =
        safetyhook::create_inline(
            presentAddress,
            reinterpret_cast<void*>(
                PresentDetour
                )
        );

    if (!g_presentHook)
    {
        LogMessage(
            L"ERROR: Present hook FAILED."
        );

        g_resizeBuffersHook.reset();

        return false;
    }

    g_initialized.store(
        true,
        std::memory_order_relaxed
    );

#ifdef _DEBUG
    LogMessage(
        L"Overlay renderer: native D3D11."
    );
#endif

    return true;
}

void OverlayShutdown()
{
    if (!g_initialized.exchange(
        false,
        std::memory_order_relaxed
    ))
    {
        return;
    }

    g_presentHook.reset();
    g_resizeBuffersHook.reset();

    {
        std::scoped_lock lock(
            g_renderMutex
        );

        ReleaseDeviceResourcesLocked();

        g_gameWindow =
            nullptr;
    }

    g_ownerModule =
        nullptr;

    g_inOptions =
        nullptr;

    g_difficulty =
        nullptr;

    g_log =
        nullptr;
}