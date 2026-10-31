#include "RecordedCounterBoundary.h"
#include "CKGlobals.h"
#ifdef BALLANCE_COUNTER_CLOCK_INTEGRATION
#include "TimeManager.h"
#endif
#include <cstdio>
#include <cstdlib>
static int checks=0;
static void Check(bool ok,const char *why) {++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
struct Loader {
 CKContext *context;CKBehavior *energy;
 CKContext *GetCKContext(){return context;}
 CKBehavior *GetScriptByName(const char *){return energy;}
};
static CKBehavior *Behavior(CKContext *c,const char *name){auto *b=static_cast<CKBehavior *>(c->CreateObject(CKCID_BEHAVIOR,name));Check(b!=nullptr,"behavior");b->UseGraph();return b;}
int main(int argc,char **argv){
 Check(argc==2,"Logics plugin argument");Check(CKStartUp()==CK_OK,"startup");
 Check(CKGetPluginManager()->RegisterPlugin(argv[1])>=CK_OK,"register actual Logics");
 CKContext *context=nullptr;Check(CKCreateContext(&context,nullptr,0,0)==CK_OK,"context");
 auto *energy=Behavior(context,"Gameplay_Energy");auto *timer=Behavior(context,"Timer");energy->AddSubBehavior(timer);
 auto *test=Behavior(context,"Test");test->UseFunction();Check(test->InitFromGuid(CKGUID(0x17d66d26,0x726b7dec))==CK_OK,"real Test prototype");timer->AddSubBehavior(test);
 auto *op=timer->CreateLocalParameter("Test",CKPGUID_COMPOPERATOR);CK_COMPOPERATOR value=CKGREATEREQUAL;op->SetValue(&value);test->GetInputParameter(0)->SetDirectSource(op);
 auto *a=timer->CreateLocalParameter("elapsed difference",CKPGUID_TIME);float zero=0;a->SetValue(&zero);test->GetInputParameter(1)->SetGUID(CKPGUID_TIME);test->GetInputParameter(1)->SetDirectSource(a);
 auto *b=test->GetInputParameter(2);b->SetGUID(CKPGUID_TIME);b->SetDirectSource(nullptr);
 Loader loader{context,energy};RecordedCounterBoundary profile;
 auto current=[&](){CK_COMPOPERATOR v=CKEQUAL;op->GetValue(&v);return v;};
 profile.Apply(&loader,false);Check(current()==CKGREATEREQUAL,"ordinary rule retained");
 profile.Apply(&loader,true);Check(current()==CKGREATER,"derived-time disconnected input recognized");
 auto *function=test->GetFunction();CKBehaviorContext bc{};bc.Context=context;bc.Behavior=test;bc.ParameterManager=context->GetParameterManager();
 test->ActivateOutput(0,FALSE);test->ActivateOutput(1,FALSE);function(bc);Check(!test->IsOutputActive(0)&&test->IsOutputActive(1),"zero recorded boundary false");
 float positive=0.000030517578125f;a->SetValue(&positive);test->ActivateOutput(0,FALSE);test->ActivateOutput(1,FALSE);function(bc);Check(test->IsOutputActive(0),"smallest 500ms-adjacent difference true");
 for(int i=0;i<4;++i){profile.Apply(&loader,false);Check(current()==CKGREATEREQUAL,"leave restores ordinary boundary");profile.Apply(&loader,true);Check(current()==CKGREATER,"reenter recorded boundary");}
 value=CKGREATEREQUAL;op->SetValue(&value);profile.Apply(&loader,true);Check(current()==CKGREATER,"resynchronize graph parameter after restore");
 profile.Apply(&loader,false);a->SetValue(&zero);test->ActivateOutput(0,FALSE);test->ActivateOutput(1,FALSE);function(bc);Check(test->IsOutputActive(0),"zero ordinary boundary true");
 b->SetDirectSource(a);profile.Apply(&loader,true);Check(current()==CKGREATEREQUAL,"connected custom input untouched");
 b->SetDirectSource(nullptr);profile.Apply(&loader,true);Check(current()==CKGREATER,"recognize after disconnect");
 profile.Apply(&loader,false);
 value=CKGREATER;op->SetValue(&value);profile.Apply(&loader,true);profile.Apply(&loader,false);Check(current()==CKGREATER,"externally owned strict comparator retained");
 value=CKGREATEREQUAL;op->SetValue(&value);
#ifdef BALLANCE_COUNTER_CLOCK_INTEGRATION
 auto *clock=new TimeManager(context);auto *host=context->GetTimeManager();host->SetFixedDeltaTime(500.0f/132.0f);
 for(float rate:{30.0f,60.0f,120.0f,264.0f}) {
  host->SetFixedProcessRate(rate);clock->SetRecordedClock(false);clock->Reset();profile.Apply(&loader,false);
  for(int i=1;i<=132;++i){clock->Elapse(1000.0f/rate);float difference=clock->Now()-500.0f;a->SetValue(&difference);test->ActivateOutput(0,FALSE);test->ActivateOutput(1,FALSE);function(bc);Check(bool(test->IsOutputActive(0))==(i==132),"ordinary score triggers precisely sample132 at every pacing rate");}
 }
#endif
 profile.Forget();CKCloseContext(context);CKShutdown();std::printf("PASS %d real-CK counter boundary checks\n",checks);
}
