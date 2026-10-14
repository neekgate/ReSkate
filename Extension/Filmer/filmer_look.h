#pragma once
// The VX1000 look, drawn by the overlay over the game's finished frame while the Filmer has the camera
// (filmer.h). The frame is copied and drawn back through one pixel shader: the Century MK1's bend and
// its dark hood edge, 4:3, the camcorder's colour, standard-definition softness with its sharpening
// halo, interlaced fields, tape grain, the hand that slaps the lens, and the date and viewfinder text
// in the camcorder's own blocky characters, under the tape's softness like everything else.
//
// Render thread only (the overlay's present hook, with its render lock held).
#include <d3d12.h>
#include <dxgi.h>

namespace dingosdk::filmer::look {
// The look wants this frame drawn: it is on, and the Filmer is filming (or the look is set to be drawn
// over any camera). Cheap: once a frame.
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
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
} // namespace dingosdk::filmer::look
