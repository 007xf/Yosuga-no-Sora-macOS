#include "runtime/cotopha_port/branch_guide_store.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace entis::guide {
namespace {
namespace fs = std::filesystem;
constexpr size_t MaxIndex = 8 * 1024 * 1024, MaxSnapshot = 8 * 1024 * 1024;
constexpr size_t MaxNodes = 4096, MaxEdges = 16384, MaxString = 4096, MaxCheckpoints = 5;
const char* IndexName = "01 分支记录.json";
[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }
std::int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
bool Hex(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool Identifier(const std::string& s) {
    return !s.empty() && s.size() <= 160 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
    });
}
bool Game(const std::string& s) { return s == "yosuga" || s == "haruka"; }

// Small bounded JSON value/parser. External indexes never invoke script code.
struct J {
    enum Kind { Null, Boolean, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    std::string text;
    std::vector<J> array;
    std::map<std::string, J> object;
    J() = default;
    J(const char* s) : kind(String), text(s) {}
    J(std::string s) : kind(String), text(std::move(s)) {}
    J(bool v) : kind(Boolean), boolean(v) {}
    static J N(std::int64_t v) { J j; j.kind = Number; j.text = std::to_string(v); return j; }
    static J A() { J j; j.kind = Array; return j; }
    static J O(std::initializer_list<std::pair<const std::string,J>> values = {}) { J j; j.kind = Object; j.object = values; return j; }
    const J& at(const char* key) const { auto i = object.find(key); if (kind != Object || i == object.end()) Fail("攻略记录缺少必要字段"); return i->second; }
    std::string str(size_t max = MaxString) const { if (kind != String || text.size() > max) Fail("攻略记录字符串格式异常"); return text; }
    std::int64_t integer() const {
        if (kind != Number || text.find_first_of(".eE") != std::string::npos) Fail("攻略记录整数格式异常");
        size_t used = 0; const auto v = std::stoll(text, &used); if (used != text.size()) Fail("攻略记录整数格式异常"); return v;
    }
    bool flag() const { if (kind != Boolean) Fail("攻略记录布尔格式异常"); return boolean; }
};
void Utf8(std::string& out, unsigned cp) {
    if (cp < 128) out += char(cp);
    else if (cp < 2048) { out += char(0xc0 | cp >> 6); out += char(0x80 | (cp & 63)); }
    else if (cp < 65536) { out += char(0xe0 | cp >> 12); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    else { out += char(0xf0 | cp >> 18); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
}
class Parser {
    const std::string& input; size_t p = 0, count = 0;
    void ws() { while (p < input.size() && (input[p] == ' ' || input[p] == '\r' || input[p] == '\n' || input[p] == '\t')) ++p; }
    char take() { if (p >= input.size()) Fail("攻略记录截断"); return input[p++]; }
    unsigned hex4() { unsigned n = 0; for (unsigned i = 0; i < 4; ++i) { const char c = take(); n <<= 4; if (c >= '0' && c <= '9') n |= c-'0'; else if (c >= 'a' && c <= 'f') n |= c-'a'+10; else if (c >= 'A' && c <= 'F') n |= c-'A'+10; else Fail("攻略记录转义异常"); } return n; }
    std::string string() {
        if (take() != '"') Fail("攻略记录字符串异常"); std::string s;
        for (;;) {
            unsigned char c = take(); if (c == '"') return s; if (c < 32) Fail("攻略记录含控制字符");
            if (c == '\\') {
                const char e = take();
                if (e == '"' || e == '\\' || e == '/') s += e;
                else if (e == 'b') s += '\b'; else if (e == 'f') s += '\f'; else if (e == 'n') s += '\n'; else if (e == 'r') s += '\r'; else if (e == 't') s += '\t';
                else if (e == 'u') {
                    unsigned cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff) { if (take() != '\\' || take() != 'u') Fail("攻略记录代理字符异常"); const unsigned low = hex4(); if (low < 0xdc00 || low > 0xdfff) Fail("攻略记录代理字符异常"); cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00; }
                    else if (cp >= 0xdc00 && cp <= 0xdfff) Fail("攻略记录代理字符异常");
                    Utf8(s, cp);
                } else Fail("攻略记录转义异常");
            } else if (c < 128) s += char(c);
            else {
                unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 99;
                if (n == 99) Fail("攻略记录UTF-8异常"); unsigned cp = c & ((1u << (6-n))-1), minimum = n == 1 ? 128 : n == 2 ? 2048 : 65536;
                s += char(c); for (unsigned i = 0; i < n; ++i) { const unsigned char next = take(); if ((next & 0xc0) != 0x80) Fail("攻略记录UTF-8异常"); cp = (cp << 6) | (next & 63); s += char(next); }
                if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) Fail("攻略记录UTF-8异常");
            }
            if (s.size() > MaxString) Fail("攻略记录字符串过长");
        }
    }
    J value(unsigned depth) {
        if (depth > 24 || ++count > 400000) Fail("攻略记录结构过大"); ws(); const char c = p < input.size() ? input[p] : 0;
        if (c == '"') return J(string());
        if (c == '[') { ++p; J j = J::A(); ws(); if (p < input.size() && input[p] == ']') { ++p; return j; } for (;;) { j.array.push_back(value(depth+1)); ws(); const char end = take(); if (end == ']') return j; if (end != ',') Fail("攻略记录数组异常"); } }
        if (c == '{') { ++p; J j = J::O(); ws(); if (p < input.size() && input[p] == '}') { ++p; return j; } for (;;) { ws(); const auto key = string(); ws(); if (take() != ':') Fail("攻略记录对象异常"); J v = value(depth+1); if (!j.object.emplace(key,std::move(v)).second) Fail("攻略记录字段重复"); ws(); const char end = take(); if (end == '}') return j; if (end != ',') Fail("攻略记录对象异常"); } }
        for (auto literal : {"true", "false", "null"}) { const size_t n = std::strlen(literal); if (input.compare(p,n,literal) == 0) { p += n; return literal[0] == 'n' ? J() : J(literal[0] == 't'); } }
        const size_t start = p; if (c == '-') ++p;
        if (p < input.size() && input[p] == '0') ++p;
        else { const size_t digits = p; while (p < input.size() && input[p] >= '0' && input[p] <= '9') ++p; if (digits == p) Fail("攻略记录数值异常"); }
        if (p < input.size() && input[p] == '.') { ++p; const size_t at = p; while (p < input.size() && input[p] >= '0' && input[p] <= '9') ++p; if (at == p) Fail("攻略记录数值异常"); }
        if (p < input.size() && (input[p] == 'e' || input[p] == 'E')) { ++p; if (p < input.size() && (input[p] == '+' || input[p] == '-')) ++p; const size_t at = p; while (p < input.size() && input[p] >= '0' && input[p] <= '9') ++p; if (at == p) Fail("攻略记录数值异常"); }
        J j; j.kind = J::Number; j.text = input.substr(start,p-start); if (j.text.size() > 32) Fail("攻略记录数值过长"); return j;
    }
public:
    explicit Parser(const std::string& s) : input(s) {}
    J parse() { if (input.size() > MaxIndex) Fail("攻略记录超过8MiB限制"); J j = value(0); ws(); if (p != input.size()) Fail("攻略记录有额外内容"); return j; }
};
std::string Quote(const std::string& s) {
    constexpr char hex[] = "0123456789abcdef"; std::string out = "\"";
    for (unsigned char c : s) { if (c == '"' || c == '\\') { out += '\\'; out += char(c); } else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; } else out += char(c); }
    return out + '"';
}
std::string Encode(const J& j) {
    switch (j.kind) {
        case J::Null: return "null"; case J::Boolean: return j.boolean ? "true" : "false"; case J::Number: return j.text; case J::String: return Quote(j.text);
        case J::Array: { std::string out = "["; for (const auto& v : j.array) { if (out.size() > 1) out += ','; out += Encode(v); } return out + ']'; }
        case J::Object: { std::string out = "{"; for (const auto& [k,v] : j.object) { if (out.size() > 1) out += ','; out += Quote(k) + ':' + Encode(v); } return out + '}'; }
    } return "null";
}

class Sha256 {
public:
    void Add(const void* data,size_t size) { const auto* p=static_cast<const uint8_t*>(data);total_+=size;
        while(size){const auto n=std::min(size,block_.size()-used_);std::memcpy(block_.data()+used_,p,n);used_+=n;p+=n;size-=n;if(used_==64){Transform();used_=0;}} }
    std::string Finish() { const uint64_t bits=total_*8;const uint8_t one=0x80,zero=0;Add(&one,1);while(used_!=56)Add(&zero,1);std::array<uint8_t,8> length{};
        for(unsigned i=0;i<8;++i)length[7-i]=uint8_t(bits>>(8*i));Add(length.data(),8);constexpr char hex[]="0123456789abcdef";std::string out;
        for(auto word:state_)for(int shift=28;shift>=0;shift-=4)out+=hex[(word>>shift)&15];return out; }
private:
    static uint32_t R(uint32_t v,unsigned n){return(v>>n)|(v<<(32-n));}
    void Transform() { constexpr uint32_t k[]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];for(unsigned i=0;i<16;++i){const auto* p=block_.data()+4*i;w[i]=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
        for(unsigned i=16;i<64;++i)w[i]=w[i-16]+(R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10));
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];for(unsigned i=0;i<64;++i){const auto t1=h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+k[i]+w[i];const auto t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h; }
    std::array<uint32_t,8> state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};std::array<uint8_t,64> block_{};uint64_t total_=0;size_t used_=0;
};
std::string Hash(const std::vector<uint8_t>& b) { Sha256 h; h.Add(b.data(),b.size()); return h.Finish(); }
std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(),s.end()}; }
bool Exists(const fs::path& path) { std::error_code e; const auto s=fs::symlink_status(path,e); if(e && e != std::errc::no_such_file_or_directory) Fail("无法检查攻略路径"); return fs::exists(s); }
void Directory(const fs::path& path) {
    if (!Exists(path)) fs::create_directory(path);
    const auto state=fs::symlink_status(path);
    if (fs::is_symlink(state) || !fs::is_directory(state)) Fail("攻略目录不是安全的真实文件夹");
}
std::vector<uint8_t> ReadFile(const fs::path& path, size_t limit) {
    const auto state=fs::symlink_status(path); if(fs::is_symlink(state)||!fs::is_regular_file(state)) Fail("攻略文件不是安全的普通文件");
#if !defined(_WIN32)
    const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW);if(fd<0)Fail("无法读取攻略文件");
    struct Guard{int fd;~Guard(){::close(fd);}} guard{fd};struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||uint64_t(st.st_size)>limit)Fail("攻略文件超过大小限制");
    std::vector<uint8_t> bytes(size_t(st.st_size));size_t at=0;
    while(at<bytes.size()){const auto n=::read(fd,bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)Fail("攻略文件读取不完整");at+=size_t(n);}
    uint8_t extra; if(::read(fd,&extra,1)!=0)Fail("攻略文件在读取时变化");return bytes;
#else
    const auto size=fs::file_size(path);if(size>limit)Fail("攻略文件超过大小限制");std::ifstream in(path,std::ios::binary);if(!in)Fail("无法读取攻略文件");std::vector<uint8_t> bytes(size);if(size&&!in.read(reinterpret_cast<char*>(bytes.data()),size))Fail("攻略文件读取不完整");if(in.peek()!=EOF)Fail("攻略文件在读取时变化");return bytes;
#endif
}
void AtomicWrite(const fs::path& directory,const std::string& leaf,const std::vector<uint8_t>& bytes) {
    Directory(directory);
#if defined(_WIN32)
    static std::atomic<uint64_t> seq{0};const auto temp=directory/(L".guide."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(seq++)+L".tmp");
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)Fail("无法暂存攻略记录");bool okay=true;size_t at=0;
    while(at<bytes.size()){DWORD got=0;const DWORD n=DWORD(std::min<size_t>(bytes.size()-at,0x100000));if(!WriteFile(file,bytes.data()+at,n,&got,nullptr)||!got){okay=false;break;}at+=got;}
    if(okay&&!FlushFileBuffers(file))okay=false;if(!CloseHandle(file))okay=false;if(okay&&!MoveFileExW(temp.c_str(),(directory/fs::u8path(leaf)).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))okay=false;if(!okay){DeleteFileW(temp.c_str());Fail("攻略记录原子保存失败");}
#else
    const int dir=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(dir<0)Fail("无法安全打开攻略目录");
    struct DirGuard{int fd;~DirGuard(){::close(fd);}} dg{dir};static std::atomic<uint64_t> seq{0};
    const std::string temp=".guide."+std::to_string(getpid())+"."+std::to_string(seq++)+".tmp";
    const int fd=::openat(dir,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)Fail("无法暂存攻略记录");bool okay=true;size_t at=0;
    while(at<bytes.size()){const auto n=::write(fd,bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0){okay=false;break;}at+=size_t(n);}
    if(okay&&::fsync(fd))okay=false;if(::close(fd))okay=false;if(okay&&::renameat(dir,temp.c_str(),dir,leaf.c_str()))okay=false;
    if(!okay){::unlinkat(dir,temp.c_str(),0);Fail("攻略记录原子保存失败");}::fsync(dir);
#endif
}

struct Checkpoint { std::string id,resource;std::int64_t size=0,created=0;bool usable=false;std::string error; };
struct Node { Observation observation;std::int64_t visits=0,last=0;std::vector<Checkpoint> checkpoints; };
struct Edge {std::string source,target;int option=0;std::int64_t last=0;};
struct Action {std::string state="idle",message,node,checkpoint;};
struct State {
    std::string game,resource,resourceFile,indexHash,status="loading",lastError,active,pendingSource;
    int pendingOption=0;uint64_t revision=0;bool ready=false,blocked=false,overviewLoaded=false;
    fs::path root,directory,snapshots;
    std::map<std::string,Node> nodes,chapters;
    std::vector<Edge> edges;Action action;J overviewChapters=J::A(),endings=J::A(),specials=J::A();
};
J OptionJson(const Option& o) { return J::O({{"index",J::N(o.index)},{"text",o.text},{"available",o.available},{"chosen",o.chosen}}); }
J NodeJson(const Node& n,bool privateData) {
    const auto& o=n.observation;J options=J::A(),checkpoints=J::A();bool restore=false;
    for(const auto& option:o.options)options.array.push_back(OptionJson(option));
    for(const auto& c:n.checkpoints){J cp=J::O({{"id",c.id},{"created_at",J::N(c.created)},{"usable",c.usable},{"error",c.error},{"thumbnail_path",""}});restore|=c.usable;
        if(privateData){cp.object["sha256"]=c.id;cp.object["file_name"]=c.id+".dat";cp.object["resource_hash"]=c.resource;cp.object["size"]=J::N(c.size);}checkpoints.array.push_back(std::move(cp));}
    return J::O({{"id",o.nodeId},{"kind",o.kind},{"visited",true},{"title",o.title},{"scenario",o.scenario},{"chapter",o.chapter},{"route",o.route},{"ordinal",J::N(o.ordinal)},
        {"visit_count",J::N(n.visits)},{"last_visited",J::N(n.last)},{"options",options},{"checkpoints",checkpoints},{"can_restore",restore}});
}
J StateJson(const State& s,bool privateData) {
    J nodes=J::A(),chapters=J::A(),edges=J::A();for(const auto& [id,n]:s.nodes)nodes.array.push_back(NodeJson(n,privateData));for(const auto& [id,n]:s.chapters)chapters.array.push_back(NodeJson(n,privateData));
    for(const auto& e:s.edges)edges.array.push_back(J::O({{"source_id",e.source},{"target_id",e.target},{"option_index",J::N(e.option)},{"last_visited",J::N(e.last)}}));
    J overview = s.overviewChapters;
    for (auto& chapter : overview.array) {
        if (chapter.kind != J::Object) continue;
        const auto key = chapter.object.find("chapter_node_id");
        const auto found = key != chapter.object.end() && key->second.kind == J::String ?
            s.chapters.find(key->second.text) : s.chapters.end();
        const bool usable = found != s.chapters.end() && std::any_of(found->second.checkpoints.begin(),
            found->second.checkpoints.end(), [](const Checkpoint& cp) { return cp.usable; });
        chapter.object["enterable"] = usable;
        const auto entry = chapter.object.find("id");
        if (entry != chapter.object.end()) chapter.object["entry_id"] = entry->second;
    }
    J result=J::O({{"schema_version",J::N(1)},{"game",s.game},{"revision",J::N(s.revision)},{"status",s.status},{"ready",s.ready},{"last_error",s.lastError},{"active_node_id",s.active},
        {"action",J::O({{"state",s.action.state},{"message",s.action.message},{"node_id",s.action.node},{"checkpoint_id",s.action.checkpoint}})},
        {"nodes",nodes},{"edges",edges},{"captured_chapters",chapters},{"chapters",overview},{"endings",s.endings},{"specials",s.specials},{"overview_loaded",s.overviewLoaded}});
    if(privateData){result.object["resource_hash"]=s.resource;result.object["pending_source_id"]=s.pendingSource;result.object["pending_option_index"]=J::N(s.pendingOption);}return result;
}
void SafeDirectories(const State& s) {
    if(fs::is_symlink(fs::symlink_status(s.root))||!fs::is_directory(s.root))Fail("存档根目录不可用");
    Directory(s.directory);Directory(s.snapshots);
    if(fs::canonical(s.directory).parent_path()!=fs::canonical(s.root)||fs::canonical(s.snapshots).parent_path()!=fs::canonical(s.directory))Fail("攻略路径越界");
}
void BackupCorrupt(State& s,const std::vector<uint8_t>& bytes) {
    s.blocked=true;
    if(!bytes.empty())try{AtomicWrite(s.directory,"01 分支记录.corrupt."+Hash(bytes).substr(0,16)+".json",bytes);}catch(...){}
}
void Persist(State& s) {
    if(s.blocked)Fail("原攻略记录异常已保留，请先检查备份；未覆盖原文件");SafeDirectories(s);
    const auto path=s.directory/fs::u8path(IndexName);
    if(Exists(path)) {
        auto bytes=ReadFile(path,MaxIndex);
        if(s.indexHash.empty()||Hash(bytes)!=s.indexHash){BackupCorrupt(s,bytes);Fail("攻略记录被外部修改，已保留原文件与备份");}
    } else if(!s.indexHash.empty())Fail("攻略记录被外部移除，未自动覆盖");
    const auto bytes=Bytes(Encode(StateJson(s,true)));if(bytes.size()>MaxIndex)Fail("攻略记录已达8MiB限制");AtomicWrite(s.directory,IndexName,bytes);s.indexHash=Hash(bytes);
}
Checkpoint ReadCheckpoint(const J& j) {
    Checkpoint c;c.id=j.at("id").str(64);const auto filename=j.at("file_name").str(80);
    c.resource=j.at("resource_hash").str(64);c.size=j.at("size").integer();c.created=j.at("created_at").integer();
    if(!Hex(c.id)||j.at("sha256").str(64)!=c.id||filename!=c.id+".dat"||!Hex(c.resource)||c.size<=0||uint64_t(c.size)>MaxSnapshot||c.created<0)Fail("选择快照元数据异常");return c;
}
Node ReadNode(const J& j,const std::string& game,bool choice) {
    Node n;auto& o=n.observation;o.game=game;o.nodeId=j.at("id").str(160);o.kind=choice?"choice":"chapter";o.title=j.at("title").str();o.scenario=j.at("scenario").str();o.chapter=j.at("chapter").str();o.route=j.at("route").str();
    const auto ordinal=j.at("ordinal").integer();n.visits=j.at("visit_count").integer();n.last=j.at("last_visited").integer();
    if(!Identifier(o.nodeId)||ordinal<0||ordinal>100000||n.visits<1||n.visits>1000000000||n.last<0||!j.at("visited").flag())Fail("选择节点元数据异常");o.ordinal=int(ordinal);
    const auto& opts=j.at("options");if(opts.kind!=J::Array||opts.array.size()>16)Fail("选项数量异常");std::set<int> indices;
    for(const auto& v:opts.array){const auto i=v.at("index").integer();if(i<1||i>16||!indices.insert(int(i)).second)Fail("选择编号异常");o.options.push_back({int(i),v.at("text").str(),v.at("available").flag(),v.at("chosen").flag()});}
    if(choice&&o.options.empty())Fail("选择节点没有选项");const auto& cps=j.at("checkpoints");if(cps.kind!=J::Array||cps.array.size()>MaxCheckpoints)Fail("选择快照数量异常");std::set<std::string> ids;
    for(const auto& v:cps.array){auto c=ReadCheckpoint(v);if(!ids.insert(c.id).second)Fail("重复选择快照");n.checkpoints.push_back(std::move(c));}return n;
}
std::vector<uint8_t> ValidateCheckpoint(const State& s,const Checkpoint& cp,bool freshResource) {
    SafeDirectories(s);if(cp.resource!=s.resource)Fail("剧本资源版本已变化，需要在当前版本重新到达选择");
    if(freshResource&&!s.resourceFile.empty()&&Hash(ReadFile(fs::u8path(s.resourceFile),32*1024*1024))!=cp.resource)Fail("剧本资源校验失败，当前剧情未改变");
    auto bytes=ReadFile(s.snapshots/(cp.id+".dat"),MaxSnapshot);if(std::int64_t(bytes.size())!=cp.size||Hash(bytes)!=cp.id)Fail("选择快照校验失败，当前剧情未改变");return bytes;
}
void LoadIndex(State& s) {
    SafeDirectories(s);const auto path=s.directory/fs::u8path(IndexName);if(!Exists(path))return;
    auto bytes=ReadFile(path,MaxIndex);s.indexHash=Hash(bytes);
    try {
        const J j=Parser(std::string(bytes.begin(),bytes.end())).parse();if(j.at("schema_version").integer()!=1||j.at("game").str()!=s.game)Fail("攻略记录版本或作品不匹配");
        const auto previousResource = j.at("resource_hash").str(64);
        if (!Hex(previousResource)) Fail("攻略资源校验值异常");
        if (previousResource != s.resource) {
            // Preserve the old catalogue, but never present its choices or
            // chapter names as observations of a different script revision.
            AtomicWrite(s.directory, "01 分支记录.version-" + previousResource.substr(0,16) + ".json", bytes);
            s.action = {"idle", "剧本版本已更新；旧分支记录已保留，当前版本从实际到达处重新记录", {}, {}};
            return;
        }
        const auto& nodes=j.at("nodes");const auto& chapters=j.at("captured_chapters");const auto& edges=j.at("edges");
        if(nodes.kind!=J::Array||nodes.array.size()>MaxNodes||chapters.kind!=J::Array||chapters.array.size()>MaxNodes||edges.kind!=J::Array||edges.array.size()>MaxEdges)Fail("攻略记录条目过多");
        std::map<std::string,Node> nextNodes,nextChapters;
        for(const auto& row:nodes.array){auto n=ReadNode(row,s.game,true);const auto id=n.observation.nodeId;if(!nextNodes.emplace(id,std::move(n)).second)Fail("重复选择节点");}
        for(const auto& row:chapters.array){auto n=ReadNode(row,s.game,false);const auto id=n.observation.nodeId;if(!nextChapters.emplace(id,std::move(n)).second)Fail("重复章节节点");}
        std::vector<Edge> nextEdges;std::set<std::string> edgeIds;
        for(const auto& row:edges.array){Edge e;e.source=row.at("source_id").str(160);e.target=row.at("target_id").str(160);const auto option=row.at("option_index").integer();e.last=row.at("last_visited").integer();
            if(!nextNodes.count(e.source)||!nextNodes.count(e.target)||option<1||option>16||e.last<0)Fail("分支连线元数据异常");e.option=int(option);const auto& options=nextNodes.at(e.source).observation.options;
            if(std::none_of(options.begin(),options.end(),[&](const Option& o){return o.index==e.option&&o.chosen;}))Fail("连线没有实际已选答案");
            if(!edgeIds.insert(e.source+"/"+std::to_string(e.option)+"/"+e.target).second)Fail("重复分支连线");nextEdges.push_back(std::move(e));}
        s.nodes=std::move(nextNodes);s.chapters=std::move(nextChapters);s.edges=std::move(nextEdges);
        for(auto* collection:{&s.nodes,&s.chapters})for(auto& [id,n]:*collection)for(auto& cp:n.checkpoints)try{ValidateCheckpoint(s,cp,false);cp.usable=true;cp.error.clear();}catch(const std::exception& e){cp.usable=false;cp.error=e.what();}
        // Session traversal is never resumed from disk: a new game/load cannot
        // manufacture an edge from the previous process's last selected node.
    }catch(...){BackupCorrupt(s,bytes);throw;}
}
void ValidateObservation(const Observation& o) {
    if(!Game(o.game)||!Identifier(o.nodeId)||(o.kind!="choice"&&o.kind!="chapter")||o.scenario.size()>MaxString||o.title.size()>MaxString||o.chapter.size()>MaxString||o.route.size()>MaxString||o.ordinal<0||o.ordinal>100000||o.options.size()>16||o.snapshot.size()>MaxSnapshot)Fail("选择记录参数异常");
    if(o.kind=="choice"&&o.options.empty())Fail("未显示的选择不能加入攻略");std::set<int> indices;
    for(const auto& option:o.options)if(option.index<1||option.index>16||option.text.size()>MaxString||!indices.insert(option.index).second)Fail("选择记录选项异常");
}
} // namespace

struct Store::Impl {
    mutable std::mutex mutex;std::condition_variable changed;std::deque<std::function<void()>> jobs;bool stop=false,working=false;
    std::map<std::string,State> states;std::map<std::string,std::string> cache,resourceCache;std::map<std::string,PreparedRestore> prepared;std::thread worker;
    Impl():worker([this]{Run();}){}
    ~Impl(){ {std::lock_guard<std::mutex> l(mutex);stop=true;}changed.notify_all();worker.join(); }
    void Publish(State& s){++s.revision;const auto json=Encode(StateJson(s,false));std::lock_guard<std::mutex> l(mutex);cache[s.game]=json;resourceCache[s.game]=s.resource;}
    void Enqueue(std::string game,std::function<void()> job){std::lock_guard<std::mutex> l(mutex);jobs.emplace_back([this,game=std::move(game),job=std::move(job)]{try{job();}catch(const std::exception& e){Error(game,e.what());}catch(...){Error(game,"攻略处理失败");}});changed.notify_one();}
    void Error(const std::string& game,const std::string& message){auto& s=states[game];s.game=game;s.lastError=message;s.status=s.ready?"ready":"error";s.action.state="failed";s.action.message=message;Publish(s);}
    void Run(){for(;;){std::function<void()> job;{std::unique_lock<std::mutex> l(mutex);changed.wait(l,[this]{return stop||!jobs.empty();});if(stop&&jobs.empty())break;job=std::move(jobs.front());jobs.pop_front();working=true;}job();{std::lock_guard<std::mutex> l(mutex);working=false;}changed.notify_all();}}
    void Configure(std::string game,std::string root,std::string hash,std::vector<uint8_t> bytes,std::string resourceFile){
        Enqueue(game,[this,game,root=std::move(root),hash=std::move(hash),bytes=std::move(bytes),resourceFile=std::move(resourceFile)]()mutable{
            if(!Game(game))Fail("不支持的攻略作品");
            auto raw=fs::absolute(fs::u8path(root));if(fs::is_symlink(fs::symlink_status(raw))||!fs::is_directory(raw))Fail("存档根目录无效");const auto canonicalRoot=fs::canonical(raw);
            if(!resourceFile.empty())bytes=ReadFile(fs::u8path(resourceFile),32*1024*1024);if(!bytes.empty())hash=Hash(bytes);if(!Hex(hash))Fail("剧本资源校验值无效");
            auto previous=states.find(game);
            if(previous!=states.end()&&previous->second.ready&&!previous->second.blocked&&
               previous->second.root==canonicalRoot&&previous->second.resource==hash) {
                previous->second.resourceFile=resourceFile;
                return; // A sibling/session refresh must not discard an in-flight request.
            }
            State s;s.game=game;s.resourceFile=resourceFile;states[game]=std::move(s);auto& state=states[game];{std::lock_guard<std::mutex> l(mutex);prepared.erase(game);}Publish(state);
            state.root=canonicalRoot;state.directory=state.root/fs::u8path("01 攻略进度");state.snapshots=state.directory/fs::u8path("02 选择快照");state.resource=hash;
            LoadIndex(state);state.ready=true;state.status="ready";state.lastError.clear();Publish(state);
        });
    }
};

Store::Store():impl_(new Impl){}
Store::~Store()=default;
Store& Store::Get(){static Store store;return store;}
std::string Store::HashBytes(const std::vector<uint8_t>& bytes){return Hash(bytes);}
bool Store::ResourceFileMatches(const std::string& path,const std::string& expected)noexcept{
    if(!Hex(expected)||path.empty())return false;
    try{return Hash(ReadFile(fs::u8path(path),32*1024*1024))==expected;}catch(...){return false;}
}
std::string Store::ResourceHash(const std::string& game)const{std::lock_guard<std::mutex> l(impl_->mutex);const auto i=impl_->resourceCache.find(game);return i==impl_->resourceCache.end()?std::string():i->second;}
void Store::Configure(std::string game,std::string root,std::string hash){impl_->Configure(std::move(game),std::move(root),std::move(hash),{},{});}
void Store::Configure(std::string game,std::string root,std::vector<uint8_t> bytes){impl_->Configure(std::move(game),std::move(root),{},std::move(bytes),{});}
void Store::ConfigureResourceFile(std::string game,std::string root,std::string path){impl_->Configure(std::move(game),std::move(root),{},{},std::move(path));}
std::string Store::LatestJson(const std::string& game)const{std::lock_guard<std::mutex> l(impl_->mutex);const auto i=impl_->cache.find(game);return i==impl_->cache.end()?std::string():i->second;}
void Store::Flush(){std::unique_lock<std::mutex> l(impl_->mutex);impl_->changed.wait(l,[this]{return impl_->jobs.empty()&&!impl_->working;});}
void Store::Observe(Observation o){const auto game=o.game;impl_->Enqueue(game,[this,o=std::move(o)]()mutable{
    ValidateObservation(o);auto& s=impl_->states.at(o.game);if(!s.ready||s.blocked)Fail("攻略记录尚未就绪");if(o.createdAt<=0)o.createdAt=Now();
    auto& collection=o.kind=="choice"?s.nodes:s.chapters;if(!collection.count(o.nodeId)&&collection.size()>=MaxNodes)Fail("攻略节点已达数量限制");auto& n=collection[o.nodeId];const bool first=n.visits==0;
    for(auto& option:o.options)for(const auto& previous:n.observation.options)if(previous.index==option.index&&previous.text==option.text)option.chosen|=previous.chosen;
    const bool arrival=o.kind!="choice"||s.active!=o.nodeId||!s.pendingSource.empty();if(first||arrival)++n.visits;n.last=o.createdAt;
    auto snapshot=std::move(o.snapshot);n.observation=std::move(o);
    if(!snapshot.empty()){
        const auto hash=Hash(snapshot);auto cp=std::find_if(n.checkpoints.begin(),n.checkpoints.end(),[&](const Checkpoint& c){return c.id==hash;});
        if(cp==n.checkpoints.end()||!cp->usable){SafeDirectories(s);AtomicWrite(s.snapshots,hash+".dat",snapshot);Checkpoint c{hash,s.resource,std::int64_t(snapshot.size()),n.last,true,{}};if(cp!=n.checkpoints.end())*cp=c;else n.checkpoints.push_back(std::move(c));}
        std::sort(n.checkpoints.begin(),n.checkpoints.end(),[](const auto& a,const auto& b){return a.created>b.created;});
        while(n.checkpoints.size()>MaxCheckpoints){const auto removed=n.checkpoints.back().id;n.checkpoints.pop_back();bool used=false;for(const auto* group:{&s.nodes,&s.chapters})for(const auto& [id,node]:*group)for(const auto& c:node.checkpoints)used|=c.id==removed;
            if(!used){const auto path=s.snapshots/(removed+".dat");if(Exists(path)&&!fs::is_symlink(fs::symlink_status(path)))fs::remove(path);}}
    }
    if(n.observation.kind=="choice"){
        if(!s.pendingSource.empty()&&s.pendingOption>0){auto edge=std::find_if(s.edges.begin(),s.edges.end(),[&](const Edge& e){return e.source==s.pendingSource&&e.option==s.pendingOption&&e.target==n.observation.nodeId;});
            if(edge!=s.edges.end())edge->last=n.last;else{if(s.edges.size()>=MaxEdges)Fail("攻略连线已达数量限制");s.edges.push_back({s.pendingSource,n.observation.nodeId,s.pendingOption,n.last});}}
        s.pendingSource.clear();s.pendingOption=0;s.active=n.observation.nodeId;
    }
    s.lastError.clear();Persist(s);impl_->Publish(s);
});}
void Store::Choice(std::string game,std::string nodeId,int option){impl_->Enqueue(game,[this,game,nodeId,option]{auto& s=impl_->states.at(game);auto n=s.nodes.find(nodeId);if(n==s.nodes.end())Fail("选择尚未实际到达");auto o=std::find_if(n->second.observation.options.begin(),n->second.observation.options.end(),[&](const Option& v){return v.index==option&&v.available;});if(o==n->second.observation.options.end())Fail("选择答案当前不可用");o->chosen=true;n->second.last=Now();s.pendingSource=nodeId;s.pendingOption=option;s.active.clear();Persist(s);impl_->Publish(s);});}
void Store::ResetTraversal(std::string game){impl_->Enqueue(game,[this,game]{auto i=impl_->states.find(game);if(i==impl_->states.end())return;auto& s=i->second;
    if(s.active.empty()&&s.pendingSource.empty()&&!s.pendingOption)return;
    s.active.clear();s.pendingSource.clear();s.pendingOption=0;impl_->Publish(s);
});}
void Store::PrepareRestore(std::string game,std::string nodeId,std::string checkpointId){impl_->Enqueue(game,[this,game,nodeId,checkpointId]{auto& s=impl_->states.at(game);s.action={"preparing","正在校验选择快照",nodeId,checkpointId};{std::lock_guard<std::mutex> l(impl_->mutex);impl_->prepared.erase(game);}impl_->Publish(s);
    PreparedRestore result;result.game=game;result.nodeId=nodeId;result.checkpointId=checkpointId;
    try{if(!s.ready||s.blocked)Fail("攻略记录尚未就绪");
        Node* node = nullptr;
        if (auto n=s.nodes.find(nodeId); n!=s.nodes.end()) node=&n->second;
        else if (auto chapter=s.chapters.find(nodeId); chapter!=s.chapters.end()) node=&chapter->second;
        if(!node)Fail("这个选择或章节尚未实际到达");auto candidates=node->checkpoints;std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.created>b.created;});
        std::string failure="历史选择没有可恢复的快照，需要在剧情中再次到达";
        for(const auto& cp:candidates){if(!checkpointId.empty()&&cp.id!=checkpointId)continue;try{result.bytes=ValidateCheckpoint(s,cp,true);result.checkpointId=cp.id;result.scenario=node->observation.scenario;result.resourceHash=cp.resource;result.ok=true;break;}catch(const std::exception& e){failure=e.what();for(auto& stored:node->checkpoints)if(stored.id==cp.id){stored.usable=false;stored.error=failure;}}}
        if(!result.ok)throw std::runtime_error(failure);s.action={"prepared","选择快照已校验",nodeId,result.checkpointId};s.lastError.clear();
    }catch(const std::exception& e){result.error=e.what();s.lastError=result.error;s.action={"failed",result.error,nodeId,checkpointId};}
    {std::lock_guard<std::mutex> l(impl_->mutex);impl_->prepared[game]=std::move(result);}impl_->Publish(s);
});}
bool Store::TakePreparedRestore(const std::string& game,PreparedRestore& result){std::lock_guard<std::mutex> l(impl_->mutex);auto i=impl_->prepared.find(game);if(i==impl_->prepared.end())return false;result=std::move(i->second);impl_->prepared.erase(i);return true;}
void Store::CompleteRestore(std::string game,bool success,std::string message){impl_->Enqueue(game,[this,game,success,message]{auto& s=impl_->states.at(game);s.action.state=success?"completed":"failed";s.action.message=message.empty()?(success?"已恢复选择节点":"选择节点恢复失败，当前剧情未改变"):message;if(success){s.pendingSource.clear();s.pendingOption=0;s.lastError.clear();}else s.lastError=s.action.message;impl_->Publish(s);});}
void Store::SetOverview(std::string game,std::string chapters,std::string endings,std::string specials){impl_->Enqueue(game,[this,game,chapters,endings,specials]{auto& s=impl_->states.at(game);auto a=Parser(chapters).parse(),b=Parser(endings).parse(),c=Parser(specials).parse();if(a.kind!=J::Array||b.kind!=J::Array||c.kind!=J::Array)Fail("攻略概览格式异常");s.overviewChapters=std::move(a);s.endings=std::move(b);s.specials=std::move(c);s.overviewLoaded=true;impl_->Publish(s);});}
void Store::SetDefaultSpecials(std::string game,std::string specials){impl_->Enqueue(game,[this,game,specials]{auto& s=impl_->states.at(game);if(s.overviewLoaded)return;auto value=Parser(specials).parse();if(value.kind!=J::Array)Fail("攻略入口格式异常");s.specials=std::move(value);impl_->Publish(s);});}
void Store::SetError(std::string game,std::string message){impl_->Enqueue(game,[this,game,message]{impl_->Error(game,message);});}
void Store::SetAction(std::string game,std::string state,std::string message,std::string nodeId,std::string checkpointId){impl_->Enqueue(game,[this,game,state,message,nodeId,checkpointId]{auto& s=impl_->states.at(game);s.action={state,message,nodeId,checkpointId};impl_->Publish(s);});}
} // namespace entis::guide
