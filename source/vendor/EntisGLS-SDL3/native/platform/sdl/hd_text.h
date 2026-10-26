#pragma once

#include <sakuragl/sakuragl.h>
#include <sakuragl/sgl2d/sgl_font.h>
#include <sakuragl/sgl2d/sgl_lettering.h>
#include <memory>

namespace study::platform::sdl {

// High density text. The SDK letters every string at the logical size; these
// helpers letter the same characters again at the companion density with the
// same face, style and decoration (border widths and shadow offsets scaled),
// place every glyph at exactly density x its logical writing position, and
// publish the result as the logical image's HD companion. Logical pixels,
// metrics, line breaks and hit areas are untouched; without an HD catalog
// these calls do nothing.

// One decorated glyph, as SGLSpriteMessage keeps them (dialogue, history).
// logicalOffset is the glyph's logical image offset (Character::ptOffset).
void AttachHDGlyph(SakuraGL::SGLImageObject& glyph, const SakuraGL::SGLFontStyle& style,
                   const SakuraGL::SGLLetterer::Decoration& decoration, uint32_t code,
                   const SakuraGL::SGLPoint& logicalOffset);

// A combined, then decorated, string as SGLSpriteText builds it (names,
// choices, save slots, confirmations). Capture before CombineLetter, attach
// after the final logical image exists.
struct HDTextPlan;
std::shared_ptr<HDTextPlan> PrepareHDText(const SakuraGL::SGLLetterer& letters,
                                          const SakuraGL::SGLFontStyle& style,
                                          const SakuraGL::SGLLetterer::Decoration& decoration);
void AttachHDText(SakuraGL::SGLImageObject& logical, const std::shared_ptr<HDTextPlan>& plan);

} // namespace study::platform::sdl
