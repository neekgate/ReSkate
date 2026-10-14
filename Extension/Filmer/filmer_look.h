#pragma once
// The VX1000 look, drawn by the overlay over the game's finished frame while the Filmer has the camera
// (filmer.h). The frame is copied and drawn back through one pixel shader: the Century MK1's bend and
// its dark hood edge, 4:3, the camcorder's colour, standard-definition softness with its sharpening
// halo, interlaced fields, tape grain, and the hand that slaps over the lens. The date stamp and the
// viewfinder's REC are drawn after it with ImGui.
//
// Render thread only (the overlay's present hook, with its render lock held).
#include <d3d12.h>
#include <dxgi.h>

struct ImFont;

namespace dingosdk::filmer::look {
// The look wants this frame drawn (the Filmer is filming and the look is on). Cheap: once a frame.
bool pending() noexcept;

struct Target {
    ID3D12Device *device{};
    ID3D12GraphicsCommandList *commands{};
    ID3D12Resource *buffer{};             // the back buffer, in the PRESENT state; left in it
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12DescriptorHeap *srvs{};         // the overlay's shader-visible heap (already set on the list)
    unsigned slot{};                      // the heap slot the look may use for its copy
    DXGI_FORMAT format{};
    int transfer{};                       // the overlay's output transfer: 0 SDR, 1 scRGB, 2 HDR10
    // Waits until the GPU has finished every frame submitted so far (a copy about to be replaced may
    // still be read). False when it could not.
    bool (*wait_idle)(){};
};
// Records the look on the frame's command list, ahead of ImGui's draws. False when nothing was drawn
// (the look is off, or this system cannot build it: said once in the log).
bool record(const Target &target) noexcept;
// ImGui, between NewFrame and Render: the date stamp and the viewfinder, when they are on.
void draw_overlay(ImFont *font) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
} // namespace dingosdk::filmer::look
