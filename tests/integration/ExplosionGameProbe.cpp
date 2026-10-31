// Test-only BML mod: load the actual game, keep TAS enabled but idle, and run
// each original explosion graph. Count actual Physicalize callbacks for debris.
#include <BML/BMLAll.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
MOD_EXPORT IMod *BMLEntry(IBML *);
MOD_EXPORT void BMLExit(IMod *);
class ExplosionGameProbe final:public IMod {
    unsigned ticks=0,phase=0,spawned=0,checks=0,failures=0;bool started=false,done=false;FILE *log=nullptr;
    const char *types[3]={"Wood","Paper","Stone"};
    void check(bool ok,const char *what){++checks;if(!ok)++failures;fprintf(log,"%s %s\n",ok?"PASS":"FAIL",what);fflush(log);}
public:
    explicit ExplosionGameProbe(IBML *b):IMod(b){}
    const char *GetID()override{return "ExplosionGameProbe";}
    const char *GetVersion()override{return "1";}
    const char *GetName()override{return "Actual explosion regression";}
    const char *GetAuthor()override{return "gzz2000";}
    const char *GetDescription()override{return "Test-only loaded original explosion graphs";}
    DECLARE_BML_VERSION;
    void OnPostStartMenu()override {
        if(!getenv("BALLANCE_EXPLOSION_PROBE_LOG"))return;
        m_BML->AddTimer(CKDWORD(5),[this](){
            auto *ctx=m_BML->GetCKContext();auto *array=m_BML->GetArrayByName("CurrentLevel");int level=2;array->SetElementValue(0,0,&level);
            auto *messages=ctx->GetMessageManager();
            messages->SendMessageSingle(messages->AddMessageType((CKSTRING)"Load Level"),ctx->GetCurrentLevel());
            messages->SendMessageSingle(messages->AddMessageType((CKSTRING)"Menu_Load"),m_BML->GetGroupByName("All_Sound"));
            if(auto *black=m_BML->Get2dEntityByName("M_BlackScreen"))black->Show(CKHIDE);
            auto *exit=ScriptHelper::FindFirstBB(m_BML->GetScriptByName("Menu_Main"),"Exit",false,1,0);exit->ActivateInput(0);exit->Activate();
        });
    }
    void OnPhysicalize(CK3dEntity *target,CKBOOL,float,float,float,const char*,CKBOOL,CKBOOL,CKBOOL,float,float,const char*,VxVector,int,CKMesh**,int,VxVector*,float*,int,CKMesh**)override {
        if(!started||done||!target||!target->GetName())return;
        char prefix[64];snprintf(prefix,sizeof(prefix),"Ball_%s_piece",types[phase]);
        if(!strncmp(target->GetName(),prefix,strlen(prefix))){++spawned;fprintf(log,"PHYSICALIZED %s\n",target->GetName());fflush(log);}
    }
    void OnProcess()override {
        if(done||!getenv("BALLANCE_EXPLOSION_PROBE_LOG")||!m_BML->IsPlaying()||++ticks<4000)return;
        if(!log){log=fopen(getenv("BALLANCE_EXPLOSION_PROBE_LOG"),"w");if(!log){done=true;return;}}
        if(!started){
            char name[64];snprintf(name,sizeof(name),"Ball_Explosion_%s",types[phase]);auto *script=m_BML->GetScriptByName(name);
            check(script!=nullptr,"original explosion graph present");spawned=0;started=true;
            fprintf(log,"TRIGGER %s\n",name);if(script){script->ActivateInput(0);script->Activate();}
        }
        if(ticks>=4256){
            fprintf(log,"RESULT type=%s spawned=%u\n",types[phase],spawned);check(spawned>0,"ordinary debris physically instantiated while TAS idle");
            if(++phase==3){done=true;fprintf(log,"RESULT checks=%u failures=%u\n",checks,failures);fclose(log);log=nullptr;m_BML->ExitGame();return;}
            started=false;ticks=3999;
        }
    }
};
IMod *BMLEntry(IBML *b){return new ExplosionGameProbe(b);}
void BMLExit(IMod *m){delete m;}
