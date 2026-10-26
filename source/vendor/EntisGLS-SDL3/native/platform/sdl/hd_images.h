#pragma once

#include <sakuragl/sakuragl.h>
#include <sakuragl/sgl2d_image.h>
#include <sakuragl/sgl3d/sgl_render_context.h>
#include <sakuragl/sgl2d/sgl_paint_buffer.h>
#include <memory>
#include <string>

namespace study::platform::sdl {

// A companion never changes the logical image's size, origin, frames, alpha
// representation or CPU pixels. The clip is in physical companion pixels.
struct HDImageView {
    std::shared_ptr<SakuraGL::SGLImageObject> image;
    SakuraGL::SGLImageRect clip;
    unsigned scale = 1;
};

// Called only after a successful SDK image read. Hashes the complete encoded
// stream while preserving its cursor, then tags every decoded frame separately.
void RegisterDecodedHDImage(SakuraGL::SGLImageObject& image, SSystem::SFileInterface& file);
// SDK reference creation collapses intermediate buffers. Keep provenance on
// root storage and migrate it when texture normalization replaces that storage.
void PreserveHDImageBuffer(SakuraGL::SGLImageBuffer* before, SakuraGL::SGLImageBuffer* after, bool detachBefore = true);
void CopyHDImageMetadata(SakuraGL::SGLImageObject& source, SakuraGL::SGLImageObject& destination);
// Mirror one CPU drawing session while keeping its original logical buffer.
void BeginHDCPUCanvas(SakuraGL::SGLPaintBuffer& context, SakuraGL::SGLImageObject* target,
                      SakuraGL::SGLImageObject* depth, const SakuraGL::SGLImageRect& view);
void EndHDCPUCanvas(SakuraGL::SGLPaintBuffer& context);
void InvalidateHDCPUCanvas(SakuraGL::SGLPaintBuffer& context);
void MirrorHDCPUImage(SakuraGL::SGLPaintBuffer& context, const SakuraGL::SGLPaintParam& paint,
                      SakuraGL::SGLImageObject* source, const SakuraGL::SGLImageRect* clip);
void MirrorHDCPUClear(SakuraGL::SGLPaintBuffer& context, uint32_t color, int64_t flags);
void MirrorHDCPUShape(SakuraGL::SGLPaintBuffer& context, unsigned kind,
                      const SakuraGL::S2DVector* vertices, size_t count, uint32_t color, double depth, uint32_t flags);
void MirrorHDCPUGradient(SakuraGL::SGLPaintBuffer& context, unsigned kind,
                         float x0, float y0, float x1, float y1,
                         const SakuraGL::SGLPalette* colors, size_t count);
// CopyImage writes only the selected frame. Retain provenance when the copy
// covers the complete destination, including an atlas region copied at (0,0).
void CopyHDImageRegionMetadata(SakuraGL::SGLImageObject& source, SakuraGL::SGLImageObject& destination,
                              int x, int y, const SakuraGL::SGLImageRect* sourceClip);
bool ResolveHDImage(SakuraGL::SGLImageObject* logical, HDImageView& view);
// A write-observing revision, attached to the actual SDK buffer rather than an
// object address. Useful when caching pure CPU operations on stable HD inputs.
uint64_t HDImageRevision(SakuraGL::SGLImageObject& image);
std::shared_ptr<SakuraGL::SGLImageObject> CloneHDImage(const HDImageView& view);
std::unique_ptr<SakuraGL::SGLImageObject> MakeHDAlphaMask(SakuraGL::SGLImageObject& logicalMask);

// CPU filters may explicitly publish a newly computed high density result.
// Any later write to logical invalidates the companion automatically.
bool AttachHDCompanion(SakuraGL::SGLImageObject& logical,
                       std::shared_ptr<SakuraGL::SGLImageObject> physical,
                       const std::string& diagnosticKey);

bool DrawHDImage(SakuraGL::S3DRenderContextInterface& renderer,
                 const SakuraGL::SGLPaintParam& paint,
                 SakuraGL::SGLImageObject* image, const SakuraGL::SGLImageRect* clip,
                 SakuraGL::SGLError& result);
bool DrawHDMesh(SakuraGL::S3DRenderContextInterface& renderer,
                const SakuraGL::S2DVector* destination, const SakuraGL::S2DVector* source,
                size_t width, size_t height, const SakuraGL::SGLPaintParam& paint,
                SakuraGL::SGLImageObject* image, const SakuraGL::SGLImageRect* clip,
                SakuraGL::SGLError& result);

// Buffered render entries hold raw material pointers; retain companions until
// the renderer has actually finished consuming the corresponding draw queue.
void FinishHDRenderer(const void* renderer);
bool HDImagesEnabled();
bool IsHDPhysicalImage(SakuraGL::SGLImageObject* image);
void TraceHDSampler(SakuraGL::SGLImageObject* image, unsigned width, unsigned height, bool cubic, bool uniformPresent);
// Bounded, read-only diagnostics: never resolves/loads a companion as a side effect.
void TraceHDImageOperation(const char* operation, SakuraGL::SGLImageObject* source,
                           SakuraGL::SGLImageObject* target,
                           const SakuraGL::SGLImageRect* clip = nullptr,
                           const SakuraGL::SGLAffine* affine = nullptr);
// Patches the decoded standard material fragment source only. Other/custom
// shaders and non-HD games retain their normal texture path.
void PatchHDFragmentSource(SSystem::SString& source);

} // namespace study::platform::sdl
