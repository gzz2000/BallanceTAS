#pragma once
#include <cstdint>

// Opt-in, native replay diagnostics. The hash encoding is field name + length +
// exact bytes, not floating-point comparison. Identities are (class,name,ordinal
// among equal names in ID order); ID numbers are retained in the detail stream.
// This observes semantic transforms separately from lazy renderer caches. It
// does not force an inverse calculation, render, or change any CK object.
#ifdef __APPLE__
#include <CKEntityTransformState.h>
#include <CKEntityUserBoxState.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class VisualStateTrace {
    enum Category { Local, Cache, Rect, Text, Camera, Animation, Material, Texture,
                    CacheWorld, CacheInverse, CacheHistory, CacheFlags, UserBox, Count };
    static constexpr const char *Names[Count]={"entity_local","entity_cache","entity_2d","sprite_text","camera","animation","material","texture",
        "cache_world","cache_inverse","cache_history","cache_flags","entity_user_box"};
    struct Digest {
        CC_SHA256_CTX ctx{};
        Digest() { CC_SHA256_Init(&ctx); }
        void Bytes(const void *p, std::size_t n) { CC_SHA256_Update(&ctx,p,static_cast<CC_LONG>(n)); }
        void String(const std::string &s) { const std::uint32_t n=static_cast<std::uint32_t>(s.size());Bytes(&n,sizeof(n));Bytes(s.data(),n); }
        std::array<unsigned char,32> End() { std::array<unsigned char,32> out{};CC_SHA256_Final(out.data(),&ctx);return out; }
    };
    struct Object { CKObject *pointer; std::string name,key; };
    struct Fields {
        VisualStateTrace &trace; Digest digest; Category category; const Object &object;
        Fields(VisualStateTrace &t,Category c,const Object &o):trace(t),category(c),object(o){}
        void Bytes(const char *name,const void *p,std::size_t size) {
            digest.String(name); const std::uint32_t n=static_cast<std::uint32_t>(size);digest.Bytes(&n,sizeof(n));digest.Bytes(p,n);
            if(trace.detail && trace.InDetailRange(trace.cursor)) {
                std::fprintf(trace.detail,"%llu,%llu,%s,%u,",(unsigned long long)trace.pass,(unsigned long long)trace.cursor,Names[category],unsigned(object.pointer->GetID()));
                Hex(trace.detail,object.key.data(),object.key.size());std::fprintf(trace.detail,",%s,",name);Hex(trace.detail,p,n);std::fputc('\n',trace.detail);
            }
        }
        template<class T> void Value(const char *name,const T &v) { Bytes(name,&v,sizeof(v)); }
        void String(const char *name,const char *s) { Bytes(name,s?s:"",s?std::strlen(s):0); }
        void Reference(const char *name,CKObject *p) {
            if(!p) { String(name,"<null>");return; }
            auto i=trace.identities.find(p->GetID());
            if(i==trace.identities.end()) { trace.failed=true;String(name,"<unresolved>");return; }
            String(name,i->second.c_str());
        }
        void End() { auto hash=digest.End();trace.roots[category].String(object.key);trace.roots[category].Bytes(hash.data(),hash.size());++trace.counts[category]; }
    };
    using Capture=int(*)(CKObject *,CKEntityTransformState *,std::size_t);
    using CaptureUserBox=int(*)(CKObject *,CKEntityUserBoxState *,std::size_t);
public:
    explicit VisualStateTrace(const char *pathEnvironment = "BALLANCE_VISUAL_STATE_TRACE")
        : pathEnvironment(pathEnvironment) {}
    ~VisualStateTrace() { if(file)std::fclose(file);if(detail)std::fclose(detail);if(renders)std::fclose(renders); }
    void MarkRender(std::uint64_t tick) {
        if(!initialized) Initialize();
        if(!renders || tick<first || tick>last)return;
        // OnRender runs after the engine's scene render. Record the precise
        // preceding Sample and playback cursor, without changing either one.
        std::fprintf(renders,"%llu,%llu,%llu\n",(unsigned long long)pass,
                     (unsigned long long)cursor,(unsigned long long)tick);
        std::fflush(renders);
    }
    template<class Loader> void Sample(Loader *bml,std::uint64_t tick) {
        if(!initialized) Initialize();
        if(!file || tick<first || tick>last)return;
        cursor=tick;++pass;failed=false;identities.clear();objects.clear();
        // Query only these classes; snapshot-owned parameter clones are not
        // visual objects and do not enter identity ordinals or root hashes.
        const CK_CLASSID types[]={CKCID_3DENTITY,CKCID_2DENTITY,CKCID_OBJECTANIMATION,CKCID_ANIMATION,CKCID_MATERIAL,CKCID_TEXTURE,CKCID_MESH};
        for(auto type:types) {
            const auto &list=bml->GetCKContext()->GetObjectListByType(type,TRUE);
            for(int i=0;i<list.Size();++i)if(auto *p=list[i]) objects.push_back({p,p->GetName()?p->GetName():"",{}});
        }
        std::sort(objects.begin(),objects.end(),[](const Object&a,const Object&b){
            if(a.pointer->GetClassID()!=b.pointer->GetClassID())return a.pointer->GetClassID()<b.pointer->GetClassID();
            if(a.name!=b.name)return a.name<b.name;
            return a.pointer->GetID()<b.pointer->GetID();
        });
        CK_CLASSID prev=-1;std::string name;unsigned ordinal=0;
        for(auto &o:objects) {
            auto cls=o.pointer->GetClassID();if(cls!=prev || o.name!=name)ordinal=0;else ++ordinal;
            o.key=std::to_string(cls)+":"+std::to_string(o.name.size())+":"+o.name+":"+std::to_string(ordinal);
            identities.emplace(o.pointer->GetID(),o.key);prev=cls;name=o.name;
        }
        for(int c=0;c<Count;++c) { roots[c]=Digest{};counts[c]=0; }
        for(const auto &o:objects) Observe(o);
        for(int c=0;c<Count;++c) {
            auto hash=roots[c].End();std::fprintf(file,"%llu,%llu,%s,%u,%d,",(unsigned long long)pass,(unsigned long long)cursor,Names[c],counts[c],int(!failed));Hex(file,hash.data(),hash.size());std::fputc('\n',file);
        }
        if((pass&255)==0) { std::fflush(file);if(detail)std::fflush(detail); }
    }
private:
    static void Hex(FILE *f,const void *p,std::size_t n) { const auto *s=static_cast<const unsigned char *>(p);for(std::size_t i=0;i<n;++i)std::fprintf(f,"%02x",unsigned(s[i])); }
    void Initialize() {
        initialized=true;const char *path=std::getenv(pathEnvironment);const char *range=std::getenv("BALLANCE_VISUAL_STATE_RANGE");
        unsigned long long a=0,b=0;if(!path||!*path||!range||std::sscanf(range,"%llu:%llu",&a,&b)!=2||a>b)return;
        first=a;last=b;
        for(std::uint32_t i=0;i<_dyld_image_count() && !capture;++i) {
            const char *image=_dyld_get_image_name(i);if(!image||!std::strstr(image,"/libCK2_3D."))continue;
            if(void *h=dlopen(image,RTLD_LAZY|RTLD_NOLOAD)) {
                capture=reinterpret_cast<Capture>(dlsym(h,"CKCaptureEntityTransformState"));
                captureUserBox=reinterpret_cast<CaptureUserBox>(dlsym(h,"CKCaptureEntityUserBoxState"));
                dlclose(h);
            }
        }
        if(!capture) { std::fputs("VISUAL_STATE_TRACE missing immutable transform capture\n",stderr);return; }
        if(!captureUserBox) { std::fputs("VISUAL_STATE_TRACE missing immutable user-box capture\n",stderr);return; }
        file=std::fopen(path,"wb");if(!file)return;
        std::fputs("pass,cursor,category,objects,valid,sha256\n",file);
        renders=std::fopen((std::string(path)+".renders.csv").c_str(),"wb");
        if(renders)std::fputs("last_sample_pass,last_sample_cursor,playback_cursor\n",renders);
        range=std::getenv("BALLANCE_VISUAL_DETAIL_RANGE");
        while(range && *range) {
            int consumed=0;
            if(std::sscanf(range,"%llu:%llu%n",&a,&b,&consumed)!=2 || a>b || !consumed)break;
            detailRanges.push_back({a,b});range+=consumed;
            if(*range!=',')break;
            ++range;
        }
        if(!detailRanges.empty()) {
            detail=std::fopen((std::string(path)+".details.csv").c_str(),"wb");
            if(detail)std::fputs("pass,cursor,category,id,identity_hex,field,value_hex\n",detail);
        }
    }
    bool InDetailRange(std::uint64_t tick) const {
        for(const auto &range:detailRanges)if(tick>=range.first && tick<=range.second)return true;
        return false;
    }
    void Observe(const Object &o) {
        auto *p=o.pointer;const auto cid=p->GetClassID();
        if(CKIsChildClassOf(cid,CKCID_3DENTITY)) {
            auto *e=static_cast<CK3dEntity *>(p);CKEntityTransformState state{};
            if(!capture(p,&state,sizeof(state))) { failed=true;return; }
            Fields a(*this,Local,o);a.Value("local",state.local);a.Value("flags",e->GetFlags());a.Value("visible",e->IsVisible());a.Value("zOrder",e->GetZOrder());a.Reference("parent",e->GetParent());a.Reference("currentMesh",e->GetCurrentMesh());
            a.Value("meshCount",e->GetMeshCount());for(int i=0;i<e->GetMeshCount();++i)a.Reference("mesh",e->GetMesh(i));
            a.Value("childCount",e->GetChildrenCount());for(int i=0;i<e->GetChildrenCount();++i)a.Reference("child",e->GetChild(i));a.End();
            Fields c(*this,Cache,o);c.Value("world",state.world);c.Value("inverseValid",state.inverseValid);c.Value("inverseGeneration",state.inverseGeneration);if(state.inverseValid)c.Value("inverse",state.inverse);c.Value("hasLastFrame",state.hasLastFrame);if(state.hasLastFrame)c.Value("lastFrame",state.lastFrame);c.Value("moveableFlags",e->GetMoveableFlags());c.End();
            // Keep the legacy composite hash above byte-for-byte comparable.
            // Separate payloads from validity flags so a stale extent flag
            // cannot conceal a changed matrix elsewhere in the full trace.
            Fields w(*this,CacheWorld,o);w.Value("world",state.world);w.End();
            Fields i(*this,CacheInverse,o);i.Value("inverseValid",state.inverseValid);i.Value("inverseGeneration",state.inverseGeneration);if(state.inverseValid)i.Value("inverse",state.inverse);i.End();
            Fields h(*this,CacheHistory,o);h.Value("hasLastFrame",state.hasLastFrame);if(state.hasLastFrame)h.Value("lastFrame",state.lastFrame);h.End();
            Fields f(*this,CacheFlags,o);f.Value("moveableFlags",e->GetMoveableFlags());f.End();
            // Authored bounds affect culling and which callbacks execute. Read
            // their raw snapshot API; GetBoundingBox would mutate lazy caches.
            CKEntityUserBoxState box{};
            if(!captureUserBox(p,&box,sizeof(box))) { failed=true;return; }
            Fields b(*this,UserBox,o);b.Value("enabled",box.enabled);
            if(box.enabled)b.Value("localBox",box.local);
            b.End();
        } else if(CKIsChildClassOf(cid,CKCID_2DENTITY)) {
            auto *e=static_cast<CK2dEntity *>(p);Fields f(*this,Rect,o);VxRect rect{},hom{},src{};e->GetRect(rect);auto hasHom=e->GetHomogeneousRect(hom)==CK_OK;e->GetSourceRect(src);
            f.Value("rect",rect);f.Value("hasHomogeneous",hasHom);if(hasHom)f.Value("homogeneous",hom);f.Value("source",src);f.Value("flags",e->GetFlags());f.Value("visible",e->IsVisible());f.Value("zOrder",e->GetZOrder());f.Reference("parent",e->GetParent());f.Reference("material",e->GetMaterial());
            f.Value("childCount",e->GetChildrenCount());for(int i=0;i<e->GetChildrenCount();++i)f.Reference("child",e->GetChild(i));
            if(CKIsChildClassOf(cid,CKCID_SPRITE))f.Value("slot",static_cast<CKSprite *>(p)->GetCurrentSlot());f.End();
            if(CKIsChildClassOf(cid,CKCID_SPRITETEXT)) { auto *t=static_cast<CKSpriteText *>(p);Fields f(*this,Text,o);f.String("text",t->GetText());f.Value("color",t->GetTextColor());f.Value("background",t->GetBackgroundTextColor());f.Value("alignment",t->GetAlign());f.End(); }
        }
        if(CKIsChildClassOf(cid,CKCID_CAMERA)) { auto *c=static_cast<CKCamera *>(p);Fields f(*this,Camera,o);int w=0,h=0;c->GetAspectRatio(w,h);f.Value("front",c->GetFrontPlane());f.Value("back",c->GetBackPlane());f.Value("fov",c->GetFov());f.Value("projection",c->GetProjectionType());f.Value("zoom",c->GetOrthographicZoom());f.Value("width",w);f.Value("height",h);f.Reference("target",c->GetTarget());f.End(); }
        if(CKIsChildClassOf(cid,CKCID_OBJECTANIMATION)) { Fields f(*this,Animation,o);f.Value("step",static_cast<CKObjectAnimation *>(p)->GetCurrentStep());f.End(); }
        if(CKIsChildClassOf(cid,CKCID_ANIMATION)) { Fields f(*this,Animation,o);f.Value("step",static_cast<CKAnimation *>(p)->GetStep());f.End(); }
        if(CKIsChildClassOf(cid,CKCID_MATERIAL)) {
            auto *m=static_cast<CKMaterial *>(p);Fields f(*this,Material,o);f.Value("ambient",m->GetAmbient());f.Value("diffuse",m->GetDiffuse());f.Value("specular",m->GetSpecular());f.Value("emissive",m->GetEmissive());f.Value("power",m->GetPower());
            for(int i=0;i<4;++i)f.Reference("texture",m->GetTexture(i));f.Value("blendSource",m->GetSourceBlend());f.Value("blendDestination",m->GetDestBlend());f.Value("alphaBlend",m->AlphaBlendEnabled());f.Value("alphaTest",m->AlphaTestEnabled());f.Value("alphaFunction",m->GetAlphaFunc());f.Value("alphaReference",m->GetAlphaRef());f.Value("zWrite",m->ZWriteEnabled());f.Value("twoSided",m->IsTwoSided());f.End();
        }
        if(CKIsChildClassOf(cid,CKCID_TEXTURE)) { auto *t=static_cast<CKTexture *>(p);Fields f(*this,Texture,o);f.Value("slot",t->GetCurrentSlot());f.Value("transparent",t->IsTransparent());f.Value("transparentColor",t->GetTransparentColor());f.End(); }
    }
    const char *pathEnvironment;
    bool initialized=false,failed=false;FILE *file=nullptr,*detail=nullptr,*renders=nullptr;Capture capture=nullptr;
    CaptureUserBox captureUserBox=nullptr;
    std::uint64_t first=0,last=0,pass=0,cursor=0;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> detailRanges;
    std::vector<Object> objects;std::unordered_map<CK_ID,std::string> identities;
    Digest roots[Count];unsigned counts[Count]{};
};
#else
class VisualStateTrace { public: explicit VisualStateTrace(const char * = nullptr) {} void MarkRender(std::uint64_t) {} template<class Loader> void Sample(Loader *,std::uint64_t) {} };
#endif
