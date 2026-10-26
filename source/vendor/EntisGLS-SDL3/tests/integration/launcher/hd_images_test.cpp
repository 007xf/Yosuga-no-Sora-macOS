// Headless, metadata/pixel-only regression for real archived ERI HD companions.
// Inputs are isolated copies made by tools/hd/bulk/test_runtime.py. No windows,
// screenshots, rendering, game state or writes to the original archives.
#include "platform/sdl/hd_images.h"
#include "platform/sdl/image_codec.h"
#include "platform/sdl/sdk_image_codec.h"
#include "platform/sdl/opentype_font.h"
#include "platform/sdl/system.h"
#include <sakuragl/sgl_erisa_lib.h>
#include <sakuragl/sgl2d/sgl_lettering.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace SakuraGL;
using namespace study::platform::sdl;
namespace fs = std::filesystem;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::wstring Wide(const std::string& value) {
    SSystem::SString text; text.FromUTF8(reinterpret_cast<const uint8_t*>(value.c_str()));
    return static_cast<const wchar_t*>(text);
}
std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); Require(bool(file), "read fixture file");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
studysteady::platform::RgbaImage ReadPNG(const fs::path& path) {
    std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> input(SDL_IOFromFile(path.string().c_str(), "rb"), &SDL_CloseIO);
    studysteady::platform::RgbaImage image;
    Require(input && studysteady::platform::DecodeImage(input.get(), image), "read fixture PNG");
    return image;
}
// Raw channel-order normalization only. Do not multiply/unmultiply alpha here:
// compare the actual logical pixels to the independently exported SDK snapshot.
std::vector<uint8_t> RawRGBA(SGLImageObject& image) {
    SGLImageInfo info; const auto* pixels = image.LockBuffer(info, SGLImageObject::lockRead);
    Require(pixels != nullptr, "lock logical pixels");
    std::vector<uint8_t> rgba(size_t(info.width) * info.height * 4);
    bool supported = true;
    for (uint32_t y=0; y<info.height; ++y) for (uint32_t x=0; x<info.width; ++x) {
        const auto* p = pixels + ptrdiff_t(y)*info.pitchLine + x*info.pitchPixel;
        auto* out = rgba.data() + (size_t(y)*info.width+x)*4;
        const auto type = info.format & formatImageTypeMask;
        if (type == formatImageGray && info.depth == 8) out[0]=out[1]=out[2]=p[0];
        else if (type == formatImageRGB && (info.depth == 32 || info.depth == 24)) {out[0]=p[2];out[1]=p[1];out[2]=p[0];}
        else if (type == formatImageBGR && (info.depth == 32 || info.depth == 24)) {out[0]=p[0];out[1]=p[1];out[2]=p[2];}
        else supported = false;
        out[3] = (info.format & formatImageFlagAlpha) && info.depth == 32 ? p[3] : 255;
    }
    image.UnlockBuffer(SGLImageObject::lockRead);
    Require(supported, "raw logical comparison supports source format");
    return rgba;
}
struct Fixture {
    std::string id, sourceHash, encodedHash; fs::path source, rawRoot, physicalRoot;
    unsigned width=0,height=0,frames=0,format=0,depth=0; int originX=0,originY=0;
};
std::vector<Fixture> ReadFixtures(const fs::path& path) {
    std::ifstream file(path); Require(bool(file), "open fixture TSV"); std::string line;
    std::vector<Fixture> result;
    while (std::getline(file,line)) {
        if (line.empty() || line[0]=='#') continue;
        std::istringstream input(line); std::string item; std::vector<std::string> c;
        while (std::getline(input,item,'\t')) c.push_back(item);
        Require(c.size()==13,"fixture TSV field count");
        Fixture f;f.id=c[0];f.sourceHash=c[1];f.encodedHash=c[2];f.source=fs::u8path(c[3]);
        f.rawRoot=fs::u8path(c[4]);f.physicalRoot=fs::u8path(c[5]);f.width=std::stoul(c[6]);f.height=std::stoul(c[7]);
        f.frames=std::stoul(c[8]);f.format=std::stoul(c[9]);f.depth=std::stoul(c[10]);f.originX=std::stoi(c[11]);f.originY=std::stoi(c[12]);
        result.push_back(f);
    }
    Require(!result.empty(),"nonempty real-image fixtures");return result;
}
fs::path FramePNG(const fs::path& root, size_t frame) {
    char name[32]; std::snprintf(name,sizeof name,"frame%04zu.png",frame);return root/name;
}
bool SameInfo(const SGLImageInfo& a,const SGLImageInfo& b) {
    return a.width==b.width && a.height==b.height && a.format==b.format && a.depth==b.depth &&
           a.ptOrigin.x==b.ptOrigin.x && a.ptOrigin.y==b.ptOrigin.y;
}
void CheckPaddedReference(SGLImageObject& decoded, SSystem::SFileInterface& stream, const Fixture& f) {
    Require(!decoded.SelectFrame(0),"select padding fixture frame");SGLImageInfo originalInfo;decoded.GetImageInfo(originalInfo);
    // Reproduce decoder/texture padding with the visible decoded area held by
    // a reference above a larger storage buffer. Reference creation collapses
    // intermediate nodes, so the HD tag must live on the actual root storage.
    SGLImage padded;Require(!padded.CreateImage(f.width+16,f.height+16,originalInfo.format,originalInfo.depth),"create padded decoded storage");
    const SGLImageRect visible(3,5,f.width,f.height);
    std::unique_ptr<SGLImageObject> image(padded.NewReference(&visible));Require(bool(image),"visible reference above padded root");
    image->SetImageOrigin(f.originX,f.originY);SGLImageInfo fromInfo,toInfo;
    const auto* from=decoded.LockBuffer(fromInfo,SGLImageObject::lockRead);auto* to=image->LockBuffer(toInfo,SGLImageObject::lockWrite);
    Require(from && to && fromInfo.pitchPixel==toInfo.pitchPixel,"copy actual decoded pixels into padded fixture");
    for(unsigned y=0;y<f.height;++y)std::memcpy(to+ptrdiff_t(y)*toInfo.pitchLine,from+ptrdiff_t(y)*fromInfo.pitchLine,size_t(f.width)*fromInfo.pitchPixel);
    decoded.UnlockBuffer(SGLImageObject::lockRead);image->UnlockBuffer(SGLImageObject::lockWrite);
    const auto original=RawRGBA(decoded);Require(RawRGBA(*image)==original,"padded visible pixels exactly match decoded source");
    RegisterDecodedHDImage(*image,stream);HDImageView before;
    Require(ResolveHDImage(image.get(),before) && before.clip.x==0 && before.clip.y==0 && before.clip.w==int(f.width*4) && before.clip.h==int(f.height*4),
            "padded root anchors logical HD extent correctly");
    Require(!image->NormalizeToTexture(),"headless NormalizeToTexture");SGLImageInfo normalized;image->GetImageInfo(normalized);
    Require(normalized.width==f.width && normalized.height==f.height && normalized.ptOrigin.x==f.originX && normalized.ptOrigin.y==f.originY && RawRGBA(*image)==original,
            "texture normalization preserves visible size, origin and source pixels");
    HDImageView after;Require(ResolveHDImage(image.get(),after) && after.clip.x==0 && after.clip.y==0 && after.clip.w==int(f.width*4) && after.clip.h==int(f.height*4),
            "HD provenance survives padded texture storage replacement");
    const SGLImageRect crop(1,2,std::min(13u,f.width-1),std::min(11u,f.height-2));
    std::unique_ptr<SGLImageObject> child(image->NewReference(&crop));HDImageView cropped;
    Require(child && ResolveHDImage(child.get(),cropped) && cropped.clip.x==4 && cropped.clip.y==8 && cropped.clip.w==crop.w*4 && cropped.clip.h==crop.h*4,
            "cropped NewReference retains correct HD coordinates after padding normalization");
    if((originalInfo.format&formatImageTypeMask)!=formatImageGray) {
        // A pure pixel-format conversion replaces storage through a different
        // SDK path from texture normalization; both must migrate provenance.
        Require(!image->NormalizeFormat(formatImageARGB|formatImageFlagNoProductOfAlpha,32) &&
                !image->NormalizeFormat(formatImageARGB,32),"normalize straight then premultiplied ARGB32");
        std::unique_ptr<SGLImageObject> convertedChild(image->NewReference(&crop));HDImageView convertedView;
        Require(convertedChild && ResolveHDImage(convertedChild.get(),convertedView) && convertedView.clip.x==4 && convertedView.clip.y==8 &&
                convertedView.clip.w==crop.w*4 && convertedView.clip.h==crop.h*4,
                "format-only replacement preserves HD fingerprint and physical crop");
    }
}
std::unique_ptr<SGLImage> ReferenceGlyph4x(SGLImageObject& glyph){
    const unsigned width=glyph.GetImageWidth(),height=glyph.GetImageHeight();const auto rgba=RawRGBA(glyph);
    auto output=std::make_unique<SGLImage>();Require(!output->CreateImage(width*4,height*4,formatImageARGB,32,SGLImageObject::bufferOnMemory),"independent physical glyph allocation");
    SGLImageInfo info;auto* bytes=output->LockBuffer(info,SGLImageObject::lockWrite);Require(bytes,"independent physical glyph lock");
    for(unsigned y=0;y<height*4;++y)for(unsigned x=0;x<width*4;++x){
        const double sx=(double(x)+0.5)/4-0.5,sy=(double(y)+0.5)/4-0.5;
        const int left=int(std::floor(sx)),top=int(std::floor(sy));const double wx=sx-left,wy=sy-top;
        auto pixel=[&](int px,int py,unsigned c){return rgba[(size_t(std::clamp(py,0,int(height)-1))*width+std::clamp(px,0,int(width)-1))*4+c];};
        for(unsigned c=0;c<4;++c){const double a=pixel(left,top,c)*(1-wx)+pixel(left+1,top,c)*wx;
            const double b=pixel(left,top+1,c)*(1-wx)+pixel(left+1,top+1,c)*wx;
            bytes[ptrdiff_t(y)*info.pitchLine+x*4+(c<3?2-c:3)]=uint8_t(std::lround(a*(1-wy)+b*wy));
        }
    }
    Require(!output->UnlockBuffer(SGLImageObject::lockWrite),"independent physical glyph unlock");return output;
}
void CheckRealLettering(SGLImageObject& source){
    const auto registered=RegisterOpenTypeFont(L"/System/Library/Fonts/Hiragino Sans GB.ttc");
    Require(registered.loaded,"register actual Hiragino CJK font for HD text regression");
    SGLFontStyle style;style.pszFace=registered.registeredName.c_str();style.nSize=24;style.nStyles=0;
    SGLFont font;Require(!font.SetStyle(style),"select actual Hiragino 24px font");
    for(unsigned decoration:{0u,unsigned(SGLLetterer::flagBorder|SGLLetterer::flagShadow)}){
        SGLLetterer letterer;SGLLetteringContext context;
        context.ptStartWriting=SGLPoint(0,0);context.rectWritable=SGLRect(0,0,255,95);
        Require(letterer.WriteLetter(font,context,L"蔚蓝天空")>0,"raster actual Chinese glyphs");
        SGLLetterer::Decoration deco;deco.nFlags=decoration;deco.rgbaBody=SGLPalette(0xffffffff);
        deco.rgbaBorder=SGLPalette(0xff000000);deco.widthBorder=1;
        deco.rgbaShadow=SGLPalette(0x80000000);deco.ptShadow=SGLPoint(1,1);
        Require(!letterer.DecorateLetter(deco),"decorate real white CJK glyph bodies");
        // MessageBridge clones each decorated character, then Message draws a
        // batched image list. Exercise that actual source ownership and batch
        // path as well as SGLLetterer's direct draw on the first refresh.
        std::vector<std::unique_ptr<SGLImage>> glyphs,referenceGlyphs;SGLDrawImageParamList batch,referenceBatch;
        for(size_t i=0;i<letterer.GetLetterLength();++i){auto* character=letterer.GetCharacterAt(i);if(!character||!character->pImage)continue;
            auto glyph=std::make_unique<SGLImage>();Require(!glyph->CreateCloneBuffer(*character->pImage),"clone actual decorated Message glyph");
            HDImageView absent;Require(!ResolveHDImage(glyph.get(),absent),"actual decorated Message glyph has no invented companion");
            SGLPaintParam pp;pp.ptPaint=character->ptWriting+character->ptOffset+SGLPoint(6,10);pp.nFlags=paintDelayable;
            auto physical=ReferenceGlyph4x(*glyph);SGLPaintParam reference=pp;reference.ptPaint.x*=4;reference.ptPaint.y*=4;
            referenceBatch.AddDrawParam(reference,physical.get());referenceGlyphs.push_back(std::move(physical));
            batch.AddDrawParam(pp,glyph.get());glyphs.push_back(std::move(glyph));
        }
        SGLImage target;Require(!target.CreateImage(256,96,formatImageARGB,32),"real text parent canvas");
        for(unsigned backgroundTransparency:{0u,128u})for(unsigned refresh=0;refresh<2;++refresh){
        SGLPaintBuffer paint;Require(!paint.AttachTargetImage(&target,nullptr)&&!paint.FillClearTarget(0),"attach real text parent");
        SGLPaintParam parameter;parameter.nFlags=paintSmoothStretch;parameter.nTransparency=backgroundTransparency;
        Require(!paint.DrawImage(parameter,&source),"draw real HD background into text parent");
        if(!refresh)Require(!letterer.DrawLetterTo(paint,6,10),"draw actual SGLLetterer into HD background parent");
        else batch.Draw(paint);
        Require(!paint.DetachTargetImage(),"finish actual decorated text parent");
        HDImageView hd;Require(ResolveHDImage(&target,hd),"real lettering parent has actual HD source");
        const auto logical=RawRGBA(target),physical=RawRGBA(*hd.image);
        HDImageView background;Require(ResolveHDImage(&source,background),"physical background for independent lettering reference");
        std::unique_ptr<SGLImageObject> physicalBackground(background.image->NewReference(&background.clip));
        SGLImage expected;Require(!expected.CreateImage(1024,384,formatImageARGB,32),"physical lettering reference parent");
        SGLPaintBuffer reference;Require(!reference.AttachTargetImage(&expected,nullptr)&&!reference.FillClearTarget(0)&&
            !reference.DrawImage(parameter,physicalBackground.get()),"paint independent physical lettering background");
        referenceBatch.Draw(reference);Require(!reference.DetachTargetImage(),"finish independent physical lettering reference");
        const auto expectedPixels=RawRGBA(expected);
        unsigned maxError=0;for(size_t i=0;i<physical.size();++i)
            maxError=std::max(maxError,unsigned(std::abs(int(physical[i])-int(expectedPixels[i]))));
        size_t white=0,missing=0;
        for(unsigned y=0;y<96;++y)for(unsigned x=0;x<256;++x){const auto low=(size_t(y)*256+x)*4;
            if(std::min({logical[low],logical[low+1],logical[low+2]})<230)continue;
            ++white;const auto high=(size_t(y*4+2)*1024+x*4+2)*4;
            unsigned expected[4];bool differs=false;
            for(unsigned c=0;c<4;++c){expected[c]=expectedPixels[high+c];
                differs|=std::abs(int(physical[high+c])-int(expected[c]))>3;
            }
            if(differs){
                if(!missing)std::fprintf(stderr,"HD real-text DIFF deco=%u opacity=%u refresh=%u logical=%u,%u rgba=%u,%u,%u,%u physical=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                    decoration,backgroundTransparency,refresh,x,y,logical[low],logical[low+1],logical[low+2],logical[low+3],physical[high],physical[high+1],physical[high+2],physical[high+3],expected[0],expected[1],expected[2],expected[3]);
                ++missing;
            }
        }
        std::printf("HD actual Hiragino text deco=%u background-transparency=%u refresh=%u message-batch=%s white-centres=%zu missing=%zu full-RGBA-max-error=%u\n",decoration,backgroundTransparency,refresh,refresh?"true":"false",white,missing,maxError);
        Require(white>20&&missing==0&&maxError<=3,"actual SGLLetterer composite matches independent centred physical-glyph reference");
        if(backgroundTransparency==128&&refresh==1){
            const SGLImageRect cursor(0,0,1,1);
            const auto outsideHash=[](const std::vector<uint8_t>& pixels){uint64_t hash=14695981039346656037ull;
                for(unsigned y=0;y<384;++y)for(unsigned x=0;x<1024;++x){if(x<4&&y<4)continue;
                    for(unsigned c=0;c<4;++c){hash^=pixels[(size_t(y)*1024+x)*4+c];hash*=1099511628211ull;}}
                return hash;};
            const auto beforeHash=outsideHash(physical);
            for(unsigned tick=1;tick<=128;++tick){SGLPaintBuffer cursorPaint;
                Require(!cursorPaint.AttachTargetImage(&target,nullptr,&cursor)&&!cursorPaint.FillClearTarget(0)&&!cursorPaint.DetachTargetImage(),"actual text parent cursor-only refresh");
                if(tick!=32&&tick!=128)continue;
            HDImageView retained;Require(ResolveHDImage(&target,retained),"dirty cursor update retains HD text parent");
            const auto after=RawRGBA(*retained.image);size_t changed=0;
            for(unsigned y=0;y<384;++y)for(unsigned x=0;x<1024;++x)for(unsigned c=0;c<4;++c){if(x<4&&y<4)continue;
                const auto p=(size_t(y)*1024+x)*4+c;
                if(after[p]!=physical[p]){if(!changed)std::fprintf(stderr,"HD text dirty-loss deco=%u xy=%u,%u channel=%u before=%u after=%u ticks=%u\n",decoration,x,y,c,physical[p],after[p],tick);++changed;}}
            const auto afterHash=outsideHash(after);
            std::printf("HD actual Hiragino dirty-cursor deco=%u ticks=%u changed-outside-update=%zu FNV1a64=%016llx/%016llx\n",decoration,tick,changed,
                static_cast<unsigned long long>(beforeHash),static_cast<unsigned long long>(afterHash));
            Require(!changed&&beforeHash==afterHash,"cursor-only refresh never degrades retained real translucent text");
            }
        }
        }
    }
}
void CheckCPUParent(SGLImageObject& source) {
    CheckRealLettering(source);
    // Gameplay draws an 800x1200 background through Hybrid -> PaintBuffer into
    // an 800x600 parent. Test both atlas halves and a subsequent dirty viewport.
    SGLImage target;Require(!target.CreateImage(800,600,formatImageARGB,32),"CPU parent canvas");
    target.FillImage(SGLPalette(0));HDImageView original;
    Require(ResolveHDImage(&source,original),"background HD source");
    const auto physicalSource=RawRGBA(*original.image);const auto logicalSource=RawRGBA(source);
    // Generated glyph coverage and filled glyph canvases carry CPU format/
    // decoration semantics, rather than an offline image companion identity.
    SGLImage glyph,mask;
    Require(!glyph.CreateImage(24,24,formatImageARGB,32)&&!mask.CreateImage(24,24,formatImageGray,8),"generated glyph and coverage buffers");
    for(auto* generated:{&glyph,&mask}){
        SGLPaintBuffer paint;Require(!paint.AttachTargetImage(generated,nullptr)&&!paint.FillClearTarget(0)&&
            !paint.FillRectangle(4,3,12,16,0xffffffff)&&!paint.DetachTargetImage(),"generate CPU glyph fill and mask coverage");
        HDImageView absent;Require(!ResolveHDImage(generated,absent),"pure CPU glyph generation never invents an HD companion");
        SGLPalette centre;Require(!generated->GetPixelRGBA(centre,10,10)&&centre.argb.Red==255&&centre.argb.Green==255&&centre.argb.Blue==255,
            "original CPU glyph retains its white filled centre");
    }
    for(int half=0;half<2;++half){
        SGLPaintBuffer paint;Require(!paint.AttachTargetImage(&target,nullptr),"attach real CPU parent");
        Require(!paint.FillClearTarget(0),"clear CPU parent");
        SGLPaintParam parameter;parameter.nFlags=paintNoBlendAlpha|paintSmoothStretch;parameter.ptPaint=SGLPoint(0,-half*600);
        Require(!paint.DrawImage(parameter,&source),"draw real background into parent");
        Require(!paint.Finish()&&!paint.DetachTargetImage(),"finish and unlock CPU parent");
        Require(target.GetImageWidth()==800&&target.GetImageHeight()==600,"parent logical dimensions unchanged");
        HDImageView result;Require(ResolveHDImage(&target,result)&&result.clip.w==3200&&result.clip.h==2400,
            "CPU parent publishes actual 4x composition");
        const auto physical=RawRGBA(*result.image),logical=RawRGBA(target);
        for(unsigned y:{0u,127u,599u})for(unsigned x:{0u,213u,799u})for(unsigned c=0;c<4;++c){
            Require(logical[(size_t(y)*800+x)*4+c]==logicalSource[(size_t(y+half*600)*800+x)*4+c],
                "original CPU composition exactly retains selected atlas half");
            const size_t dst=(size_t(y*4)*3200+x*4)*4+c,src=(size_t((y+half*600)*4)*3200+x*4)*4+c;
            Require(physical[dst]==physicalSource[src],"4x parent uses physical source pixels from selected atlas half");
        }
    }
    {
        SGLPaintBuffer paint;Require(!paint.AttachTargetImage(&target,nullptr)&&!paint.FillClearTarget(0),"attach mixed HD background and original CPU glyph");
        SGLPaintParam background;background.nFlags=paintNoBlendAlpha|paintSmoothStretch;
        Require(!paint.DrawImage(background,&source),"consume actual HD background in mixed parent");
        SGLPaintParam text;text.nFlags=paintSmoothStretch;text.ptPaint=SGLPoint(40,50);
        Require(!paint.DrawImage(text,&glyph)&&!paint.DetachTargetImage(),"compose unchanged CPU glyph into HD parent");
        HDImageView mixed;Require(ResolveHDImage(&target,mixed),"mixed parent retains real HD source provenance");
        const auto pixels=RawRGBA(*mixed.image);const auto i=(size_t(60*4)*3200+50*4)*4;
        Require(pixels[i]>=250&&pixels[i+1]>=250&&pixels[i+2]>=250,"HD parent preserves white glyph fill rather than an empty outline");
    }
    HDImageView before;Require(ResolveHDImage(&target,before),"parent before partial refresh");
    const auto previous=RawRGBA(*before.image);const SGLImageRect view(10,20,30,40);
    SGLPaintBuffer partial;Require(!partial.AttachTargetImage(&target,nullptr,&view)&&!partial.FillClearTarget(0xff112233)&&!partial.DetachTargetImage(),
        "partial parent refresh");
    HDImageView after;Require(ResolveHDImage(&target,after),"partial refresh retains physical canvas");
    const auto current=RawRGBA(*after.image);
    Require(current[0]==previous[0]&&current[1]==previous[1]&&current[2]==previous[2],"dirty viewport retains pixels outside update");
    const size_t pixel=(size_t(20*4)*3200+10*4)*4;
    Require(current[pixel]==0x11&&current[pixel+1]==0x22&&current[pixel+2]==0x33&&current[pixel+3]==255,
        "dirty viewport scales all four bounds correctly");
    // DrawChildren uses a transformation stack even at zero transparency.
    // Compare the mirrored parent against a separate direct physical render;
    // the SDK's CurrentTransparency getter must not change stack alpha here.
    const SGLImageRect logicalClip(0,0,800,600),physicalClip(original.clip.x,original.clip.y,3200,2400);
    std::unique_ptr<SGLImageObject> logicalHalf(source.NewReference(&logicalClip,source.GetSelectedFrame()));
    std::unique_ptr<SGLImageObject> physicalHalf(original.image->NewReference(&physicalClip));
    Require(logicalHalf&&physicalHalf,"stack-alpha source references");
    for(unsigned transparency:{0u,128u}){
        Require(!target.FillImage(SGLPalette(0xff203040)),"reset transformed parent");
        HDImageView stale;Require(!ResolveHDImage(&target,stale),"FillImage invalidates prior parent companion");
        SGLPaintBuffer paint;Require(!paint.AttachTargetImage(&target,nullptr),"attach transformed parent");
        const SGLAffine global(1.25f,0,11,0,0.75f,17);
        Require(!paint.SetTransformation(global,transparency),"set real parent affine and alpha");
        SGLPaintParam parameter;parameter.nFlags=paintSmoothStretch;parameter.nTransparency=64;parameter.ptPaint=SGLPoint(7,13);
        Require(!paint.DrawImage(parameter,logicalHalf.get())&&!paint.DetachTargetImage(),"draw transformed parent with stack alpha");
        HDImageView transformed;Require(ResolveHDImage(&target,transformed),"transformed parent retains actual 4x composition");
        SGLImage expected;Require(!expected.CreateImage(3200,2400,formatImageARGB,32)&&!expected.FillImage(SGLPalette(0xff203040)),
            "independent physical reference canvas");
        SGLPaintBuffer reference;Require(!reference.AttachTargetImage(&expected,nullptr),"attach independent physical reference");
        const SGLAffine physical(1.25f,0,(11+1.25f*7)*4,0,0.75f,(17+0.75f*13)*4);
        Require(!reference.SetTransformation(physical,transparency),"set physical reference transform");
        SGLPaintParam referenceParameter;referenceParameter.nFlags=paintSmoothStretch;referenceParameter.nTransparency=64;
        Require(!reference.DrawImage(referenceParameter,physicalHalf.get())&&!reference.DetachTargetImage(),"draw physical reference alpha");
        const auto actualPixels=RawRGBA(*transformed.image),expectedPixels=RawRGBA(expected);
        if(actualPixels!=expectedPixels){
            const auto first=std::mismatch(actualPixels.begin(),actualPixels.end(),expectedPixels.begin()).first-actualPixels.begin();
            const size_t firstPixel=size_t(first)/4;SGLImageInfo actualInfo,expectedInfo,logicalInfo;
            transformed.image->GetImageInfo(actualInfo);expected.GetImageInfo(expectedInfo);target.GetImageInfo(logicalInfo);
            const auto i=firstPixel*4;
            std::fprintf(stderr,"HD CPU alpha DIFF stack=%u child=%u xy=%zu,%zu channel=%zu actual=%u,%u,%u,%u expected=%u,%u,%u,%u formats=%08x/%08x logical-format=%08x clip=%d,%d,%d,%d\n",
                transparency,parameter.nTransparency,firstPixel%3200,firstPixel/3200,size_t(first)%4,
                actualPixels[i],actualPixels[i+1],actualPixels[i+2],actualPixels[i+3],expectedPixels[i],expectedPixels[i+1],expectedPixels[i+2],expectedPixels[i+3],
                actualInfo.format,expectedInfo.format,logicalInfo.format,transformed.clip.x,transformed.clip.y,transformed.clip.w,transformed.clip.h);
        }
        Require(actualPixels==expectedPixels,"parent affine and stack alpha 0/128 exactly match physical reference");
    }
    // Thin lines have adjacent-segment and endpoint raster rules. Until their
    // exact physical reconstruction is implemented, retain the logical result
    // and discard the session rather than publish incomplete pairwise lines.
    const S2DVector polyline[]={{10,10},{20,10},{20,20},{30,20}};
    for(size_t segments:{size_t(1),size_t(3)}){
        Require(!target.FillImage(SGLPalette(0xff203040)),"reset thin-line parent");
        SGLPaintBuffer paint;Require(!paint.AttachTargetImage(&target,nullptr)&&
            !paint.DrawThinLines(polyline,segments,0xffabcdef)&&!paint.DetachTargetImage(),"draw actual adjacent thin-line segments");
        const auto pixels=RawRGBA(target);
        auto linePixel=[&](unsigned x,unsigned y){const auto i=(size_t(y)*800+x)*4;return pixels[i]==0xab&&pixels[i+1]==0xcd&&pixels[i+2]==0xef;};
        Require(linePixel(15,10),"single SDK thin-line segment remains visible");
        if(segments==3)Require(linePixel(20,15)&&linePixel(25,20),"all adjacent SDK thin-line segments remain visible");
        HDImageView discarded;Require(!ResolveHDImage(&target,discarded),"thin-line session safely uses current-texture fallback");
    }
    SGLImage gradientTarget;Require(!gradientTarget.CreateImage(64,32,formatImageARGB,32),"persistent-gradient target");
    const SGLPalette colors[]={SGLPalette(0xff102030),SGLPalette(0xffd0e0f0)};
    SGLPaintBuffer gradient;Require(!gradient.SetLinearGradation(0,0,64,0,colors,2)&&
        !gradient.AttachTargetImage(&gradientTarget,nullptr)&&!gradient.FillRectangle(0,0,64,32,0xffffffff)&&
        !gradient.DetachTargetImage(),"SDK gradient set before attach remains active");
    const auto gradientPixels=RawRGBA(gradientTarget);
    Require(gradientPixels[0]!=gradientPixels[(size_t(31)*64+63)*4],"original persistent gradient remains non-flat");
    HDImageView gradientFallback;Require(!ResolveHDImage(&gradientTarget,gradientFallback),"pre-existing gradient safely uses current-texture fallback");
    std::printf("HD CPU parent PASS real=800x1200 canvas=800x600 physical=3200x2400 halves=2 partial-refresh=true stack-alpha=0,128 thin-lines=faithful-fallback\n");
}
void CheckFixture(const Fixture& f) {
    auto bytes=ReadBytes(f.source);SSystem::SMemoryReferenceFile stream;stream.AttachMemory(bytes.data(),bytes.size());
    SGLImage image;Require(!image.ReadImage(&stream),"SDK ReadImage");
    SGLImageInfo logical;Require(!image.GetImageInfo(logical),"logical info");
    Require(logical.width==f.width && logical.height==f.height && image.GetFrameCount()==f.frames && logical.format==f.format &&
            logical.depth==(f.depth==24?32:f.depth) && logical.ptOrigin.x==f.originX && logical.ptOrigin.y==f.originY,
            "source logical dimensions/origin/format/frames unchanged");
    CheckPaddedReference(image,stream,f);
    if(f.width==800&&f.height==1200)CheckCPUParent(image);
    // Real gameplay converts decoded RGB/gray/alpha resources into the
    // renderer's ARGB format before creating atlas/crop references.
    SGLImage converted;
    Require(!converted.CreateCloneImage(image),"clone decoded image before format normalization");
    Require(!converted.NormalizeFormat(formatImageARGB,32),"real ARGB format normalization");
    HDImageView convertedView;
    Require(ResolveHDImage(&converted,convertedView) && convertedView.clip.x==0 && convertedView.clip.y==0 &&
            convertedView.clip.w==int(f.width*4) && convertedView.clip.h==int(f.height*4),
            "format normalization retains full HD provenance");
    std::vector<size_t> frames{0};if(f.frames>1)frames.push_back(f.frames-1);
    for(size_t frame:frames) {
        Require(!image.SelectFrame(frame),"select first/last logical frame");
        const auto original=ReadPNG(FramePNG(f.rawRoot,frame));const auto before=RawRGBA(image);
        Require(original.width==int(f.width) && original.height==int(f.height) && original.pixels==before,
                "unmodified logical pixels match independent native SDK export");
        HDImageView view;Require(ResolveHDImage(&image,view),"automatic ReadImage fingerprint lookup");
        Require(view.scale==4 && view.clip.x==0 && view.clip.y==0 && view.clip.w==int(f.width*4) && view.clip.h==int(f.height*4),
                "first/last HD frame is 4x physical with full source clip");
        SGLImageInfo physical;Require(!view.image->GetImageInfo(physical) && physical.width==f.width*4 && physical.height==f.height*4,
                "physical size is 4x");
        Require(IsHDPhysicalImage(view.image.get()),"physical companion marker exists");
        const auto physicalBytes=RawRGBA(*view.image);const auto accepted=ReadPNG(FramePNG(f.physicalRoot,frame));
        Require(accepted.width==int(f.width*4) && accepted.height==int(f.height*4) && accepted.pixels.size()==physicalBytes.size(),"accepted physical PNG size");
        for(size_t pixel=0;pixel<physicalBytes.size();pixel+=4) {
            Require(physicalBytes[pixel+3]==accepted.pixels[pixel+3],"physical alpha preserved");
            for(size_t c=0;c<3;++c)Require(physicalBytes[pixel+c]==uint8_t((unsigned(accepted.pixels[pixel+c])*accepted.pixels[pixel+3]+127)/255),
                    "physical straight PNG converts to runtime premultiplied RGB exactly once");
        }
        Require(RawRGBA(image)==before,"resolving physical companion does not alter logical pixels");
        SGLImageInfo after;image.GetImageInfo(after);Require(SameInfo(logical,after),"logical info unchanged after resolve");
        const SGLImageRect crop(2,3,std::min(19u,f.width-2),std::min(17u,f.height-3));
        std::unique_ptr<SGLImageObject> reference(image.NewReference(&crop));Require(bool(reference),"create logical cropped reference");
        SGLImageInfo referenceInfo;reference->GetImageInfo(referenceInfo);HDImageView cropView;
        Require(ResolveHDImage(reference.get(),cropView) && cropView.image==view.image && cropView.clip.x==8 && cropView.clip.y==12 &&
                cropView.clip.w==crop.w*4 && cropView.clip.h==crop.h*4 && referenceInfo.width==uint32_t(crop.w) && referenceInfo.height==uint32_t(crop.h),
                "NewReference inherits HD source with correct physical crop");
    }
    Require(!image.SelectFrame(f.frames-1),"restore last frame");stream.Seek(13);const auto selected=image.GetSelectedFrame();
    const auto before=RawRGBA(image);RegisterDecodedHDImage(image,stream);
    Require(stream.GetPosition()==13 && image.GetSelectedFrame()==selected && RawRGBA(image)==before,
            "explicit fingerprint preserves stream cursor, selected frame and CPU pixels");
    const auto revision=HDImageRevision(image);SGLImageInfo writeInfo;auto* pixel=image.LockBuffer(writeInfo,SGLImageObject::lockWrite);
    Require(pixel!=nullptr,"lock isolated decoded copy for write");pixel[0]^=1;image.UnlockBuffer(SGLImageObject::lockWrite);
    HDImageView invalid;Require(!ResolveHDImage(&image,invalid) && HDImageRevision(image)!=revision,
            "source buffer write invalidates HD provenance and advances revision");
    // Extra bytes do not change the decoded ERI but must change its complete
    // stream fingerprint; lookup must use the real bytes rather than a filename.
    auto unknownBytes=bytes;unknownBytes.push_back(0);SSystem::SMemoryReferenceFile unknownStream;
    unknownStream.AttachMemory(unknownBytes.data(),unknownBytes.size());SGLImage unknown;
    Require(!unknown.ReadImage(&unknownStream) && !ResolveHDImage(&unknown,invalid),"unknown encoded source falls back safely");
    // Also cover the path/extension dispatch route, not only memory/MIME reads.
    SGLImage fromPath;Require(!fromPath.LoadImage(Wide(f.source.u8string()).c_str()) && ResolveHDImage(&fromPath,invalid),
            "LoadImage extension route registers the same source fingerprint");
    std::printf("HD fixture PASS source=%s encoded=%s frames=%u logical=%ux%u alpha_raw=true\n",
                f.sourceHash.c_str(),f.encodedHash.c_str(),f.frames,f.width,f.height);
}
} // namespace
int main(int argc,char** argv) {
    if(argc!=4){std::fprintf(stderr,"Usage: launcher_hd_images_test <isolated-root> <fixtures.tsv> <hd-root>\n");return 2;}
    if(!SDL_Init(0))return 2;int result=0;
    try {
        const auto root=fs::absolute(argv[1]);fs::create_directories(root/"storage");fs::create_directories(root/"local");
        SystemPaths paths;paths.assetsRoot=root.string();paths.gameRoot=root.string();paths.storageRoot=(root/"storage").string();paths.localRoot=(root/"local").string();
        Require(ConfigureSystemPaths(paths),"isolated test paths");SDL_setenv_unsafe("ENTISGLS_HD_ROOT",argv[3],1);
        SSystem::Initialize();ERISA::sclfInitializeMatrix();SGLImageDecoderManager::Initialzie();RegisterSDLImageDecoder();
        Require(HDImagesEnabled(),"HD catalog available");const auto fixtures=ReadFixtures(argv[2]);
        for(const auto& f:fixtures)CheckFixture(f);
        std::printf("HD binding test PASS fixtures=%zu; logical state preserved, real SHA lookup, first/last frames, crops, alpha, write invalidation and fallback\n",fixtures.size());
    }catch(const std::exception& error){std::fprintf(stderr,"HD binding test FAIL: %s\n",error.what());result=1;}
    SDL_Quit();return result;
}
