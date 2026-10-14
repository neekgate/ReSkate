#include "filmer_look.h"
#include "filmer.h"
#include "filmer_look_hlsl.h" // generated from filmer_look.hlsl (cmake/UI.cmake)
#include "Engine/Core/Log/logging.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <numbers>
#include <string>

namespace dingosdk::filmer::look {
namespace {
using Microsoft::WRL::ComPtr;

// One full-screen triangle; the pixel shader does everything. Positions are in half the frame's
// height from its centre (q), so the lens maths reads the same at any resolution.
constexpr const char *shader_source = filmer_look_hlsl;

struct Graphics {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    DXGI_FORMAT pipeline_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3D12Resource> copy;
    D3D12_RESOURCE_DESC copy_desc{};
    D3D12_RESOURCE_STATES copy_state{D3D12_RESOURCE_STATE_COPY_DEST};
    ID3D12DescriptorHeap *view_heap{};
    unsigned view_slot{~0u};
    bool failed{};
};
Graphics &graphics() {
    static auto *value = new Graphics;
    return *value;
}

void say_failed(const std::string &why) {
    auto &g = graphics();
    if (g.failed) return;
    g.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics, "Filmer look unavailable: " + why + " The Filmer still films, without the VX look.");
}

bool build_root(ID3D12Device *device) {
    auto &g = graphics();
    if (g.root) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = 0;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 24;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = 0.0f;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob) {
        say_failed("its root signature could not be built.");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        say_failed("its root signature was refused.");
        return false;
    }
    return true;
}

bool build_pipeline(ID3D12Device *device, DXGI_FORMAT format) {
    auto &g = graphics();
    if (g.pipeline && g.pipeline_format == format) return true;
    g.pipeline.Reset();
    if (!build_root(device)) return false;
    ComPtr<ID3DBlob> vertex, pixel, error;
    const auto length = std::strlen(shader_source);
    if (FAILED(D3DCompile(shader_source, length, "filmer_look", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vertex, &error)) || !vertex) {
        say_failed(std::string("its vertex shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char *>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    error.Reset();
    if (FAILED(D3DCompile(shader_source, length, "filmer_look", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &pixel, &error)) || !pixel) {
        say_failed(std::string("its pixel shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char *>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.StencilEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    description.NodeMask = 0;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.pipeline)))) {
        say_failed("its pipeline was refused for this swapchain format.");
        return false;
    }
    g.pipeline_format = format;
    return true;
}

// The copy of the frame the shader reads, the size and format of the back buffer.
bool ensure_copy(const Target &t) {
    auto &g = graphics();
    auto wanted = t.buffer->GetDesc();
    wanted.Flags = D3D12_RESOURCE_FLAG_NONE;
    wanted.Alignment = 0;
    wanted.MipLevels = 1;
    wanted.DepthOrArraySize = 1;
    wanted.SampleDesc = {1, 0};
    wanted.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const bool same = g.copy && g.copy_desc.Width == wanted.Width && g.copy_desc.Height == wanted.Height &&
                      g.copy_desc.Format == wanted.Format;
    if (!same) {
        // Frames still on the GPU may read the copy this one replaces.
        if (g.copy && (!t.wait_idle || !t.wait_idle())) return false;
        g.copy.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &wanted, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g.copy)))) {
            say_failed("the copy of the frame could not be made.");
            return false;
        }
        g.copy->SetName(L"ReSkate Filmer frame copy");
        g.copy_desc = wanted;
        g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g.view_heap = nullptr;
    }
    // Its view, in the overlay's heap (a new heap after the overlay rebuilt its graphics).
    if (g.view_heap != t.srvs || g.view_slot != t.slot) {
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = t.srvs->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(step) * t.slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = wanted.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        t.device->CreateShaderResourceView(g.copy.Get(), &view, cpu);
        g.view_heap = t.srvs;
        g.view_slot = t.slot;
    }
    return true;
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

// The hand's path through a slap: in from one side, over the lens, off the other way.
struct Hand { float x{}, y{}, angle{}, on{}, size{2.8f}; bool mirrored{}; }; // big: it fills the lens
Hand hand_at(double since, std::uint64_t serial) {
    Hand hand;
    if (since < 0.0 || since > 0.75) return hand;
    const float t = static_cast<float>(since / 0.75);
    hand.mirrored = (serial & 1u) != 0;
    const float side = hand.mirrored ? -1.0f : 1.0f;
    const auto ease_out = [](float v) { return 1.0f - (1.0f - v) * (1.0f - v); };
    const auto ease_in = [](float v) { return v * v; };
    // Picture coordinates: half heights from the centre, y down. In from low on one side, a moment
    // over the lens, off across and up the other way.
    if (t < 0.3f) {
        const float u = ease_out(t / 0.3f);
        hand.x = side * (3.2f - 3.2f * u);
        hand.y = 1.4f - 1.95f * u;
        hand.angle = side * (0.55f - 0.4f * u);
    } else if (t < 0.55f) {
        const float u = (t - 0.3f) / 0.25f;
        hand.x = side * (-0.2f * u);
        hand.y = -0.55f - 0.1f * u;
        hand.angle = side * (0.15f - 0.1f * u);
    } else {
        const float u = ease_in((t - 0.55f) / 0.45f);
        hand.x = side * (-0.2f - 3.4f * u);
        hand.y = -0.65f - 0.9f * u;
        hand.angle = side * (0.05f - 0.5f * u);
    }
    hand.on = 1.0f;
    return hand;
}

// The camcorder's iris: closing down while the hand is over the lens, overshooting when it comes off.
float exposure_at(double since) {
    if (since < 0.0 || since > 1.6) return 1.0f;
    const float t = static_cast<float>(since);
    if (t < 0.4f) return 1.0f - 0.35f * std::sin(std::min(t / 0.4f, 1.0f) * std::numbers::pi_v<float> * 0.5f);
    return 1.0f + 0.35f * std::exp(-(t - 0.4f) * 4.0f) * std::sin(std::min((t - 0.4f) / 0.15f, 1.0f) * std::numbers::pi_v<float> * 0.5f);
}
} // namespace

bool pending() noexcept {
    const auto v = view();
    return v && v->active && v->settings.look.enabled;
}

bool record(const Target &t) noexcept {
    try {
        auto &g = graphics();
        const auto v = view();
        if (!v || !v->active || !v->settings.look.enabled || g.failed) return false;
        if (!t.device || !t.commands || !t.buffer || !t.srvs) return false;
        if (!build_pipeline(t.device, t.format) || !ensure_copy(t)) return false;
        const auto &look = v->settings.look;
        const auto &rig = v->settings.rig;
        const auto desc = t.buffer->GetDesc();
        const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
        if (width < 16.0f || height < 16.0f) return false;
        // Copy the frame, then draw it back through the shader.
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(t.commands, g.copy.Get(), g.copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
        t.commands->CopyResource(g.copy.Get(), t.buffer);
        transition(t.commands, g.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g.copy_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const double seconds = now();
        const double since = seconds - v->slap_at;
        const Hand hand = hand_at(since, v->slap_serial);
        const bool fish = rig.lens == Lens::fisheye && look.fisheye;
        const float fov = render_fov(rig.lens) * std::numbers::pi_v<float> / 180.0f;
        const float screen_aspect = width / height;
        std::array<float, 24> constants{
            fish ? std::clamp(look.bend, 0.0f, 1.0f) : 0.0f, fov * 0.5f, std::tan(fov * 0.5f), look.bars ? 4.0f / 3.0f : screen_aspect,
            screen_aspect, look.bars ? 1.0f : 0.0f, look.hood ? 1.0f : 0.0f, static_cast<float>(std::fmod(seconds, 1000.0)),
            look.grade ? 1.0f : 0.0f, look.scanlines ? 1.0f : 0.0f, look.grain ? 1.0f : 0.0f, look.soft ? 1.0f : 0.0f,
            static_cast<float>(t.transfer), exposure_at(since), width, height,
            hand.x, hand.y, hand.angle, hand.on,
            hand.size, hand.mirrored ? 1.0f : 0.0f, width, height};
        t.commands->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        t.commands->RSSetViewports(1, &viewport);
        t.commands->RSSetScissorRects(1, &scissor);
        t.commands->SetGraphicsRootSignature(g.root.Get());
        t.commands->SetPipelineState(g.pipeline.Get());
        t.commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto gpu = t.srvs->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(step) * t.slot;
        t.commands->SetGraphicsRootDescriptorTable(1, gpu);
        t.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.commands->DrawInstanced(3, 1, 0, 0);
        // The overlay carries on from PRESENT, as without the look.
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing it.");
        return false;
    }
}

void draw_overlay(ImFont *font) noexcept {
    try {
        const auto v = view();
        if (!v || !v->active || !v->settings.look.enabled) return;
        const auto &look = v->settings.look;
        if (!look.date && !look.viewfinder) return;
        auto *draw = ImGui::GetBackgroundDrawList(); // under the menus, over the game
        const auto size = ImGui::GetIO().DisplaySize;
        if (size.x < 16 || size.y < 16) return;
        // The 4:3 frame the look draws into.
        const float frame_width = look.bars ? std::min(size.x, size.y * 4.0f / 3.0f) : size.x;
        const float left = (size.x - frame_width) * 0.5f, right = left + frame_width;
        const float text = std::max(14.0f, size.y * 0.042f);
        ImFont *face = font ? font : ImGui::GetFont();
        const auto shadowed = [&](ImVec2 at, ImU32 colour, const std::string &line, float scale) {
            draw->AddText(face, text * scale, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 170), line.c_str());
            draw->AddText(face, text * scale, at, colour, line.c_str());
        };
        const double seconds = now();
        // Hidden under the hand.
        if (seconds - v->slap_at >= 0.1 && seconds - v->slap_at <= 0.6) return;
        if (look.date) {
            const auto now_time = std::time(nullptr);
            std::tm local{};
            if (localtime_s(&local, &now_time) == 0) {
                static constexpr std::array<const char *, 12> months{"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
                const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
                const auto line1 = std::format("{} {:2}:{:02}:{:02}", local.tm_hour < 12 ? "AM" : "PM", hour, local.tm_min, local.tm_sec);
                const auto line2 = std::format("{} {:2} {}", months[static_cast<std::size_t>(local.tm_mon) % 12], local.tm_mday, local.tm_year + 1900);
                const ImU32 amber = IM_COL32(255, 196, 92, 235);
                const auto w1 = face->CalcTextSizeA(text, FLT_MAX, 0, line1.c_str()).x;
                const auto w2 = face->CalcTextSizeA(text, FLT_MAX, 0, line2.c_str()).x;
                shadowed(ImVec2(right - w1 - size.y * 0.06f, size.y * 0.80f), amber, line1, 1.0f);
                shadowed(ImVec2(right - w2 - size.y * 0.06f, size.y * 0.80f + text * 1.1f), amber, line2, 1.0f);
            }
        }
        if (look.viewfinder) {
            const ImU32 white = IM_COL32(240, 240, 240, 235);
            const float pad = size.y * 0.06f;
            // REC blinks as on the camcorder.
            if (std::fmod(seconds, 1.0) < 0.6) {
                draw->AddCircleFilled(ImVec2(left + pad + text * 0.35f, pad + text * 0.55f), text * 0.32f, IM_COL32(230, 30, 30, 240));
                shadowed(ImVec2(left + pad + text * 0.85f, pad), white, "REC", 1.0f);
            }
            const double rolled = std::max(0.0, seconds - v->rolling_since);
            const int total = static_cast<int>(rolled);
            const auto counter = std::format("{}:{:02}:{:02}", total / 3600, total / 60 % 60, total % 60);
            const auto cw = face->CalcTextSizeA(text, FLT_MAX, 0, counter.c_str()).x;
            shadowed(ImVec2(right - cw - pad, pad), white, counter, 1.0f);
            shadowed(ImVec2(left + pad, size.y - pad - text), white, "SP", 0.9f);
            // The battery, three bars.
            const ImVec2 at(right - pad - text * 1.6f, size.y - pad - text * 0.8f);
            draw->AddRect(at, ImVec2(at.x + text * 1.4f, at.y + text * 0.7f), white, 0, 0, 2.0f);
            draw->AddRectFilled(ImVec2(at.x + text * 1.4f, at.y + text * 0.2f), ImVec2(at.x + text * 1.55f, at.y + text * 0.5f), white);
            for (int i = 0; i < 3; ++i) {
                const float x = at.x + text * (0.12f + 0.42f * static_cast<float>(i));
                draw->AddRectFilled(ImVec2(x, at.y + text * 0.12f), ImVec2(x + text * 0.32f, at.y + text * 0.58f), white);
            }
        }
    } catch (...) {}
}

void release() noexcept {
    auto &g = graphics();
    g.copy.Reset();
    g.copy_desc = {};
    g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
    g.pipeline.Reset();
    g.pipeline_format = DXGI_FORMAT_UNKNOWN;
    g.root.Reset();
    g.view_heap = nullptr;
    g.view_slot = ~0u;
}
} // namespace dingosdk::filmer::look
