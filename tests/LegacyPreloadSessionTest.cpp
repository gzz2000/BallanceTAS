#include <gtest/gtest.h>
#include "../src/Game/LegacyPreloadSession.h"

TEST(LegacyPreloadSession, CompletesBeforeRetirementAndNeverRearmsInGameplay) {
    LegacyPreloadSession session;
    int first, second;
    session.Register(11, &first, "Ball_Paper_piece01");
    session.Register(12, &second, "Ball_Wood_piece02");
    int calls = 0;
    auto complete = [&](const auto &owners) {
        ++calls;
        EXPECT_EQ(owners.size(), 2u);
        EXPECT_FALSE(owners[0].retired);
        EXPECT_FALSE(owners[1].retired);
        return std::string();
    };
    session.Retire(11, &first, complete);
    EXPECT_FALSE(session.Error().empty());
    session.Retire(12, &second, complete);
    session.FinishMenu();
    EXPECT_TRUE(session.Error().empty());
    // Same names/identities may be reused by later levels and rewinds.
    session.Register(11, &first, "Ball_Paper_piece01");
    session.Retire(11, &first, complete);
    session.FinishMenu();
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(session.Error().empty());
}

TEST(LegacyPreloadSession, ContextsDoNotShareCompletionState) {
    LegacyPreloadSession a, b;
    int owner;
    auto complete = [](const auto &) { return std::string(); };
    a.Register(1, &owner, "Ball_Stone_piece01");
    a.Retire(1, &owner, complete);
    a.FinishMenu();
    EXPECT_TRUE(a.Error().empty());
    EXPECT_FALSE(b.Error().empty());
    b.Register(1, &owner, "Ball_Stone_piece01");
    b.Retire(1, &owner, complete);
    b.FinishMenu();
    EXPECT_TRUE(b.Error().empty());
}

TEST(LegacyPreloadSession, RejectsReplacedOwnerBeforeCallingPhysics) {
    LegacyPreloadSession session;
    int oldOwner, replacement;
    session.Register(1, &oldOwner, "Ball_Wood_piece01");
    int calls = 0;
    session.Retire(1, &replacement, [&](const auto &) { ++calls; return std::string(); });
    session.FinishMenu();
    EXPECT_EQ(calls, 0);
    EXPECT_NE(session.Error().find("identity changed"), std::string::npos);
}

TEST(LegacyPreloadSession, UnsupportedPhysicsFailsClosedWithoutRetry) {
    LegacyPreloadSession session;
    int owner;
    session.Register(1, &owner, "Ball_Wood_piece01");
    int calls = 0;
    auto reject = [&](const auto &) { ++calls; return std::string("unsupported controller"); };
    session.Retire(1, &owner, reject);
    session.Retire(1, &owner, reject);
    session.FinishMenu();
    EXPECT_EQ(calls, 1);
    EXPECT_NE(session.Error().find("unsupported controller"), std::string::npos);
}

TEST(LegacyPreloadSession, IncompleteOrUnobservedStartupCannotBecomeReady) {
    LegacyPreloadSession missing, incomplete;
    missing.FinishMenu();
    EXPECT_FALSE(missing.Error().empty());
    int first, second;
    incomplete.Register(1, &first, "Ball_Paper_piece01");
    incomplete.Register(2, &second, "Ball_Paper_piece02");
    incomplete.Retire(1, &first, [](const auto &) { return std::string(); });
    incomplete.FinishMenu();
    EXPECT_NE(incomplete.Error().find("retiring"), std::string::npos);
}

TEST(LegacyPreloadSession, DuplicateAndUnexpectedRegistrationsCannotMaskChangedAssets) {
    int owner;
    LegacyPreloadSession duplicate;
    duplicate.Register(1, &owner, "Ball_Paper_piece01");
    duplicate.Register(1, &owner, "Ball_Paper_piece01");
    EXPECT_NE(duplicate.Error().find("Duplicate"), std::string::npos);
    EXPECT_FALSE(LegacyPreloadSession::IsFragment("Ball_Wood_piece"));
    EXPECT_FALSE(LegacyPreloadSession::IsFragment("Ball_Wood_piece01_mod"));
    EXPECT_FALSE(LegacyPreloadSession::IsFragment("Ball_Wood"));
}
