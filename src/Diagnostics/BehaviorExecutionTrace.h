#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include "BehaviorGraphTrace.h"

// Diagnostic only; keep native TAS and retail probe copies identical.
namespace BehaviorExecutionTrace {
struct Entry { CKBEHAVIORFCT function; std::string path; };
inline std::map<CK_ID, Entry> entries;
inline FILE *stream = nullptr;
inline FILE *prelude = nullptr;
inline FILE *lateStream = nullptr;
inline std::function<CKObject *(CK_ID)> objectLookup;
inline unsigned lateBegin = 0, lateEnd = 0;
inline bool preludeOpened = false;
inline unsigned firstTick = 0;
inline unsigned sequence = 0;
inline std::set<CK_ID> referenced;
inline void Walk(CKBehavior *node, const std::string &path);
inline bool IsMovement(CKBehavior *behavior) {
    const char *name = behavior->GetName();
    return name && (!std::strcmp(name,"Move To") || !std::strcmp(name,"Translate")
        || !std::strcmp(name,"TT Set Dynamic Position") || !std::strcmp(name,"Set Position") || !std::strcmp(name,"Interpolator Position"));
}
inline void StoredParameter(const char *kind, int index, CKParameter *parameter) {
    if (!parameter) return;
    const int size=parameter->GetDataSize();
    // FALSE is essential: reading an output must not execute its operation.
    const auto *bytes=static_cast<const unsigned char *>(parameter->GetReadDataPtr(FALSE));
    std::fprintf(lateStream,"PARAM\t%s\t%d\t%s\t%d\t",kind,index,BehaviorGraphTrace::Name(parameter),size);
    if (bytes && size>=0 && size<=128) for (int i=0;i<size;++i) std::fprintf(lateStream,"%02x",bytes[i]);
    else std::fprintf(lateStream,"not-copied");
    std::fprintf(lateStream,"\n");
}
#ifdef _WIN32
inline void CachedInverse(CKObject *object) {
    if (!object) return;
    // Original CK2_3D.dll GetInverseWorldMatrix 0x10008909. Never invoke
    // lazy recomputation: observe the original cache and its validity bit.
    unsigned flags=0; const auto *base=reinterpret_cast<const unsigned char *>(object);
    std::memcpy(&flags,base+256,4);
    std::fprintf(lateStream,"CACHED_INVERSE\t%u\t%s\t%08x",(unsigned)object->GetID(),BehaviorGraphTrace::Name(object),flags);
    if (flags & 0x400000) {
        float matrix[16];std::memcpy(matrix,base+260,sizeof(matrix));
        for (float v:matrix) std::fprintf(lateStream,"\t%a",v);
    }
    std::fprintf(lateStream,"\n");
}
#endif
inline void MovementState(const char *phase, CKBehavior *behavior) {
#ifdef _WIN32
    unsigned short cw=0; __asm__ __volatile__("fnstcw %0":"=m"(cw));
    std::fprintf(lateStream,"FPU\t%s\t%04x\n",phase,cw);
#endif
    auto *target=behavior->GetTarget();
    const char *name=BehaviorGraphTrace::Name(target);
#ifdef _WIN32
    CachedInverse(target);
#endif
    std::fprintf(lateStream,"MOVEMENT\t%s\t%u\t%s\n",phase,(unsigned)behavior->GetID(),name);
    if (target && (!std::strcmp(name,"Ball_Wood") || !std::strcmp(name,"Ball_Paper") || !std::strcmp(name,"Ball_Stone"))) {
        const VxMatrix &matrix=static_cast<CK3dEntity *>(target)->GetWorldMatrix();
        std::fprintf(lateStream,"MATRIX");
        for (int r=0;r<4;++r) for (int c=0;c<4;++c) std::fprintf(lateStream,"\t%a",matrix[r][c]);
        std::fprintf(lateStream,"\n");
    }
    if (objectLookup && behavior->GetName() && !std::strcmp(behavior->GetName(),"TT Set Dynamic Position")) {
        // The two declared entity parameters are read without evaluating links.
        for (int index : {0,10}) {
            auto *input=behavior->GetInputParameter(index);
            auto *source=input ? input->GetRealSource():nullptr;
            CK_ID id=0;
            if (source && source->GetDataSize()==sizeof(id))
                if (auto *bytes=source->GetReadDataPtr(FALSE)) std::memcpy(&id,bytes,sizeof(id));
            CKObject *object=id ? objectLookup(id):nullptr;
#ifdef _WIN32
            CachedInverse(object);
#endif
            std::fprintf(lateStream,"ENTITY_INPUT\t%d\t%u\t%s",index,(unsigned)id,BehaviorGraphTrace::Name(object));
            if (object) {
                const VxMatrix &matrix=static_cast<CK3dEntity *>(object)->GetWorldMatrix();
                for (int r=0;r<4;++r) for (int c=0;c<4;++c) std::fprintf(lateStream,"\t%a",matrix[r][c]);
            }
            std::fprintf(lateStream,"\n");
        }
    }
    for (int i=0;i<behavior->GetInputParameterCount();++i) {
        auto *input=behavior->GetInputParameter(i);
        StoredParameter("input",i,input ? input->GetRealSource():nullptr);
    }
    for (int i=0;i<behavior->GetLocalParameterCount();++i) StoredParameter("local",i,behavior->GetLocalParameter(i));
    for (int i=0;i<behavior->GetOutputParameterCount();++i) StoredParameter("output",i,behavior->GetOutputParameter(i));
}
inline void IteratorState(const char *phase, unsigned id, unsigned tick, CKBehavior *behavior) {
    auto *group=static_cast<CKGroup *>(behavior->GetInputParameterObject(0));
    int index=-999; unsigned element=0;
    if (auto *parameter=behavior->GetOutputParameter(1))
        if (void *data=parameter->GetReadDataPtr(FALSE)) std::memcpy(&index,data,4);
    if (auto *parameter=behavior->GetOutputParameter(0))
        if (void *data=parameter->GetReadDataPtr(FALSE)) std::memcpy(&element,data,4);
    std::fprintf(stream,"ITER_%s\t%u\t%u\t%d\t%d\t%u\n",phase,id,tick,
        group ? group->GetObjectCount():-1,index,element);
}
inline int Invoke(const CKBehaviorContext &context) {
    CKBehavior *behavior = context.Behavior;
    auto found = entries.find(behavior->GetID());
    if (found == entries.end()) {
        if (stream) std::fprintf(stream, "MISSING\t%u\n", (unsigned)behavior->GetID());
        return CKBR_GENERICERROR;
    }
    // Copy before calling: a behavior can load scripts and grow the map.
    const Entry entry = found->second;
    const unsigned tick = (unsigned)context.TimeManager->GetMainTickCount();
    const unsigned relativeTick=tick-firstTick;
    if (lateStream && relativeTick>lateEnd) { std::fclose(lateStream); lateStream=nullptr; }
    const bool logLate=lateStream && relativeTick>=lateBegin && relativeTick<=lateEnd;
    if (stream && tick - firstTick >= 150) { std::fclose(stream); stream = nullptr; }
    const bool log = stream != nullptr;
    const bool logPrelude = prelude && entry.path.find("/Default Level#0/")==0;
    unsigned mask = 0;
    if (log || logPrelude || logLate) for (int i = 0; i < behavior->GetInputCount() && i < 32; ++i)
        if (behavior->IsInputActive(i)) mask |= 1u << i;
    const unsigned id = sequence++;
    const bool movement=logLate && IsMovement(behavior);
    if (logLate) std::fprintf(lateStream,"IN\t%u\t%u\t%u\t%a\t%u\t%u\t%s\n",
        id,tick,relativeTick,context.DeltaTime,(unsigned)behavior->GetID(),mask,entry.path.c_str());
    if (movement) MovementState("IN",behavior);
    if (logPrelude) std::fprintf(prelude,"IN\t%u\t%u\t%a\t%u\t%u\t%s\n",
        id,tick,context.DeltaTime,(unsigned)behavior->GetID(),mask,entry.path.c_str());
    if (log) std::fprintf(stream, "IN\t%u\t%u\t%u\t%a\t%u\t%u\t%s\n",
        id, tick, tick-firstTick, context.DeltaTime, (unsigned)behavior->GetID(), mask, entry.path.c_str());
    const bool iterator=log && entry.path.find("/Preload Sound#0/Preload#0/Group Iterator#0") != std::string::npos;
    if (iterator) IteratorState("IN",id,tick-firstTick,behavior);
    if (log && (mask & 1) && entry.path.find("/Preload Sound#0/Preload#0/Group Iterator#0") != std::string::npos) {
        auto *group=static_cast<CKGroup *>(behavior->GetInputParameterObject(0));
        if (group) {
            std::fprintf(stream,"GROUP\t%u\t%u\t%d\t%s\n",tick-firstTick,(unsigned)group->GetID(),group->GetObjectCount(),BehaviorGraphTrace::Name(group));
            for (int i=0; i<group->GetObjectCount(); ++i) {
                CKBeObject *object=group->GetObject(i);
                std::fprintf(stream,"MEMBER\t%d\t%u\t%d\t%u\t%s\n",i,object ? (unsigned)object->GetID():0,
                    object ? (int)object->GetClassID():0,object ? (unsigned)object->GetObjectFlags():0,BehaviorGraphTrace::Name(object));
            }
        }
    }
    if ((log || logLate) && behavior->GetName() && !std::strcmp(behavior->GetName(), "Execute Script")) {
        auto *target=static_cast<CKBehavior *>(behavior->GetInputParameterObject(1));
        if (target) {
            std::fprintf(log ? stream:lateStream,"TARGET\t%u\t%u\t%u\t%u\t%d\t%s\n",id,tick,tick-firstTick,
                (unsigned)target->GetID(),(int)target->IsActive(),BehaviorGraphTrace::Name(target));
            if (referenced.insert(target->GetID()).second) {
                Walk(target,std::string("/")+BehaviorGraphTrace::Name(target)+"#0");
                const char *path=std::getenv("BALLANCE_TAS_REFERENCED_GRAPHS");
                if (path && *path) {
                    if (FILE *file=std::fopen(path,"a")) {
                        BehaviorGraphTrace::Write(file,target,0); std::fclose(file);
                    }
                }
            }
        }
    }
    const int result = entry.function(context);
    // Only movement functions are inspected after returning; delete/unload
    // callbacks may invalidate their own behavior or target.
    if (movement && lateStream) MovementState("OUT",behavior);
    if (logLate && lateStream) std::fprintf(lateStream,"OUT\t%u\t%u\t%u\t%d\t%s\n",id,tick,relativeTick,result,entry.path.c_str());
    if (logPrelude && prelude) std::fprintf(prelude,"OUT\t%u\t%u\t%d\t%s\n",id,tick,result,entry.path.c_str());
    if (iterator && stream) IteratorState("OUT",id,tick-firstTick,behavior);
    if (log && stream) std::fprintf(stream, "OUT\t%u\t%u\t%u\t%d\t%s\n",
        id, tick, tick-firstTick, result, entry.path.c_str());
    return result;
}
inline void Walk(CKBehavior *node, const std::string &path) {
    CKBEHAVIORFCT function = node->GetFunction();
    if (function && function != &Invoke) {
        entries[node->GetID()] = {function, path};
        node->SetFunction(&Invoke);
    }
    std::map<std::string, unsigned> ordinals;
    for (int i=0; i<node->GetSubBehaviorCount(); ++i) {
        CKBehavior *child=node->GetSubBehavior(i);
        const std::string name=child->GetName() ? child->GetName() : "(null)";
        const unsigned ordinal=ordinals[name]++;
        Walk(child,path+"/"+name+"#"+std::to_string(ordinal));
    }
}
inline void Install(CKBehavior *script) {
    if (!std::getenv("BALLANCE_TAS_EXECUTION_TRACE") || !script || !script->GetName()) return;
    const char *name=script->GetName();
    if (!std::strcmp(name,"Default Level") && !preludeOpened) {
        const char *path=std::getenv("BALLANCE_TAS_STARTUP_TRACE");
        if (path && *path) { prelude=std::fopen(path,"w"); preludeOpened=true; }
    }
    if (!std::strcmp(name,"Default Level") || !std::strcmp(name,"Event_handler") || !std::strcmp(name,"Level_Init") || !std::strcmp(name,"Gameplay_Ingame"))
        Walk(script,std::string("/")+name+"#0");
}
inline void Arm(CKTimeManager *time, CKBehavior *startup) {
    const char *path=std::getenv("BALLANCE_TAS_EXECUTION_TRACE");
    if (!path || !*path) return;
    if (stream) std::fclose(stream);
    if (prelude) { std::fprintf(prelude,"PRELOAD\t%u\n",(unsigned)time->GetMainTickCount()); std::fclose(prelude); prelude=nullptr; }
    stream=std::fopen(path,"w"); firstTick=(unsigned)time->GetMainTickCount(); sequence=0;
    if (lateStream) { std::fclose(lateStream); lateStream=nullptr; }
    const char *latePath=std::getenv("BALLANCE_TAS_ANIMATION_TRACE");
    const char *range=std::getenv("BALLANCE_TAS_ANIMATION_RANGE");
    if (latePath && range && std::sscanf(range,"%u,%u",&lateBegin,&lateEnd)==2 && lateBegin<=lateEnd)
        lateStream=std::fopen(latePath,"w");
    if (stream && startup) {
        unsigned registered=0;
        for (const auto &entry:entries) if (entry.second.path.find("/Default Level#0/")==0) ++registered;
        std::fprintf(stream,"STARTUP_ROOT\t%u\t%d\t%u\n",(unsigned)startup->GetID(),(int)startup->IsActive(),registered);
        Install(startup);
    }
}
template<class Lookup> inline void Stop(Lookup lookup) {
    if (lateStream) { std::fclose(lateStream); lateStream=nullptr; }
    if (prelude) { std::fclose(prelude); prelude=nullptr; }
    if (stream) { std::fclose(stream); stream=nullptr; }
    for (const auto &entry:entries) {
        CKObject *object=lookup(entry.first);
        if (object && object->GetClassID()==CKCID_BEHAVIOR) {
            auto *behavior=static_cast<CKBehavior *>(object);
            if (behavior->GetFunction()==&Invoke) behavior->SetFunction(entry.second.function);
        }
    }
    entries.clear();
    referenced.clear();
}
}
