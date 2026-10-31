#include "ExplosionPolicy.h"
#include "CKGlobals.h"
#include <cstdio>
#include <cstdlib>
static unsigned checks=0;
static void Check(bool ok,const char *why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
int main(int argc,char **argv){
 Check(CKStartUp()==CK_OK,"startup");Check(argc==2&&CKGetPluginManager()->RegisterPlugin(argv[1])>=CK_OK,"Logics plugin");CKContext *ctx=nullptr;Check(CKCreateContext(&ctx,nullptr,0,0)==CK_OK,"context");
 auto make=[&](const char *name){auto *b=static_cast<CKBehavior*>(ctx->CreateObject(CKCID_BEHAVIOR,name));b->UseGraph();b->CreateInput("In");b->CreateOutput("Out");return b;};
 ExplosionPolicy policy;
 for(const char *name:{"Ball_Explosion_Wood","Ball_Explosion_Paper","Ball_Explosion_Stone"}){
  auto *script=make(name),*block=make("Set Position"),*after=make("physicalize pieces");script->AddSubBehavior(block);script->AddSubBehavior(after);
  auto *link=ScriptHelper::CreateLink(script,block,after);Check(link!=nullptr,"link");
  auto *from=link->GetInBehaviorIO(),*to=link->GetOutBehaviorIO();
  policy.Register(script);policy.Apply(ctx,false);Check(link->GetInBehaviorIO()==from&&link->GetOutBehaviorIO()==to,"ordinary original endpoints");
  policy.Register(script); // No duplicate ownership or capture of blocked endpoints.
  for(int i=0;i<5;++i){
   policy.Apply(ctx,true);auto *blockedFrom=link->GetInBehaviorIO(),*blockedTo=link->GetOutBehaviorIO();
   Check(blockedFrom!=from&&blockedTo!=to,"TAS matches legacy sink");
   link->SetInBehaviorIO(from);link->SetOutBehaviorIO(to);policy.Apply(ctx,true);
   Check(link->GetInBehaviorIO()==blockedFrom&&link->GetOutBehaviorIO()==blockedTo,"reconcile ordinary snapshot during TAS");
   policy.Apply(ctx,false);Check(link->GetInBehaviorIO()==from&&link->GetOutBehaviorIO()==to,"disable reconnects");
   link->SetInBehaviorIO(blockedFrom);link->SetOutBehaviorIO(blockedTo);policy.Apply(ctx,false);
   Check(link->GetInBehaviorIO()==from&&link->GetOutBehaviorIO()==to,"reconcile TAS snapshot during ordinary play");
  }
  auto *custom=make("custom");link->SetInBehaviorIO(custom->GetOutput(0));policy.Apply(ctx,true);policy.Apply(ctx,false);
  Check(link->GetInBehaviorIO()==custom->GetOutput(0),"third-party edge retained");
  ctx->DestroyObject(script->GetID());policy.Apply(ctx,false);Check(true,"deleted script ignored");
 }
 policy.Forget();CKCloseContext(ctx);CKShutdown();std::printf("PASS %u actual CK graph switch checks\n",checks);
}
