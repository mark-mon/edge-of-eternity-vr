// Desktop window mirror of the left eye, drawn into the game's back buffer at Present (render thread).
#pragma once
#include <d3d11.h>
#include <cstdint>
namespace uxr::mirror {
bool Install();                                                  // hooks IDXGISwapChain::Present/Present1 (after MH_Initialize)
void SetSource(ID3D11Texture2D* eye, DXGI_FORMAT viewFormat);    // main thread; the texture is AddRef'd
void SetEnabled(bool on);                                        // draw over the back buffer at the next Presents
uint32_t TakeDraws();
}
