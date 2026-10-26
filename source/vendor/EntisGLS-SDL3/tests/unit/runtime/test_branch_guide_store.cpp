#include "runtime/cotopha_port/branch_guide_store.h"
#include "runtime/cotopha_port/branch_native_state.h"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace entis::guide;
namespace {
std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(),s.end()}; }
std::string Read(const fs::path& p) { std::ifstream in(p,std::ios::binary); return {(std::istreambuf_iterator<char>(in)),{}}; }
void Write(const fs::path& p,const std::string& s) { std::ofstream out(p,std::ios::binary|std::ios::trunc); out.write(s.data(),s.size()); assert(out.good()); }
bool Has(const std::string& json,const std::string& text) { return json.find(text)!=std::string::npos; }
Observation ChoiceNode(const std::string& id,const std::string& snapshot="native-object") {
    Observation o;o.game="yosuga";o.nodeId=id;o.scenario="00_Z008";o.title="真实选择 \"一\"";o.chapter="序章";o.route="共通";o.ordinal=1;o.createdAt=12345;
    o.options={{1,"第一项",true,false},{2,"第二项",true,false}};o.snapshot=Bytes(snapshot);return o;
}
fs::path Index(const fs::path& root) { return root / "01 攻略进度" / "01 分支记录.json"; }
fs::path Snapshots(const fs::path& root) { return root / "01 攻略进度" / "02 选择快照"; }
// A synthetic metadata-only Haruka-style layout, without game/save bytes.
std::vector<uint8_t> BustupWire(bool includeNativeSprite) {
    std::vector<uint8_t> b;
    auto number=[&](unsigned n){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(n>>(i*8)));};
    auto text=[&](const std::string& s){number(unsigned(s.size()));for(char c:s){b.push_back(uint8_t(c));b.push_back(0);}};
    auto begin=[&](const std::string& name,unsigned count){b.push_back(0);text(name);number(count);};
    auto endArray=[&]{b.push_back(0xff);number(0x80000000u);};
    begin("ScenarioObject",1);
    b.push_back(2);number(1);
    begin("BustupInfo",2);
    if(includeNativeSprite){begin("Sprite",0);endArray();number(0);}else b.push_back(0xff);
    b.push_back(0xff);endArray();number(2);text("spr");text("sprImage");
    endArray();endArray();number(1);text("bustup");
    return b;
}
}

int main(int argc, char** argv) {
    assert(IsBoundedNativeState(BustupWire(false)));
    assert(!IsBoundedNativeState(BustupWire(true)));
    // A real isolated game's native object stream can be supplied locally;
    // proprietary game/save bytes are never embedded in the test sources.
    if (argc == 2) {
        const auto valid=Bytes(Read(argv[1]));assert(IsBoundedNativeState(valid));
        auto bad=valid;bad.push_back(0);assert(!IsBoundedNativeState(bad));
        for(size_t length=0;length<valid.size();length+=std::max<size_t>(1,valid.size()/100)){
            bad.assign(valid.begin(),valid.begin()+length);assert(!IsBoundedNativeState(bad));
        }
        assert(valid.size()>5);const size_t nameLength=valid[1]|(size_t(valid[2])<<8)|(size_t(valid[3])<<16)|(size_t(valid[4])<<24);
        const size_t countOffset=5+nameLength*2;assert(countOffset+5<valid.size());
        bad=valid;for(size_t i=0;i<4;++i)bad[countOffset+i]=0xff;assert(!IsBoundedNativeState(bad));
        bad=valid;bad[countOffset+4]=1;assert(!IsBoundedNativeState(bad)); // external VM reference
        bad=valid;bad[5]='W';assert(!IsBoundedNativeState(bad)); // foreign/native class name
    }
    assert(Store::HashBytes({})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Store::HashBytes(Bytes("abc"))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto base=fs::temp_directory_path()/("branch-guide-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base);
    struct Cleanup {fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{base};
    const auto root=base/"savedata";fs::create_directory(root);
    const auto resource=Store::HashBytes(Bytes("resource-v1"));
    {
        Store store;store.Configure("yosuga",root.string(),Bytes("resource-v1"));store.Observe(ChoiceNode("first"));store.Flush();
        auto json=store.LatestJson("yosuga");assert(Has(json,"\"ready\":true"));assert(Has(json,"\"can_restore\":true"));assert(Has(json,"真实选择 \\\"一\\\""));assert(Has(json,"\"edges\":[]"));
        store.Choice("yosuga","first",1);Observation chapter;chapter.game="yosuga";chapter.kind="chapter";chapter.nodeId="chapter-one";chapter.scenario="00_Z009";chapter.title="下一章";chapter.snapshot=Bytes("chapter-native");store.Observe(chapter);store.Flush();
        json=store.LatestJson("yosuga");assert(Has(json,"\"edges\":[]"));assert(Has(json,"\"captured_chapters\":[{"));
        store.SetOverview("yosuga","[{\"id\":\"chapter:Z\",\"chapter_node_id\":\"chapter-one\"}]","[]","[]");
        store.PrepareRestore("yosuga","chapter-one");store.ResetTraversal("yosuga");store.Flush();
        PreparedRestore chapterReady;assert(store.TakePreparedRestore("yosuga",chapterReady));
        assert(chapterReady.ok&&chapterReady.bytes==Bytes("chapter-native"));
        assert(Has(store.LatestJson("yosuga"),"\"enterable\":true"));
        // Reconfigure an already loaded sibling without losing the request.
        store.PrepareRestore("yosuga","chapter-one");store.Configure("yosuga",root.string(),resource);store.Flush();
        assert(store.TakePreparedRestore("yosuga",chapterReady)&&chapterReady.ok);
        store.Choice("yosuga","first",1);store.Flush();
        store.Observe(ChoiceNode("second","object-two"));store.Flush();json=store.LatestJson("yosuga");
        assert(Has(json,"\"source_id\":\"first\""));assert(Has(json,"\"target_id\":\"second\""));assert(Has(json,"\"option_index\":1"));
        store.ResetTraversal("yosuga");store.Observe(ChoiceNode("third","object-three"));store.SetOverview("yosuga","[{\"id\":\"chapter-one\"}]","[]","[]");store.Flush();
        json=store.LatestJson("yosuga");assert(!Has(json,"\"source_id\":\"second\""));assert(Has(json,"\"entry_id\":\"chapter-one\""));
        store.PrepareRestore("yosuga","first");store.Flush();PreparedRestore ready;assert(store.TakePreparedRestore("yosuga",ready));assert(ready.ok&&ready.bytes==Bytes("native-object")&&ready.resourceHash==resource);assert(!store.TakePreparedRestore("yosuga",ready));
        const auto digest=Store::HashBytes(Bytes("native-object"));Write(Snapshots(root)/(digest+".dat"),"native-objecx");
        store.PrepareRestore("yosuga","first");store.Flush();assert(store.TakePreparedRestore("yosuga",ready));assert(!ready.ok&&!ready.error.empty()&&ready.bytes.empty());
        assert(Has(store.LatestJson("yosuga"),"\"active_node_id\":\"third\""));
        store.PrepareRestore("yosuga","unknown");store.Flush();assert(store.TakePreparedRestore("yosuga",ready)&&!ready.ok);
        // Distinct contexts are capped, and identical snapshot bytes are deduplicated.
        for(unsigned i=0;i<8;++i){auto o=ChoiceNode("bounded","context-"+std::to_string(i));o.createdAt=20000+i;store.Observe(std::move(o));}
        store.Observe(ChoiceNode("bounded","context-7"));store.Flush();
        size_t files=0;for(const auto& p:fs::directory_iterator(Snapshots(root)))if(p.is_regular_file())++files;
        assert(files==9); // three choices, one chapter, five retained contexts.
        store.Choice("yosuga","bounded",2);store.Flush();
    }
    {
        const auto fresh=base/"new-resource";fs::create_directory(fresh);
        { Store store;store.Configure("yosuga",fresh.string(),resource);store.Observe(ChoiceNode("old-choice"));store.Flush(); }
        { Store store;store.Configure("yosuga",fresh.string(),Store::HashBytes(Bytes("resource-v2")));store.Flush();
          assert(Has(store.LatestJson("yosuga"),"\"nodes\":[]"));
          assert(!Has(store.LatestJson("yosuga"),"old-choice"));
          bool archived=false;for(const auto& p:fs::directory_iterator(fresh/"01 攻略进度"))archived|=p.path().filename().string().find(".version-")!=std::string::npos;
          assert(archived);store.Observe(ChoiceNode("new-choice"));store.Flush();
          assert(Has(store.LatestJson("yosuga"),"new-choice")); }
    }
    {
        Store store;store.Configure("yosuga",root.string(),resource);store.Flush();auto json=store.LatestJson("yosuga");assert(Has(json,"\"ready\":true"));assert(Has(json,"\"source_id\":\"first\""));assert(Has(json,"\"usable\":false"));
        // Pending traversal is process-local; opening a new game cannot join old nodes.
        store.Observe(ChoiceNode("fresh","new-object"));store.Flush();assert(!Has(store.LatestJson("yosuga"),"\"source_id\":\"bounded\""));
    }
    {
        // A valid JSON index with an escaping checkpoint path is refused and preserved.
        auto tampered=Read(Index(root));const auto field=tampered.find("\"file_name\":\"");assert(field!=std::string::npos);const auto start=field+std::string("\"file_name\":\"").size();const auto end=tampered.find('"',start);tampered.replace(start,end-start,"../outside.dat");Write(Index(root),tampered);
        Store store;store.Configure("yosuga",root.string(),resource);store.Observe(ChoiceNode("never-written"));store.Flush();assert(Has(store.LatestJson("yosuga"),"\"ready\":false"));assert(Read(Index(root))==tampered);
        bool backup=false;for(const auto& entry:fs::directory_iterator(root/"01 攻略进度"))backup|=entry.path().filename().string().find(".corrupt.")!=std::string::npos;assert(backup);
    }
    {
        const auto fresh=base/"fresh";fs::create_directory(fresh);const auto script=base/"game.csx";Write(script,"resource-v1");Store store;store.ConfigureResourceFile("yosuga",fresh.string(),script.string());store.Observe(ChoiceNode("resource-node"));store.Flush();
        assert(Store::ResourceFileMatches(script.string(),resource));
        assert(store.ResourceHash("yosuga")==resource);
        Write(script,"resource-v2");store.PrepareRestore("yosuga","resource-node");store.Flush();PreparedRestore ready;assert(store.TakePreparedRestore("yosuga",ready)&&!ready.ok);assert(Has(store.LatestJson("yosuga"),"剧本资源校验失败"));
        assert(!Store::ResourceFileMatches(script.string(),resource));
    }
#if !defined(_WIN32)
    {
        const auto fresh=base/"symlink";const auto outside=base/"outside";fs::create_directory(fresh);fs::create_directory(outside);fs::create_directory_symlink(outside,fresh/"01 攻略进度");
        Store store;store.Configure("yosuga",fresh.string(),resource);store.Observe(ChoiceNode("no-escape"));store.Flush();assert(Has(store.LatestJson("yosuga"),"\"status\":\"error\""));assert(fs::is_empty(outside));
    }
#endif
    {
        const auto fresh=base/"oversized";fs::create_directories(fresh/"01 攻略进度");const std::string tooLarge(8*1024*1024+1,' ');Write(Index(fresh),tooLarge);Store store;store.Configure("yosuga",fresh.string(),resource);store.Flush();assert(Has(store.LatestJson("yosuga"),"\"ready\":false"));assert(fs::file_size(Index(fresh))==tooLarge.size());
    }
    std::cout<<"branch_guide_store: SHA256, async persistence/reload, real edges, chapter separation, checkpoint cap/dedupe, restore validation, resource freshness, traversal reset, corrupt-index backup and path restrictions PASS\n";
}
