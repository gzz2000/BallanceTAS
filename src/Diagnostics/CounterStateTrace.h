#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <cctype>

// Bounded read-only counter diagnostics. Never evaluate parameter operations:
// GetReadDataPtr(FALSE) reads only the value already stored by the game.
class CounterStateTrace {
public:
    ~CounterStateTrace() { Reset(); }
    void Reset() { if (m_File) std::fclose(m_File); m_File=nullptr; m_Opened=false; m_Inventory=false; }
    template<class Loader> void Sample(Loader *bml, std::uint64_t cursor) {
        if (!m_Opened) {
            m_Opened=true;
            const char *path=std::getenv("BALLANCE_COUNTER_STATE_TRACE");
            const char *range=std::getenv("BALLANCE_COUNTER_STATE_RANGE");
            unsigned long long a=0,b=0;
            if (path && *path && range && std::sscanf(range,"%llu:%llu",&a,&b)==2 && a<=b) {
                m_First=a; m_Last=b; m_File=std::fopen(path,"wb");
            }
        }
        if (!m_File || cursor<m_First || cursor>m_Last) return;
        auto *context=bml->GetCKContext();
        double elapsed=0; std::uint64_t ticks=0; CKBOOL active=FALSE,recorded=FALSE; int valid=0;
#ifdef _WIN32
        if (auto *manager=context->GetManagerByGuid(CKGUID(0x60632E28,0x7D3B3C7D))) {
            const auto *base=reinterpret_cast<const unsigned char *>(manager);
            float value=0; std::memcpy(&value,base+44,4);elapsed=value;active=base[40]!=0;valid=1;recorded=TRUE;
        }
        unsigned short cw=0; __asm__ __volatile__("fnstcw %0":"=m"(cw));
        std::fprintf(m_File,"FPU\t%llu\t%04x\n",(unsigned long long)cursor,cw);
#else
        auto *plugins=CKGetPluginManager();
        auto *entry=plugins?plugins->FindComponent(CKGUID(0x60632E28,0x7D3B3C7D)):nullptr;
        auto *dll=entry?plugins->GetPluginDllInfo(entry->m_PluginDllIndex):nullptr;
        using Capture=int (*)(CKContext *,std::uint64_t *,double *,CKBOOL *,CKBOOL *);
        auto capture=dll?reinterpret_cast<Capture>(dll->GetFunctionPtr(const_cast<char *>("TT_TimeCaptureRuntimeStateV2"))):nullptr;
        if (capture) valid=capture(context,&ticks,&elapsed,&active,&recorded)==CK_OK;
#endif
        std::fprintf(m_File,"CLOCK\t%llu\t%d\t%a\t%llu\t%d\t%d\n",(unsigned long long)cursor,valid,elapsed,(unsigned long long)ticks,int(active),int(recorded));
        const auto &objects=context->GetObjectListByType(CKCID_BEHAVIOR,TRUE);
        for (int i=0;i<objects.Size();++i) {
            auto *node=static_cast<CKBehavior *>(objects[i]);
            if (node && !node->GetParent()) Walk(node,Name(node),0);
        }
        m_Inventory=true;
        std::fputs("END\n",m_File);std::fflush(m_File);
    }
private:
    static const char *Name(CKObject *object) { return object && object->GetName()?object->GetName():"(null)"; }
    void Parameter(const char *kind,int i,CKParameter *p) {
        if (!p) return;
        const auto *bytes=static_cast<const unsigned char *>(p->GetReadDataPtr(FALSE));
        const int size=p->GetDataSize();
        std::fprintf(m_File,"PARAM\t%s\t%d\t%s\t%d\t",kind,i,Name(p),size);
        if (bytes && size>=0 && size<=64) for(int j=0;j<size;++j)std::fprintf(m_File,"%02x",bytes[j]);
        else std::fputs("not-copied",m_File);
        std::fputc('\n',m_File);
    }
    void Walk(CKBehavior *node,const std::string &path,int depth) {
        if (!node || depth>64) return;
        std::string lower=path;
        for(char &c:lower)c=char(std::tolower(static_cast<unsigned char>(c)));
        const bool selected=lower.find("counter")!=std::string::npos || lower.find("energy")!=std::string::npos || lower.find("tt_timer")!=std::string::npos;
        if (!m_Inventory) std::fprintf(m_File,"INVENTORY\t%u\t%s\n",(unsigned)node->GetID(),path.c_str());
        if (selected) {
            std::fprintf(m_File,"NODE\t%u\t%s\n",(unsigned)node->GetID(),path.c_str());
            if (!m_Inventory && path=="Gameplay_Energy/Timer/Test") {
                auto guid=node->GetPrototypeGuid();
                std::fprintf(m_File,"PROTOTYPE\t%08x\t%08x\n",guid.d1,guid.d2);
                for (int i=0;i<node->GetInputParameterCount();++i) {
                    auto *pin=node->GetInputParameter(i); if(!pin)continue;
                    auto type=pin->GetGUID();
                    std::fprintf(m_File,"PIN\t%d\t%08x\t%08x\t%d\n",i,type.d1,type.d2,int(pin->GetRealSource()!=nullptr));
                }
            }
            for(int i=0;i<node->GetInputCount();++i)std::fprintf(m_File,"ACTIVE\t%d\t%d\n",i,int(node->IsInputActive(i)));
            for(int i=0;i<node->GetInputParameterCount();++i){auto *p=node->GetInputParameter(i);Parameter("input",i,p?p->GetRealSource():nullptr);}
            for(int i=0;i<node->GetOutputParameterCount();++i)Parameter("output",i,node->GetOutputParameter(i));
            for(int i=0;i<node->GetLocalParameterCount();++i)Parameter("local",i,node->GetLocalParameter(i));
            for(int i=0;i<node->GetParameterOperationCount();++i){
                auto *op=node->GetParameterOperation(i);if(!op)continue;
                std::fprintf(m_File,"OP\t%d\t%s\t%p\n",i,Name(op),reinterpret_cast<void *>(op->GetOperationFunction()));
                auto *a=op->GetInParameter1(),*b=op->GetInParameter2();
                Parameter("op-input",0,a?a->GetRealSource():nullptr);Parameter("op-input",1,b?b->GetRealSource():nullptr);Parameter("op-output",0,op->GetOutParameter());
            }
        }
        for(int i=0;i<node->GetSubBehaviorCount();++i){auto *child=node->GetSubBehavior(i);if(child)Walk(child,path+"/"+Name(child),depth+1);}
    }
    FILE *m_File=nullptr;bool m_Opened=false,m_Inventory=false;std::uint64_t m_First=0,m_Last=0;
};
