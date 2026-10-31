#pragma once

#include <BML/BMLAll.h>
#include <cstring>
#include <vector>
#ifdef TAS_SOURCE_ENGINE
#include "CKObjectLifetime.h"
#endif

// The legacy TAS profile disconnects Set Position in the three explosion
// scripts. Keep both sets of endpoints: installing the mod must not permanently
// remove ordinary ball-transform debris, and a practice rewind can restore the
// graph independently of the current TAS session.
class ExplosionPolicy {
    struct Object {
        CK_ID id = 0;
        CKObject *pointer = nullptr;
#ifdef TAS_SOURCE_ENGINE
        std::uint64_t token = 0;
#endif
        explicit Object(CKObject *p) : id(p ? p->GetID() : 0), pointer(p) {
#ifdef TAS_SOURCE_ENGINE
            if (p) token = CKGetObjectLifetimeToken(p->GetCKContext(), id);
#endif
        }
        bool Live(CKContext *context) const {
            return pointer && context->GetObject(id) == pointer
#ifdef TAS_SOURCE_ENGINE
                && token && CKGetObjectLifetimeToken(context, id) == token
#endif
                ;
        }
    };
    struct Edge {
        Object link, from, to, blockedFrom, blockedTo;
    };
    struct Script {
        Object block;
        std::vector<Edge> edges;
    };
public:
    void Register(CKBehavior *script) {
        if (!script || !script->GetName()) return;
        const char *name = script->GetName();
        if (std::strcmp(name, "Ball_Explosion_Wood") &&
            std::strcmp(name, "Ball_Explosion_Paper") &&
            std::strcmp(name, "Ball_Explosion_Stone")) return;
        auto *block = ScriptHelper::FindFirstBB(script, "Set Position");
        if (!block) return;
        auto *context = script->GetCKContext();
        for (const auto &entry : m_Scripts)
            if (entry.block.Live(context) && entry.block.pointer == block) return;
        Script entry{Object(block), {}};
        for (int i = 0; i < script->GetSubBehaviorLinkCount(); ++i) {
            auto *link = script->GetSubBehaviorLink(i);
            auto *from = link->GetInBehaviorIO();
            auto *to = link->GetOutBehaviorIO();
            if (!from || !to || (from->GetOwner() != block && to->GetOwner() != block)) continue;
            // Allocate the same persistent sink as the legacy patch, before
            // gameplay/checkpoint capture. No objects are created on toggles.
            ScriptHelper::DeleteLink(script, link);
            entry.edges.push_back({Object(link), Object(from), Object(to),
                Object(link->GetInBehaviorIO()), Object(link->GetOutBehaviorIO())});
            link->SetInBehaviorIO(from);
            link->SetOutBehaviorIO(to);
        }
        m_Scripts.push_back(std::move(entry));
    }

    void Apply(CKContext *context, bool tas) {
        if (!context) return;
        for (auto it = m_Scripts.begin(); it != m_Scripts.end();) {
            if (!it->block.Live(context)) { it = m_Scripts.erase(it); continue; }
            for (const auto &edge : it->edges) {
                if (!edge.link.Live(context) || !edge.from.Live(context) || !edge.to.Live(context) ||
                    !edge.blockedFrom.Live(context) || !edge.blockedTo.Live(context)) continue;
                auto *link = static_cast<CKBehaviorLink *>(edge.link.pointer);
                // Do not overwrite an unrelated mod's new edge.
                const bool normal = link->GetInBehaviorIO() == edge.from.pointer && link->GetOutBehaviorIO() == edge.to.pointer;
                const bool blocked = link->GetInBehaviorIO() == edge.blockedFrom.pointer && link->GetOutBehaviorIO() == edge.blockedTo.pointer;
                if ((tas && normal) || (!tas && blocked)) {
                    link->SetInBehaviorIO(static_cast<CKBehaviorIO *>((tas ? edge.blockedFrom : edge.from).pointer));
                    link->SetOutBehaviorIO(static_cast<CKBehaviorIO *>((tas ? edge.blockedTo : edge.to).pointer));
                }
            }
            if (tas) static_cast<CKBehavior *>(it->block.pointer)->Activate(FALSE);
            ++it;
        }
    }
    void Forget() { m_Scripts.clear(); }
private:
    std::vector<Script> m_Scripts;
};
