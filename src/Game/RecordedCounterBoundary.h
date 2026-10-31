#pragma once

#include <BML/IBML.h>
#include <BML/ScriptHelper.h>
#include <cstring>

// Retail Gameplay_Energy/Timer has an unconnected B input on its >= Test.
// Logics.dll 0x25483a80 leaves that float scratch slot uninitialized. The
// original B reference reads a tiny positive stack value, so an exact-zero
// elapsed-time difference does NOT decrement the score (E2370/E2375).
//
// Give this legacy *counter graph* a defined boundary during recorded-delta
// playback: elapsed must be strictly past the next 500 ms threshold. At these
// thresholds, a positive binary32 time difference is many orders of magnitude
// larger than the retail scratch value. No address/garbage bits are copied into
// portable game state, and no generic Test implementation is made undefined.
// Ordinary fixed-step play keeps >=, including its exact 132-sample interval.
// This describes the qualified original B profile, not every possible outcome
// of an arbitrary Windows stack layout or a custom map's connected comparator.
class RecordedCounterBoundary {
public:
    void Forget() { m_Context=nullptr; m_Test=0; m_Changed=false; }

    template<class Loader> void Apply(Loader *bml, bool recorded) {
        auto *context=bml ? bml->GetCKContext() : nullptr;
        if (context!=m_Context) { Forget(); m_Context=context; }
        if (!context || (!recorded && !m_Changed)) return;
        auto *test=Resolve(bml);
        if (!test) return;
        auto *source=test->GetInputParameter(0)->GetRealSource();
        CK_COMPOPERATOR value=CKEQUAL;
        if (!source || source->GetValue(&value)!=CK_OK) return;
        if (recorded && value==CKGREATEREQUAL) {
            value=CKGREATER;
            source->SetValue(&value);
            m_Changed=true;
        } else if (!recorded && m_Changed && value==CKGREATER) {
            value=CKGREATEREQUAL;
            source->SetValue(&value);
            m_Changed=false;
        }
    }

private:
    static bool Named(CKObject *object, const char *name) {
        return object && object->GetName() && !std::strcmp(object->GetName(),name);
    }
    static bool IsCounterTest(CKBehavior *test) {
        if (!Named(test,"Test") || test->GetInputParameterCount()!=3 ||
            test->GetPrototypeGuid()!=CKGUID(0x17d66d26,0x726b7dec)) return false;
        auto *timer=test->GetParent();
        if (!Named(timer,"Timer") || !Named(timer->GetParent(),"Gameplay_Energy")) return false;
        auto *op=test->GetInputParameter(0);
        auto *a=test->GetInputParameter(1);
        auto *b=test->GetInputParameter(2);
        return op && a && b && op->GetGUID()==CKPGUID_COMPOPERATOR &&
            a->GetRealSource() && !b->GetRealSource() && test->GetCKContext()->GetParameterManager()->IsDerivedFrom(b->GetGUID(),CKPGUID_FLOAT);
    }
    template<class Loader> CKBehavior *Resolve(Loader *bml) {
        auto *object=m_Context->GetObject(m_Test);
        auto *cached=object && object->GetClassID()==CKCID_BEHAVIOR
            ? static_cast<CKBehavior *>(object) : nullptr;
        if (IsCounterTest(cached)) return cached;
        m_Test=0;
        m_Changed=false; // The previous object was removed or modified by another owner.
        auto *energy=bml->GetScriptByName("Gameplay_Energy");
        auto *timer=energy ? ScriptHelper::FindFirstBB(energy,"Timer") : nullptr;
        if (!timer) return nullptr;
        for (int i=0;i<timer->GetSubBehaviorCount();++i) {
            auto *test=timer->GetSubBehavior(i);
            if (!IsCounterTest(test)) continue;
            CK_COMPOPERATOR value=CKEQUAL;
            test->GetInputParameterValue(0,&value);
            if (value!=CKGREATEREQUAL && !(m_Changed && value==CKGREATER)) continue;
            m_Test=test->GetID();
            return test;
        }
        return nullptr;
    }
    CKContext *m_Context=nullptr;
    CK_ID m_Test=0;
    bool m_Changed=false;
};
