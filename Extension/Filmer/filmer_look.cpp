#include "filmer_look.h"
#include "filmer.h"
#include "filmer_look_hlsl.h" // generated from filmer_look.hlsl (cmake/UI.cmake)
#include "Engine/Core/Log/logging.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <numbers>
#include <string>

namespace dingosdk::filmer::look {
namespace {
using Microsoft::WRL::ComPtr;
// The shader's cbuffer: seven float4s, then five lines of twelve characters packed four to a uint.
constexpr std::size_t constant_count = 7 * 4 + 16, text_lines = 5, text_columns = 12;

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
    parameters[0].Constants.Num32BitValues = static_cast<UINT>(constant_count);
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

// A slap: the hand rushes at the lens, smacks it, is held there a beat, and is pulled away.
// Picture coordinates: half heights from the centre, y down. Size: the hand's scale (it fills the
// frame from about 3).
struct Hand { float x{}, y{}, angle{}, size{}, blur{}; bool mirrored{}; };
constexpr double slap_impact = 0.10, slap_release = 0.30, slap_gone = 0.60;
Hand hand_at(double since, std::uint64_t serial) {
    Hand hand;
    if (since < 0.0 || since > slap_gone) return hand;
    hand.mirrored = (serial & 1u) != 0;
    const float side = hand.mirrored ? -1.0f : 1.0f;
    if (since < slap_impact) {
        float u = static_cast<float>(since / slap_impact);
        u *= u; // accelerating at the lens
        hand.x = side * (0.9f - 0.75f * u);
        hand.y = 0.75f - 0.7f * u;
        hand.angle = side * (0.35f - 0.25f * u);
        hand.size = 1.3f + 3.2f * u;
        hand.blur = 0.02f + 0.05f * u;
    } else if (since < slap_release) {
        const float u = static_cast<float>((since - slap_impact) / (slap_release - slap_impact));
        hand.x = side * (0.15f + 0.05f * u);
        hand.y = 0.05f + 0.04f * u;
        hand.angle = side * 0.1f;
        hand.size = 4.5f + 0.2f * u;
        hand.blur = 0.02f;
    } else {
        float u = static_cast<float>((since - slap_release) / (slap_gone - slap_release));
        u = u * u * (3.0f - 2.0f * u);
        hand.x = side * (0.2f + 0.9f * u);
        hand.y = 0.09f + 1.2f * u;
        hand.angle = side * (0.1f + 0.3f * u);
        hand.size = 4.7f - 2.7f * u;
        hand.blur = 0.04f + 0.06f * u;
    }
    return hand;
}

// The camcorder's iris: shutting down while the palm is on the glass, overshooting as it comes off.
float exposure_at(double since) {
    if (since < slap_impact || since > 1.3) return 1.0f;
    if (since < slap_release) return 1.0f - 0.4f * static_cast<float>(std::min((since - slap_impact) / 0.08, 1.0));
    const float t = static_cast<float>(since - slap_release);
    return 1.0f + 0.35f * std::exp(-t * 4.0f) * std::min(t / 0.1f, 1.0f);
}

// The burnt-in text, as the camcorder's character generator would put it: top left, top right,
// bottom left, then the date's two lines bottom right. Right-hand lines are padded on the left.
std::array<std::string, text_lines> text_for(const View &v, double seconds) {
    std::array<std::string, text_lines> lines;
    const auto &look = v.settings.look;
    const auto right = [](std::string text) {
        if (text.size() < text_columns) text.insert(0, text_columns - text.size(), ' ');
        return text.substr(0, text_columns);
    };
    if (look.viewfinder) {
        if (std::fmod(seconds, 1.0) < 0.65) lines[0] = "#REC"; // blinks, as in the viewfinder
        const int total = static_cast<int>(std::max(0.0, seconds - v.rolling_since));
        lines[1] = right(std::format("{}:{:02}:{:02}", total / 3600, total / 60 % 60, total % 60));
        lines[2] = "[==]  SP";
    }
    if (look.date) {
        const auto now_time = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now_time) == 0) {
            static constexpr std::array<const char *, 12> months{"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
            const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
            lines[3] = right(std::format("{}:{:02}:{:02} {}", hour, local.tm_min, local.tm_sec, local.tm_hour < 12 ? "AM" : "PM"));
            lines[4] = right(std::format("{}.{:2} {}", months[static_cast<std::size_t>(local.tm_mon) % 12], local.tm_mday, local.tm_year + 1900));
        }
    }
    return lines;
}

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
} // namespace

bool pending() noexcept {
    const auto v = view();
    return v && v->settings.look.enabled && (v->active || v->settings.look.anywhere);
}

bool record(const Target &t) noexcept {
    try {
        auto &g = graphics();
        const auto v = view();
        if (!v || !v->settings.look.enabled || !(v->active || v->settings.look.anywhere) || g.failed) return false;
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
        // The bend can only squeeze what the game drew: with the Filmer's camera that is the fisheye's
        // wide view; over any other camera (a replay's), whatever that camera's own field of view is.
        const bool fish = (v->active ? rig.lens == Lens::fisheye : true) && look.fisheye;
        const float fov_degrees = v->active ? render_fov(rig.lens) : (v->camera_fov > 10.0f ? v->camera_fov : 60.0f);
        const float fov = fov_degrees * std::numbers::pi_v<float> / 180.0f;
        const float screen_aspect = width / height;
        // Text white: the overlay's output (SDR, scRGB in units of 80 nits, or HDR10's PQ code for 200 nits).
        const float white = t.transfer == 1 ? 2.5f : t.transfer == 2 ? 0.58f : 0.95f;
        const auto lines = text_for(*v, seconds);
        const bool any_text = std::ranges::any_of(lines, [](const std::string &line) { return !line.empty(); });
        std::array<std::uint32_t, constant_count> constants{
            bits(fish ? std::clamp(look.bend, 0.0f, 1.0f) : 0.0f), bits(fov * 0.5f), bits(std::tan(fov * 0.5f)), bits(look.bars ? 4.0f / 3.0f : screen_aspect),
            bits(screen_aspect), bits(look.bars ? 1.0f : 0.0f), bits(look.hood ? 1.0f : 0.0f), bits(static_cast<float>(std::fmod(seconds, 1000.0))),
            bits(look.grade ? 1.0f : 0.0f), bits(look.scanlines ? 1.0f : 0.0f), bits(look.grain ? 1.0f : 0.0f), bits(look.soft ? 1.0f : 0.0f),
            bits(static_cast<float>(t.transfer)), bits(exposure_at(since)), bits(width), bits(height),
            bits(hand.x), bits(hand.y), bits(hand.angle), bits(hand.size),
            bits(hand.mirrored ? 1.0f : 0.0f), bits(hand.blur), bits(0.0f), bits(0.0f),
            bits(2.0f / 150.0f), bits(white), bits(any_text ? 1.0f : 0.0f), bits(0.0f)};
        for (std::size_t line = 0; line < text_lines; ++line)
            for (std::size_t column = 0; column < text_columns && column < lines[line].size(); ++column) {
                const auto code = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(lines[line][column])));
                const std::size_t slot = 28 + line * 3 + column / 4;
                constants[slot] |= std::uint32_t{code} << (8 * (column % 4));
            }
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
