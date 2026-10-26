#include "platform/sdl/hd_images.h"
#include "platform/sdl/image_codec.h"
#include <sakuragl/sgl2d/sgl_image_conversion.h>

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace study::platform::sdl {
namespace {
using namespace SakuraGL;
namespace fs = std::filesystem;
constexpr unsigned density = 4;
constexpr uint32_t companionType = imageObjectAppExtension | 0x4844;
constexpr uint32_t physicalType = imageObjectAppExtension | 0x4845;
constexpr uint32_t revisionType = imageObjectAppExtension | 0x4846;
std::atomic<uint64_t> nextRevision{1};
constexpr size_t cacheLimit = 256u * 1024u * 1024u;
constexpr uint64_t maxEncodedBytes = 256u * 1024u * 1024u;
thread_local unsigned internalOperation = 0;
struct InternalOperation { InternalOperation(){++internalOperation;} ~InternalOperation(){--internalOperation;} };
bool DebugEnabled(){const char* env=SDL_getenv("ENTISGLS_HD_DEBUG");return env&&*env&&*env!='0';}
void Debug(const char* format,...) {
    static std::atomic<unsigned> messages{0};if(!DebugEnabled()||messages.fetch_add(1)>=1024)return;
    va_list arguments;va_start(arguments,format);SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION,SDL_LOG_PRIORITY_INFO,format,arguments);va_end(arguments);
}

// Standard SHA-256, deliberately independent of platform crypto libraries.
class Sha256 {
public:
    void Add(const void* data, size_t size) {
        const auto* p=static_cast<const uint8_t*>(data);total_+=size;
        while(size){const auto n=std::min(size,block_.size()-used_);std::memcpy(block_.data()+used_,p,n);
            used_+=n;p+=n;size-=n;if(used_==64){Transform();used_=0;}}
    }
    std::string Finish() {
        const uint64_t bits=total_*8;const uint8_t one=0x80,zero=0;Add(&one,1);
        while(used_!=56)Add(&zero,1);std::array<uint8_t,8> length{};
        for(unsigned i=0;i<8;++i)length[7-i]=uint8_t(bits>>(8*i));Add(length.data(),8);
        constexpr char hex[]="0123456789abcdef";std::string out;
        for(auto word:state_)for(int shift=28;shift>=0;shift-=4)out+=hex[(word>>shift)&15];return out;
    }
private:
    static uint32_t R(uint32_t v,unsigned n){return(v>>n)|(v<<(32-n));}
    void Transform() {
        constexpr uint32_t k[]={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];for(unsigned i=0;i<16;++i){const auto* p=block_.data()+4*i;
            w[i]=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
        for(unsigned i=16;i<64;++i)w[i]=w[i-16]+(R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3))+
            w[i-7]+(R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10));
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for(unsigned i=0;i<64;++i){const auto t1=h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            const auto t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }
    std::array<uint32_t,8> state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<uint8_t,64> block_{};uint64_t total_=0;size_t used_=0;
};

struct Entry { uint32_t width=0,height=0;fs::path path; };
struct Cached { std::shared_ptr<SGLImageObject> image;size_t bytes=0;uint64_t touched=0; };
struct Catalog {
    fs::path root;
    std::map<std::string,Entry> entries;
    std::map<std::string,Cached> cache;
    std::map<const void*,std::map<std::string,std::shared_ptr<SGLImageObject>>> pending;
    std::set<std::string> reported,failed;
    size_t bytes=0;uint64_t clock=0;
    std::recursive_mutex mutex;
};
Catalog& State(){static auto* state=new Catalog;return *state;}

bool SafeRelative(const fs::path& path){if(path.empty()||path.is_absolute())return false;
    for(const auto& part:path)if(part=="..")return false;return true;}
bool HashText(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}

void Initialize() {
    static std::once_flag once;
    std::call_once(once,[]{
        auto& state=State();std::vector<fs::path> roots;
        if(const char* env=SDL_getenv("ENTISGLS_HD_ROOT")){if(*env)roots.emplace_back(fs::u8path(env));}
        if(roots.empty())if(const char* base=SDL_GetBasePath()){
            roots.emplace_back(fs::u8path(base)/"hd");
            roots.emplace_back(fs::u8path(base)/"../Resources/hd");
        }
        for(const auto& root:roots){std::ifstream file(root/"manifest.tsv");if(!file)continue;
            state.root=root;std::string line;size_t rejected=0;
            while(std::getline(file,line)){
                if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
                std::istringstream fields(line);std::string hash,frame,w,h,path;
                if(!std::getline(fields,hash,'\t')||!std::getline(fields,frame,'\t')||!std::getline(fields,w,'\t')||
                   !std::getline(fields,h,'\t')||!std::getline(fields,path)||!HashText(hash)){++rejected;continue;}
                try{size_t used=0;const auto index=std::stoull(frame,&used);if(used!=frame.size()||index>65535)throw 0;
                    const auto width=std::stoul(w,&used);if(used!=w.size()||!width||width>8192)throw 0;
                    const auto height=std::stoul(h,&used);if(used!=h.size()||!height||height>8192||uint64_t(width)*height*density*density>0x4000000)throw 0;
                    const fs::path relative=fs::u8path(path);if(!SafeRelative(relative))throw 0;
                    const std::string key=hash+":"+std::to_string(index);const Entry entry{uint32_t(width),uint32_t(height),relative};
                    const auto inserted=state.entries.emplace(key,entry);
                    if(!inserted.second&&(inserted.first->second.width!=width||inserted.first->second.height!=height||inserted.first->second.path!=relative))throw 0;
                }catch(...){++rejected;}
            }
            SDL_Log("HD catalog root=%s frames=%zu rejected=%zu density=%u cache=%zuMiB",state.root.u8string().c_str(),state.entries.size(),rejected,density,cacheLimit/1024/1024);
            if(!state.entries.empty())SDL_Log("HD runtime reconstruction=cubic-b-spline bilinear-fetches=4 current-texture=true video-frame-cache=false");
            return;
        }
    });
}

std::string Fingerprint(SSystem::SFileInterface& file) {
    const auto position=file.GetPosition(),length=file.GetLength();
    if(!file.IsSeekable()||position<0||length<=0||uint64_t(length)>maxEncodedBytes){
        Debug("HD debug fingerprint-skipped stream=%p seekable=%d position=%lld length=%lld",&file,int(file.IsSeekable()),static_cast<long long>(position),static_cast<long long>(length));return {};}
    if(file.Seek(0)!=0){Debug("HD debug fingerprint-seek-failed stream=%p position=%lld length=%lld",&file,static_cast<long long>(position),static_cast<long long>(length));return {};}
    Sha256 hash;std::array<uint8_t,65536> bytes;int64_t remaining=length;bool valid=true;
    while(remaining){const auto count=size_t(std::min<int64_t>(remaining,bytes.size()));const auto read=file.Read(bytes.data(),count);
        if(read!=count){Debug("HD debug fingerprint-read-failed stream=%p expected=%zu actual=%zu remaining=%lld",&file,count,read,static_cast<long long>(remaining));valid=false;break;}hash.Add(bytes.data(),read);remaining-=read;}
    if(file.Seek(position)!=position){Debug("HD debug fingerprint-restore-failed stream=%p position=%lld",&file,static_cast<long long>(position));valid=false;}
    return valid?hash.Finish():std::string();
}

// This interface belongs to the *logical* buffer. References inherit it via
// ptrRefOriginal. Never infer provenance from a recycled object address.
class Companion final:public SGLImageBufferInterface {
public:
    std::string key;uint32_t width,height;bool valid=true;
    int32_t anchorX=0,anchorY=0,physicalX=0,physicalY=0;
    std::shared_ptr<SGLImageObject> computed;
    Companion(std::string identity,uint32_t w,uint32_t h):key(std::move(identity)),width(w),height(h){m_typeObject=companionType;}
    SGLError ClearBuffer(SGLImageBuffer* buffer,SGLPalette) override {if(valid)Debug("HD debug invalidate clear key=%s root=%p",key.c_str(),buffer);valid=false;computed.reset();return sglErrSuccess;}
    SGLError UpdateBuffer(SGLImageBuffer* buffer,const SGLImageRect*) override {if(valid)Debug("HD debug invalidate write key=%s root=%p",key.c_str(),buffer);valid=false;computed.reset();return sglErrSuccess;}
    SGLError CommitBuffer(SGLImageBuffer*) override {return sglErrSuccess;}
    SGLError ReflectBuffer(SGLImageBuffer*,const SGLImageRect*) override {return sglErrSuccess;}
    SGLError MakeMipmap() override {return sglErrSuccess;}
    bool OnImageReBuffered(SGLImageBuffer* buffer) override {Debug("HD debug rebuffer key=%s root=%p",key.c_str(),buffer);return false;}
    bool OnDestroyObject(ESLObject*) override {return false;}
};

class PhysicalMarker final:public SGLImageBufferInterface {
public:
    PhysicalMarker(){m_typeObject=physicalType;}
    SGLError UpdateBuffer(SGLImageBuffer*,const SGLImageRect*) override{return sglErrSuccess;}
    SGLError CommitBuffer(SGLImageBuffer*) override{return sglErrSuccess;}
    SGLError ReflectBuffer(SGLImageBuffer*,const SGLImageRect*) override{return sglErrSuccess;}
    SGLError MakeMipmap() override{return sglErrSuccess;}
    bool OnDestroyObject(ESLObject*) override{return false;}
};
void MarkPhysical(SGLImageObject& image){auto* buffer=image.GetImageBuffer();if(buffer){buffer->DeleteImageObject(physicalType);buffer->AddImageObject(new PhysicalMarker);}}

class RevisionMarker final:public SGLImageBufferInterface {
public:
    uint64_t revision=nextRevision.fetch_add(1);
    RevisionMarker(){m_typeObject=revisionType;}
    SGLError ClearBuffer(SGLImageBuffer*,SGLPalette) override{revision=nextRevision.fetch_add(1);return sglErrSuccess;}
    SGLError UpdateBuffer(SGLImageBuffer*,const SGLImageRect*) override{revision=nextRevision.fetch_add(1);return sglErrSuccess;}
    SGLError CommitBuffer(SGLImageBuffer*) override{return sglErrSuccess;}
    SGLError ReflectBuffer(SGLImageBuffer*,const SGLImageRect*) override{return sglErrSuccess;}
    SGLError MakeMipmap() override{return sglErrSuccess;}
    bool OnImageReBuffered(SGLImageBuffer*) override{revision=nextRevision.fetch_add(1);return false;}
    bool OnDestroyObject(ESLObject*) override{return false;}
};

void TrimCache(Catalog& state) {
    while(state.bytes>cacheLimit){auto oldest=state.cache.end();
        for(auto at=state.cache.begin();at!=state.cache.end();++at)if(at->second.image.use_count()==1&&
            (oldest==state.cache.end()||at->second.touched<oldest->second.touched))oldest=at;
        if(oldest==state.cache.end())break;state.bytes-=oldest->second.bytes;state.cache.erase(oldest);
    }
}

std::shared_ptr<SGLImageObject> Load(const std::string& key) {
    auto& state=State();std::lock_guard<std::recursive_mutex> guard(state.mutex);
    auto cached=state.cache.find(key);if(cached!=state.cache.end()){cached->second.touched=++state.clock;return cached->second.image;}
    const auto entry=state.entries.find(key);if(entry==state.entries.end()||state.failed.count(key))return {};
    const auto path=state.root/entry->second.path;
    std::unique_ptr<SDL_IOStream,decltype(&SDL_CloseIO)> input(SDL_IOFromFile(path.u8string().c_str(),"rb"),&SDL_CloseIO);
    studysteady::platform::RgbaImage rgba;
    if(!input||!studysteady::platform::DecodeImage(input.get(),rgba)||rgba.width!=int(entry->second.width*density)||rgba.height!=int(entry->second.height*density)){
        state.failed.insert(key);SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD rejected key=%s expected=%ux%u file=%s",key.c_str(),entry->second.width*density,entry->second.height*density,path.u8string().c_str());return {};}
    InternalOperation operation;
    auto image=std::make_shared<SGLImage>();
    // The material shader blends premultiplied textures. Convert straight PNG
    // bytes exactly once before filtering, preventing transparent black halos.
    if(image->CreateImage(rgba.width,rgba.height,formatImageARGB,32,SGLImageObject::bufferOnMemory))return {};
    SGLImageInfo info;auto* to=image->LockBuffer(info,SGLImageObject::lockWrite);if(!to)return {};
    for(int y=0;y<rgba.height;++y){auto* row=to+ptrdiff_t(y)*info.pitchLine;const auto* from=rgba.pixels.data()+size_t(y)*rgba.width*4;
        for(int x=0;x<rgba.width;++x){const unsigned alpha=from[4*x+3];row[4*x]=(from[4*x+2]*alpha+127)/255;
            row[4*x+1]=(from[4*x+1]*alpha+127)/255;row[4*x+2]=(from[4*x]*alpha+127)/255;row[4*x+3]=alpha;}}
    if(image->UnlockBuffer(SGLImageObject::lockWrite))return {};
    MarkPhysical(*image);
    const size_t bytes=size_t(rgba.width)*rgba.height*4;
    state.cache.emplace(key,Cached{image,bytes,++state.clock});state.bytes+=bytes;TrimCache(state);
    if(state.reported.insert(key).second)SDL_Log("HD loaded key=%s logical=%ux%u physical=%dx%d png=straight runtime=premultiplied file=%s",key.c_str(),entry->second.width,entry->second.height,rgba.width,rgba.height,path.u8string().c_str());
    return image;
}

void Retain(const void* renderer,const std::string& key,const std::shared_ptr<SGLImageObject>& image){
    auto& state=State();std::lock_guard<std::recursive_mutex> guard(state.mutex);state.pending[renderer][key]=image;}

bool ScaledClip(const SGLImageRect* clip,const HDImageView& view,SGLImageRect& out){
    out=view.clip;if(!clip)return true;
    const int64_t x=int64_t(view.clip.x)+int64_t(clip->x)*view.scale,y=int64_t(view.clip.y)+int64_t(clip->y)*view.scale;
    const int64_t w=int64_t(clip->w)*view.scale,h=int64_t(clip->h)*view.scale;
    if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||w<0||w>INT32_MAX||h<0||h>INT32_MAX)return false;
    out=SGLImageRect(int32_t(x),int32_t(y),int32_t(w),int32_t(h));return true;
}
} // namespace

void RegisterDecodedHDImage(SGLImageObject& image,SSystem::SFileInterface& file) try {
    if(internalOperation)return;Initialize();auto& state=State();if(state.entries.empty())return;
    const auto hash=Fingerprint(file);if(hash.empty())return;
    int side=0;const auto selected=image.GetSelectedFrame(&side),count=image.GetFrameCount();size_t matched=0;
    for(size_t frame=0;frame<count;++frame){const auto key=hash+":"+std::to_string(frame);const auto entry=state.entries.find(key);if(entry==state.entries.end())continue;
        if(image.SelectFrame(frame,stereoImageBoth))continue;SGLImageInfo info;
        auto* buffer=image.GetImageBuffer();if(!buffer||image.GetImageInfo(info)||info.width!=entry->second.width||info.height!=entry->second.height){
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD logical mismatch key=%s",key.c_str());continue;}
        int32_t x=0,y=0;auto* root=buffer;
        for(unsigned depth=0;root->ptrRefOriginal&&depth<64;++depth){x+=root->rctRefOriginal.x;y+=root->rctRefOriginal.y;root=root->ptrRefOriginal;}
        auto* tag=new Companion(key,info.width,info.height);tag->anchorX=x;tag->anchorY=y;
        root->DeleteImageObject(companionType);root->AddImageObject(tag);++matched;
        Debug("HD debug registered key=%s logical=%ux%u buffer=%p root=%p root-size=%ux%u anchor=%d,%d",key.c_str(),info.width,info.height,buffer,root,root->width,root->height,x,y);
    }
    Debug("HD debug source sha=%s bytes=%lld frames=%zu matches=%zu",hash.c_str(),static_cast<long long>(file.GetLength()),count,matched);
    image.SelectFrame(selected,side);
}catch(const std::exception& error){SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD registration: %s",error.what());}

bool ResolveHDImage(SGLImageObject* logical,HDImageView& view) try {
    if(!logical||internalOperation)return false;
    auto* buffer=logical->GetImageBuffer();if(!buffer)return false;
    SGLImageRect crop{0,0,int32_t(buffer->width),int32_t(buffer->height)};
    for(unsigned depth=0;buffer&&depth<64;++depth){
        for(auto* interface=buffer->ptrInterface;interface;interface=interface->m_ptrNext)if(interface->m_typeObject==companionType){
            auto* tag=static_cast<Companion*>(interface);if(!tag->valid){Debug("HD debug lookup-invalid key=%s logical=%p root=%p",tag->key.c_str(),logical,buffer);return false;}
            auto physical=tag->computed?tag->computed:Load(tag->key);if(!physical)return false;
            const int64_t x=int64_t(crop.x)-tag->anchorX,y=int64_t(crop.y)-tag->anchorY;
            if(x<0||y<0||x+crop.w>tag->width||y+crop.h>tag->height){Debug("HD debug lookup-outside key=%s crop=%d,%d,%d,%d anchor=%d,%d size=%u,%u",tag->key.c_str(),crop.x,crop.y,crop.w,crop.h,tag->anchorX,tag->anchorY,tag->width,tag->height);return false;}
            view.image=std::move(physical);view.scale=density;view.clip=SGLImageRect(int32_t(x+tag->physicalX)*density,int32_t(y+tag->physicalY)*density,crop.w*density,crop.h*density);return true;
        }
        if(!buffer->ptrRefOriginal)break;crop.x+=buffer->rctRefOriginal.x;crop.y+=buffer->rctRefOriginal.y;buffer=buffer->ptrRefOriginal;
    }
    if(DebugEnabled()){
        static std::set<const void*> missing;auto& state=State();std::lock_guard<std::recursive_mutex> guard(state.mutex);
        if(missing.insert(logical->GetImageBuffer()).second)Debug("HD debug lookup-unregistered logical=%p buffer=%p size=%ux%u frame=%zu",logical,logical->GetImageBuffer(),logical->GetImageWidth(),logical->GetImageHeight(),logical->GetSelectedFrame());
    }
    return false;
}catch(const std::exception& error){SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD resolve: %s",error.what());return false;}

bool AttachHDCompanion(SGLImageObject& logical,std::shared_ptr<SGLImageObject> physical,const std::string& key){
    if(!physical)return false;SGLImageInfo lo,hi;auto* buffer=logical.GetImageBuffer();
    if(!buffer||logical.GetImageInfo(lo)||physical->GetImageInfo(hi)||hi.width!=lo.width*density||hi.height!=lo.height*density)return false;
    if((hi.format&formatImageFlagNoProductOfAlpha)&&physical->NormalizeFormat(hi.format&~formatImageFlagNoProductOfAlpha,hi.depth))return false;
    MarkPhysical(*physical);
    auto* tag=new Companion(key,lo.width,lo.height);tag->computed=std::move(physical);
    auto* root=buffer;for(unsigned depth=0;root->ptrRefOriginal&&depth<64;++depth){tag->anchorX+=root->rctRefOriginal.x;tag->anchorY+=root->rctRefOriginal.y;root=root->ptrRefOriginal;}
    root->DeleteImageObject(companionType);root->AddImageObject(tag);return true;
}

void PreserveHDImageBuffer(SGLImageBuffer* before,SGLImageBuffer* after,bool detachBefore){
    if(!before||!after||before==after)return;
    SGLImageRect crop{0,0,int32_t(before->width),int32_t(before->height)};auto* root=before;
    for(unsigned depth=0;root&&depth<64;++depth){
        for(auto* item=root->ptrInterface;item;item=item->m_ptrNext)if(item->m_typeObject==physicalType){
            after->DeleteImageObject(physicalType);after->AddImageObject(new PhysicalMarker);break;
        }
        for(auto* item=root->ptrInterface;item;item=item->m_ptrNext)if(item->m_typeObject==companionType){
            auto* source=static_cast<Companion*>(item);if(!source->valid)return;
            const int32_t left=std::max(crop.x,source->anchorX),top=std::max(crop.y,source->anchorY);
            const int32_t right=int32_t(std::min(int64_t(crop.x)+crop.w,int64_t(source->anchorX)+source->width));
            const int32_t bottom=int32_t(std::min(int64_t(crop.y)+crop.h,int64_t(source->anchorY)+source->height));
            if(right<=left||bottom<=top)return;
            auto* copy=new Companion(source->key,uint32_t(right-left),uint32_t(bottom-top));copy->computed=source->computed;
            copy->anchorX=left-crop.x;copy->anchorY=top-crop.y;
            copy->physicalX=source->physicalX+left-source->anchorX;copy->physicalY=source->physicalY+top-source->anchorY;
            Debug("HD debug migrate key=%s before=%p old-root=%p after=%p region=%d,%d,%u,%u physical-origin=%d,%d",source->key.c_str(),before,root,after,copy->anchorX,copy->anchorY,copy->width,copy->height,copy->physicalX,copy->physicalY);
            after->DeleteImageObject(companionType);after->AddImageObject(copy);
            // This node becomes a reference to `after`; keep a single write-
            // observed tag at root so writes cannot bypass a stale local tag.
            if(detachBefore)before->DeleteImageObject(companionType);return;
        }
        if(!root->ptrRefOriginal)break;crop.x+=root->rctRefOriginal.x;crop.y+=root->rctRefOriginal.y;root=root->ptrRefOriginal;
    }
}

void CopyHDImageMetadata(SGLImageObject& source,SGLImageObject& destination){
    if(internalOperation||!HDImagesEnabled()||&source==&destination)return;
    int sourceSide=0,destinationSide=0;const auto selectedSource=source.GetSelectedFrame(&sourceSide),selectedDestination=destination.GetSelectedFrame(&destinationSide);
    const auto frames=std::min(source.GetFrameCount(),destination.GetFrameCount());
    for(size_t frame=0;frame<frames;++frame){if(source.SelectFrame(frame,stereoImageBoth)||destination.SelectFrame(frame,stereoImageBoth))continue;
        SGLImageInfo from,to;if(source.GetImageInfo(from)||destination.GetImageInfo(to)||from.width!=to.width||from.height!=to.height)continue;
        PreserveHDImageBuffer(source.GetImageBuffer(),destination.GetImageBuffer(),false);
    }
    source.SelectFrame(selectedSource,sourceSide);destination.SelectFrame(selectedDestination,destinationSide);
}

void CopyHDImageRegionMetadata(SGLImageObject& source,SGLImageObject& destination,int x,int y,const SGLImageRect* sourceClip){
    if(internalOperation||!HDImagesEnabled()||&source==&destination||x||y)return;
    const int64_t width=destination.GetImageWidth(),height=destination.GetImageHeight();
    SGLImageRect clip=sourceClip?*sourceClip:SGLImageRect(0,0,source.GetImageWidth(),source.GetImageHeight());
    // A smaller write retains other destination pixels. Its companion needs a
    // prior canvas snapshot, so do not attach an incorrect whole-image tag.
    if(!width||!height||clip.x<0||clip.y<0||clip.w<width||clip.h<height||
       int64_t(clip.x)+width>source.GetImageWidth()||int64_t(clip.y)+height>source.GetImageHeight())return;
    clip.w=int32_t(width);clip.h=int32_t(height);
    std::unique_ptr<SGLImageObject> reference(source.NewReference(&clip,source.GetSelectedFrame()));
    if(reference)PreserveHDImageBuffer(reference->GetImageBuffer(),destination.GetImageBuffer(),false);
}

uint64_t HDImageRevision(SGLImageObject& image){
    auto* buffer=image.GetImageBuffer();if(!buffer)return 0;
    // Observe the original storage so writes through another cropped reference
    // invalidate the same cache. Rebuffering itself also advances the revision.
    for(unsigned depth=0;buffer->ptrRefOriginal&&depth<64;++depth)buffer=buffer->ptrRefOriginal;
    for(auto* item=buffer->ptrInterface;item;item=item->m_ptrNext)if(item->m_typeObject==revisionType)return static_cast<RevisionMarker*>(item)->revision;
    auto* marker=new RevisionMarker;const auto revision=marker->revision;buffer->AddImageObject(marker);return revision;
}

namespace {
struct CPUCanvas {
    SGLImageObject* target=nullptr;
    std::shared_ptr<SGLImageObject> image;
    std::unique_ptr<SGLPaintBuffer> paint;
    bool valid=true,dirty=false,hasHDContent=false;
};
std::recursive_mutex cpuCanvasMutex;
std::map<const SGLPaintBuffer*,std::unique_ptr<CPUCanvas>> cpuCanvases;
CPUCanvas* CurrentCPUCanvas(SGLPaintBuffer& context){
    const auto at=cpuCanvases.find(&context);
    return at!=cpuCanvases.end()&&at->second->valid?at->second.get():nullptr;
}
void SyncCPUTransform(SGLPaintBuffer& context,CPUCanvas& canvas){
    SGLAffine affine;const bool transformed=context.GetTransformation(affine);
    if(!transformed)context.CurrentAffine(affine);
    unsigned transparency=context.CurrentTransparency();
    // This SDK's public getter omits /256 in GetTransparencyOf(0), while
    // PrepareFilterPaintProc correctly applies the stored stack transparency.
    // Invert the getter's unsigned arithmetic only when a stack is present;
    // leave the original renderer and its logical pixels untouched.
    if(transformed)transparency=256u-std::min(256u,(256u-transparency)/256u);
    affine.a11*=density;affine.a12*=density;affine.a13*=density;
    affine.a21*=density;affine.a22*=density;affine.a23*=density;
    canvas.paint->SetTransformation(affine,transparency);
    canvas.paint->SetPaintFlags(context.GetPaintFlags());
}
void CPUResult(CPUCanvas& canvas,SGLError result){canvas.dirty=true;if(result)canvas.valid=false;}

std::shared_ptr<SGLImageObject> CloneCPUCanvas(const HDImageView& view){
    if(!view.image)return {};InternalOperation operation;
    std::unique_ptr<SGLImageObject> source(view.image->NewReference(&view.clip));if(!source)return {};
    auto output=std::make_shared<SGLImage>();
    // Retained parent pixels are already premultiplied. Copy those bytes
    // directly; PM -> straight -> PM loses channel values on every cursor-
    // only refresh, eventually leaving white texel corners around dark holes.
    if(output->CreateCloneImage(*source,SGLImageObject::bufferOnMemory))return {};
    SGLImageInfo info;if(output->GetImageInfo(info))return {};
    if((info.format!=formatImageARGB||info.depth!=32)&&output->NormalizeFormat(formatImageARGB,32))return {};
    MarkPhysical(*output);return output;
}

std::shared_ptr<SGLImageObject> SeedCPUCanvas(SGLImageObject& target,const SGLImageInfo& info){
    HDImageView logical;logical.image=std::shared_ptr<SGLImageObject>(&target,[](SGLImageObject*){});
    logical.clip=SGLImageRect(0,0,info.width,info.height);logical.scale=1;
    auto snapshot=CloneHDImage(logical);
    if(!snapshot||snapshot->NormalizeFormat(formatImageARGB,32))return {};
    auto output=std::make_shared<SGLImage>();
    if(output->CreateImage(info.width*density,info.height*density,formatImageARGB,32))return {};
    SGLImageInfo from,to;const auto* source=snapshot->LockBuffer(from,SGLImageObject::lockRead);
    auto* destination=output->LockBuffer(to,SGLImageObject::lockWrite);
    const bool valid=source&&destination&&from.pitchPixel==4&&to.pitchPixel==4;
    // Pixel centres at 4x have exact eighth-pixel positions. Integer bilinear
    // weights sum to 64, with a single final rounding in premultiplied space.
    // The SDK smooth sampler truncates its weights and turns a constant 255
    // alpha into 254 between logical pixels, changing the blending backdrop.
    struct Sample {int first,second,weight;};
    const auto sample=[](unsigned position,unsigned limit){
        const int eighth=int(position)*2-3;
        const int first=eighth>=0?eighth/8:(eighth-7)/8;
        return Sample{std::clamp(first,0,int(limit)-1),std::clamp(first+1,0,int(limit)-1),eighth-first*8};
    };
    std::vector<Sample> columns;if(valid){columns.reserve(to.width);
        for(unsigned x=0;x<to.width;++x)columns.push_back(sample(x,from.width));
        for(unsigned y=0;y<to.height;++y){const auto sy=sample(y,from.height);
            const auto* top=source+ptrdiff_t(sy.first)*from.pitchLine;
            const auto* bottom=source+ptrdiff_t(sy.second)*from.pitchLine;
            auto* row=destination+ptrdiff_t(y)*to.pitchLine;
            for(unsigned x=0;x<to.width;++x){const auto sx=columns[x];
                for(unsigned c=0;c<4;++c){
                    const unsigned a=top[sx.first*4+c]*(8-sx.weight)+top[sx.second*4+c]*sx.weight;
                    const unsigned b=bottom[sx.first*4+c]*(8-sx.weight)+bottom[sx.second*4+c]*sx.weight;
                    row[x*4+c]=uint8_t((a*(8-sy.weight)+b*sy.weight+32)/64);
                }
            }
        }
    }
    if(source)snapshot->UnlockBuffer(SGLImageObject::lockRead);
    const auto result=destination?output->UnlockBuffer(SGLImageObject::lockWrite):sglErrFailed;
    return valid&&!result?std::move(output):std::shared_ptr<SGLImageObject>();
}
}

void BeginHDCPUCanvas(SGLPaintBuffer& context,SGLImageObject* target,SGLImageObject* depth,const SGLImageRect& view) try {
    if(internalOperation||!target||depth||!HDImagesEnabled()||IsHDPhysicalImage(target))return;
    std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    SGLImageInfo info;if(target->GetImageInfo(info)||!info.width||!info.height||
        uint64_t(info.width)*info.height*density*density>0x4000000u)return;
    HDImageView previous;auto canvas=std::make_unique<CPUCanvas>();canvas->target=target;
    if(ResolveHDImage(target,previous)){
        canvas->image=CloneCPUCanvas(previous);canvas->hasHDContent=bool(canvas->image);
    }
    InternalOperation operation;
    if(!canvas->image){
        canvas->image=SeedCPUCanvas(*target,info);if(!canvas->image)return;
    }
    canvas->paint=std::make_unique<SGLPaintBuffer>();
    const SGLImageRect physicalView(view.x*density,view.y*density,view.w*density,view.h*density);
    if(canvas->paint->AttachTargetImage(canvas->image.get(),nullptr,&physicalView))return;
    cpuCanvases[&context]=std::move(canvas);
}catch(const std::exception& error){SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD CPU begin: %s",error.what());}

void EndHDCPUCanvas(SGLPaintBuffer& context) try {
    if(internalOperation)return;std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    const auto at=cpuCanvases.find(&context);if(at==cpuCanvases.end())return;
    auto canvas=std::move(at->second);cpuCanvases.erase(at);
    {InternalOperation operation;canvas->paint->DetachTargetImage();}
    if(canvas->valid&&canvas->dirty&&canvas->hasHDContent){
        if(AttachHDCompanion(*canvas->target,canvas->image,"derived:cpu-parent-canvas"))
            Debug("HD debug cpu-published logical=%ux%u physical=%ux%u target=%p",canvas->target->GetImageWidth(),canvas->target->GetImageHeight(),
                  canvas->image->GetImageWidth(),canvas->image->GetImageHeight(),canvas->target);
    }
}catch(const std::exception& error){SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD CPU end: %s",error.what());}

void InvalidateHDCPUCanvas(SGLPaintBuffer& context){
    if(internalOperation)return;std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    const auto at=cpuCanvases.find(&context);if(at!=cpuCanvases.end())at->second->valid=false;
}

void MirrorHDCPUImage(SGLPaintBuffer& context,const SGLPaintParam& paint,SGLImageObject* source,const SGLImageRect* clip) try {
    if(internalOperation||!source)return;std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    auto* canvas=CurrentCPUCanvas(context);if(!canvas)return;
    // In-place source/target writes have special aliasing semantics. Keep the
    // correct logical result rather than publishing a potentially stale copy.
    const auto storage=[](SGLImageObject* image){auto* buffer=image->GetImageBuffer();
        for(unsigned i=0;buffer&&buffer->ptrRefOriginal&&i<64;++i)buffer=buffer->ptrRefOriginal;return buffer;};
    if(storage(source)==storage(canvas->target)){canvas->valid=false;return;}
    HDImageView view;const bool hd=ResolveHDImage(source,view);
    std::shared_ptr<SGLImageObject> reconstructed;
    bool physicalSource=hd;
    const uint32_t sourceFlags=paint.nFlags|uint32_t(context.GetPaintFlags());
    const bool smooth=(sourceFlags&(paintSmoothStretch|paintUnsmoothStretch))==paintSmoothStretch&&
        !(source->GetBufferFlags()&SGLImageObject::bufferSampleNoSmooth);
    // The SDK's transformed bilinear sampler anchors a logical pixel at the
    // corner of its enlarged cell. Reconstruct smooth CPU RGBA inputs at true
    // pixel centres before compositing, preserving thin glyph bodies. This
    // creates no offline provenance and keeps explicit unsmoothed sampling.
    if(!hd&&smooth&&!IsHDPhysicalImage(source)){
        SGLImageInfo info;source->GetImageInfo(info);
        const auto type=info.format&formatImageTypeMask;
        if((type==formatImageRGB||type==formatImageBGR)&&(info.depth==24||info.depth==32)&&
           !(info.format&(formatImageFlagPalette|formatImageFlagClipping))&&
           uint64_t(info.width)*info.height*density*density<=0x4000000u){
            InternalOperation operation;reconstructed=SeedCPUCanvas(*source,info);
            if(!reconstructed){canvas->valid=false;return;}
            view.image=reconstructed;view.scale=density;view.clip=SGLImageRect(0,0,info.width*density,info.height*density);
            physicalSource=true;
        }
    }
    std::unique_ptr<SGLImageObject> reference;SGLPaintParam parameter=paint;SGLAffine local;
    SGLImageRect scaledClip;std::vector<S2DVector> vertices;
    if(physicalSource){
        reference.reset(view.image->NewReference(&view.clip));if(!reference){canvas->valid=false;return;}
        if(hd)canvas->hasHDContent=true;
        paint.GetAffine(local);local.a11/=density;local.a12/=density;local.a21/=density;local.a22/=density;
        parameter.pAffine=&local;parameter.ptPaint=SGLPoint(0,0);parameter.nFlags&=~paintFixedPosition;
        if(clip)scaledClip=SGLImageRect(clip->x*density,clip->y*density,clip->w*density,clip->h*density);
        if((paint.nFlags&paintPolygonShaped)&&paint.pVertices){vertices.assign(paint.pVertices,paint.pVertices+paint.countVertex);
            for(auto& point:vertices){point.x*=density;point.y*=density;}parameter.pVertices=vertices.data();}
    }
    InternalOperation operation;SyncCPUTransform(context,*canvas);
    CPUResult(*canvas,canvas->paint->DrawImage(parameter,physicalSource?reference.get():source,clip?(physicalSource?&scaledClip:clip):nullptr));
}catch(const std::exception& error){InvalidateHDCPUCanvas(context);SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"HD CPU image: %s",error.what());}

void MirrorHDCPUClear(SGLPaintBuffer& context,uint32_t color,int64_t flags){
    if(internalOperation)return;std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    auto* canvas=CurrentCPUCanvas(context);if(!canvas)return;InternalOperation operation;
    CPUResult(*canvas,canvas->paint->FillClearTarget(color,flags));
}

void MirrorHDCPUShape(SGLPaintBuffer& context,unsigned kind,const S2DVector* vertices,size_t count,uint32_t color,double depth,uint32_t flags){
    if(internalOperation||!vertices)return;std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    auto* canvas=CurrentCPUCanvas(context);if(!canvas)return;
    if(kind==2){
        // SDK nLines means adjacent segments over nLines+1 vertices. Its
        // one-pixel region rasterizer also has start/end exclusion rules;
        // quadrilateral approximations alter those pixels and blend joins
        // twice. Keep this session on its faithful current-texture fallback.
        canvas->valid=false;Debug("HD debug cpu-discard reason=thin-line-rasterization segments=%zu target=%p",count,canvas->target);return;
    }
    InternalOperation operation;SyncCPUTransform(context,*canvas);
    if(kind==0)CPUResult(*canvas,canvas->paint->FillPolygon(vertices,count,color,depth,flags));
    else if(kind==1){
        SGLAffine affine;context.CurrentAffine(affine);
        if(affine.a11!=1||affine.a22!=1||affine.a12!=0||affine.a21!=0){canvas->valid=false;return;}
        for(size_t i=0;i<count;++i){const auto& p=vertices[i];
        const S2DVector box[]={{p.x,p.y},{p.x+1,p.y},{p.x+1,p.y+1},{p.x,p.y+1}};
        CPUResult(*canvas,canvas->paint->FillPolygon(box,4,color,depth,flags));}}
}

void MirrorHDCPUGradient(SGLPaintBuffer& context,unsigned kind,float x0,float y0,float x1,float y1,const SGLPalette* colors,size_t count){
    if(internalOperation)return;std::lock_guard<std::recursive_mutex> guard(cpuCanvasMutex);
    auto* canvas=CurrentCPUCanvas(context);if(!canvas)return;InternalOperation operation;
    if(kind==0)canvas->paint->FreeGradation();
    else if(kind==1){if(canvas->paint->SetLinearGradation(x0*density,y0*density,x1*density,y1*density,colors,count))canvas->valid=false;}
    else if(canvas->paint->SetRingedGradation(x0*density,y0*density,x1,colors,count))canvas->valid=false;
}

std::shared_ptr<SGLImageObject> CloneHDImage(const HDImageView& view){
    if(!view.image)return {};std::unique_ptr<SGLImageObject> source(view.image->NewReference(&view.clip));if(!source)return {};
    SGLImageInfo info;if(source->GetImageInfo(info)||!info.width||!info.height)return {};
    SGLImage native,rgb;auto output=std::make_shared<SGLImage>();
    const auto rgbFormat=formatImageRGB|(info.format&(formatImageFlagAlpha|formatImageFlagNoProductOfAlpha));
    if(native.CreateImage(info.width,info.height,info.format,info.depth)||rgb.CreateImage(info.width,info.height,rgbFormat,32)||
       output->CreateImage(info.width,info.height,formatImageARGB|formatImageFlagNoProductOfAlpha,32))return {};
    SGLImageBuffer from,middle,to;from.ptrBuffer=native.LockBuffer(from);middle.ptrBuffer=rgb.LockBuffer(middle);to.ptrBuffer=output->LockBuffer(to);
    const bool locked=from.ptrBuffer&&middle.ptrBuffer&&to.ptrBuffer;
    SGLError error=locked?source->ReadFrameBuffer(from,from.ptrBuffer):sglErrFailed;
    if(!error)error=sglConvertImageBuffer(middle,from);if(!error)error=sglConvertImageBuffer(to,middle);
    if(from.ptrBuffer)native.UnlockBuffer();if(middle.ptrBuffer)rgb.UnlockBuffer();if(to.ptrBuffer)output->UnlockBuffer();
    if(!error&&view.scale==density)MarkPhysical(*output);
    return error?std::shared_ptr<SGLImageObject>():output;
}

std::unique_ptr<SGLImageObject> MakeHDAlphaMask(SGLImageObject& logicalMask){
    SGLImageInfo info;if(logicalMask.GetImageInfo(info)||info.depth!=8||(info.format&formatImageTypeMask)!=formatImageGray)return {};
    auto output=std::make_unique<SGLImage>();if(output->CreateImage(info.width*density,info.height*density,formatImageGray,8))return {};
    HDImageView view;if(ResolveHDImage(&logicalMask,view)){
        auto source=CloneHDImage(view);if(!source)return {};SGLImageBuffer from,to;from.ptrBuffer=source->LockBuffer(from,SGLImageObject::lockRead);to.ptrBuffer=output->LockBuffer(to,SGLImageObject::lockWrite);
        const auto error=from.ptrBuffer&&to.ptrBuffer?sglConvertImageBuffer(to,from):sglErrFailed;
        if(from.ptrBuffer)source->UnlockBuffer(SGLImageObject::lockRead);if(to.ptrBuffer)output->UnlockBuffer(SGLImageObject::lockWrite);
        return error?std::unique_ptr<SGLImageObject>():std::move(output);
    }
    SGLImageBuffer from,to;from.ptrBuffer=logicalMask.LockBuffer(from,SGLImageObject::lockRead);to.ptrBuffer=output->LockBuffer(to,SGLImageObject::lockWrite);
    const bool valid=from.ptrBuffer&&to.ptrBuffer;
    if(valid)for(uint32_t y=0;y<to.height;++y){const double fy=(double(y)+0.5)/density-0.5;const int y0=int(std::floor(fy));const double wy=fy-y0;
        for(uint32_t x=0;x<to.width;++x){const double fx=(double(x)+0.5)/density-0.5;const int x0=int(std::floor(fx));const double wx=fx-x0;
            auto pixel=[&](int px,int py){return from.ptrBuffer[ptrdiff_t(std::clamp(py,0,int(from.height)-1))*from.pitchLine+std::clamp(px,0,int(from.width)-1)*from.pitchPixel];};
            const double top=pixel(x0,y0)*(1-wx)+pixel(x0+1,y0)*wx,bottom=pixel(x0,y0+1)*(1-wx)+pixel(x0+1,y0+1)*wx;
            to.ptrBuffer[ptrdiff_t(y)*to.pitchLine+x*to.pitchPixel]=uint8_t(std::lround(top*(1-wy)+bottom*wy));}}
    if(from.ptrBuffer)logicalMask.UnlockBuffer(SGLImageObject::lockRead);if(to.ptrBuffer)output->UnlockBuffer(SGLImageObject::lockWrite);
    return valid?std::move(output):std::unique_ptr<SGLImageObject>();
}

bool DrawHDImage(S3DRenderContextInterface& renderer,const SGLPaintParam& paint,SGLImageObject* image,const SGLImageRect* clip,SGLError& result){
    HDImageView view;if(!ResolveHDImage(image,view))return false;SGLImageRect physicalClip;if(!ScaledClip(clip,view,physicalClip))return false;
    SGLPaintParam parameter=paint;SGLAffine affine;paint.GetAffine(affine);
    affine.a11/=view.scale;affine.a12/=view.scale;affine.a21/=view.scale;affine.a22/=view.scale;
    parameter.pAffine=&affine;parameter.ptPaint=SGLPoint(0,0);parameter.nFlags&=~paintFixedPosition;
    std::vector<S2DVector> vertices;
    if((paint.nFlags&paintPolygonShaped)&&paint.pVertices){vertices.assign(paint.pVertices,paint.pVertices+paint.countVertex);
        for(auto& point:vertices){point.x*=view.scale;point.y*=view.scale;}parameter.pVertices=vertices.data();}
    // NewReference prevents the physical crop origin from leaking into output
    // geometry: SDK source clips are local to the selected logical subimage.
    std::unique_ptr<SGLImageObject> reference(view.image->NewReference(&view.clip));if(!reference)return false;
    SGLImageRect localClip;if(clip)localClip=SGLImageRect(clip->x*view.scale,clip->y*view.scale,clip->w*view.scale,clip->h*view.scale);
    Retain(&renderer,std::to_string(reinterpret_cast<uintptr_t>(view.image.get())),view.image);
    InternalOperation operation;result=renderer.DrawImage(parameter,reference.get(),clip?&localClip:nullptr);return true;
}

bool DrawHDMesh(S3DRenderContextInterface& renderer,const S2DVector* destination,const S2DVector* source,size_t width,size_t height,
    const SGLPaintParam& paint,SGLImageObject* image,const SGLImageRect* clip,SGLError& result){
    if(width>4096||height>4096||width+1>1048576/(height+1))return false;
    HDImageView view;if(!ResolveHDImage(image,view))return false;
    std::unique_ptr<SGLImageObject> reference(view.image->NewReference(&view.clip));if(!reference)return false;
    std::vector<S2DVector> sourcePoints;if(source){sourcePoints.assign(source,source+(width+1)*(height+1));
        for(auto& point:sourcePoints){point.x*=view.scale;point.y*=view.scale;}}
    SGLImageRect localClip;if(clip)localClip=SGLImageRect(clip->x*view.scale,clip->y*view.scale,clip->w*view.scale,clip->h*view.scale);
    Retain(&renderer,std::to_string(reinterpret_cast<uintptr_t>(view.image.get())),view.image);
    InternalOperation operation;result=renderer.DrawMesh(destination,source?sourcePoints.data():nullptr,width,height,paint,reference.get(),clip?&localClip:nullptr);return true;
}

void FinishHDRenderer(const void* renderer){auto& state=State();std::lock_guard<std::recursive_mutex> guard(state.mutex);state.pending.erase(renderer);TrimCache(state);}

bool HDImagesEnabled(){Initialize();return !State().entries.empty();}
bool IsHDPhysicalImage(SGLImageObject* image){
    if(!image)return false;auto* buffer=image->GetImageBuffer();
    for(unsigned depth=0;buffer&&depth<64;++depth){for(auto* item=buffer->ptrInterface;item;item=item->m_ptrNext)if(item->m_typeObject==physicalType)return true;buffer=buffer->ptrRefOriginal;}
    return false;
}

void TraceHDSampler(SGLImageObject* image,unsigned width,unsigned height,bool cubic,bool uniformPresent){
    if(!DebugEnabled()||!HDImagesEnabled())return;
    auto& state=State();std::lock_guard<std::recursive_mutex> guard(state.mutex);
    static std::set<std::string> sampled;
    const auto key=std::to_string(reinterpret_cast<uintptr_t>(image))+":"+std::to_string(width)+":"+std::to_string(height)+":"+std::to_string(cubic)+":"+std::to_string(uniformPresent);
    if(sampled.size()<64&&sampled.insert(key).second)Debug("HD debug sampler cubic=%s uniform=%s texture=%ux%u logical=%ux%u frame=%zu image=%p current-texture=true fragment-magnification-only=true",
        cubic?"true":"false",uniformPresent?"present":"missing",width,height,image?image->GetImageWidth():0,image?image->GetImageHeight():0,image?image->GetSelectedFrame():0,image);
}

void TraceHDImageOperation(const char* operation,SGLImageObject* source,SGLImageObject* target,const SGLImageRect* clip,const SGLAffine* affine){
    if(!DebugEnabled()||internalOperation||!HDImagesEnabled())return;
    auto* buffer=source?source->GetImageBuffer():nullptr;auto* root=buffer;Companion* tag=nullptr;
    int32_t originX=0,originY=0;
    for(unsigned depth=0;root&&depth<64;++depth){
        for(auto* item=root->ptrInterface;item;item=item->m_ptrNext)
            if(item->m_typeObject==companionType){tag=static_cast<Companion*>(item);break;}
        if(tag||!root->ptrRefOriginal)break;
        originX+=root->rctRefOriginal.x;originY+=root->rctRefOriginal.y;root=root->ptrRefOriginal;
    }
    // Follow backgrounds and wide UI strips; small glyph/button draws would
    // otherwise exhaust the trace budget before the first scene is loaded.
    if(source&&source->GetImageWidth()<700&&source->GetImageHeight()<550&&std::strncmp(operation,"legacy.ReadImage",16))return;
    auto& state=State();std::lock_guard<std::recursive_mutex> guard(state.mutex);
    static std::set<std::string> traced;
    const auto key=std::string(operation)+":"+std::to_string(reinterpret_cast<uintptr_t>(buffer))+":"+
        std::to_string(reinterpret_cast<uintptr_t>(target?target->GetImageBuffer():nullptr));
    if(traced.size()>=128||!traced.insert(key).second)return;
    const SGLImageRect rectangle=clip?*clip:SGLImageRect(0,0,source?source->GetImageWidth():0,source?source->GetImageHeight():0);
    const SGLAffine matrix=affine?*affine:SGLAffine();
    Debug("HD debug operation=%s source=%p buffer=%p root=%p source-size=%ux%u root-size=%ux%u source-origin=%d,%d key=%s valid=%d target=%p target-buffer=%p target-size=%ux%u clip=%d,%d,%d,%d affine=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
        operation,source,buffer,root,source?source->GetImageWidth():0,source?source->GetImageHeight():0,root?root->width:0,root?root->height:0,originX,originY,
        tag?tag->key.c_str():"none",tag?int(tag->valid):0,target,target?target->GetImageBuffer():nullptr,target?target->GetImageWidth():0,target?target->GetImageHeight():0,
        rectangle.x,rectangle.y,rectangle.w,rectangle.h,matrix.a11,matrix.a12,matrix.a13,matrix.a21,matrix.a22,matrix.a23);
}

void PatchHDFragmentSource(SSystem::SString& source){
    std::wstring text(static_cast<const wchar_t*>(source),source.GetLength());
    // SDK sources may use CRLF; normalize only for the exact reviewed standard
    // texture mapping function, retaining every other shader operation.
    text.erase(std::remove(text.begin(),text.end(),L'\r'),text.end());
    const std::wstring sample=L"tex_sample_px\n( u_samplerMaterialTexture,\ntex_coord_map\n( vTextureCoord,\nu_vMaterialTextureScale,\nu_vMaterialTextureBase ) )";
    const auto call=text.find(sample);if(call==std::wstring::npos)return;
    const std::wstring marker=L"float texture_mapping\n";const auto insertion=text.find(marker);
    if(insertion==std::wstring::npos)return;
    const std::wstring helper=LR"HD(
// Faithful runtime resampling for video and CPU-derived images. Four bilinear
// samples reproduce a separable nonnegative cubic B-spline kernel. No negative
// sharpening lobes, invented detail, cached video frames or altered geometry.
#ifndef _ENABLE_MATERIAL_TEXTURE_3D
uniform highp vec3 u_vHDResample;
vec4 hdCubicWeights(float f) {
    float n=1.0-f;
    return vec4(n*n*n,3.0*f*f*f-6.0*f*f+4.0,
                -3.0*f*f*f+3.0*f*f+3.0*f+1.0,f*f*f)/6.0;
}
vec4 hdSampleMaterial(sampler2D image,vec2 uv) {
    if(u_vHDResample.z<0.5) return texture2D(image,uv);
    vec2 pixel=u_vHDResample.xy;
    // Preserve exact-size and shrinking renders (thumbnails, masks, snapshots).
    // The fallback is specifically a reconstruction filter for enlargement.
    if(max(length(dFdx(uv)/pixel),length(dFdy(uv)/pixel))>=0.999)
        return texture2D(image,uv);
    vec2 position=uv/pixel-0.5;
    vec2 base=floor(position);
    vec2 fraction=position-base;
    vec4 wx=hdCubicWeights(fraction.x),wy=hdCubicWeights(fraction.y);
    vec2 gx=vec2(wx.x+wx.y,wx.z+wx.w);
    vec2 gy=vec2(wy.x+wy.y,wy.z+wy.w);
    vec2 px=base.xx+vec2(-1.0+wx.y/gx.x,1.0+wx.w/gx.y)+0.5;
    vec2 py=base.yy+vec2(-1.0+wy.y/gy.x,1.0+wy.w/gy.y)+0.5;
    vec4 a=texture2D(image,vec2(px.x,py.x)*pixel);
    vec4 b=texture2D(image,vec2(px.y,py.x)*pixel);
    vec4 c=texture2D(image,vec2(px.x,py.y)*pixel);
    vec4 d=texture2D(image,vec2(px.y,py.y)*pixel);
    return mix(mix(a,b,gx.y),mix(c,d,gx.y),gy.y);
}
#else
#define hdSampleMaterial tex_sample_px
#endif
)HD";
    text.replace(call,sample.size(),L"hdSampleMaterial\n( u_samplerMaterialTexture,\ntex_coord_map\n( vTextureCoord,\nu_vMaterialTextureScale,\nu_vMaterialTextureBase ) )");
    text.insert(insertion,helper);source=text.c_str();
}

} // namespace study::platform::sdl
