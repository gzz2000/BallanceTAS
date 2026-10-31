#pragma once
#include <cstdio>
#include <cstdlib>

// Diagnostic-only topology capture. Keep the native TAS copy identical.
namespace BehaviorGraphTrace {
inline const char *Name(CKObject *object) {
    return object && object->GetName() ? object->GetName() : "(null)";
}
inline void Write(FILE *file, CKBehavior *node, unsigned parent) {
    if (!node) return;
    std::fprintf(file, "NODE\t%u\t%u\t%s\n", (unsigned)node->GetID(), parent, Name(node));
    for (int i = 0; i < node->GetSubBehaviorLinkCount(); ++i) {
        CKBehaviorLink *link = node->GetSubBehaviorLink(i);
        CKBehaviorIO *from = link->GetInBehaviorIO(), *to = link->GetOutBehaviorIO();
        CKBehavior *a = from ? from->GetOwner() : nullptr, *b = to ? to->GetOwner() : nullptr;
        std::fprintf(file, "LINK\t%u\t%u\t%u\t%s\t%u\t%s\t%d\t%d\n",
            (unsigned)node->GetID(), (unsigned)link->GetID(), a ? (unsigned)a->GetID() : 0,
            Name(from), b ? (unsigned)b->GetID() : 0, Name(to),
            link->GetInitialActivationDelay(), link->GetActivationDelay());
    }
    for (int i = 0; i < node->GetSubBehaviorCount(); ++i)
        Write(file, node->GetSubBehavior(i), (unsigned)node->GetID());
}
inline void Capture(IBML *bml) {
    const char *path = std::getenv("BALLANCE_TAS_GRAPH_DUMP");
    if (!path || !*path) return;
    FILE *file = std::fopen(path, "w");
    if (!file) return;
    Write(file, bml->GetScriptByName("Default Level"), 0);
    Write(file, bml->GetScriptByName("Event_handler"), 0);
    Write(file, bml->GetScriptByName("Level_Init"), 0);
    Write(file, bml->GetScriptByName("Gameplay_Ingame"), 0);
    std::fclose(file);
}
}
