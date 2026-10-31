#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

// Diagnostic view of the TT Extra private list, checked against original x86
// disassembly: next0, entity4, previous-position8, force20, active24. Native uses
// the same declared fields with pointer-size alignment. No mutation callbacks.
class ExtraStateTrace {
    struct Node { Node *next; CK3dEntity *ball; VxVector previous; float force; bool active; };
public:
    ~ExtraStateTrace() { if (m_File) std::fclose(m_File); }
    template<class Loader> void Sample(Loader *bml, std::uint64_t cursor) {
        if (!m_Initialized) {
            m_Initialized = true;
            const char *path = std::getenv("BALLANCE_EXTRA_STATE_TRACE");
            const char *range = std::getenv("BALLANCE_EXTRA_STATE_RANGE");
            unsigned long long first=0,last=0;
            if (path && *path && range && std::sscanf(range,"%llu:%llu",&first,&last)==2 && first<=last) {
                m_First=first; m_Last=last; m_File=std::fopen(path,"wb");
                if (m_File) std::fputs("pass,cursor,dt,playing,paused,target,behavior,status,timevalue,timecounter,nextcheck,ballcount,node,active,x,y,z,previous_x,previous_y,previous_z,force,ball_x,ball_y,ball_z,damping,away_damping,away_force,collision_distance,rotation_speed,activation_distance,input_force,force_width,flyaway_time,away_x,away_y,away_z\n",m_File);
            }
        }
        if (!m_File || cursor<m_First || cursor>m_Last) return;
        ++m_Pass;
        const auto &objects=bml->GetCKContext()->GetObjectListByType(CKCID_BEHAVIOR,TRUE);
        for (int i=0;i<objects.Size();++i) {
            auto *beh=static_cast<CKBehavior *>(objects[i]);
            if (!beh || beh->GetPrototypeGuid()!=CKGUID(0x36106bd9,0x51813906)) continue;
            int status=-1,count=0,nextcheck=0;float tv=0,tc=0;Node *list=nullptr;
            beh->GetLocalParameterValue(4,&status);
            if (status<0 || status>2) continue;
            beh->GetLocalParameterValue(1,&list);beh->GetLocalParameterValue(7,&count);
            beh->GetLocalParameterValue(8,&tv);beh->GetLocalParameterValue(6,&tc);beh->GetLocalParameterValue(9,&nextcheck);
            auto *target=beh->GetTarget();auto *ball=static_cast<CK3dEntity *>(beh->GetInputParameterObject(0));
            const char *filter=std::getenv("BALLANCE_EXTRA_TARGET_PREFIX");
            if (filter && *filter && (!target || !target->GetName() ||
                std::strncmp(target->GetName(),filter,std::strlen(filter))!=0)) continue;
            const int indices[]={8,6,5,3,4,2,7,9,10};
            float inputs[9]={};
            for (int k=0;k<9;++k) beh->GetInputParameterValue(indices[k],&inputs[k]);
            VxVector away{};beh->GetLocalParameterValue(10,&away);
            VxVector bp{};if(ball)ball->GetPosition(&bp);
            int node=0;
            for (Node *p=list;p && node<9;p=p->next,++node) {
                VxVector pos{};if(p->ball)p->ball->GetPosition(&pos);
                std::fprintf(m_File,"%llu,%llu,%a,%d,%d,%s,%u,%d,%a,%a,%d,%d,%d,%d,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a",
                    static_cast<unsigned long long>(m_Pass),static_cast<unsigned long long>(cursor),
                    double(bml->GetTimeManager()->GetLastDeltaTime()),int(bml->IsPlaying()),int(bml->IsPaused()),
                    target && target->GetName()?target->GetName():"<none>",unsigned(beh->GetID()),status,double(tv),double(tc),nextcheck,count,node,int(p->active),
                    double(pos.x),double(pos.y),double(pos.z),double(p->previous.x),double(p->previous.y),double(p->previous.z),double(p->force),double(bp.x),double(bp.y),double(bp.z));
                for(float value:inputs) std::fprintf(m_File,",%a",double(value));
                std::fprintf(m_File,",%a,%a,%a\n",double(away.x),double(away.y),double(away.z));
            }
        }
        std::fflush(m_File);
    }
private:
    bool m_Initialized=false;
    std::FILE *m_File=nullptr;
    std::uint64_t m_First=0,m_Last=0,m_Pass=0;
};
